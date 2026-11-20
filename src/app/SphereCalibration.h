#pragma once
#include "Graphics.h"
#include "SpectacleSpace.h"
#include <cstdio>

namespace OASIS {

void DiagnosticLine(const char*) noexcept;

class SphereCalibration {
    using Level=SphereRepresentation;
    static constexpr std::array levels{Level::Geometry,Level::Depth,Level::DepthLite,Level::Mean};
    static constexpr std::array names{"3d","relief","relief_allege","boule","point","masquee"};
    static constexpr std::array fixedNames{"A_centrale","B_centrale","recouvrement","hors_champ","relais"};
    static constexpr std::array fieldNames{"local","autour","loin_panorama","interieur","haut_arriere","relais"};
    static constexpr std::array solarNames{"oasis_proche","premiere_orbite","panorama_9_orbites"};
    static constexpr size_t fixedCount=fixedNames.size(),stageCount=fixedCount+3u*levels.size();
    static constexpr XrDuration warmup=6'000'000'000,settle=330'000'000,hold=3'000'000'000;
    static constexpr XrDuration relay=2'000'000'000,orbit=5'000'000'000;
    static constexpr float arcDegrees=62.f;
    struct Window {
        uint64_t first{},last{};
        std::array<unsigned,SpectaclePairCount> native{};
        std::array<std::array<unsigned,names.size()>,SpectaclePairCount> counts{};
        std::array<unsigned,3> zones{};
        std::array<unsigned,MaxViews> visibleMax{};
        unsigned projections{},insideLite{},nativeFrames{},nativeChanges{};
        uint32_t lastNative{Invalid};
        double solarFirst{},solarLast{};
        std::vector<float> direct,compute,present,cpu;
        unsigned submitted{},requested{},invalid{};
        int lastAngle{-1000};
    };
    std::array<Window,stageCount> windows_;
    SphereLodSettings settings_;
    XrTime first_{},start_{};
    size_t stage_{};
    bool tracked_{},field_{},solar_{},solarArmed_{};
    XrTime solarOrigin_{};

    size_t Band()const{return (stage_-fixedCount)%3u;}
    Level Quality()const{return levels[(stage_-fixedCount)/3u];}
    XrDuration Duration()const{return field_?hold:stage_>=fixedCount?orbit:stage_==4u?relay:hold;}

    Pose SolarPose(double time)const{
        // Track the body during warmup. Fix each measured view at a predicted
        // midpoint so the orbits move against a stationary camera.
        const double middle=start_?double(start_-solarOrigin_+settle+hold/2)*1.e-9:time;
        if(stage_==0u)return SolarOasisApproach(middle);
        if(stage_==1u){
            const auto planet=SolarCenter(1u,middle);
            const auto radial=Unit(XMFLOAT3{planet.x,0.f,planet.z});
            const auto midpoint=Scale(planet,.5f);
            const auto camera=Add(midpoint,{radial.z*48.f,10.f,-radial.x*48.f});
            return SolarLookAt(camera,midpoint);
        }
        return SolarLookAt({95.f,245.f,-275.f},{});
    }

    Pose FieldPose(XrTime time)const{
        const auto center=SpectacleCenters[0];
        auto at=[](XMFLOAT3 position,float pitch,float yaw){
            Pose pose{position};
            XMStoreFloat4(&pose.q,XMQuaternionRotationRollPitchYaw(pitch,yaw,0.f));
            return pose;
        };
        if(CaptureReady()||stage_==2u)return at({-29.f,9.f,-87.f},-.04f,.27f);
        if(stage_==0u)return {Add(center,{0.f,0.f,-9.4f})};
        if(stage_==1u)return {Add(center,{0.f,0.f,-19.f})};
        if(stage_==3u)return at(Add(center,{0.f,.35f,-3.5f}),-.12f,.80f);
        if(stage_==4u)return at({0.f,8.f,58.f},-.60f,XM_PI);
        const float t=std::clamp(float(double(time-start_-settle)/double(hold)),0.f,1.f);
        return {Add(center,{0.f,0.f,-(11.f+2.f*std::cos(XM_2PI*t))})};
    }

    Pose OrbitPose(size_t band,float degrees)const{
        const float angle=XMConvertToRadians(degrees),x=std::sin(angle),z=std::cos(angle);
        const auto& limits=settings_.bands[band];
        const float a=limits.enterDistance+SpectacleRadius,b=limits.width;
        const float r=std::max(SpectacleRadius+.25f,1.f/std::sqrt(x*x/(b*b)+z*z/(a*a)));
        return {Sub(SpectacleCenters[1],{r*x,0.f,r*z})};
    }

