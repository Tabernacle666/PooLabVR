#pragma once
#include "Graphics.h"
#include "SpectacleSpace.h"

namespace OASIS {

void DiagnosticLine(const char*) noexcept;

class SphereBaseline {
    struct Position {
        const char* name;
        float distance;
        float yaw{};
    };
    static constexpr XrDuration Warmup=6'000'000'000;
    static constexpr XrDuration Settle=200'000'000;
    static constexpr XrDuration Hold=3'000'000'000;
    static constexpr XrDuration Sweep=600'000'000;
    static constexpr size_t Rotation=10;
    std::array<Position,Rotation+1> positions_{{
        {"centre_6m",6.f},{"centre_48m",48.f},{"centre_80m",80.f},
        {"centre_115m",115.f},{"centre_270m",270.f},{"centre_25m",25.f},
        {"bord_E_25m",25.f},{"bord_F_25m",25.f},{"un_oeil_25m",25.f},
        {"hors_champ_25m",25.f,-XM_PIDIV2},{"rotation_25m",25.f,-XM_PIDIV2}
    }};
    struct Projection {
        bool visible{};
        float boundsPercent{};
    };
    struct Window {
        uint64_t first{},last{};
        unsigned frames{},wanted{},prepared{},submitted{},tracked{},invalid{},gpu{};
        std::array<unsigned,MaxViews> visible{};
        std::array<double,MaxViews> coverage{};
        std::array<XMFLOAT3,MaxViews> eyes{};
        std::array<XrFovf,MaxViews> fov{};
        uint32_t eyeCount{};
        uint64_t vram{},budget{};
        std::vector<double> cpu,acquire,direct,compute,present,membrane;
    };
    std::array<Window,Rotation+1> windows_;
    std::optional<Pose> trackingOrigin_;
    XrTime firstSubmitted_{},phaseStart_{};
    size_t position_{};
    bool measuring_{},finished_{},headValid_{},monocularAvailable_{};
    float headsetHz_{};
    uint32_t width_{},height_{};

    static Pose Camera(float distance,float yaw){
        Pose result;
        result.p=Add(SpectacleCenter,{0.f,0.f,-(SpectacleRadius+distance)});
        XMStoreFloat4(&result.q,XMQuaternionRotationRollPitchYaw(0.f,yaw,0.f));
        return result;
    }

    Pose Target(XrTime time)const{
        if(!measuring_||finished_)return Camera(positions_[0].distance,0.f);
        const auto& position=positions_[position_];
        float yaw=position.yaw;
        if(position_==Rotation){
            const float t=std::clamp(float(double(time-phaseStart_-Settle)/double(Sweep)),0.f,1.f);
            yaw*=1.f-t*t*(3.f-2.f*t);
        }
        return Camera(position.distance,yaw);
    }

    static bool Visible(FXMMATRIX vp){
        const auto columns=XMMatrixTranspose(vp);
        const auto center=XMVectorSet(SpectacleCenter.x,SpectacleCenter.y,SpectacleCenter.z,1.f);
        const XMVECTOR planes[]{columns.r[3]+columns.r[0],columns.r[3]-columns.r[0],
            columns.r[3]+columns.r[1],columns.r[3]-columns.r[1],columns.r[2],
            columns.r[3]-columns.r[2]};
        for(const auto& plane:planes){
            if(XMVectorGetX(XMVector4Dot(center,plane))<
                -SpectacleRadius*XMVectorGetX(XMVector3Length(plane)))return false;
        }
        return true;
    }

    static Projection Project(FXMMATRIX vp){
        if(!Visible(vp))return {};
        // Conservative screen bounds of the sphere's box, clipped to this eye.
        XMFLOAT2 lo{1.f,1.f},hi{-1.f,-1.f};
        for(unsigned corner=0;corner<8;++corner){
            const XMFLOAT3 p=Add(SpectacleCenter,{corner&1u?SpectacleRadius:-SpectacleRadius,
                corner&2u?SpectacleRadius:-SpectacleRadius,corner&4u?SpectacleRadius:-SpectacleRadius});
            const auto clip=XMVector3Transform(XMLoadFloat3(&p),vp);
            const float w=XMVectorGetW(clip);
            if(w<=0.f)return {true,100.f};
            const float x=std::clamp(XMVectorGetX(clip)/w,-1.f,1.f);
            const float y=std::clamp(XMVectorGetY(clip)/w,-1.f,1.f);
            lo.x=std::min(lo.x,x);lo.y=std::min(lo.y,y);
            hi.x=std::max(hi.x,x);hi.y=std::max(hi.y,y);
        }
        return {true,25.f*(hi.x-lo.x)*(hi.y-lo.y)};
    }

