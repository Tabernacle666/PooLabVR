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
        std::optional<SphereRepresentation> force;
    };
    static constexpr XrDuration Warmup=6'000'000'000;
    static constexpr XrDuration Settle=330'000'000;
    static constexpr XrDuration Hold=3'000'000'000;
    static constexpr XrDuration Sweep=600'000'000;
    static constexpr std::array RepresentationNames{"3d","relief","relief_allege","boule","point","masquee","relief_ultra"};
    static constexpr size_t PerceptionZones=3;
    static constexpr size_t PerceptionSamples=SpherePerceptionDistances.size()*PerceptionZones*2u;
    std::array<float,PerceptionZones> perceptionYaw_{};
    size_t perceptionPosition_{};
    XrTime perceptionStart_{};
    struct PerceptionPose {
        uint32_t distance,zone;
        SphereRepresentation representation;
    };

    PerceptionPose CurrentPerception()const{
        using Level=SphereRepresentation;
        if(CaptureReady())return {1u,2u,Level::DepthLite};
        const size_t pose=perceptionPosition_/2u;
        const auto distance=uint32_t(pose/PerceptionZones),zone=uint32_t(pose%PerceptionZones);
        const bool second=(perceptionPosition_%2u)!=0u;
        if(zone==2u)return {distance,zone,second?Level::DepthLite:Level::Depth};
        if(distance==2u)return {distance,zone,second?Level::Mean:Level::DepthLite};
        return {distance,zone,second?Level::Depth:Level::Geometry};
    }
    static constexpr std::array<size_t,2> ZoneCComparisons{12,15};
    static constexpr size_t Rotation=18;
    std::array<Position,Rotation+1> positions_{{
        {"centre_6m",6.f},{"centre_48m",48.f},{"centre_80m",80.f},
        {"centre_115m",115.f},{"centre_270m",270.f},{"centre_25m",25.f},
        {"bord_E_25m",25.f},{"bord_F_25m",25.f},{"un_oeil_25m",25.f},
        {"hors_champ_25m",25.f,-XM_PIDIV2},
        {"compare_3d_48m",48.f,0.f,SphereRepresentation::Geometry},
        {"compare_relief_48m",48.f,0.f,SphereRepresentation::Depth},
        {"zone_C_relief_25m",25.f,0.f,SphereRepresentation::Depth},
        {"zone_C_relief_allege_25m",25.f,0.f,SphereRepresentation::DepthLite},
        {"zone_C_masquee_25m",25.f,0.f,SphereRepresentation::Hidden},
        {"zone_C_relief_48m",48.f,0.f,SphereRepresentation::Depth},
        {"zone_C_relief_allege_48m",48.f,0.f,SphereRepresentation::DepthLite},
        {"zone_C_masquee_48m",48.f,0.f,SphereRepresentation::Hidden},
        {"rotation_25m",25.f,-XM_PIDIV2}
    }};
    struct Projection {
        bool visible{};
        float boundsPercent{};
    };
    struct Window {
        uint64_t first{},last{},missingIntervals{};
        XrTime previousDisplay{};
        unsigned frames{},wanted{},prepared{},submitted{},tracked{},invalid{},gpu{};
        std::array<unsigned,MaxViews> visible{};
        std::array<double,MaxViews> coverage{};
        std::array<XMFLOAT3,MaxViews> eyes{};
        std::array<XrFovf,MaxViews> fov{};
        uint32_t eyeCount{};
        std::array<unsigned,RepresentationNames.size()> representations{};
        uint64_t vram{},budget{};
        std::vector<double> cpu,acquire,direct,compute,present,membrane;
    };
    std::array<Window,Rotation+1> windows_;
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
        if(finished_){
            const auto pose=CurrentPerception();
            return Camera(SpherePerceptionDistances[pose.distance],perceptionYaw_[pose.zone]);
        }
        if(!measuring_)return Camera(positions_[0].distance,0.f);
        const auto& position=positions_[position_];
        float yaw=position.yaw;
        if(position_==Rotation){
            const float t=std::clamp(float(double(time-phaseStart_-Settle)/double(Sweep)),0.f,1.f);
            yaw*=1.f-t*t*(3.f-2.f*t);
        }
        return Camera(position.distance,yaw);
    }

    static Projection Project(FXMMATRIX vp,SphereRepresentation representation){
        if(representation==SphereRepresentation::Hidden||!SphereIntersectsView(vp,
            SpectacleCenter,SpectacleRadius,representation==SphereRepresentation::Geometry))return {};
        const auto b=SphereScreenBounds(vp,SpectacleCenter,SpectacleRadius);
        const auto span=[](float lo,float hi){return std::clamp(hi,0.f,1.f)-std::clamp(lo,0.f,1.f);};
        return {true,100.f*span(b.x,b.z)*span(b.y,b.w)};
    }

    void SetEdgePositions(const XRFrame& frame){
        const float right=std::tan(frame.fov[0].angleRight);
        positions_[6].yaw=-std::atan(.87f*right);
        positions_[7].yaw=-std::atan(.97f*right);

        const auto initial=Matrix(Camera(positions_[0].distance,0.f));
        auto eyeMask=[&](float yaw){
            const auto toInitial=XMMatrixInverse(nullptr,Matrix(Camera(25.f,yaw)))*initial;
            unsigned mask=0;
            for(uint32_t eye=0;eye<frame.viewCount;++eye)
                if(SphereIntersectsView(toInitial*XMLoadFloat4x4(&frame.vp[eye]),
                    SpectacleCenter,SpectacleRadius,true))mask|=1u<<eye;
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
        const float otherRight=std::tan(frame.fov[1].angleRight);
        // Aim at the overlap of the two eyes' B and C bands.
        for(size_t zone=1;zone<perceptionYaw_.size();++zone){
            const float low=.20f*float(zone),high=low+.20f;
            const float minTangent=low*std::max(right,otherRight);
            const float maxTangent=high*std::min(right,otherRight);
            perceptionYaw_[zone]=-std::atan(minTangent<=maxTangent?
                .5f*(minTangent+maxTangent):.5f*(low+high)*right);
        }
        for(const size_t first:ZoneCComparisons)
            for(size_t i=first;i<first+3u;++i)positions_[i].yaw=perceptionYaw_[2];
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
        return {this,[](void* context,FrameInput& input,const Pose* raw,bool,XMFLOAT4X4&){
            auto& self=*static_cast<SphereBaseline*>(context);
            self.headValid_=raw!=nullptr;
            input.head=self.Target(input.targetDisplayTime);
            input.trackingRevision=self.finished_?Rotation+2u+self.perceptionPosition_:self.position_+1u;
            return false;
        },[](void* context,TrackingEvent){
            auto& self=*static_cast<SphereBaseline*>(context);
            if(self.measuring_&&!self.finished_)++self.windows_[self.position_].invalid;
            else self.firstSubmitted_=0;
        }};
    }

    bool CaptureReady()const{return perceptionPosition_==PerceptionSamples;}

    void ApplyView(XRFrame& frame)const{
        if(finished_){
            const auto pose=CurrentPerception();
            frame.spherePerception=true;
            frame.spherePerceptionZone=pose.zone;
            frame.spherePerceptionDistance=pose.distance;
            frame.sphereForce=pose.representation;
        }else if(measuring_)frame.sphereForce=positions_[position_].force;
    }

    void Record(const XRFrame& frame,bool prepared,double cpuMs,double acquireMs,
                const GraphicsMetrics& metrics){
        const auto time=frame.displayTime;
        if(finished_){
            if(CaptureReady())return;
            if(!frame.input.focused||!headValid_){
                perceptionStart_=0;
                return;
            }
            if(!frame.submitted)return;
            if(!perceptionStart_)perceptionStart_=time;
            if(time-perceptionStart_>=Hold){
                const auto pose=CurrentPerception();
                char line[192];
                std::snprintf(line,sizeof(line),
                    "perception_zone=%c distance_peau_m=%.0f demande=%s affiche=%s\r\n",
                    char('A'+pose.zone),SpherePerceptionDistances[pose.distance],
                    RepresentationNames[size_t(pose.representation)],
                    RepresentationNames[size_t(frame.sphereRepresentation)]);
                DiagnosticLine(line);
                ++perceptionPosition_;
                perceptionStart_=0;
            }
            return;
        }
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
        if(window.previousDisplay&&time>window.previousDisplay){
            const auto intervals=std::llround(double(time-window.previousDisplay)*headsetHz_/1e9);
            if(intervals>1)window.missingIntervals+=uint64_t(intervals-1);
        }
        window.previousDisplay=time;
        ++window.frames;
        window.wanted+=frame.render;
        window.prepared+=prepared;
        window.submitted+=frame.submitted;
        if(frame.submitted)++window.representations[size_t(frame.sphereRepresentation)];
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
                const auto projection=Project(XMLoadFloat4x4(&frame.vp[eye]),frame.sphereRepresentation);
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
        bool valid=finished_&&CaptureReady();
        std::array<bool,Rotation+1> validWindows{};
        char line[768];
        std::snprintf(line,sizeof(line),
            "scenario=sphere_phase1 rendu=natif representations=auto_et_forcees camera=script_verrouille warmup_s=6 pose_s=3 "
            "cible_hz=%.2f resolution_oeil=%ux%u far_m=%.0f monoculaire_prevu=%u "
            "perception_vues=%zu perception_secondes_par_vue=3\r\n",
            headsetHz_,width_,height_,FarZ,unsigned(monocularAvailable_),perceptionPosition_);
        DiagnosticLine(line);
        for(size_t i=0;i<windows_.size();++i){
            const auto& position=positions_[i];
            auto& window=windows_[i];
            if(!window.frames){valid=false;continue;}
            const auto seconds=double(i==Rotation?Sweep:Hold)/1e9;
            const auto expected=unsigned(std::lround(seconds*headsetHz_));
            const bool requestedLevel=(!position.force||
                window.representations[size_t(*position.force)]==window.submitted);
            const bool usable=window.submitted&&window.present.size()&&!window.invalid&&requestedLevel;
            valid&=usable;
            validWindows[i]=usable;
            std::snprintf(line,sizeof(line),
                "pose=%s distance_peau_m=%.1f lacet_deg=%.2f secondes=%.3f "
                "image_debut=%llu image_fin=%llu images=%u demandees=%u preparees=%u "
                "soumises=%u manquees=%u cible_images=%u intervalles_xr_absents=%llu "
                "invalides=%u gpu_n=%u fps_soumis=%.2f "
                "visibles_g=%u visibles_d=%u boite_g_pct=%.2f boite_d_pct=%.2f "
                "vram_max_mio=%llu budget_mio=%llu valide=%u\r\n",
                position.name,position.distance,XMConvertToDegrees(position.yaw),seconds,
                static_cast<unsigned long long>(window.first),static_cast<unsigned long long>(window.last),
                window.frames,window.wanted,window.prepared,window.submitted,window.wanted-window.submitted,
                expected,static_cast<unsigned long long>(window.missingIntervals),window.invalid,window.gpu,double(window.submitted)/seconds,window.visible[0],window.visible[1],
                window.coverage[0]/std::max(1u,window.tracked),window.coverage[1]/std::max(1u,window.tracked),
                static_cast<unsigned long long>(window.vram>>20),
                static_cast<unsigned long long>(window.budget>>20),unsigned(usable));
            DiagnosticLine(line);
            const auto& levels=window.representations;
            std::snprintf(line,sizeof(line),
                "pose=%s representation_3d=%u relief=%u relief_allege=%u boule=%u point=%u masquee=%u\r\n",
                position.name,levels[0],levels[1],levels[2],levels[3],levels[4],levels[5]);
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
        for(const size_t first:ZoneCComparisons){
            const auto& relief=windows_[first].direct;
            const auto& lite=windows_[first+1u].direct;
            const auto& hidden=windows_[first+2u].direct;
            if(!validWindows[first]||!validWindows[first+1u]||!validWindows[first+2u]||
               relief.empty()||lite.empty()||hidden.empty())continue;
            const double baseline=hidden[(hidden.size()-1u)/2u];
            const double reliefCost=relief[(relief.size()-1u)/2u]-baseline;
            const double liteCost=lite[(lite.size()-1u)/2u]-baseline;
            std::snprintf(line,sizeof(line),
                "comparaison=zone_C distance_peau_m=%.0f gpu_direct_masque_us=%.3f "
                "surcout_relief_us=%.3f surcout_relief_allege_us=%.3f\r\n",
                positions_[first].distance,1000.*baseline,1000.*reliefCost,1000.*liteCost);
            DiagnosticLine(line);
        }
        return valid;
    }
};

}
