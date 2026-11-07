#include "LabInputMapping.h"
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cstdlib>
namespace lab {
namespace { LabInputActions standardVrActions; }
const LabInputActions& StandardVrActions() noexcept{return standardVrActions;}
void DeclareStandardVrInputActions(InputSystem& input){LabInputActions a;a.navigationSet=input.RegisterActionSet("navigation","Navigation");a.interactionSet=input.RegisterActionSet("interaction","Interaction");a.trackingSet=input.RegisterActionSet("tracking","Tracking");a.systemSet=input.RegisterActionSet("system","System");a.move=input.RegisterAction(a.navigationSet,"navigation.move","Move",InputActionType::Vector2);a.turn=input.RegisterAction(a.navigationSet,"navigation.turn","Turn",InputActionType::Vector2);a.recenter=input.RegisterAction(a.navigationSet,"navigation.recenter","Recenter",InputActionType::Boolean);a.heightUp=input.RegisterAction(a.navigationSet,"navigation.height_up","Height up",InputActionType::Boolean);a.heightDown=input.RegisterAction(a.navigationSet,"navigation.height_down","Height down",InputActionType::Boolean);a.grab=input.RegisterAction(a.interactionSet,"interaction.grab","Grab",InputActionType::Float);a.primary=input.RegisterAction(a.interactionSet,"interaction.primary","Primary",InputActionType::Float);a.secondary=input.RegisterAction(a.interactionSet,"interaction.secondary","Secondary",InputActionType::Float);a.touch=input.RegisterAction(a.interactionSet,"interaction.touch","Touch",InputActionType::Boolean);a.physical=input.RegisterAction(a.interactionSet,"interaction.physical","Physical hand",InputActionType::Boolean);a.proximity=input.RegisterAction(a.interactionSet,"interaction.proximity","Proximity",InputActionType::Boolean);a.mode=input.RegisterAction(a.interactionSet,"interaction.mode","Mode",InputActionType::Boolean);a.next=input.RegisterAction(a.interactionSet,"interaction.next","Next",InputActionType::Boolean);a.feedback=input.RegisterAction(a.interactionSet,"interaction.feedback","Feedback",InputActionType::Haptic);a.handPose=input.RegisterAction(a.trackingSet,"tracking.hand_pose","Hand pose",InputActionType::Pose);a.aimPose=input.RegisterAction(a.trackingSet,"tracking.aim_pose","Aim pose",InputActionType::Pose);a.quit=input.RegisterAction(a.systemSet,"system.quit","Quit",InputActionType::Boolean);a.menu=input.RegisterAction(a.systemSet,"system.menu","Menu",InputActionType::Boolean);standardVrActions=a;}
void StandardVrInput::Anchor(const Pose& p,bool stage,XMFLOAT4X4& trackingToWorld){XMFLOAT3 forward=Sub(Transform({0,0,1},p),p.p);float yaw=std::atan2(forward.x,forward.z);float y=(stage?0.0f:1.72f-p.p.y)+heightOffset_;XMStoreFloat4x4(&trackingToWorld,XMMatrixTranslation(-p.p.x,y,-p.p.z)*XMMatrixRotationY(-yaw));anchored_=true;anchorPending_=false;++trackingRevision_;locomotionTime_=0;}
void StandardVrInput::Locomotion(FrameInput& input,const Pose& raw,XMFLOAT4X4& trackingToWorld){
    if(!locomotionEnabled_&&!heightAdjustEnabled_){locomotionTime_=0;locomotionFocused_=false;return;}
    double dt=locomotionTime_?double(input.targetDisplayTime-locomotionTime_)*1e-9:0;locomotionTime_=input.targetDisplayTime;
    bool advance=input.focused&&locomotionFocused_&&dt>0&&dt<=.25;locomotionFocused_=input.focused;if(!advance)return;
    auto transform=XMLoadFloat4x4(&trackingToWorld);Pose head=FromMatrix(Matrix(raw)*transform);XMFLOAT3 shift{};float yaw{};
    if(locomotionEnabled_){
        Pose reference=head;
        if(locomotion_.reference==LocomotionSettings::Reference::LeftHand||locomotion_.reference==LocomotionSettings::Reference::RightHand){
            uint32_t hand=locomotion_.reference==LocomotionSettings::Reference::LeftHand?0u:1u;
            const auto& pose=input.PoseState(StandardVrActions().handPose,hand);if(pose.tracked)reference=FromMatrix(Matrix(pose.pose)*transform);
        }
        XMFLOAT3 forward=locomotion_.reference==LocomotionSettings::Reference::World?XMFLOAT3{0,0,1}:Sub(Transform({0,0,1},reference),reference.p);forward.y=0;
        if(Length(forward)<.001f){forward=Sub(Transform({0,0,1},FromMatrix(transform)),FromMatrix(transform).p);forward.y=0;}
        forward=Unit(forward);XMFLOAT3 right{forward.z,0,-forward.x};auto move=input.Vector(StandardVrActions().move),turn=input.Vector(StandardVrActions().turn);
        shift=Scale(Add(Scale(right,move.x),Scale(forward,move.y)),locomotion_.speed*float(dt));yaw=turn.x*locomotion_.yawSpeed*float(dt);
    }
    float dy=0;
    if(heightAdjustEnabled_){int direction=(input.Down(StandardVrActions().heightUp,1)?1:0)-(input.Down(StandardVrActions().heightDown,1)?1:0);float nextHeight=std::clamp(heightOffset_+locomotion_.heightSpeed*float(direction)*float(dt),locomotion_.heightMin,locomotion_.heightMax);dy=nextHeight-heightOffset_;heightOffset_=nextHeight;}
    // Apply artificial movement to the tracking origin. The head and both hands
    // retain their independent room-space tracking and share this world transform.
    XMStoreFloat4x4(&trackingToWorld,transform*XMMatrixTranslation(-head.p.x,0,-head.p.z)*XMMatrixRotationY(yaw)*XMMatrixTranslation(head.p.x+shift.x,dy,head.p.z+shift.z));
}
bool StandardVrInput::Frame(FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& trackingToWorld){if(input.Pressed(StandardVrActions().recenter))anchorPending_=true;if(raw){if(anchorPending_||!anchored_)Anchor(*raw,stage,trackingToWorld);Locomotion(input,*raw,trackingToWorld);input.head=FromMatrix(Matrix(*raw)*XMLoadFloat4x4(&trackingToWorld));}input.trackingRevision=trackingRevision_;return false;}
void StandardVrInput::Event(TrackingEvent event){if(event==TrackingEvent::SessionStopped){locomotionTime_=0;locomotionFocused_=false;}else anchorPending_=true;}
TrackingMapping StandardVrInput::Mapping(){return {this,[](void* context,FrameInput& input,const Pose* raw,bool stage,XMFLOAT4X4& transform){return static_cast<StandardVrInput*>(context)->Frame(input,raw,stage,transform);},[](void* context,TrackingEvent event){static_cast<StandardVrInput*>(context)->Event(event);}};}
void StandardVrInput::LoadConfiguration(const std::wstring& path){
    std::ifstream file{std::filesystem::path(path)};if(!file)Fail("input.ini: cannot open locomotion configuration");
    LocomotionSettings next;std::string line;
    while(std::getline(file,line)){
        auto end=line.find_first_of("#;\r");if(end!=std::string::npos)line.resize(end);
        auto trim=[](std::string value){auto first=value.find_first_not_of(" \t"),last=value.find_last_not_of(" \t");return first==std::string::npos?std::string{}:value.substr(first,last-first+1);};
        line=trim(line);if(line.empty())continue;auto equals=line.find('=');if(equals==std::string::npos)Fail("input.ini: expected key=value");
        auto key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));if(key.rfind("lab.",0)!=0)continue;key.erase(0,4);
        if(key=="locomotion_reference"){
            if(value=="head")next.reference=LocomotionSettings::Reference::Head;
            else if(value=="left_hand")next.reference=LocomotionSettings::Reference::LeftHand;
            else if(value=="right_hand")next.reference=LocomotionSettings::Reference::RightHand;
            else if(value=="world")next.reference=LocomotionSettings::Reference::World;
            else Fail("input.ini: locomotion reference must be head, left_hand, right_hand or world");
            continue;
        }
        char* tail=nullptr;float number=std::strtof(value.c_str(),&tail);if(value.empty()||*tail||!std::isfinite(number))Fail("input.ini: invalid LaboVR numeric value");
        if(key=="speed"&&number>=0&&number<=10)next.speed=number;
        else if(key=="yaw_speed"&&number>=0&&number<=10)next.yawSpeed=number;
        else if(key=="height_speed"&&number>=0&&number<=5)next.heightSpeed=number;
        else if(key=="height_min")next.heightMin=number;
        else if(key=="height_max")next.heightMax=number;
        else Fail("input.ini: unknown LaboVR input key or value outside bounds");
    }
    if(next.heightMin>next.heightMax)Fail("input.ini: lab.height_min must be <= lab.height_max");locomotion_=next;
}
}
