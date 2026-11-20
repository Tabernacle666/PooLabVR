#include "VrInputMapping.h"
#include "OrbitTool.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
namespace OASIS {
namespace { VrInputActions standardVrActions;constexpr std::array<float,4> stanceOffsets{-1.15f,-.72f,-.36f,0.f}; }
const VrInputActions& StandardVrActions() noexcept{return standardVrActions;}
void DeclareStandardVrInputActions(InputSystem& input){VrInputActions a;a.navigationSet=input.RegisterActionSet("navigation","Navigation");a.interactionSet=input.RegisterActionSet("interaction","Interaction");a.trackingSet=input.RegisterActionSet("tracking","Tracking");a.move=input.RegisterAction(a.navigationSet,"navigation.move","Move",InputActionType::Vector2);a.turn=input.RegisterAction(a.navigationSet,"navigation.turn","Turn",InputActionType::Vector2);a.recenter=input.RegisterAction(a.navigationSet,"navigation.recenter","Recenter",InputActionType::Boolean);a.heightUp=input.RegisterAction(a.navigationSet,"navigation.height_up","Height up",InputActionType::Boolean);a.heightDown=input.RegisterAction(a.navigationSet,"navigation.height_down","Height down",InputActionType::Boolean);a.grab=input.RegisterAction(a.interactionSet,"interaction.grab","Grab",InputActionType::Float);a.primary=input.RegisterAction(a.interactionSet,"interaction.primary","Primary",InputActionType::Float);a.secondary=input.RegisterAction(a.interactionSet,"interaction.secondary","Secondary",InputActionType::Float);a.touch=input.RegisterAction(a.interactionSet,"interaction.touch","Touch",InputActionType::Boolean);a.physical=input.RegisterAction(a.interactionSet,"interaction.physical","Physical hand",InputActionType::Boolean);a.proximity=input.RegisterAction(a.interactionSet,"interaction.proximity","Proximity",InputActionType::Boolean);a.equipRightTool=input.RegisterAction(a.interactionSet,"interaction.mode","Cycle right-hand tool",InputActionType::Boolean);a.capture=input.RegisterAction(a.interactionSet,"interaction.capture","Capture",InputActionType::Boolean);a.next=input.RegisterAction(a.interactionSet,"interaction.next","Capture (legacy)",InputActionType::Boolean);a.quit=input.RegisterAction(a.interactionSet,"interaction.quit","Quit",InputActionType::Boolean);a.feedback=input.RegisterAction(a.interactionSet,"interaction.feedback","Feedback",InputActionType::Haptic);a.handPose=input.RegisterAction(a.trackingSet,"tracking.hand_pose","Hand pose",InputActionType::Pose);a.aimPose=input.RegisterAction(a.trackingSet,"tracking.aim_pose","Aim pose",InputActionType::Pose);standardVrActions=a;}
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
    XMFLOAT3 shift{};float yaw{};
    if(locomotionEnabled_){
        Pose reference=FromMatrix(Matrix(raw)*transform);
        if(locomotion_.reference==LocomotionSettings::Reference::LeftHand||locomotion_.reference==LocomotionSettings::Reference::RightHand){
            uint32_t hand=locomotion_.reference==LocomotionSettings::Reference::LeftHand?0u:1u;
            const auto& pose=input.PoseState(StandardVrActions().handPose,hand);if(pose.tracked)reference=FromMatrix(Matrix(pose.pose)*transform);
        }
        XMFLOAT3 forward=locomotion_.reference==LocomotionSettings::Reference::World?XMFLOAT3{0,0,1}:Sub(Transform({0,0,1},reference),reference.p);forward.y=0;
        if(Length(forward)<.001f){forward=Sub(Transform({0,0,1},FromMatrix(transform)),FromMatrix(transform).p);forward.y=0;}
        forward=Unit(forward);XMFLOAT3 right{forward.z,0,-forward.x};auto move=input.Vector(StandardVrActions().move);
        const float boost=solarSystem_&&input.Down(StandardVrActions().grab,0u)?10.f:1.f;
        shift=Scale(Add(Scale(right,move.x),Scale(forward,move.y)),locomotion_.speed*boost*float(dt));
        yaw=turn.x*locomotion_.yawSpeed*float(dt);
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
    const float stanceAxis=locomotionEnabled_?turn.y:0.f;
    const int direction=stanceAxis>.6f?1:stanceAxis<-.6f?-1:0;
    if(direction&&input.targetDisplayTime>=nextStanceStep_){
        const int next=std::clamp(stance_+direction,0,int(stanceOffsets.size())-1);
        if(next!=stance_){dy+=stanceOffsets[next]-stanceOffsets[stance_];stance_=next;nextStanceStep_=input.targetDisplayTime+250000000;}
    }
    shift.y=dy;
    return {shift,yaw};
}
void StandardVrInput::SolarFrame(FrameInput& input,const Pose* raw,XMFLOAT4X4& trackingToWorld,
                                XMFLOAT2 turn,bool recenter){
    input.solarTime=(solarOrigin_||(raw&&input.focused))?
        SolarElapsed(input.targetDisplayTime,solarOrigin_):0.;
    solarRecenterPending_|=recenter;
    if(raw&&(input.focused||!anchored_)){
        const double elapsed=input.solarTime-lastSolarTime_;
        const bool starting=!anchored_||initialPosePending_;
        const bool continuous=!starting&&!anchorPending_&&!solarRecenterPending_&&trackingContinuous_&&
            elapsed>0.&&elapsed<=.25;
        if(continuous){
            const auto travel=Locomotion(input,*raw,solarPassage_.TrackingToFrame(*raw),turn);
            solarPassage_.Advance(travel,Sub(raw->p,lastSolarRaw_.p),lastSolarTime_,input);
            trackingRevision_+=input.crossingCount;
        }else{
            if(starting){
                const auto spawn=initialPosePending_?initialPose_:SolarOasisApproach(input.solarTime);
                solarPassage_.Start(spawn,*raw,input.solarTime);
            }else solarPassage_.Resume(lastSolarRaw_,*raw,input,solarRecenterPending_);
            anchored_=true;initialPosePending_=anchorPending_=solarRecenterPending_=false;
            locomotionTime_=input.targetDisplayTime;locomotionFocused_=input.focused;
            ++trackingRevision_;
        }
        lastSolarRaw_=*raw;
    }
    if(anchored_)solarPassage_.Publish(lastSolarRaw_,input,trackingToWorld);
    trackingContinuous_=raw&&input.focused;
    lastSolarTime_=input.solarTime;
    input.trackingRevision=trackingRevision_;
}
bool StandardVrInput::Frame(FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& trackingToWorld){
    // Both hands are still in tracking space here. Resolve the grab before
    // locomotion can use this frame's thumbstick for turning or stance.
    const bool toolOwnsStick=orbit_&&orbit_->ReadInput(input);
    const bool recenter=input.Pressed(StandardVrActions().recenter);
    const auto turn=toolOwnsStick?XMFLOAT2{}:input.Vector(StandardVrActions().turn);
    input.crossingCount=0;
    if(solarSystem_){
        SolarFrame(input,raw,trackingToWorld,turn,recenter);
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
void StandardVrInput::LoadConfiguration(const std::wstring& path){
    std::ifstream file{std::filesystem::path(path)};if(!file)Fail("input.ini: cannot open locomotion configuration");
    LocomotionSettings next;std::string line;
    while(std::getline(file,line)){
        auto end=line.find_first_of("#;\r");if(end!=std::string::npos)line.resize(end);
        auto trim=[](std::string value){auto first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");return first==std::string::npos?std::string{}:value.substr(first,last-first+1);};
        line=trim(line);if(line.empty())continue;auto equals=line.find('=');if(equals==std::string::npos)Fail("input.ini: expected key=value");
        auto key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));if(key.rfind("oasis.",0)!=0)continue;key.erase(0,6);
        if(key=="locomotion_reference"){
            if(value=="head")next.reference=LocomotionSettings::Reference::Head;
            else if(value=="left_hand")next.reference=LocomotionSettings::Reference::LeftHand;
            else if(value=="right_hand")next.reference=LocomotionSettings::Reference::RightHand;
            else if(value=="world")next.reference=LocomotionSettings::Reference::World;
            else Fail("input.ini: locomotion reference must be head, left_hand, right_hand or world");
            continue;
        }
        char* tail=nullptr;float number=std::strtof(value.c_str(),&tail);if(value.empty()||*tail||!std::isfinite(number))Fail("input.ini: invalid numeric value");
        if(key=="speed"&&number>=0&&number<=10)next.speed=number;
        else if(key=="yaw_speed"&&number>=0&&number<=10)next.yawSpeed=number;
        else if(key=="height_speed"&&number>=0&&number<=5)next.heightSpeed=number;
        else if(key=="height_min")next.heightMin=number;
        else if(key=="height_max")next.heightMax=number;
        else Fail("input.ini: unknown input key or value outside bounds");
    }
    if(next.heightMin>next.heightMax)Fail("input.ini: oasis.height_min must be <= oasis.height_max");locomotion_=next;
}
}
