#include "ShuttlePilot.h"
#include "VrInputMapping.h"
#include "shuttle/ShuttleLayout.h"

#include <cstdio>
#include <utility>

namespace OASIS {
void DiagnosticLine(const char*) noexcept;
namespace {
using Vaisseaux::Vec3;
using Vaisseaux::Cross;

constexpr int64_t ReadyTime=2'000'000'000, ExitTime=3'000'000'000;
constexpr float Degrees=XM_PI/180.f;
constexpr float Mass=Vaisseaux::Lunaire::DryKg+Vaisseaux::Lunaire::FuelCapacityKg;
constexpr Vec3 Inertia{1900.,3300.,3600.};
constexpr float UprightTolerance=27.f*Degrees;
constexpr float StickEngage=.68f, StickRelease=.24f;
constexpr float SeatStepMetres=.025f;
constexpr int FanRpmStep=50;

// The right-handed propulsion frame is X forward, Y left, Z up.
// The imported hull uses X forward, Y up, Z left.
XMFLOAT3 ModelDirection(Vec3 v) {return {float(v.x),float(v.z),float(v.y)};}
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

XMMATRIX DashboardTool() {
    static const auto model=[]{
        const auto anchor=XMLoadFloat3(&ShuttleLayout::ToolAnchor);
        const auto outward=XMVectorSubtract(anchor,XMLoadFloat3(&ShuttleLayout::Eye));
        const auto view=XMMatrixLookToLH(anchor,outward,XMVectorSet(0,1,0,0));
        return XMMatrixScaling(.034f,.034f,.034f)*XMMatrixInverse(nullptr,view);
    }();
    return model;
}

const auto Centre=Vaisseaux::CentreOfMass(Vaisseaux::Lunaire::DryKg,Vaisseaux::Lunaire::DryCentreMetres,
                                         Vaisseaux::Lunaire::FuelCapacityKg,Vaisseaux::Lunaire::FuelCentreMetres);
}

void ShuttlePilot::Board(const Pose& head) {
    const auto direction=TransformDirection({0.f,0.f,1.f},Matrix(head));
    const float yaw=std::atan2(direction.x,direction.z);
    const auto rotation=XMMatrixRotationY(yaw-XM_PIDIV2);
    seatPosition_=float(seat_);
    auto eye=ShuttleLayout::Eye;
    eye.y+=(seatPosition_-8.f)*SeatStepMetres;
    const auto origin=Sub(head.p,TransformDirection(eye,rotation));
    XMStoreFloat4x4(&modelToTracking_,rotation*XMMatrixTranslation(origin.x,origin.y,origin.z));
    occupied_=true;
    map_.periodSeconds=Vaisseaux::Lunaire::MapPeriodSeconds;
    exitArmed_=false;
    aHeldSince_=0;
    velocity_={};localVelocity_={};angularVelocity_={};pitchDegrees_=0.f;
    devices_={};loads_={};
    brake_={};
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
    velocity_={};localVelocity_={};angularVelocity_={};pitchDegrees_=0.f;
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
                case Setting::ReactorLimit:{
                    reactorLimit_=std::clamp(reactorLimit_+5*step,0,125);
                    auto& curve=propulsion_.reactor.curve;
                    curve=Vaisseaux::Lunaire::Propulsion.reactor.curve;
                    for(auto& point:curve){
                        point.forceN*=.01*reactorLimit_;
                        point.inputW*=.01*reactorLimit_;
                    }
                    break;
                }
                case Setting::FanRpm:
                    fanRpmSteps_=std::clamp(fanRpmSteps_+step,12,120);
                    propulsion_.fan.maxRpm=fanRpmSteps_*FanRpmStep;
                    break;
                case Setting::FanRise:
                    fanRiseTenths_=std::clamp(fanRiseTenths_+step,1,50);
                    propulsion_.fan.spinUpSeconds=fanRiseTenths_*.1;
                    break;
                case Setting::Seat:seat_=std::clamp(seat_+step,0,10);break;
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
        if(hands[hand].tracked){
            const float dead=.01f*float(deadZone_);
            brake_[hand]=std::clamp((input.Analog(actions.grab,hand)-dead)/(1.f-dead),0.f,1.f);
        }
    }
}

