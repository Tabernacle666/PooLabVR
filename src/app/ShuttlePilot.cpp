#include "ShuttlePilot.h"
#include "VrInputMapping.h"
#include "shuttle/ShuttleLayout.h"

#include <utility>

namespace OASIS {
namespace {
using Vaisseaux::Vec3;
using Vaisseaux::Cross;

constexpr int64_t ReadyTime=2'000'000'000, ExitTime=3'000'000'000;
constexpr float Degrees=XM_PI/180.f;
constexpr float Mass=Vaisseaux::Lunaire::DryKg+Vaisseaux::Lunaire::FuelCapacityKg;
constexpr Vec3 Inertia{1900.,3300.,3600.};
constexpr float UprightTolerance=27.f*Degrees;
constexpr float StickEngage=.68f, StickRelease=.24f;

// The right-handed propulsion frame is X forward, Y left, Z up.
// The imported hull uses X forward, Y up, Z left.
XMFLOAT3 ModelDirection(Vec3 v) {return {float(v.x),float(v.z),float(v.y)};}
Vec3 FlightDirection(XMFLOAT3 v) {return {v.x,v.z,v.y};}
XMFLOAT3 CrossDirection(XMFLOAT3 a,XMFLOAT3 b) {
    XMFLOAT3 result;
    XMStoreFloat3(&result,XMVector3Cross(XMLoadFloat3(&a),XMLoadFloat3(&b)));
    return result;
}

double Shape(double radians,double travel,int percent) {
    const double axis=std::clamp(radians/travel,-1.,1.);
    const double dead=double(percent)*.01;
    return std::copysign(std::pow(std::max(0.,(std::abs(axis)-dead)/(1.-dead)),1.35),axis);
}

XMMATRIX RotationOnly(FXMMATRIX matrix) {
    auto result=matrix;
    result.r[3]=XMVectorSet(0,0,0,1);
    return result;
}

double ApproachZero(double v,double maxDelta) {
    return v-std::clamp(v,-maxDelta,maxDelta);
}

const auto Centre=Vaisseaux::CentreOfMass(Vaisseaux::Lunaire::DryKg,Vaisseaux::Lunaire::DryCentreMetres,
                                         Vaisseaux::Lunaire::FuelCapacityKg,Vaisseaux::Lunaire::FuelCentreMetres);
}

void ShuttlePilot::Board(const Pose& head) {
    const auto direction=TransformDirection({0.f,0.f,1.f},Matrix(head));
    const float yaw=std::atan2(direction.x,direction.z);
    const auto rotation=XMMatrixRotationY(yaw-XM_PIDIV2);
    const auto origin=Sub(head.p,TransformDirection(ShuttleLayout::Eye,rotation));
    XMStoreFloat4x4(&modelToTracking_,rotation*XMMatrixTranslation(origin.x,origin.y,origin.z));
    occupied_=true;
    map_.periodSeconds=Vaisseaux::Lunaire::MapPeriodSeconds;
    exitArmed_=false;
    aHeldSince_=0;
    velocity_={};angularVelocity_={};
    devices_={};loads_={};
    brake_={};turnSide_=Invalid;
    stickLatched_=true;
    ResetNeutral();
}

void ShuttlePilot::Leave() {
    occupied_=false;
    power_=Power::Off;
    hasAlignedSince_=false;
    aHeldSince_=0;
    demand_={};
    brake_={};
    devices_={};
    loads_={};
    ignition_=false;
}

void ShuttlePilot::ResetNeutral() {
    power_=Power::Aligning;
    hasAlignedSince_=false;
    heldFraction_=0.f;
    aligned_={};
    demand_={};
    turnSide_=Invalid;
}

void ShuttlePilot::AdjustSettings(const FrameInput& input,const VrInputActions& actions) {
    const auto move=input.Vector(actions.move);
    if(std::abs(move.x)<StickRelease&&std::abs(move.y)<StickRelease)stickLatched_=false;
    if(!stickLatched_&&std::max(std::abs(move.x),std::abs(move.y))>=StickEngage){
        stickLatched_=true;
        if(std::abs(move.y)>std::abs(move.x)){
            const int count=int(Setting::Count);
            const int row=(int(setting_)+(move.y>0.f?count-1:1))%count;
            setting_=Setting(row);
        }else{
            const int step=move.x>0.f?1:-1;
            switch(setting_){
                case Setting::LeftAngle:
                case Setting::RightAngle:{
                    auto& angle=offset_[uint32_t(setting_)];
                    const int next=std::clamp(angle+step,-40,40);
                    if(next!=angle){angle=next;ResetNeutral();}
                    break;
                }
                case Setting::DeadZone:deadZone_=std::clamp(deadZone_+step,0,45);break;
                case Setting::Color:{
                    const int count=int(Vaisseaux::Lunaire::Themes.size());
                    theme_=uint32_t((int(theme_)+step+count)%count);
                    break;
                }
                default:break;
            }
        }
    }
    if(setting_==Setting::Calibrate&&input.Pressed(actions.recenter,0u))
        ResetNeutral();
}

bool ShuttlePilot::Upright(const PoseActionState& hand,uint32_t slot) const {
    if(!hand.tracked)return false;
    // The grip-space Z axis points from the little finger to the thumb.
    const auto up=TransformDirection({0,0,1},Matrix(hand.pose));
    const auto forward=TransformDirection({1,0,0},XMLoadFloat4x4(&modelToTracking_));
    const float angle=float(offset_[slot])*Degrees;
    const auto target=Add({0,std::cos(angle),0},Scale(Unit({forward.x,0,forward.z}),std::sin(angle)));
    return Dot(up,target)>=std::cos(UprightTolerance);
}

Vaisseaux::HandDemand ShuttlePilot::HandAxes(const PoseActionState& hand,uint32_t slot) const {
    if(!hand.tracked)return {};
    const auto neutral=Matrix(neutral_[slot]);
    const auto current=Matrix(hand.pose);
    const auto restUp=TransformDirection({0,0,1},neutral);
    const auto up=TransformDirection({0,0,1},current);
    const auto headward=TransformDirection({1,0,0},XMLoadFloat4x4(&modelToTracking_));
    const auto forward=Unit(Sub(headward,Scale(restUp,Dot(headward,restUp))));
    const auto left=CrossDirection(forward,restUp);
    const double keepUp=std::clamp(double(Dot(up,restUp)),-1.,1.);
    if(keepUp<=0.)return {};

    // Swing the neutral grip to the current shaft direction before measuring twist.
    const auto axis=CrossDirection(restUp,up);
    auto neutralSide=TransformDirection({1,0,0},neutral);
    if(Length(axis)>.00001f)
        neutralSide=TransformDirection(neutralSide,
            XMMatrixRotationAxis(XMLoadFloat3(&axis),float(std::acos(keepUp))));
    const auto side=TransformDirection({1,0,0},current);
    const double twist=std::atan2(Dot(up,CrossDirection(neutralSide,side)),Dot(neutralSide,side));
    return {Shape(std::atan2(Dot(up,forward),keepUp),24.*Degrees,deadZone_),
            Shape(std::atan2(Dot(up,left),keepUp),24.*Degrees,deadZone_),
            Shape(twist*(slot==0?1.:-1.),30.*Degrees,deadZone_)};
}

void ShuttlePilot::ReadInput(const FrameInput& input,const Pose* raw,const VrInputActions& actions) {
    const int64_t now=input.targetDisplayTime;
    const bool discontinuity=lastSample_&&(now<=lastSample_||now-lastSample_>250'000'000);
    lastSample_=now;
    if(!input.focused||!raw||discontinuity){
        aHeldSince_=0;
        hasAlignedSince_=false;
        heldFraction_=0.f;
        aligned_={};
        demand_={};
        brake_={};
        return;
    }

    if(!occupied_){
        if(input.Pressed(actions.shuttle,1u))Board(*raw);
        else return;
    }
    if(!input.Down(actions.shuttle,1u)){
        exitArmed_=true;
        aHeldSince_=0;
    }else if(exitArmed_){
        if(!aHeldSince_)aHeldSince_=now;
        if(now-aHeldSince_>=ExitTime){Leave();return;}
    }
    AdjustSettings(input,actions);
    const auto& hands=input.State(actions.handPose).pose;
    for(uint32_t hand=0;hand<2;++hand)aligned_[hand]=Upright(hands[hand],hand);
    demand_={};
    brake_={};
    if(power_==Power::Aligning){
        if(!aligned_[0]||!aligned_[1]){
            hasAlignedSince_=false;heldFraction_=0.f;
            return;
        }
        if(!hasAlignedSince_){
            alignedSince_=now;
            hasAlignedSince_=true;
        }
        heldFraction_=std::clamp(float(double(now-alignedSince_)/ReadyTime),0.f,1.f);
        if(now-alignedSince_<ReadyTime)return;
        for(uint32_t hand=0;hand<2;++hand)neutral_[hand]=hands[hand].pose;
        power_=Power::Running;
        ignition_=true;
    }
    // Poses can drop independently. A lost controller cannot leave a command latched.
    demand_.powered=hands[0].tracked||hands[1].tracked;
    for(uint32_t hand=0;hand<2;++hand){
        demand_.hands[hand]=HandAxes(hands[hand],hand);
        if(hands[hand].tracked)
            brake_[hand]=std::clamp((input.Analog(actions.grab,hand)-.12f)/.88f,0.f,1.f);
    }
}

VrTravel ShuttlePilot::Advance(double seconds,SolarPassage& passage,const Pose& previousHead) {
    if(!Active()||seconds<=0.)return {};
    const auto model=XMLoadFloat4x4(&modelToTracking_);
    const auto modelRotation=RotationOnly(model);
    const auto inverseModel=XMMatrixTranspose(modelRotation);
    const auto pivotRaw=TransformPoint(ModelDirection(Centre),model);
    const auto fromPivot=Sub(previousHead.p,pivotRaw);
    auto demand=demand_;
    const double braking=std::min(brake_[0],brake_[1]);
    const uint32_t one=braking>.02||std::max(brake_[0],brake_[1])<=.02f?
        Invalid:(brake_[0]>brake_[1]?0u:1u);
    if(one!=turnSide_){turnSide_=one;turned_=0.;}
    if(braking>.02)demand.hands={};
    const float density=passage.Inside()?1.f:0.f;
    const unsigned steps=unsigned(std::ceil(seconds*120.));
    const double dt=seconds/steps;
    XMFLOAT3 translation{};
    for(unsigned i=0;i<steps;++i){
        if(one!=Invalid){
            const double sign=one==0u?1.:-1.;
            const double target=sign*brake_[one]*(XM_PI-.035);
            const double correction=std::clamp((target-turned_)*1.45-angularVelocity_.z*.94,-1.,1.);
            demand.hands[0].longitudinal=-correction;
            demand.hands[1].longitudinal=correction;
        }
        const auto wrench=Vaisseaux::StepPropulsion(Vaisseaux::Lunaire::Propulsion,Vaisseaux::Lunaire::Mounts,
            demand,density,Centre,dt,devices_,loads_);
        const auto toFrame=modelRotation*passage.RigRotation();
        const auto acceleration=TransformDirection(ModelDirection(wrench.forceN*(1./Mass)),toFrame);
        velocity_=Add(velocity_,Scale(acceleration,float(dt)));
        const float speed=Length(velocity_);
        if(braking>.02&&speed>0.f){
            const float stop=float(std::min(8.*braking,3.*speed)*dt);
            velocity_=Scale(velocity_,std::max(0.f,1.f-stop/speed));
        }
        if(density>0.f)velocity_=Scale(velocity_,float(1./(1.+.09*speed*dt)));
        translation=Add(translation,Scale(velocity_,float(dt)));

        const Vec3 momentum{Inertia.x*angularVelocity_.x,Inertia.y*angularVelocity_.y,Inertia.z*angularVelocity_.z};
        const auto torque=wrench.torqueNm-Cross(angularVelocity_,momentum);
        angularVelocity_=angularVelocity_+Vec3{torque.x/Inertia.x,torque.y/Inertia.y,torque.z/Inertia.z}*dt;
        if(braking>.02){
            angularVelocity_.x=ApproachZero(angularVelocity_.x,braking*2.4*dt);
            angularVelocity_.y=ApproachZero(angularVelocity_.y,braking*2.4*dt);
            angularVelocity_.z=ApproachZero(angularVelocity_.z,braking*2.4*dt);
        }
        const double rate=std::sqrt(angularVelocity_.x*angularVelocity_.x+
                                    angularVelocity_.y*angularVelocity_.y+angularVelocity_.z*angularVelocity_.z);
        constexpr double MaxRate=1.6;
        if(rate>MaxRate)angularVelocity_=angularVelocity_*(MaxRate/rate);
        if(one!=Invalid)turned_+=angularVelocity_.z*dt;
        if(rate>1.e-8){
            const auto axis=ModelDirection(angularVelocity_*(-1.));
            const auto rotation=XMMatrixRotationAxis(XMLoadFloat3(&axis),float(std::min(rate,MaxRate)*dt));
            passage.RotateRig(fromPivot,inverseModel*rotation*modelRotation);
        }
    }
    return {translation};
}

void ShuttlePilot::Crossed(std::span<const MembraneCrossing> crossings) {
    if(!Active())return;
    for(const auto& crossing:crossings){
        const auto body=SolarBodyToCosmos(crossing.sphere,SolarBody(crossing.sphere,crossing.solarTime));
        const auto change=crossing.entering?XMMatrixInverse(nullptr,body):body;
        velocity_=TransformDirection(velocity_,change);
    }
}

void ShuttlePilot::Reanchor(const Pose& previous,const Pose& head) {
    if(!Active())return;
    const auto change=XMMatrixInverse(nullptr,Matrix(previous))*Matrix(head);
    XMStoreFloat4x4(&modelToTracking_,XMLoadFloat4x4(&modelToTracking_)*change);
    ResetNeutral();
}

void ShuttlePilot::Publish(FXMMATRIX trackingToWorld,const FrameInput& input) {
    if(!Active())return;
    XMStoreFloat4x4(&modelToWorld_,XMLoadFloat4x4(&modelToTracking_)*trackingToWorld);
    activeBody_=input.insideSphere;
    map_.Refresh(input.solarTime,{input.head.p.x,input.head.p.z},activeBody_,[&](size_t owner){
        const auto position=SolarCenter(uint32_t(owner),input.solarTime);
        return Vaisseaux::MapDot{position.x,position.z};
    });
}

bool ShuttlePilot::ConsumeIgnition() noexcept {return std::exchange(ignition_,false);}

XMMATRIX ShuttlePilot::ModelToWorld() const noexcept {return XMLoadFloat4x4(&modelToWorld_);}

uint32_t ShuttlePilot::DisplayData(uint32_t page) const noexcept {
    uint32_t payload=0;
    if(page==3u){
        const int value=setting_==Setting::LeftAngle||setting_==Setting::RightAngle?
            offset_[uint32_t(setting_)]:setting_==Setting::DeadZone?deadZone_:int(theme_);
        const uint32_t state=power_==Power::Running?21u:uint32_t(heldFraction_*20.f);
        payload=uint32_t(setting_)|(uint32_t(value+40)<<3u)|(state<<10u)|
                (uint32_t(aligned_[0])<<15u)|(uint32_t(aligned_[1])<<16u);
    }else if(page==1u){
        payload=uint32_t(std::clamp(Length(velocity_)*10.f,0.f,9999.f));
    }else if(page==0u){
        payload=activeBody_==Invalid?15u:activeBody_;
    }else if(page==5u){
        for(uint32_t i=0;i<PropulsorCount;++i)
            if(Length(ModelDirection(loads_[i].forceN))>10.f)payload|=1u<<i;
    }
    // RenderObject::visualPose arrives at the shader as a finite FP32 integer.
    return 1u+theme_+(page<<3u)+(payload<<6u);
}
}