    void SetEdgePositions(const XRFrame& frame){
        const float right=std::tan((frame.fov[0].angleRight+frame.fov[1].angleRight)*.5f);
        positions_[6].yaw=-std::atan(.87f*right);
        positions_[7].yaw=-std::atan(.97f*right);

        const auto initial=Matrix(Camera(positions_[0].distance,0.f));
        auto eyeMask=[&](float yaw){
            const auto toInitial=XMMatrixInverse(nullptr,Matrix(Camera(25.f,yaw)))*initial;
            unsigned mask=0;
            for(uint32_t eye=0;eye<frame.viewCount;++eye)
                if(Visible(toInitial*XMLoadFloat4x4(&frame.vp[eye])))mask|=1u<<eye;
            return mask;
        };
        constexpr float step=.0025f;
        float start=0.f,end=0.f;
        for(float yaw=-.25f;yaw>=-XM_PIDIV2;yaw-=step){
            const unsigned mask=eyeMask(yaw);
            if(mask&&(mask&(mask-1u))==0){
                if(!start)start=yaw;
                end=yaw;
            }else if(start)break;
        }
        monocularAvailable_=start!=0.f;
        positions_[8].yaw=monocularAvailable_?(start+end)*.5f:-std::atan(right);
    }

    static void WriteStats(const char* pose,const char* name,std::vector<double>& values){
        if(values.empty())return;
        std::sort(values.begin(),values.end());
        const auto count=values.size();
        auto percentile=[&](double p){
            return values[size_t(std::ceil(double(count)*p))-1u];
        };
        char line[256];
        std::snprintf(line,sizeof(line),
            "pose=%s mesure=%s n=%zu p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\r\n",
            pose,name,count,percentile(.5),percentile(.95),values.back());
        DiagnosticLine(line);
    }

public:
    SphereBaseline(){
        for(auto& window:windows_){
            window.cpu.reserve(256);window.acquire.reserve(256);
            window.direct.reserve(256);window.compute.reserve(256);
            window.present.reserve(256);window.membrane.reserve(256);
        }
    }

    TrackingMapping Mapping(){
        return {this,[](void* context,FrameInput& input,const Pose* raw,bool,XMFLOAT4X4& transform){
            auto& self=*static_cast<SphereBaseline*>(context);
            self.headValid_=raw!=nullptr;
            const auto target=self.Target(input.targetDisplayTime);
            if(raw){
                if(!self.trackingOrigin_)self.trackingOrigin_=*raw;
                const auto tracking=XMMatrixInverse(nullptr,Matrix(*self.trackingOrigin_))*Matrix(target);
                XMStoreFloat4x4(&transform,tracking);
                input.head=FromMatrix(Matrix(*raw)*tracking);
            }else input.head=target;
            input.trackingRevision=self.finished_?Rotation+2u:self.position_+1u;
            return false;
        },[](void* context,TrackingEvent){
            auto& self=*static_cast<SphereBaseline*>(context);
            self.trackingOrigin_.reset();
            if(self.measuring_&&!self.finished_)++self.windows_[self.position_].invalid;
            else self.firstSubmitted_=0;
        }};
    }

    bool CaptureReady()const{return finished_;}

    void Record(const XRFrame& frame,bool prepared,double cpuMs,double acquireMs,
                const GraphicsMetrics& metrics){
        if(finished_)return;
        const auto time=frame.displayTime;
        if(!measuring_){
            if(!frame.submitted||!frame.tracked||!headValid_||!frame.input.focused)return;
            if(!firstSubmitted_)firstSubmitted_=time;
            if(time-firstSubmitted_<Warmup)return;
            if(frame.viewCount!=2u)Fail("Sphere baseline requires two OpenXR views");
            SetEdgePositions(frame);
            width_=metrics.width;height_=metrics.height;headsetHz_=frame.headsetTargetHz;
            measuring_=true;phaseStart_=time;
            return;
        }
        const auto elapsed=time-phaseStart_-Settle;
        if(elapsed<0)return;
        if(elapsed>=(position_==Rotation?Sweep:Hold)){
            if(++position_==positions_.size())finished_=true;
            phaseStart_=time;
            return;
        }
        auto& window=windows_[position_];
        if(!window.first)window.first=frame.input.frameId;
        window.last=frame.input.frameId;
        ++window.frames;
        window.wanted+=frame.render;
        window.prepared+=prepared;
        window.submitted+=frame.submitted;
        window.invalid+=!headValid_||!frame.input.focused||(prepared&&!frame.tracked);
        window.vram=std::max(window.vram,metrics.vramUsed);
        window.budget=metrics.vramBudget;
        if(prepared)window.cpu.push_back(cpuMs);
        if(prepared&&frame.tracked)window.acquire.push_back(acquireMs);
        if(frame.tracked){
            ++window.tracked;
            if(!window.eyeCount){
                window.eyeCount=frame.viewCount;
                window.eyes=frame.eyes;
                window.fov=frame.fov;
            }
            for(uint32_t eye=0;eye<frame.viewCount;++eye){
                const auto projection=Project(XMLoadFloat4x4(&frame.vp[eye]));
                window.visible[eye]+=projection.visible;
                window.coverage[eye]+=projection.boundsPercent;
            }
        }
    }