    Pose Target(XrTime time)const{
        if(field_)return FieldPose(time);
        if(CaptureReady())return OrbitPose(1u,27.f);
        const auto center=SpectacleCenters[0];
        if(stage_==0u)return {Add(center,{0.f,0.f,-9.4f})};
        if(stage_==1u)return {Add(SpectacleCenters[1],{0.f,0.f,-9.4f})};
        if(stage_==2u||stage_==3u){
            Pose result{Add(center,{-18.f,0.f,0.f})};
            const float yaw=stage_==2u?XM_PIDIV2:-XM_PIDIV2;
            XMStoreFloat4(&result.q,XMQuaternionRotationRollPitchYaw(0.f,yaw,0.f));
            return result;
        }
        const float t=std::clamp(float(double(time-start_-settle)/double(Duration())),0.f,1.f);
        if(stage_==4u){
            const float eased=t*t*(3.f-2.f*t);
            return {Add(center,{12.f*eased,0.f,-9.4f})};
        }
        return OrbitPose(Band(),arcDegrees*(2.f*t-1.f));
    }

    void LogAngles(const XRFrame& frame,float command)const{
        char line[448];
        for(uint32_t i=0;i<frame.viewCount;++i){
            const auto vp=XMLoadFloat4x4(&frame.vp[i]);
            const auto columns=XMMatrixTranspose(vp);
            const auto forward=Unit(XMFLOAT3{XMVectorGetX(columns.r[3]),0.f,XMVectorGetZ(columns.r[3])});
            const auto delta=Sub(SpectacleCenters[1],frame.eyes[i]);
            const float z=Dot(delta,forward),x=Dot(delta,{forward.z,0.f,-forward.x});
            const float direction=XMConvertToDegrees(std::atan2(x,z));
            const float radius=XMConvertToDegrees(std::asin(std::min(1.f,SpectacleRadius/std::hypot(x,z))));
            const float edge=std::copysign(std::max(0.f,std::abs(direction)-radius),direction);
            const auto b=SphereScreenBounds(vp,SpectacleCenters[1],SpectacleRadius);
            const auto span=[](float a,float b){return std::clamp(b,0.f,1.f)-std::clamp(a,0.f,1.f);};
            const bool show=frame.sphereLevels[1]!=Level::Hidden&&
                SphereIntersectsView(vp,SpectacleCenters[1],SpectacleRadius,false);
            std::snprintf(line,sizeof(line),
                "ellipse=%c demande=%s affiche=%s angle_consigne=%.1f oeil=%s "
                "x_m=%.3f z_m=%.3f peau_m=%.3f centre_deg=%.2f bord_deg=%.2f boite_pct=%.3f\r\n",
                char('A'+Band()),names[size_t(Quality())],names[size_t(frame.sphereLevels[1])],command,i==0?"G":"D",
                x,z,Length(delta)-SpectacleRadius,direction,edge,show?100.f*span(b.x,b.z)*span(b.y,b.w):0.f);
            DiagnosticLine(line);
        }
    }

public:
    explicit SphereCalibration(const SphereLodSettings& settings,bool field=false,bool solar=false):
        settings_(settings),field_(field||solar),solar_(solar){
        for(auto& window:windows_){
            window.direct.reserve(480);window.compute.reserve(480);
            if(field_){window.present.reserve(256);window.cpu.reserve(256);}
        }
    }
    void StartSolarSystem(){solarArmed_=true;}
    TrackingMapping Mapping(){
        return {this,[](void* context,FrameInput& input,const Pose* raw,bool,XMFLOAT4X4&){
            auto& self=*static_cast<SphereCalibration*>(context);
            self.tracked_=raw!=nullptr;
            if(self.solar_){
                if(self.solarArmed_)input.solarTime=SolarElapsed(input.targetDisplayTime,self.solarOrigin_);
                input.head=self.SolarPose(std::max(0.,input.solarTime));
            }else input.head=self.Target(input.targetDisplayTime);
            input.trackingRevision=self.stage_+1u;
            if(self.field_&&!self.solar_&&self.stage_==3u){
                input.insideSphere=0u;
                input.sceneUnitsPerMeter=SpectacleInteriorUnitsPerMeter;
                input.farClipMeters=SpectacleInteriorFarMeters;
            }
            return false;
        },[](void* context,TrackingEvent){
            auto& self=*static_cast<SphereCalibration*>(context);
            if(!self.CaptureReady()&&self.start_)++self.windows_[self.stage_].invalid;
        }};
    }
    size_t PoseCount()const{return solar_?solarNames.size():field_?fieldNames.size():stageCount;}
    bool CaptureReady()const{return stage_==PoseCount();}

