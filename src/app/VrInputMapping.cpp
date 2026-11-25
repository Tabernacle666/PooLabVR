#include "VrInputMapping.h"
#include "SessionLaunch.h"
#include "BuildingCampus.h"
#include "OrbitTool.h"
#include "ShuttlePilot.h"
#include "LocalProfile.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
namespace OASIS {
namespace { VrInputActions standardVrActions;constexpr std::array<float,4> stanceOffsets{-1.15f,-.72f,-.36f,0.f}; }
const VrInputActions& StandardVrActions() noexcept{return standardVrActions;}
void DeclareStandardVrInputActions(InputSystem& input){VrInputActions a;a.navigationSet=input.RegisterActionSet("navigation","Navigation");a.interactionSet=input.RegisterActionSet("interaction","Interaction");a.trackingSet=input.RegisterActionSet("tracking","Tracking");a.move=input.RegisterAction(a.navigationSet,"navigation.move","Move",InputActionType::Vector2);a.turn=input.RegisterAction(a.navigationSet,"navigation.turn","Turn",InputActionType::Vector2);a.recenter=input.RegisterAction(a.navigationSet,"navigation.recenter","Recenter",InputActionType::Boolean);a.heightUp=input.RegisterAction(a.navigationSet,"navigation.height_up","Height up",InputActionType::Boolean);a.heightDown=input.RegisterAction(a.navigationSet,"navigation.height_down","Height down",InputActionType::Boolean);a.grab=input.RegisterAction(a.interactionSet,"interaction.grab","Grab",InputActionType::Float);a.primary=input.RegisterAction(a.interactionSet,"interaction.primary","Primary",InputActionType::Float);a.secondary=input.RegisterAction(a.interactionSet,"interaction.secondary","Secondary",InputActionType::Float);a.touch=input.RegisterAction(a.interactionSet,"interaction.touch","Touch",InputActionType::Boolean);a.physical=input.RegisterAction(a.interactionSet,"interaction.physical","Physical hand",InputActionType::Boolean);a.proximity=input.RegisterAction(a.interactionSet,"interaction.proximity","Proximity",InputActionType::Boolean);a.equipRightTool=input.RegisterAction(a.interactionSet,"interaction.mode","Cycle right-hand tool",InputActionType::Boolean);a.shuttle=input.RegisterAction(a.interactionSet,"interaction.shuttle","Board or leave shuttle",InputActionType::Boolean);a.capture=input.RegisterAction(a.interactionSet,"interaction.capture","Capture",InputActionType::Boolean);a.next=input.RegisterAction(a.interactionSet,"interaction.next","Capture (legacy)",InputActionType::Boolean);a.quit=input.RegisterAction(a.interactionSet,"interaction.quit","Quit",InputActionType::Boolean);a.feedback=input.RegisterAction(a.interactionSet,"interaction.feedback","Feedback",InputActionType::Haptic);a.handPose=input.RegisterAction(a.trackingSet,"tracking.hand_pose","Hand pose",InputActionType::Pose);a.aimPose=input.RegisterAction(a.trackingSet,"tracking.aim_pose","Aim pose",InputActionType::Pose);a.shuttleMixReset=input.RegisterAction(a.interactionSet,"interaction.shuttle_mix_reset","Reset shuttle mixer setting",InputActionType::Boolean);
    a.gravity=input.RegisterAction(a.navigationSet,"navigation.gravity","Toggle gravity",InputActionType::Boolean);
    a.shuttleSet=input.RegisterActionSet("shuttle","Shuttle");
    a.shuttleOptions=input.RegisterAction(a.shuttleSet,"shuttle.options","Shuttle option stick",InputActionType::Vector2);
    a.shuttleOptionsPage=input.RegisterAction(a.shuttleSet,"shuttle.options_page","Shuttle option page",InputActionType::Boolean);
    a.shuttleSystems=input.RegisterAction(a.shuttleSet,"shuttle.systems","Shuttle systems stick",InputActionType::Vector2);
    a.shuttleCalibrate=input.RegisterAction(a.shuttleSet,"shuttle.calibrate","Shuttle calibrate",InputActionType::Boolean);
    a.shuttleBrake=input.RegisterAction(a.shuttleSet,"shuttle.brake","Shuttle brakes",InputActionType::Float);
    standardVrActions=a;}
namespace {
void ReanchorTracking(XMFLOAT4X4& trackingToWorld,const Pose& raw,
                      XMFLOAT3 head,float unitsPerMeter){
    const auto orientation=FromMatrix(XMLoadFloat4x4(&trackingToWorld)).q;
    const auto rotate=XMMatrixRotationQuaternion(XMLoadFloat4(&orientation));
    XMStoreFloat4x4(&trackingToWorld,
        XMMatrixTranslation(-raw.p.x,-raw.p.y,-raw.p.z)*
        XMMatrixScaling(unitsPerMeter,unitsPerMeter,unitsPerMeter)*rotate*
        XMMatrixTranslation(head.x,head.y,head.z));
}
}
void StandardVrInput::Anchor(const Pose& p,bool stage,XMFLOAT4X4& trackingToWorld){
    const auto forward=Sub(Transform({0,0,1},p),p.p);const float yaw=std::atan2(forward.x,forward.z);
    if(initialPosePending_){
        const auto targetForward=Sub(Transform({0,0,1},initialPose_),initialPose_.p);
        const float targetYaw=std::atan2(targetForward.x,targetForward.z);
        XMStoreFloat4x4(&trackingToWorld,XMMatrixTranslation(-p.p.x,-p.p.y,-p.p.z)*XMMatrixRotationY(targetYaw-yaw)*XMMatrixTranslation(initialPose_.p.x,initialPose_.p.y,initialPose_.p.z));
        if(spectacleEnabled_){
            spectacle_.Locate(initialPose_.p);
            if(spectacle_.Inside())ReanchorTracking(trackingToWorld,p,initialPose_.p,spectacle_.UnitsPerMeter());
        }
        initialPosePending_=false;
    }else if(spectacleEnabled_&&hasLastHead_){
        XMStoreFloat4x4(&trackingToWorld,XMMatrixRotationY(-yaw));
        ReanchorTracking(trackingToWorld,p,lastHead_,spectacle_.UnitsPerMeter());
    }else{
        const float y=(stage?0.0f:1.72f-p.p.y)+heightOffset_+stanceOffsets[stance_];
        XMStoreFloat4x4(&trackingToWorld,XMMatrixTranslation(-p.p.x,y,-p.p.z)*XMMatrixRotationY(-yaw));
        if(spectacleEnabled_){
            const auto head=FromMatrix(Matrix(p)*XMLoadFloat4x4(&trackingToWorld)).p;
            spectacle_.Locate(head);
            if(spectacle_.Inside())ReanchorTracking(trackingToWorld,p,head,spectacle_.UnitsPerMeter());
        }
    }
    anchored_=true;anchorPending_=false;++trackingRevision_;locomotionTime_=0;
}
VrTravel StandardVrInput::Locomotion(const FrameInput& input,const Pose& raw,FXMMATRIX transform,XMFLOAT2 turn){
    if(!locomotionEnabled_&&!heightAdjustEnabled_){locomotionTime_=0;locomotionFocused_=false;return {};}
    double dt=locomotionTime_?double(input.targetDisplayTime-locomotionTime_)*1e-9:0;locomotionTime_=input.targetDisplayTime;
    bool advance=input.focused&&locomotionFocused_&&dt>0&&dt<=.25;locomotionFocused_=input.focused;
    if(!advance)return {};
    const bool freeFlight=(solarSystem_&&!solarPassage_.Inside())||
        (building_&&!buildingGravity_&&(solarSystem_?solarPassage_.Sphere()==0u:spectacle_.Inside()));
    XMFLOAT3 shift{};float yaw{},pitch{};
    if(locomotionEnabled_){
        Pose reference=FromMatrix(Matrix(raw)*transform);
        if(locomotion_.reference==LocomotionSettings::Reference::LeftHand||locomotion_.reference==LocomotionSettings::Reference::RightHand){
            uint32_t hand=locomotion_.reference==LocomotionSettings::Reference::LeftHand?0u:1u;
            const auto& pose=input.PoseState(StandardVrActions().handPose,hand);if(pose.tracked)reference=FromMatrix(Matrix(pose.pose)*transform);
        }
        const bool worldReference=locomotion_.reference==LocomotionSettings::Reference::World;
        if(worldReference&&freeFlight)reference=FromMatrix(transform);
        const auto basis=Matrix(reference);
        auto forward=TransformDirection({0.f,0.f,1.f},basis);
        auto right=TransformDirection({1.f,0.f,0.f},basis);
        if(!freeFlight){
            if(worldReference)forward={0.f,0.f,1.f};
            forward.y=0.f;
            if(Length(forward)<.001f){
                forward=TransformDirection({0.f,0.f,1.f},Matrix(FromMatrix(transform)));
                forward.y=0.f;
            }
            forward=Unit(forward);
            right={forward.z,0.f,-forward.x};
        }
        const auto move=solarEntryBrake_?XMFLOAT2{}:input.Vector(StandardVrActions().move);
        const float boost=solarSystem_&&input.Down(StandardVrActions().grab,0u)?10.f:1.f;
        shift=Scale(Add(Scale(right,move.x),Scale(forward,move.y)),locomotion_.speed*boost*float(dt));
        yaw=turn.x*locomotion_.yawSpeed*float(dt);
        if(freeFlight)pitch=-turn.y*locomotion_.yawSpeed*float(dt);
    }
    float dy=0.f;
    if(heightAdjustEnabled_){
        const int direction=int(input.Down(StandardVrActions().heightUp,0))-
                            int(input.Down(StandardVrActions().heightDown,0));
        dy=locomotion_.heightSpeed*float(direction)*float(dt);
        if(!spectacleEnabled_&&!solarSystem_){
            const float height=std::clamp(heightOffset_+dy,locomotion_.heightMin,locomotion_.heightMax);
            dy=height-heightOffset_;
            heightOffset_=height;
        }
    }
    const float stanceAxis=locomotionEnabled_&&!freeFlight?turn.y:0.f;
    const int direction=stanceAxis>.6f?1:stanceAxis<-.6f?-1:0;
    if(direction&&input.targetDisplayTime>=nextStanceStep_){
        const int next=std::clamp(stance_+direction,0,int(stanceOffsets.size())-1);
        if(next!=stance_){dy+=stanceOffsets[next]-stanceOffsets[stance_];stance_=next;nextStanceStep_=input.targetDisplayTime+250000000;}
    }
    shift.y+=dy;
    return {shift,yaw,pitch};
}
void StandardVrInput::SolarFrame(FrameInput& input,const Pose* raw,XMFLOAT4X4& trackingToWorld,
                                XMFLOAT2 turn,bool recenter,bool boardShuttle,int liftStep){
    input.solarTime=(solarOrigin_||(raw&&input.focused))?
        SolarElapsed(input.targetDisplayTime,solarOrigin_):0.;
    if(sessionTour_){
        if(sessionTourFixed_)input.solarTime=sessionTourElapsed_;
        if(raw){
            // Wall-clock trajectory: a slow engine must not slow the route.
            if(!sessionTourFixed_){
                if(sessionTourOrigin_<0.&&sessionTourStarted_&&lastSolarTime_>=0.)
                    sessionTourOrigin_=lastSolarTime_;
                if(sessionTourOrigin_>=0.)
                    sessionTourElapsed_=std::clamp(input.solarTime-sessionTourOrigin_,0.,SessionTourSeconds);
            }
            sessionTourStarted_=false;
            const auto pose=sessionTourFixed_?SessionTourPose(sessionTourElapsed_,input.solarTime):
                SessionForwardTourPose(sessionTourElapsed_,input.solarTime);
            solarPassage_.Scripted(pose,*raw,input,!anchored_);
            if(!anchored_||input.crossingCount)++trackingRevision_;
            anchored_=true;
            lastSolarRaw_=*raw;
        }
        if(anchored_)solarPassage_.Publish(lastSolarRaw_,input,trackingToWorld);
        input.trackingRevision=trackingRevision_;
        lastSolarTime_=input.solarTime;
        return;
    }
    solarRecenterPending_|=recenter;
    const bool aboard=shuttle_&&shuttle_->Active();
    const auto move=input.Vector(StandardVrActions().move);
    if(move.x==0.f&&move.y==0.f)solarEntryBrake_=false;
    if(raw&&(input.focused||!anchored_)){
        const double elapsed=input.solarTime-lastSolarTime_;
        const bool starting=!anchored_||initialPosePending_;
        const bool continuous=!starting&&!boardShuttle&&!anchorPending_&&!solarRecenterPending_&&trackingContinuous_&&
            elapsed>0.&&elapsed<=.25;
        if(continuous){
            const auto travel=aboard?shuttle_->Advance(elapsed,solarPassage_,lastSolarRaw_):
                Locomotion(input,*raw,solarPassage_.TrackingToFrame(*raw),turn);
            if(aboard){locomotionTime_=input.targetDisplayTime;locomotionFocused_=input.focused;}
            const auto oldPosition=solarPassage_.LocalHead();
            const auto oldOwner=solarPassage_.Sphere();
            solarPassage_.Advance(travel,Sub(raw->p,lastSolarRaw_.p),lastSolarTime_,input,aboard);
            if(building_&&!aboard&&solarPassage_.Sphere()==0u){
                if(liftStep)building_->RequestLift(solarPassage_.LocalHead(),liftStep);
                const auto corrected=building_->Move(oldPosition,solarPassage_.LocalHead(),elapsed,
                    buildingGravity_,oldOwner!=0u||input.crossingCount!=0);
                solarPassage_.SetLocalHead(corrected);
            }
            if(aboard)shuttle_->Crossed({input.crossings.data(),input.crossingCount});
            trackingRevision_+=input.crossingCount;
        }else{
            if(boardShuttle||starting){
                const auto spawn=boardShuttle?shuttle_->ParkingPose(input.solarTime):
                    initialPosePending_?initialPose_:SolarOasisApproach(input.solarTime);
                solarPassage_.Start(spawn,*raw,input.solarTime);
            }else{
                solarPassage_.Resume(lastSolarRaw_,*raw,input,solarRecenterPending_,!aboard);
                if(aboard){
                    shuttle_->Reanchor(lastSolarRaw_,*raw);
                    shuttle_->Crossed({input.crossings.data(),input.crossingCount});
                }
            }
            anchored_=true;initialPosePending_=anchorPending_=solarRecenterPending_=false;
            locomotionTime_=input.targetDisplayTime;locomotionFocused_=input.focused;
            ++trackingRevision_;
        }
        lastSolarRaw_=*raw;
    }
    if(input.crossingCount)
        solarEntryBrake_=input.crossings[input.crossingCount-1u].entering;
    if(anchored_){
        solarPassage_.Publish(lastSolarRaw_,input,trackingToWorld);
        if(aboard)shuttle_->Publish(XMLoadFloat4x4(&trackingToWorld),input);
    }
    trackingContinuous_=raw&&input.focused;
    lastSolarTime_=input.solarTime;
    input.trackingRevision=trackingRevision_;
}
bool StandardVrInput::Frame(FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& trackingToWorld){
    // Both hands are still in tracking space here. Resolve the grab before
    // locomotion can use this frame's thumbstick for turning or stance.
    const auto& controls=StandardVrActions();
    bool aboard=false,boardShuttle=false;
    if(solarSystem_&&shuttle_&&!sessionTour_){
        const bool wasAboard=shuttle_->Active();
        shuttle_->ReadInput(input,raw,controls);
        aboard=shuttle_->Active();
        if(aboard!=wasAboard){
            boardShuttle=aboard;
            if(!aboard)solarPassage_.LevelRig();
            if(orbit_)orbit_->Suspend();
            ++trackingRevision_;
        }
        if(input.Pressed(controls.shuttle,1u))
            input.State(controls.equipRightTool).pressed[1]=false;
    }
    const bool toolOwnsStick=orbit_&&!aboard&&orbit_->ReadInput(input);
    if(aboard)shuttle_->ReadOptions(input,controls);
    input.controlContext=aboard?ControlContext::Shuttle:
        toolOwnsStick?ControlContext::PedestrianTool:ControlContext::Pedestrian;
    if(building_&&!aboard&&input.focused&&input.Pressed(controls.gravity,0))
        buildingGravity_=!buildingGravity_;
    const bool recenter=!aboard&&input.Pressed(controls.recenter);
    auto turn=aboard||toolOwnsStick?XMFLOAT2{}:input.Vector(controls.turn);
    const bool liftControl=building_&&!aboard&&!toolOwnsStick&&input.focused&&input.Down(controls.grab,0)&&
        (solarSystem_?solarPassage_.Sphere()==0u:spectacle_.Inside());
    const int liftDirection=liftControl?(turn.y>.65f?1:turn.y<-.65f?-1:0):0;
    int liftStep=liftDirection!=liftStick_?liftDirection:0;
    liftStick_=liftDirection;
    if(liftControl)turn.y=0.f;
    if(building_&&!aboard&&!toolOwnsStick&&input.focused&&!liftStep)
        liftStep=int(input.Pressed(controls.heightUp,0))-int(input.Pressed(controls.heightDown,0));
    input.crossingCount=0;
    if(solarSystem_){
        SolarFrame(input,raw,trackingToWorld,turn,recenter,boardShuttle,liftStep);
        return false;
    }
    if(recenter)anchorPending_=true;
    if(raw){
        const bool reanchor=anchorPending_||!anchored_;
        const bool continuous=input.focused&&trackingContinuous_&&hasLastHead_&&!reanchor;
        if(reanchor)Anchor(*raw,stage,trackingToWorld);
        else if(spectacleEnabled_&&hasLastHead_&&!continuous)
            ReanchorTracking(trackingToWorld,*raw,lastHead_,spectacle_.UnitsPerMeter());

        const auto transform=XMLoadFloat4x4(&trackingToWorld);
        const auto travel=Locomotion(input,*raw,transform,turn);
        if(travel.yaw!=0.f||Dot(travel.meters,travel.meters)>0.f){
            const auto head=TransformPoint(raw->p,transform);
            const float units=spectacleEnabled_?spectacle_.UnitsPerMeter():1.f;
            const auto shift=Scale(travel.meters,units);
            XMStoreFloat4x4(&trackingToWorld,transform*XMMatrixTranslation(-head.x,0,-head.z)*
                XMMatrixRotationY(travel.yaw)*XMMatrixTranslation(head.x+shift.x,shift.y,head.z+shift.z));
        }
        input.head=FromMatrix(Matrix(*raw)*XMLoadFloat4x4(&trackingToWorld));
        if(spectacleEnabled_&&continuous){
            const auto deltaMeters=Scale(Sub(input.head.p,lastHead_),1.f/spectacle_.UnitsPerMeter());
            const auto position=spectacle_.Move(lastHead_,deltaMeters,input);
            if(input.crossingCount!=0u){
                ReanchorTracking(trackingToWorld,*raw,position,spectacle_.UnitsPerMeter());
                input.head.p=position;
                ++trackingRevision_;
            }
        }
        if(building_){
            const auto now=input.targetDisplayTime;
            const double dt=buildingTime_?double(now-buildingTime_)*1.e-9:0.;
            buildingTime_=now;
            if(continuous&&liftStep&&spectacle_.Inside())building_->RequestLift(input.head.p,liftStep);
            if(spectacle_.Inside()&&!input.crossingCount){
                const auto corrected=building_->Move(lastHead_,input.head.p,dt,buildingGravity_,!continuous);
                const auto correction=Sub(corrected,input.head.p);
                XMStoreFloat4x4(&trackingToWorld,XMLoadFloat4x4(&trackingToWorld)*
                    XMMatrixTranslation(correction.x,correction.y,correction.z));
                input.head.p=corrected;
            }
        }
        lastHead_=input.head.p;
        hasLastHead_=true;
    }else{
        locomotionTime_=0;
        locomotionFocused_=false;
    }
    trackingContinuous_=raw&&input.focused;
    if(spectacleEnabled_){
        input.insideSphere=spectacle_.Sphere();
        input.sceneUnitsPerMeter=spectacle_.UnitsPerMeter();
        input.farClipMeters=spectacle_.Inside()?SpectacleInteriorFarMeters:FarZ;
    }
    input.trackingRevision=trackingRevision_;
    return false;
}
void StandardVrInput::Event(TrackingEvent event){
    trackingContinuous_=false;
    if(event==TrackingEvent::SessionStopped){locomotionTime_=0;locomotionFocused_=false;nextStanceStep_=0;}
    else anchorPending_=true;
}
TrackingMapping StandardVrInput::Mapping(){return {this,[](void* context,FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& transform){return static_cast<StandardVrInput*>(context)->Frame(input,raw,stage,transform);},[](void* context,TrackingEvent event){static_cast<StandardVrInput*>(context)->Event(event);}};}
bool StandardVrInput::LoadConfiguration(const std::wstring& path,const std::filesystem::path& user){
    auto read=[](const std::filesystem::path& file,LocomotionSettings& settings){
        bool valid=true;
        ReadLocalProfile(file,[&](std::string_view key,std::string_view value){
            if(!key.starts_with("oasis."))return;
            key.remove_prefix(6);
            if(key=="locomotion_reference"){
                if(value=="head")settings.reference=LocomotionSettings::Reference::Head;
                else if(value=="left_hand")settings.reference=LocomotionSettings::Reference::LeftHand;
                else if(value=="right_hand")settings.reference=LocomotionSettings::Reference::RightHand;
                else if(value=="world")settings.reference=LocomotionSettings::Reference::World;
                else valid=false;
                return;
            }
            const std::string text(value);
            char* tail=nullptr;
            const float number=std::strtof(text.c_str(),&tail);
            if(text.empty()||*tail||!std::isfinite(number)){valid=false;return;}
            if(key=="speed"&&number>=0&&number<=10)settings.speed=number;
            else if(key=="yaw_speed"&&number>=0&&number<=10)settings.yawSpeed=number;
            else if(key=="height_speed"&&number>=0&&number<=5)settings.heightSpeed=number;
            else if(key=="height_min")settings.heightMin=number;
            else if(key=="height_max")settings.heightMax=number;
            else valid=false;
        });
        return valid&&settings.heightMin<=settings.heightMax;
    };
    LocomotionSettings factory;
    if(!read(std::filesystem::path(path),factory))Fail("input.ini: invalid factory locomotion");
    locomotion_=factory;
    if(user.empty())return true;
    auto next=factory;
    if(!read(user,next))return false;
    locomotion_=next;
    return true;
}
}
