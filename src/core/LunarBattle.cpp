#include "LunarBattle.h"

namespace OASIS {
namespace {
float Noise(uint32_t unit,uint32_t shot){
    uint32_t hash=(unit+1u)*0x85ebca6bu^(shot+17u)*0x9e3779b9u;
    hash=(hash^(hash>>16u))*0x7feb352du;
    return float(hash>>8u)*(1.f/16777216.f);
}

Pose SetPose(XMFLOAT3 position,float yaw,float pitch=0.f,float roll=0.f){
    Pose result{position};
    XMStoreFloat4(&result.q,XMQuaternionRotationRollPitchYaw(pitch,yaw,roll));
    return result;
}

LunarDrawPose DrawPose(const Pose& pose,float zScale=1.f){
    return {{pose.p.x,pose.p.y,pose.p.z,zScale},pose.q};
}

struct BattleContact {
    XMFLOAT3 point;
    float distance;
    bool dome;
};

BattleContact TraceShot(XMFLOAT3 origin,XMFLOAT3 direction){
    const float along=Dot(origin,direction);
    const float root=along*along+LunarRadius*LunarRadius-Dot(origin,origin);
    const float exit=std::max(0.f,-along+std::sqrt(std::max(0.f,root)));
    BattleContact contact{Add(origin,Scale(direction,exit)),exit,true};
    auto nearer=[&](float t,XMFLOAT3 point){
        if(t>0.f&&t<contact.distance)contact={point,t,false};
    };
    if(std::abs(direction.x)>.001f){
        const float face=std::copysign(.044f,origin.x);
        const float t=(face-origin.x)/direction.x;
        const auto hit=Add(origin,Scale(direction,t));
        if(LunarWallFace(hit))nearer(t,hit);
    }
    if(direction.y<-.001f){
        const float t=(LunarGroundHeight-origin.y)/direction.y;
        const auto hit=Add(origin,Scale(direction,t));
        if(std::abs(hit.x)<.76f&&std::abs(hit.z)<.48f)nearer(t,hit);
    }
    return contact;
}
}

LunarBattle::LunarBattle(){
    for(uint32_t index=0;index<units_.size();++index)
        units_[index].nextShot=.22+Noise(index,0u)*2.3;
}

LunarBattle::UnitPose LunarBattle::Animate(uint32_t index)const{
    const auto formation=LunarFormation(index);
    const auto& unit=units_[index];
    const float offset=float(index)*.79f;
    const auto phase=[&](double rate){return float(std::fmod(time_*rate,double(XM_2PI)))+offset;};
    const float wave=phase(formation.tank?.67:.94);
    const float forward=formation.position.x<0.f?1.f:-1.f;
    const float step=phase(7.4);
    const float stride=.32f+.68f*std::abs(std::cos(wave));
    const float recoil=std::exp(-float(time_-unit.lastShot)*(formation.tank?10.f:24.f));
    auto position=formation.position;
    position.x+=forward*(formation.tank?.044f:.065f)*std::sin(wave);
    position.z+=(formation.tank?.027f:.020f)*std::sin(wave+1.12f);
    const float yaw=formation.yaw+(formation.tank?.17f:.22f)*std::cos(wave+.4f);
    if(formation.tank){
        const auto base=SetPose(position,yaw,0.f,.024f*std::sin(wave));
        auto mount=Transform(LunarTurretPivot,base);
        const float turretYaw=formation.yaw+.31f*std::sin(phase(.89));
        const float pitch=-.15f+.20f*std::sin(phase(.72)+1.1f);
        auto weapon=SetPose(mount,turretYaw,pitch);
        const auto backward=TransformDirection({0.f,0.f,-.014f*recoil},Matrix(weapon));
        weapon.p=Add(weapon.p,backward);
        return {base,weapon,step,stride,.38f+.62f*std::abs(std::cos(wave))};
    }
    position.y+=.0035f*stride*std::abs(std::sin(step));
    const auto base=SetPose(position,yaw);
    const auto hips=Add(position,{0.f,LunarHipHeight,0.f});
    const float pitch=-.18f+.29f*std::sin(phase(.91))-.07f*recoil;
    const auto weapon=SetPose(hips,yaw,pitch,-.045f*stride*std::sin(step));
    return {base,weapon,step,stride,0.f};
}

void LunarBattle::Sound(LunarCue cue,XMFLOAT3 position,float strength){
    if(soundCount_<sounds_.size())sounds_[soundCount_++]={position,cue,strength};
}

void LunarBattle::Fire(uint32_t index,const UnitPose& pose){
    auto& unit=units_[index];
    const bool tank=LunarFormation(index).tank;
    const float jitter=Noise(index,++unit.shots);
    unit.nextShot=time_+(tank?1.7+jitter*2.3:unit.shots%3u?.13+jitter*.08:1.05+jitter*1.6);
    const auto shot=std::find_if(shots_.begin(),shots_.end(),[](const Shot& candidate){return !candidate.live;});
    if(shot==shots_.end())return;
    const auto muzzle=tank?Sub(XMFLOAT3{0.f,.107f,.148f},LunarTurretPivot):
        XMFLOAT3{.025f,.077f-LunarHipHeight,.067f};
    const auto point=Transform(muzzle,pose.weapon);
    const auto direction=Unit(TransformDirection({(jitter-.5f)*.06f,0.f,1.f},Matrix(pose.weapon)));
    const auto hit=TraceShot(point,direction);
    if(hit.distance<=.001f)return;
    unit.lastShot=time_;
    *shot={point,hit.point,time_,time_+hit.distance/(tank?5.6:9.0),true,tank,hit.dome};
    Sound(tank?LunarCue::Cannon:LunarCue::Rifle,point,tank?.86f:.48f+.17f*jitter);
}

void LunarBattle::Update(double solarTime){
    soundCount_=0;
    const bool first=previous_<0.;
    const double delta=first?0.:std::clamp(solarTime-previous_,0.,.05);
    previous_=solarTime;
    if(paused_&&!first)return;
    time_+=delta;
    for(auto& shot:shots_){
        if(!shot.live||time_<shot.arrive)continue;
        if(shot.dome){
            ripples_[nextRipple_]={Unit(shot.end),time_};
            nextRipple_=(nextRipple_+1u)%LunarRippleCount;
        }
        Sound(shot.dome?LunarCue::Membrane:LunarCue::Surface,shot.end,shot.shell?.79f:.51f);
        shot.live=false;
    }
    uint32_t motor=0;
    for(uint32_t index=0;index<units_.size();++index){
        const auto& unit=units_[index];
        const auto pose=Animate(index);
        if(!paused_&&time_>=unit.nextShot)Fire(index,pose);
        const uint32_t part=index*LunarPartsPerUnit;
        if(LunarFormation(index).tank){
            view_.poses[part]=DrawPose(pose.base);
            view_.poses[part+1u]=DrawPose(pose.weapon);
            motors_[motor++]={Add(pose.base.p,{0.f,.044f,0.f}),pose.motor};
        }else{
            view_.poses[part]=DrawPose(pose.weapon);
            const auto hips=Transform({0.f,LunarHipHeight,0.f},pose.base);
            const auto forward=TransformDirection({0.f,0.f,1.f},Matrix(pose.base));
            const float yaw=std::atan2(forward.x,forward.z);
            const float step=.42f*pose.stride*std::sin(pose.stepPhase);
            view_.poses[part+1u]=DrawPose(SetPose(hips,yaw,step));
            view_.poses[part+2u]=DrawPose(SetPose(hips,yaw,-step));
        }
    }
    for(uint32_t index=0;index<shots_.size();++index){
        auto& visual=view_.poses[LunarActorPoseCount+index];
        const auto& shot=shots_[index];
        visual={};
        if(!shot.live)continue;
        const auto deltaPosition=Sub(shot.end,shot.origin);
        const float fraction=float(std::clamp((time_-shot.begin)/(shot.arrive-shot.begin),0.,1.));
        const float head=std::min(1.f,fraction+.08f);
        const float tail=std::max(0.f,fraction-(shot.shell?.27f:.18f));
        const auto start=Add(shot.origin,Scale(deltaPosition,tail));
        visual=DrawPose(SolarLookAt(start,shot.end),Length(deltaPosition)*(head-tail));
    }
    for(uint32_t index=0;index<ripples_.size();++index){
        const auto& ripple=ripples_[(nextRipple_+LunarRippleCount-1u-index)%LunarRippleCount];
        const float age=float(time_-ripple.time);
        view_.ripples[index]=age<1.f?XMFLOAT4{ripple.normal.x,ripple.normal.y,ripple.normal.z,1.f+age}:XMFLOAT4{};
    }
}
}