    void ApplyView(XRFrame& frame)const{
        if(field_||stage_<fixedCount)return;
        frame.sphereForces[0]=Level::Hidden;
        frame.sphereForces[1]=CaptureReady()?Level::DepthLite:Quality();
        frame.spherePerception=true;
        frame.sphereOrbitOwner=1u;
        frame.sphereOrbitBand=CaptureReady()?1u:uint32_t(Band());
    }

    void Record(const XRFrame& frame,double cpuMs){
        if(CaptureReady())return;
        const bool tracked=tracked_&&frame.input.focused;
        if(!start_){
            if(!tracked||!frame.submitted){first_=0;return;}
            if(!first_)first_=frame.displayTime;
            if(frame.displayTime-first_>=warmup)start_=frame.displayTime;
            return;
        }
        const auto elapsed=frame.displayTime-start_-settle;
        if(elapsed<0)return;
        auto& window=windows_[stage_];
        if(elapsed>=Duration()){
            ++stage_;
            start_=frame.displayTime;
            return;
        }
        window.requested+=frame.render;
        window.invalid+=!tracked||(frame.submitted&&!frame.tracked);
        if(!frame.submitted)return;
        if(!window.first){
            window.first=frame.input.frameId;
            window.solarFirst=frame.input.solarTime;
        }
        window.last=frame.input.frameId;
        window.solarLast=frame.input.solarTime;
        ++window.submitted;
        if(field_){
            window.cpu.push_back(float(cpuMs));
            if(window.submitted>1u&&window.lastNative!=frame.sphereNative)++window.nativeChanges;
            window.lastNative=frame.sphereNative;
            window.nativeFrames+=frame.sphereNative!=Invalid;
            for(uint32_t eye=0;eye<frame.viewCount;++eye)
                window.visibleMax[eye]=std::max(window.visibleMax[eye],frame.visibleSpheres[eye]);
            const uint32_t sphereCount=solar_?SolarPlanetCount:SpectacleSphereCount;
            for(uint32_t i=0;i<sphereCount;++i){
                const auto level=frame.sphereLevels[i];
                if(level==Level::Hidden)continue;
                ++window.zones[size_t(frame.sphereZones[i])];
                window.projections+=SphereUsesRelief(level);
                window.insideLite+=level==Level::DepthLite;
            }
            return;
        }
        if(frame.sphereNative<SpectaclePairCount)++window.native[frame.sphereNative];
        for(uint32_t i=0;i<SpectaclePairCount;++i)++window.counts[i][size_t(frame.sphereLevels[i])];
        if(stage_>=fixedCount){
            const float degrees=arcDegrees*(2.f*float(double(elapsed)/double(orbit))-1.f);
            const int angle=int(std::floor((degrees+arcDegrees)/15.f));
            if(angle!=window.lastAngle){window.lastAngle=angle;LogAngles(frame,degrees);}
        }
    }

    void Collect(Graphics& graphics){
        std::array<SphereGpuTimings,12> timings{};
        const size_t count=graphics.ReadSphereGpuTimings(timings);
        for(size_t i=0;i<count;++i){
            const auto& sample=timings[i];
            for(auto& window:windows_){
                if(!window.first||sample.frame<window.first||sample.frame>window.last)continue;
                if(sample.directMs>=0.f)window.direct.push_back(sample.directMs);
                if(sample.computeMs>=0.f)window.compute.push_back(sample.computeMs);
                if(field_&&sample.presentMs>=0.f)window.present.push_back(sample.presentMs);
                break;
            }
        }
    }

