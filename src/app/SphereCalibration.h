#pragma once
#include "Graphics.h"
#include "ShuttlePilot.h"
#include "ShuttleTour.h"
#include "VrInputMapping.h"
#include <cstdio>

namespace OASIS {
void DiagnosticLine(const char*) noexcept;

// Native 3D orbit/shuttle diagnostic. The former relief-distance calibration,
// proxy quality forcing and per-level image capture no longer exist.
class SphereCalibration {
    static constexpr XrDuration Warmup=6'000'000'000,Hold=3'000'000'000;
    XrTime first_{},start_{},solarOrigin_{};
    unsigned stage_{},submitted_{};
    bool tracked_{},solarArmed_{},tourInside_{},tourLanded_{};
    bool tourOasis3d_{},entered_{},exited_{},retreated_{},outside_{};
    bool solar_{};
    StandardVrInput* passage_{};
    uint32_t passageOwner_{};
    ShuttlePilot* shuttle_{};
    XMFLOAT3 worldAnchor_{};
    Pose localAnchor_{};
    float localDrift_{},localTurn_{},transportDistance_{};

    void RestartPassage(){
        // Set the initial cosmic pose for the end of the six-second warmup.
        // The camera stays in the cosmos until the live solver finds contact.
        const auto body=SolarBody(0u,double(Warmup)*1.e-9+.25);
        const auto spawn=Transform({-3.6f,.5f,-3.8f},body);
        passage_->SetInitialPose(SolarLookAt(spawn,body.p));
        passage_->StartSolarSystem();
        entered_=exited_=retreated_=outside_=false;
    }

    bool MapPassage(FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& tracking){
        const bool approaching=start_&&stage_==0u&&!entered_;
        const bool leaving=stage_==2u&&!retreated_;
        const float drive=approaching?1.f:leaving?-1.f:0.f;
        const auto& actions=StandardVrActions();
        auto& move=input.State(actions.move);
        move.active[0]=true;
        move.vector[0]={0.f,drive};
        input.State(actions.grab).down[0]=drive!=0.f;
        const Pose fixedHead{};
        const bool quit=passage_->Frame(input,raw?&fixedHead:nullptr,stage,tracking);
        entered_|=input.insideSphere==passageOwner_;
        outside_=input.insideSphere==Invalid;
        exited_|=entered_&&outside_&&stage_==2u;
        if(exited_&&outside_)
            retreated_|=Length(Sub(input.head.p,SolarCenter(passageOwner_,input.solarTime)))>=
                SolarBodyRadius(passageOwner_)+1.5f;
        return quit;
    }


    static Pose SolarPanorama(){
        const float scale=SolarOrbits.back().radius/192.f;
        return SolarLookAt(Scale({95.f,245.f,-275.f},scale),{});
    }


    double TourTime(XrTime now)const{return first_?double(now-first_)*1.e-9:0.;}
    bool MapShuttle(FrameInput& input,const Pose* raw,XMFLOAT4X4& tracking){
        input.solarTime=SolarElapsed(input.targetDisplayTime,solarOrigin_);
        input.head=ShuttleTour::View(TourTime(input.targetDisplayTime)-ShuttleTour::StandbySeconds,
                                     input.solarTime);
        const auto center=SolarCenter(0u,input.solarTime);
        const auto radial=Sub(input.head.p,center);
        const bool inside=Length(radial)<=SpectacleRadius;
        if(inside!=tourInside_){
            input.crossings[input.crossingCount++]={Add(center,Scale(Unit(radial),SpectacleRadius)),
                                                    inside,0u,input.solarTime};
            tourInside_=inside;
        }
        if(inside){
            input.insideSphere=0u;
            input.sceneUnitsPerMeter=SpectacleInteriorUnitsPerMeter;
            input.farClipMeters=SpectacleInteriorFarMeters;
        }
        input.trackingRevision=input.frameId;
        const float scale=input.sceneUnitsPerMeter;
        const auto toWorld=XMMatrixScaling(scale,scale,scale)*Matrix(input.head);
        XMStoreFloat4x4(&tracking,toWorld);

        const auto& actions=StandardVrActions();
        auto& grips=input.State(actions.handPose);
        grips.active={raw!=nullptr,raw!=nullptr};
        for(uint32_t hand=0;hand<2u;++hand){
            auto& grip=grips.pose[hand];
            grip.tracked=grips.active[hand];
            grip.pose.p={hand==0u?-.35f:.35f,-.31f,.29f};
            XMStoreFloat4(&grip.pose.q,XMQuaternionRotationRollPitchYaw(-XM_PIDIV2,0.f,0.f));
        }
        auto& board=input.State(actions.shuttle);
        board.active[1]=true;
        board.pressed[1]=board.down[1]=!shuttle_->Active();
        const Pose head{};
        input.controlContext=ControlContext::Diagnostic;
        shuttle_->ReadInput(input,raw?&head:nullptr,actions);
        shuttle_->Publish(toWorld,input);
        for(auto& grip:grips.pose)
            grip.pose=FromMatrix(Matrix(grip.pose)*toWorld);
        return false;
    }