    void Collect(Graphics& graphics){
        std::array<SphereGpuTimings,12> timings{};
        const auto count=graphics.ReadSphereGpuTimings(timings);
        for(size_t i=0;i<count;++i){
            const auto& sample=timings[i];
            for(auto& window:windows_){
                if(!window.first||sample.frame<window.first||sample.frame>window.last)continue;
                ++window.gpu;
                if(sample.directMs>=0.f)window.direct.push_back(sample.directMs);
                if(sample.computeMs>=0.f)window.compute.push_back(sample.computeMs);
                if(sample.presentMs>=0.f)window.present.push_back(sample.presentMs);
                if(sample.membraneMs>=0.f)window.membrane.push_back(sample.membraneMs);
                break;
            }
        }
    }

    bool Write(){
        bool valid=finished_;
        char line[768];
        std::snprintf(line,sizeof(line),
            "scenario=sphere_avant rendu=natif plein_3d=1 warmup_s=6 pose_s=3 "
            "cible_hz=%.2f resolution_oeil=%ux%u far_m=%.0f monoculaire_prevu=%u\r\n",
            headsetHz_,width_,height_,FarZ,unsigned(monocularAvailable_));
        DiagnosticLine(line);
        for(size_t i=0;i<windows_.size();++i){
            const auto& position=positions_[i];
            auto& window=windows_[i];
            if(!window.frames){valid=false;continue;}
            const auto seconds=double(i==Rotation?Sweep:Hold)/1e9;
            const bool usable=window.submitted&&window.present.size()&&window.membrane.size()&&!window.invalid;
            valid&=usable;
            std::snprintf(line,sizeof(line),
                "pose=%s distance_peau_m=%.1f lacet_deg=%.2f secondes=%.3f "
                "image_debut=%llu image_fin=%llu images=%u demandees=%u preparees=%u "
                "soumises=%u manquees=%u invalides=%u gpu_n=%u fps_soumis=%.2f "
                "visibles_g=%u visibles_d=%u boite_g_pct=%.2f boite_d_pct=%.2f "
                "vram_max_mio=%llu budget_mio=%llu valide=%u\r\n",
                position.name,position.distance,XMConvertToDegrees(position.yaw),seconds,
                static_cast<unsigned long long>(window.first),static_cast<unsigned long long>(window.last),
                window.frames,window.wanted,window.prepared,window.submitted,window.wanted-window.submitted,
                window.invalid,window.gpu,double(window.submitted)/seconds,window.visible[0],window.visible[1],
                window.coverage[0]/std::max(1u,window.tracked),window.coverage[1]/std::max(1u,window.tracked),
                static_cast<unsigned long long>(window.vram>>20),
                static_cast<unsigned long long>(window.budget>>20),unsigned(usable));
            DiagnosticLine(line);
            for(uint32_t eye=0;eye<window.eyeCount;++eye){
                const auto& p=window.eyes[eye];const auto& fov=window.fov[eye];
                std::snprintf(line,sizeof(line),
                    "pose=%s oeil=%u position_m_xyz=%.6f,%.6f,%.6f "
                    "fov_rad_g_d_h_b=%.6f,%.6f,%.6f,%.6f\r\n",
                    position.name,eye,p.x,p.y,p.z,fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown);
                DiagnosticLine(line);
            }
            WriteStats(position.name,"prepare_rendu_cpu",window.cpu);
            WriteStats(position.name,"xr_acquisition",window.acquire);
            WriteStats(position.name,"direct",window.direct);
            WriteStats(position.name,"compute",window.compute);
            WriteStats(position.name,"present",window.present);
            WriteStats(position.name,"membrane_field",window.membrane);
        }
        return valid;
    }
};

}