    bool Write(){
        DiagnosticLine(solar_?"scenario=systeme_solaire spheres=9 camera=script_verrouille warmup_s=6 pose_s=3\r\n":
            field_?"scenario=sphere_phase3 spheres=100 camera=script_verrouille warmup_s=6 pose_s=3\r\n":
            "scenario=sphere_phase2 spheres=2 camera=script_verrouille warmup_s=6 pose_s=3 orbite_s=5\r\n");
        bool valid=CaptureReady();
        char line[448];
        auto p=[](std::vector<float>& data,double q){
            if(data.empty())return -1.f;
            std::sort(data.begin(),data.end());
            return data[size_t(std::ceil(double(data.size())*q))-1u];
        };
        const size_t count=PoseCount();
        for(size_t i=0;i<count;++i){
            auto& w=windows_[i];
            const bool validWindow=w.submitted&&!w.invalid&&!w.direct.empty();
            valid&=validWindow;
            if(field_){
                const double frames=double(std::max(1u,w.submitted));
                const float direct50=p(w.direct,.5),direct95=p(w.direct,.95),compute95=p(w.compute,.95);
                const float present95=p(w.present,.95),cpu95=p(w.cpu,.95);
                std::snprintf(line,sizeof(line),
                    "pose=%s demandees=%u soumises=%u natif=%u relais=%u local_moy=%.2f autour_moy=%.2f loin_moy=%.2f "
                    "relief_moy=%.2f rl_moy=%.2f proj_g_max=%u proj_d_max=%u direct_p50_ms=%.3f "
                    "direct_p95_ms=%.3f compute_p95_ms=%.3f present_p95_ms=%.3f cpu_p95_ms=%.3f valide=%u\r\n",
                    solar_?solarNames[i]:fieldNames[i],w.requested,w.submitted,w.nativeFrames,w.nativeChanges,w.zones[0]/frames,
                    w.zones[1]/frames,w.zones[2]/frames,w.projections/frames,w.insideLite/frames,
                    w.visibleMax[0],w.visibleMax[1],direct50,direct95,compute95,present95,cpu95,unsigned(validWindow));
                DiagnosticLine(line);
                continue;
            }
            char label[48];
            if(i<fixedCount)std::snprintf(label,sizeof(label),"%s",fixedNames[i]);
            else std::snprintf(label,sizeof(label),"ellipse_%c_%s",char('A'+(i-fixedCount)%3u),
                               names[size_t(levels[(i-fixedCount)/3u])]);
            const auto p50=p(w.direct,.5),p95=p(w.direct,.95),compute=p(w.compute,.5);
            std::snprintf(line,sizeof(line),"pose=%s demandees=%u soumises=%u natif_A=%u natif_B=%u "
                          "direct_p50_ms=%.3f direct_p95_ms=%.3f compute_p50_ms=%.3f valide=%u\r\n",
                          label,w.requested,w.submitted,w.native[0],w.native[1],p50,p95,compute,unsigned(validWindow));
            DiagnosticLine(line);
            for(uint32_t owner=0;owner<SpectaclePairCount;++owner){
                const auto& c=w.counts[owner];
                std::snprintf(line,sizeof(line),"pose=%s sphere=%c 3d=%u relief=%u relief_allege=%u boule=%u point=%u masquee=%u\r\n",
                    label,char('A'+owner),c[0],c[1],c[2],c[3],c[4],c[5]);
                DiagnosticLine(line);
            }
        }
        if(solar_){
            std::snprintf(line,sizeof(line),
                "soleil_xyz_m=0,0,0 soleil_rayon_m=%.2f omni_portee_m=%.2f degagement_min_m=%.2f\r\n",
                SunRadius,SunLightRange,SolarOrbits[1].radius-SunRadius-SpectacleRadius);
            DiagnosticLine(line);
            const auto& panorama=windows_[solarNames.size()-1u];
            for(uint32_t owner=0;owner<SolarPlanetCount;++owner){
                const auto& orbit=SolarOrbits[owner];
                const auto a=SolarCenter(owner,panorama.solarFirst);
                const auto b=SolarCenter(owner,panorama.solarLast);
                std::snprintf(line,sizeof(line),
                    "planete=%u orbite=%u rayon_m=%.2f inclinaison_deg=%.1f periode_s=%.2f "
                    "panorama_debut_xyz=%.3f,%.3f,%.3f panorama_fin_xyz=%.3f,%.3f,%.3f\r\n",
                    owner,SolarOrbitSlot(owner)+1u,orbit.radius,XMConvertToDegrees(orbit.inclination),
                    orbit.period,a.x,a.y,a.z,b.x,b.y,b.z);
                DiagnosticLine(line);
            }
        }
        return valid;
    }
};

}
