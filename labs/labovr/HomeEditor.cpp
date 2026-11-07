#include "HomePhysics.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace lab {
namespace {XMFLOAT3 V(LabVec3V1 v){return {v.x,v.y,v.z};}}
void HomePhysics::PublishAcoustics(){
    if(!audio_||!audio_->SetAcousticBoxes)return;
    std::vector<LabAcousticBoxV1> boxes;if(visible_)for(const auto& e:entities_)if(e.acousticHalfExtent.x>0)boxes.push_back({e.pose,e.acousticHalfExtent});
    if(!audio_->SetAcousticBoxes(audio_->serviceContext,boxes.data(),uint32_t(boxes.size())))Fail("Home audio_v1: acoustic geometry rejected");
}
bool HomePhysics::RefreshEditor(){
    std::array<LabPhysicsEditBodyV1,128> bodies{};std::array<LabPhysicsEditJointV1,256> joints{};
    LabPhysicsEditSnapshotV1 snapshot{sizeof(snapshot),uint32_t(bodies.size()),bodies.data(),uint32_t(joints.size()),0,joints.data()};
    if(!physics_->GetEditSnapshot(physics_->serviceContext,scene_,&snapshot)){status_="EDITOR SNAPSHOT FAILED";return false;}
    for(auto& e:entities_)for(uint32_t i=0;i<snapshot.bodyCount;i++)if(bodies[i].body==e.body){e.pose=bodies[i].pose;break;}
    for(auto& c:connections_)for(uint32_t i=0;i<snapshot.jointCount;i++)if(joints[i].joint==c.joint){c.frameA=joints[i].frameA;c.frameB=joints[i].frameB;break;}
    simulationSeconds_=snapshot.simulationSeconds;return true;
}
namespace {
LabU64 NodeId(const std::string& id){LabU64 h=14695981039346656037ull;for(unsigned char c:id){h^=c;h*=1099511628211ull;}return h|0x8000000000000000ull;}
}
void HomePhysics::BuildSelections(){
    selections_.clear();auto group=[&](LabU64 id,const char* name,std::initializer_list<uint32_t> groups){Selection s;s.id=id;s.name=name;for(const auto& e:entities_)if(std::find(groups.begin(),groups.end(),e.group)!=groups.end())s.bodies.push_back(e.body);selections_.push_back(std::move(s));};
    group(1,"TABLE + FAN",{1,4});group(2,"TABLE",{1});group(3,"FAN COMPLETE",{4});group(8,"SUSPENSION COMPLETE",{3});group(27,"ROOM REMAINING",{0});
    group(39,"VASE COMPLETE",{5});group(40,"ROCKS",{6});group(41,"BONSAI",{7});
    auto subset=[&](LabU64 id,const char* name,const char* prefix){Selection s;s.id=id;s.name=name;for(const auto& e:entities_)if(e.id.rfind(prefix,0)==0)s.bodies.push_back(e.body);selections_.push_back(std::move(s));};
    subset(9,"CABLE","lamp-cable-");subset(10,"HUB","lamp-socket");subset(11,"BULB","lamp-bulb");subset(12,"CHAIN + SWITCH","lamp-chain-");
    for(const auto& e:entities_)if(e.id=="lamp-switch-slider"||e.id=="lamp-pull-handle")selections_.back().bodies.push_back(e.body);
    subset(5,"ROTOR","fan-rotor");
    for(const auto& e:entities_)selections_.push_back({NodeId(e.id),e.name,{e.body}});
}
std::vector<LabTerminalNodeV2> HomePhysics::EditorNodes()const{
    std::vector<LabTerminalNodeV2> nodes;
    auto node=[&](LabU64 id,LabU64 parent,const char* label,uint32_t caps,LabU64 entity=0,LabU64 component=0){LabTerminalNodeV2 n{};n.id=id;n.parentId=parent;n.entityId=entity?entity:id;n.componentId=component;n.capabilities=caps;snprintf(n.label,sizeof(n.label),"%s",label);nodes.push_back(n);};
    node(37,0,"RENDER",4);node(38,37,"ANTIALIASING",4,37,38);
    node(1,0,"TABLE + FAN",1);node(2,1,"TABLE",1);node(3,1,"FAN",7);
    node(4,3,"MOTOR",6,3,4);node(5,3,"ROTOR",1,3,5);node(6,3,"AIRFLOW",4,3,6);node(7,3,"AUDIO",0,3,7);
    node(8,0,"SUSPENSION",3);node(9,8,"CABLE",1,8,9);node(10,8,"HUB",1,8,10);node(11,8,"BULB / LIGHT",3,8,11);node(12,8,"CHAIN / SWITCH",1,8,12);
    node(39,0,"VASE",1);node(40,39,"ROCKS",1);node(41,39,"BONSAI",1);
    node(13,0,"FLIES",6);node(14,13,"POPULATION",6,13,14);node(15,13,"FLIGHT / FOURIER",6,13,15);node(16,13,"IMPACT",4,13,16);
    node(17,13,"HAND SWEEP",4,13,17);node(18,13,"SHADOWS",4,13,18);node(19,13,"OPTICAL SWARM",6,13,19);node(20,13,"APPEARANCE",4,13,20);
    node(21,0,"ATMOSPHERE",6);node(22,21,"FOG",6,21,22);node(24,21,"WIND / ADVECTION",4,21,24);node(25,0,"CONTROL PANEL",7);node(27,0,"ROOM REMAINING",1);node(31,27,"MATERIALS",0);const char* surfaces[]{"FLOOR","CEILING","WALL -Z","WALL -X","WALL +X"};for(uint32_t i=0;i<5;i++)node(32+i,31,surfaces[i],4,27,32+i);node(28,0,"CONSOLE",2);node(29,0,"CONTROLLERS",0);node(30,29,"PHYSICAL TOUCH",4,29,30);
    if(population_.targetId!=Invalid)node(26,13,"TARGETED FLY",0,TargetIdentity(),26);
    for(const auto& e:entities_){LabU64 parent=0;
        if(e.group==0)parent=27;else if(e.group==1)parent=2;else if(e.group==4)parent=e.body==fanRotor_?5:3;
        else if(e.group==5)parent=39;else if(e.group==6)parent=40;else if(e.group==7)parent=41;
        else if(e.id.rfind("lamp-cable-",0)==0)parent=9;else if(e.id=="lamp-socket")parent=10;else if(e.id=="lamp-bulb")parent=11;
        else if(e.id.rfind("lamp-chain-",0)==0||e.id=="lamp-switch-slider"||e.id=="lamp-pull-handle")parent=12;else parent=8;
        node(NodeId(e.id),parent,e.name.c_str(),1,NodeId(e.id),NodeId(e.id));
    }
    return nodes;
}
void HomePhysics::SelectNode(LabU64 id){panelSelected_=id==25;if(panelSelected_){status_="PANEL SELECTED";return;}for(uint32_t i=0;i<selections_.size();i++)if(selections_[i].id==id){selection_=i;status_="SELECTED";return;}}
LabPoseV1 HomePhysics::PanelReadingPose()const{
    auto p=panelPose_;if(panelFace_){auto q=p.orientation;p.orientation={-q.z,q.w,q.x,-q.y};}return p;
}
void HomePhysics::RotatePanel(float sign){if(!frozen_){status_="FREEZE BEFORE ROTATING";return;}auto q=panelPose_.orientation;float a=sign*XM_PI/24,s=std::sin(a),c=std::cos(a);panelPose_.orientation={q.x*c-q.z*s,q.y*c+q.w*s,q.z*c+q.x*s,q.w*c-q.y*s};validPanelPose_=panelPose_;status_="PANEL ROTATED";}
bool HomePhysics::SetFrozen(bool value){
    if(value==frozen_)return true;
    if(value)for(uint32_t h=0;h<2;h++){
        ReleaseHand(h,false);
        if(hands_[h]&&handColliding_[h]){if(!physics_->SetBodyCollisionEnabled(physics_->serviceContext,scene_,hands_[h],0))Fail("Home physics_v1: frozen hand collision disable rejected");handColliding_[h]=false;}
    }
    if(!physics_->SetPaused(physics_->serviceContext,scene_,value?1u:0u)){
        if(value)status_="WAIT FOR GPU SETUP";
        else status_="BLOCKED: PHYSICAL LAYOUT";
        return false;
    }
    frozen_=value;wind_.paused=frozen_?1u:0u;gpu_->SetFogWind(gpu_->serviceContext,&wind_);
    sweepTracked_={};sweepVelocity_={};handBlocked_={true,true};lastUpdateSeconds_=0;
    // Keep the existing kinematic proxies and GPU identities across resume.
    // Grabbing stays blocked until the grip is released; tracking will update
    // their targets through the ordinary physics_v1 command path.
    if(!value)PublishAcoustics();
    if(value&&!RefreshEditor())return false;if(value)CacheValidSelections();status_=value?"FROZEN - EDIT ACTIVE":"RUNNING";return true;
}
void HomePhysics::Select(int direction){if(selections_.empty())return;selection_=uint32_t((int(selection_)+direction+int(selections_.size()))%int(selections_.size()));status_="SELECTED";}
const char* HomePhysics::SelectionName()const{if(panelSelected_)return "CONTROL PANEL";return selections_.empty()?"NONE":selections_[selection_].name.c_str();}
XMFLOAT3 HomePhysics::SelectionPosition()const{
    if(panelSelected_)return V(panelPose_.position);
    if(selections_.empty()||selections_[selection_].bodies.empty())return {};
    auto id=selections_[selection_].bodies.front();for(const auto& e:entities_)if(e.body==id)return V(e.pose.position);return {};
}
void HomePhysics::CycleEditSpeed(){editSpeed_=(editSpeed_+1)%3;}
void HomePhysics::TranslateSelection(uint32_t axis,float direction,double dt){
    if(!frozen_){status_="FREEZE BEFORE MOVING";return;}if(selections_.empty()||axis>2||dt<=0)return;
    const float speeds[]{.01f,.15f,.75f};float amount=direction*speeds[editSpeed_]*float(std::clamp(dt,0.,.04));
    LabVec3V1 delta{};if(axis==0)delta.x=amount;else if(axis==1)delta.y=amount;else delta.z=amount;
    if(panelSelected_){panelPose_.position.x+=delta.x;panelPose_.position.y+=delta.y;panelPose_.position.z+=delta.z;validPanelPose_=panelPose_;status_="FROZEN - PANEL MOVING";return;}
    const auto& selected=selections_[selection_];
    if(!physics_->TranslateBodies(physics_->serviceContext,scene_,selected.bodies.data(),uint32_t(selected.bodies.size()),delta)){status_="EDIT REJECTED - STILL FROZEN";return;}
    RefreshEditor();CacheSelection(selections_[selection_]);status_="FROZEN - MOVING";
}
void HomePhysics::PublishOptical(){
    if(!gpu_||!gpu_->SetPopulationOpticalSettings)return;
    auto s=optical_;s.enabled=s.enabled&&visible_;
    if(!gpu_->SetPopulationOpticalSettings(gpu_->serviceContext,&s))Fail("Home gpu_v1: population optical settings rejected");
}
void HomePhysics::AdjustOptical(uint32_t index,float sign){
    if(index==0)optical_.enabled=!optical_.enabled;
    else if(index==1)optical_.quality=uint32_t(std::clamp(int(optical_.quality)+(sign>0?1:-1),0,2));
    else if(index==2)optical_.stabilizationSeconds=std::clamp(optical_.stabilizationSeconds+sign*.005f,0.f,.08f);
    else if(index==3)optical_.coverage=std::clamp(optical_.coverage+sign*.05f,.25f,2.f);
    else if(index==4)optical_.visualization=uint32_t((int(optical_.visualization)+(sign>0?1:2))%3);
    PublishOptical();status_="OPTICAL SETTINGS ACTIVE";
}
void HomePhysics::AdjustFan(uint32_t index,float sign){
    if(index==0)fanEnabled_=!fanEnabled_;
    else if(index==1)fanRpm_=std::clamp(fanRpm_+sign*100,0.f,1800.f);
    else if(index==2)fanWind_=std::clamp(fanWind_+sign*.1f,0.f,4.f);
}
void HomePhysics::UpdateFan(uint64_t frame){
    if(!frozen_){LabPhysicsMotorV1 motor{};motor.structSize=sizeof(motor);motor.joint=fanJoint_;motor.target.orientation.w=1;motor.angularVelocity.x=fanEnabled_?fanRpm_*XM_2PI/60:0;
        if(!physics_->SetMotors(physics_->serviceContext,scene_,&motor,1))Fail("Home physics_v1: fan motor rejected");}
    XMFLOAT3 position{-.85f,1.87f,1.27f};LabQuatV1 orientation{0,0,0,1};for(const auto& e:entities_)if(e.body==fanRotor_){position=V(e.pose.position);orientation=e.pose.orientation;}
    XMFLOAT3 velocity{};if(!frozen_&&states_[4].body==fanRotor_){position=V(states_[4].pose.position);orientation=states_[4].pose.orientation;velocity=V(states_[4].linearVelocity);}
    wind_.position={position.x,position.y,position.z};wind_.direction={-2*(orientation.x*orientation.z+orientation.w*orientation.y),-2*(orientation.y*orientation.z-orientation.w*orientation.x),-(1-2*(orientation.x*orientation.x+orientation.y*orientation.y))};wind_.paused=frozen_?1u:0u;wind_.speed=std::min(20.f,fanActualRpm_/1800.f*fanWind_*8);
    LabPhysicsAirflowV1 flow{sizeof(flow),visible_&&transport_.enabled?1u:0u,fanRotor_,{0,0,-1},wind_.radius,wind_.length,fanWind_*8,1800,fog_.turbulence};
    if(!physics_->SetAirflows(physics_->serviceContext,scene_,&flow,1))Fail("Home physics_v1: rotor airflow rejected");
    if(!gpu_->SetFogWind(gpu_->serviceContext,&wind_))Fail("Home gpu_v1: fan wind rejected");
    std::array<LabAudioEmitterV1,LAB_PHYSICS_POPULATION_AUDIO_MAX_V1+3> emitters{};uint32_t count=0;
    uint32_t flies=visible_&&!frozen_?population_.voiceCount:0;
    for(uint32_t i=0;i<flies;i++){const auto& v=population_.voices[i];if(v.insect>=flyAudioIds_.size())continue;auto& e=emitters[count++];e.structSize=sizeof(e);e.id=0x7f000000u+flyAudioIds_[v.insect];e.flags=LABOVR_AUDIO_EMITTER_DIFFUSE_V1;e.wave=LABOVR_AUDIO_WAVE_BUZZ_V1;
        e.position[0]=v.position.x;e.position[1]=v.position.y;e.position[2]=v.position.z;e.velocity[0]=v.velocity.x;e.velocity[1]=v.velocity.y;e.velocity[2]=v.velocity.z;
        e.gain=.045f*std::sqrt(std::max(0.f,v.activity))/std::sqrt(float(std::max(1u,flies)));e.frequency=v.frequency;
    }
    if(visible_&&!frozen_&&fanActualRpm_>1){float rate=fanActualRpm_/60,level=std::clamp(fanActualRpm_/1800,0.f,1.3f);
        const uint32_t waves[]{LABOVR_AUDIO_WAVE_SINE_V1,LABOVR_AUDIO_WAVE_BUZZ_V1,LABOVR_AUDIO_WAVE_NOISE_V1};
        const float frequency[]{std::max(20.f,rate*2),std::max(20.f,rate*3),300+fanActualRpm_*1.2f},gain[]{.012f*level,.018f*level,.024f*level*std::sqrt(fanWind_)};
        for(uint32_t i=0;i<3;i++){auto& e=emitters[count++];e.structSize=sizeof(e);e.id=0x7e000000u+fanAudioBase_+i;e.wave=waves[i];e.frequency=frequency[i];e.gain=gain[i];e.position[0]=position.x;e.position[1]=position.y;e.position[2]=position.z;e.velocity[0]=velocity.x;e.velocity[1]=velocity.y;e.velocity[2]=velocity.z;}
    }
    LabAudioEmitterBatchV1 batch{1,sizeof(batch),frame,emitters.data(),count};if(!audio_->PublishEmitters(audio_->serviceContext,&batch))Fail("Home audio_v1: fly/fan emitters rejected");
}
void HomePhysics::ProfileCommand(uint32_t command){if(command<2)WriteProfile(command==0?LAB_PROFILE_SYSTEM_V1:LAB_PROFILE_USER_V1);else ReadProfile(command==2?LAB_PROFILE_SYSTEM_V1:LAB_PROFILE_USER_V1,false);}
void HomePhysics::CacheSelection(Selection& s){
    s.validBodies.clear();s.validJoints.clear();auto contains=[&](LabPhysicsBodyV1 b){return std::find(s.bodies.begin(),s.bodies.end(),b)!=s.bodies.end();};
    for(const auto& e:entities_)if(contains(e.body))s.validBodies.push_back({e.body,e.pose});
    for(const auto& c:connections_)if(contains(c.bodyA)||contains(c.bodyB))s.validJoints.push_back({c.joint,c.frameA,c.frameB});
}
void HomePhysics::CacheValidSelections(){
    LabPhysicsEditValidationV1 v{sizeof(v)};if(!frozen_||!physics_->ValidateEdit(physics_->serviceContext,scene_,&v))return;
    for(auto& s:selections_)CacheSelection(s);validPanelPose_=panelPose_;
}
void HomePhysics::ResetSelection(){
    if(!frozen_){status_="FREEZE BEFORE LOCAL RESET";return;}
    if(panelSelected_){panelPose_=validPanelPose_;status_="PANEL LAST VALID RESTORED";return;}
    if(selections_.empty())return;auto& s=selections_[selection_];if(s.validBodies.empty()){status_="NO VALID LOCAL CHECKPOINT";return;}
    LabPhysicsEditBatchV1 batch{sizeof(batch),uint32_t(s.validBodies.size()),s.validBodies.data(),uint32_t(s.validJoints.size()),0,s.validJoints.data()};
    if(!physics_->ApplyEdit(physics_->serviceContext,scene_,&batch)){status_="LOCAL RESET REFUSED / STATE PRESERVED";return;}
    RefreshEditor();status_="LOCAL LAST VALID RESTORED";
}
void HomePhysics::AdjustSize(float sign,double dt){
    // Keep wing loading and drag acceleration constant in this perceptual
    // size experiment: area and compensated mass scale with radius squared.
    // Collision momentum still follows the compensated physical mass.
    float old=flight_.bodyRadius,delta=dt>0?.00025f*float(std::clamp(dt,0.,.04)):.000025f;
    float radius=std::clamp(old+sign*delta,.001f,.035f);float areaRatio=radius*radius/(old*old);
    auto next=flight_;next.bodyRadius=radius;next.mass*=areaRatio;next.wingArea*=areaRatio;
    if(!physics_->SetFlightSettings(physics_->serviceContext,scene_,&next)){status_="SIZE PHYSICAL SETTINGS REFUSED";return;}flight_=next;status_="SIZE / COLLISION / AERO ACTIVE";
}
namespace {constexpr const char* TouchNames[]{"trigger_touch","primary_touch","secondary_touch","thumbstick_touch","thumbrest_touch"};}
void HomePhysics::PollPhysicalSources(){
    LabInputPhysicalSourcesV1 query{sizeof(query),uint32_t(physicalSources_.size()),0,0,physicalAction_,0,physicalSources_.data()};
    if(input_->GetPhysicalSources(input_->serviceContext,&query))physicalSourceCount_=query.count;else physicalSourceCount_=0;
}
void HomePhysics::SyncTouchSources(){PollPhysicalSources();touchMasks_={};for(uint32_t i=0;i<5;i++)for(uint32_t j=0;j<physicalSourceCount_;j++)if(std::string(physicalSources_[j].name)==TouchNames[i])for(uint32_t h=0;h<2;h++)if(physicalSources_[j].selectedMask&(1u<<h))touchMasks_[h]|=1u<<i;}
void HomePhysics::ApplyTouchSources(){
    PollPhysicalSources();for(uint32_t h=0;h<2;h++){std::vector<LabU32> ids;for(uint32_t i=0;i<5;i++)if(touchMasks_[h]&(1u<<i))for(uint32_t j=0;j<physicalSourceCount_;j++)if(std::string(physicalSources_[j].name)==TouchNames[i])ids.push_back(physicalSources_[j].id);
        LabInputBooleanSourcesV1 r{sizeof(r),1u<<h,uint32_t(ids.size()),physicalAction_,ids.data()};if(!input_->SetBooleanSources(input_->serviceContext,&r))Fail("Home input_v1: physical touch OR rejected");}PollPhysicalSources();
}
void HomePhysics::ToggleTouch(uint32_t h,uint32_t source){if(h>1||source>=5)return;touchMasks_[h]^=1u<<source;ApplyTouchSources();status_="PHYSICAL TOUCH SOURCES ACTIVE";}
uint32_t HomePhysics::TouchAvailable(uint32_t h)const{uint32_t mask=0;for(uint32_t i=0;i<5;i++)for(uint32_t j=0;j<physicalSourceCount_;j++)if(std::string(physicalSources_[j].name)==TouchNames[i]&&(physicalSources_[j].availableMask&(1u<<(h&1))))mask|=1u<<i;return mask;}
uint32_t HomePhysics::TouchDown(uint32_t h)const{uint32_t mask=0;for(uint32_t i=0;i<5;i++)for(uint32_t j=0;j<physicalSourceCount_;j++)if(std::string(physicalSources_[j].name)==TouchNames[i]&&(physicalSources_[j].downMask&(1u<<(h&1))))mask|=1u<<i;return mask;}
void HomePhysics::AdjustHandResponse(uint32_t index,float sign){auto& value=index?handSettings_.rotationTau:handSettings_.positionTau;value=std::clamp(value+sign*.002f,0.f,.2f);status_="HAND RESPONSE ACTIVE";}
void HomePhysics::Capture(){
    LabCaptureRequestV1 request{sizeof(request),captureEye_,captureInterface_};LabU64 id{};
    bool accepted=gpu_->RequestCapture(gpu_->serviceContext,&request,&id)!=0;
    captureStatus_.structSize=sizeof(captureStatus_);
    if(gpu_->GetCaptureStatus(gpu_->serviceContext,&captureStatus_)&&captureStatus_.message[0])status_=captureStatus_.message;
    else status_=accepted?"CAPTURE QUEUED":"CAPTURE BUSY / UNAVAILABLE";
}

}