VrTravel ShuttlePilot::Advance(double seconds,SolarPassage& passage,const Pose& previousHead) {
    if(!Active()||seconds<=0.)return {};
    auto model=XMLoadFloat4x4(&modelToTracking_);
    const auto modelRotation=RotationOnly(model);
    const auto inverseModel=XMMatrixTranspose(modelRotation);
    const float seatMovement=std::clamp(float(seat_)-seatPosition_,-float(10.*seconds),float(10.*seconds));
    seatPosition_+=seatMovement;
    const auto eyeShift=TransformDirection({0.f,seatMovement*SeatStepMetres,0.f},modelRotation);
    model.r[3]=XMVectorSubtract(model.r[3],XMLoadFloat3(&eyeShift));
    XMStoreFloat4x4(&modelToTracking_,model);
    const auto pivotRaw=TransformPoint(ModelDirection(Centre),model);
    const auto fromPivot=Add(Sub(previousHead.p,pivotRaw),Scale(eyeShift,-1.f));
    auto demand=demand_;
    const float braking=.5f*(brake_[0]+brake_[1]);
    demand.balancedLift=brake_[0]>0.f&&brake_[1]>0.f;
    if(demand.balancedLift)
        for(auto& hand:demand.hands)hand.longitudinal=0.;
    const float density=passage.Inside()?1.f:0.f;
    const unsigned steps=unsigned(std::ceil(seconds*120.));
    const double dt=seconds/steps;
    XMFLOAT3 translation=TransformDirection(eyeShift,passage.RigRotation());
    for(unsigned i=0;i<steps;++i){
        const auto toFrame=modelRotation*passage.RigRotation();
        const auto forward=TransformDirection({1.f,0.f,0.f},toFrame);
        const auto up=TransformDirection({0.f,1.f,0.f},toFrame);
        demand.longitudinalSpeedMps=Dot(velocity_,forward);
        const auto wrench=Vaisseaux::StepPropulsion(propulsion_,Vaisseaux::Lunaire::Mounts,
            demand,density,Centre,dt,devices_,loads_);
        const auto acceleration=TransformDirection(ModelDirection(wrench.forceN*(1./Mass)),toFrame);
        velocity_=Add(velocity_,Scale(acceleration,float(dt)));
        if(braking>0.f){
            const auto horizontal=Sub(velocity_,Scale(up,Dot(velocity_,up)));
            const float speed=Length(horizontal);
            if(speed>0.f){
                const float stop=float(braking*std::min(8.f,3.f*speed)*dt);
                velocity_=Sub(velocity_,Scale(horizontal,std::min(1.f,stop/speed)));
            }
        }
        if(density>0.f)velocity_=Scale(velocity_,float(1./(1.+.09*Length(velocity_)*dt)));
        translation=Add(translation,Scale(velocity_,float(dt)));

        if(demand.balancedLift){
            angularVelocity_={};
            continue;
        }
        const Vec3 momentum{Inertia.x*angularVelocity_.x,Inertia.y*angularVelocity_.y,Inertia.z*angularVelocity_.z};
        // Trim the fore/aft centre-of-mass offset without removing the commanded lift bias.
        const Vec3 trim{0.,Centre.x*wrench.forceN.z,0.};
        const auto torque=wrench.torqueNm-trim-Cross(angularVelocity_,momentum);
        angularVelocity_=angularVelocity_+Vec3{torque.x/Inertia.x,torque.y/Inertia.y,torque.z/Inertia.z}*dt;
        const double rate=std::sqrt(angularVelocity_.x*angularVelocity_.x+
                                    angularVelocity_.y*angularVelocity_.y+angularVelocity_.z*angularVelocity_.z);
        constexpr double MaxRate=1.6;
        if(rate>MaxRate)angularVelocity_=angularVelocity_*(MaxRate/rate);
        if(rate>1.e-8){
            const auto axis=ModelDirection(angularVelocity_*(-1.));
            const auto rotation=XMMatrixRotationAxis(XMLoadFloat3(&axis),float(std::min(rate,MaxRate)*dt));
            passage.RotateRig(fromPivot,inverseModel*rotation*modelRotation);
        }
    }
    const auto toFrame=modelRotation*passage.RigRotation();
    localVelocity_=TransformDirection(velocity_,XMMatrixTranspose(toFrame));
    const auto forward=TransformDirection({1.f,0.f,0.f},toFrame);
    pitchDegrees_=XMConvertToDegrees(std::asin(std::clamp(forward.y,-1.f,1.f)));
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
XMMATRIX ShuttlePilot::ToolInTracking() const noexcept {return DashboardTool()*XMLoadFloat4x4(&modelToTracking_);}
XMMATRIX ShuttlePilot::ToolInWorld() const noexcept {return DashboardTool()*ModelToWorld();}

void ShuttlePilot::LogSample(uint64_t frame) const noexcept {
    const auto& g=demand_.hands[0];
    const auto& d=demand_.hands[1];
    const auto jet=[&](size_t i){return std::hypot(loads_[i].forceN.x,loads_[i].forceN.y);};
    const auto forePercent=[&](size_t aft){
        const double a=std::abs(loads_[aft].forceN.z),b=std::abs(loads_[aft+1u].forceN.z);
        return a+b>1.e-6?100.*b/(a+b):50.;
    };
    char line[768];
    std::snprintf(line,sizeof(line),
        "navette_image=%llu a_bord=%u allumee=%u grip_g=%.2f grip_d=%.2f "
        "axes_g_long_lat_twist=%.2f,%.2f,%.2f axes_d_long_lat_twist=%.2f,%.2f,%.2f "
        "reacteurs_n_ar_g_av_g_ar_d_av_d=%.0f,%.0f,%.0f,%.0f "
        "helices_rpm_ar_g_av_g_ar_d_av_d=%.0f,%.0f,%.0f,%.0f "
        "helices_av_g_pct=%.1f helices_av_d_pct=%.1f vx_m_s=%.3f vy_m_s=%.3f vz_m_s=%.3f "
        "tangage_deg=%.2f zone_neutre_pct=%d assise=%.2f "
        "limite_reacteurs_pct=%d limite_helices_rpm=%d montee_90_s=%.1f\r\n",
        static_cast<unsigned long long>(frame),unsigned(Active()),unsigned(Ready()),brake_[0],brake_[1],
        g.longitudinal,g.lateral,g.vertical,d.longitudinal,d.lateral,d.vertical,
        jet(0u),jet(1u),jet(2u),jet(3u),loads_[4].rpm,loads_[5].rpm,loads_[6].rpm,loads_[7].rpm,
        forePercent(4u),forePercent(6u),localVelocity_.x,localVelocity_.y,localVelocity_.z,
        pitchDegrees_,deadZone_,seatPosition_,reactorLimit_,fanRpmSteps_*FanRpmStep,fanRiseTenths_*.1);
    DiagnosticLine(line);
}

uint32_t ShuttlePilot::DisplayData(uint32_t page) const noexcept {
    uint32_t payload=0;
    if(page==3u){
        int value=0;
        switch(setting_){
            case Setting::LeftAngle:case Setting::RightAngle:value=offset_[uint32_t(setting_)]+40;break;
            case Setting::DeadZone:value=deadZone_;break;
            case Setting::Color:value=int(theme_);break;
            case Setting::ReactorLimit:value=reactorLimit_;break;
            case Setting::FanRpm:value=fanRpmSteps_;break;
            case Setting::FanRise:value=fanRiseTenths_;break;
            case Setting::Seat:value=seat_;break;
            default:break;
        }
        const uint32_t state=power_==Power::Running?21u:uint32_t(heldFraction_*20.f);
        payload=uint32_t(setting_)|(uint32_t(value)<<4u)|(state<<11u)|
                (uint32_t(aligned_[0])<<16u)|(uint32_t(aligned_[1])<<17u);
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