    Pose SolarPose(double time)const{
        // Track the body during warmup. Fix each measured view at a predicted
        // midpoint so the orbits move against a stationary camera.
        const double middle=start_?double(start_-solarOrigin_+Hold/2)*1.e-9:time;
        if(stage_==0u)return SolarOasisApproach(middle);
        if(stage_==1u){
            const auto planet=SolarCenter(1u,middle);
            const auto radial=Unit(XMFLOAT3{planet.x,0.f,planet.z});
            const auto midpoint=Scale(planet,.5f);
            const auto camera=Add(midpoint,{radial.z*48.f,10.f,-radial.x*48.f});
            return SolarLookAt(camera,midpoint);
        }
        return SolarPanorama();
    }


public:
    explicit SphereCalibration(bool solar=false,StandardVrInput* passage=nullptr,
                               uint32_t passageOwner=0u):
        solar_(solar||passage!=nullptr),passage_(passage),passageOwner_(passageOwner){}
    void StartSolarSystem(){
        solarArmed_=true;
        if(passage_)RestartPassage();
    }
    void SetShuttle(ShuttlePilot& pilot){shuttle_=&pilot;}
    TrackingMapping Mapping(){
        return {this,[](void* context,FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& transform){
            auto& self=*static_cast<SphereCalibration*>(context);
            self.tracked_=raw!=nullptr;
            if(self.shuttle_&&self.solarArmed_)return self.MapShuttle(input,raw,transform);
            if(self.passage_&&self.solarArmed_)return self.MapPassage(input,raw,stage,transform);
            if(self.solar_&&self.solarArmed_){
                input.solarTime=SolarElapsed(input.targetDisplayTime,self.solarOrigin_);
                input.head=self.SolarPose(std::max(0.,input.solarTime));
                input.trackingRevision=input.frameId;
            }
            return false;
        },[](void*,TrackingEvent){}};
    }
    // A legacy diagnostic cannot force a proxy level: the frame is already 3D.
    void ApplyView(XRFrame&)const{}
    bool CaptureReady()const{return shuttle_?tourLanded_:stage_>=3u;}
    void Record(const XRFrame& frame,double){
        if(!tracked_||!frame.submitted)return;
        if(!first_){first_=frame.displayTime;return;}
        ++submitted_;
        if(shuttle_){
            tourOasis3d_|=frame.sphereNative==0u;
            const auto elapsed=TourTime(frame.displayTime);
            if(elapsed>=ShuttleTour::StandbySeconds+ShuttleTour::FlightSeconds&&
               frame.input.insideSphere==0u&&shuttle_->Ready()){
                tourLanded_=true;
                shuttle_->LogSample(frame.input.frameId);
            }
            return;
        }
        if(!start_){
            if(frame.displayTime-first_>=Warmup)start_=frame.displayTime;
            return;
        }
        if(passage_){
            entered_|=frame.input.insideSphere==passageOwner_;
            outside_=frame.input.insideSphere==Invalid;
            exited_|=entered_&&outside_&&stage_==2u;
            if(exited_&&outside_)
                retreated_|=Length(Sub(frame.input.head.p,SolarCenter(passageOwner_,frame.input.solarTime)))>=
                    SolarBodyRadius(passageOwner_)+1.5f;
        }
        if(frame.displayTime-start_>=Hold){start_=frame.displayTime;++stage_;}
    }
    void Collect(Graphics&){}
    bool Write(){
        char line[220];
        if(shuttle_){
            const bool valid=tourLanded_&&tourOasis3d_;
            std::snprintf(line,sizeof(line),
                "scenario=navette_natif images=%u oasis_3d=%u posee=%u valide=%u\r\n",
                submitted_,unsigned(tourOasis3d_),unsigned(tourLanded_),unsigned(valid));
            DiagnosticLine(line);return valid;
        }
        const bool valid=CaptureReady()&&submitted_>0u&&
            (!passage_||(entered_&&exited_&&retreated_&&outside_));
        std::snprintf(line,sizeof(line),
            "scenario=%s corps_solaires=9 rendu=geometrie_native poses=%u images=%u valide=%u\r\n",
            passage_?"franchissement":solar_?"systeme_solaire":"geometrie",
            stage_,submitted_,unsigned(valid));
        DiagnosticLine(line);
        return valid;
    }
};
} // namespace OASIS
