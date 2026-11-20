#include "ShuttlePilot.h"
#include "VrInputMapping.h"
#include "shuttle/ShuttleLayout.h"

#include <cstdio>
#include <charconv>
#include <fstream>
#include <string_view>
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

std::array<ShuttlePilot::PersistentSetting,16> ShuttlePilot::PersistentSettings() noexcept {
    return {{{"angle_g",offset_[0],-40,40},{"angle_d",offset_[1],-40,40},{"zone_neutre",deadZone_,0,45},
             {"couleur",theme_,0,int(Vaisseaux::Lunaire::Themes.size())-1},
             {"reacteurs",reactorLimit_,0,125},{"helices_rpm",fanRpmSteps_,12,120},
             {"helices_montee",fanRiseTenths_,1,50},{"assise",seat_,0,10},
             {"reacteurs_volume_min",mix_[0],0,100},{"reacteurs_volume_max",mix_[1],0,100},
             {"helices_volume_min",mix_[2],0,100},{"helices_volume_max",mix_[3],0,100},
             {"impacts_volume",mix_[4],0,100},{"ignition_volume",mix_[5],0,100},
             {"poussiere_densite",mix_[6],0,100},{"exterieur_volume",mix_[7],0,100}}};
}

void ShuttlePilot::ApplyPropulsionSettings() {
    auto& curve=propulsion_.reactor.curve;
    curve=Vaisseaux::Lunaire::Propulsion.reactor.curve;
    for(auto& point:curve){
        point.forceN*=.01*reactorLimit_;
        point.inputW*=.01*reactorLimit_;
    }
    propulsion_.fan.maxRpm=fanRpmSteps_*FanRpmStep;
    propulsion_.fan.spinUpSeconds=fanRiseTenths_*.1;
}

void ShuttlePilot::LoadSettings(const std::filesystem::path& path) {
    settingsPath_=path;
    if(path.empty())return;
    std::ifstream file(path);
    const auto settings=PersistentSettings();
    for(std::string line;std::getline(file,line);){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        const auto equals=line.find('=');
        if(equals==std::string::npos)continue;
        const std::string_view key(line.data(),equals);
        int value{};
        const char* end=line.data()+line.size();
        const auto parsed=std::from_chars(line.data()+equals+1,end,value);
        if(parsed.ec!=std::errc{}||parsed.ptr!=end)continue;
        for(const auto& option:settings)
            if(key==option.key)option.value=std::clamp(value,option.min,option.max);
    }
    mix_[0]=std::min(mix_[0],mix_[1]);
    mix_[2]=std::min(mix_[2],mix_[3]);
    ApplyPropulsionSettings();
}

void ShuttlePilot::SaveSettings(double time,bool flush) {
    if(!settingsDirty_||settingsPath_.empty()||
       (!flush&&(time-lastSettingEdit_<.5||time-lastSaveAttempt_<1.)))return;
    lastSaveAttempt_=time;
    std::error_code ec;
    std::filesystem::create_directories(settingsPath_.parent_path(),ec);
    auto pending=settingsPath_;
    pending+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    std::ofstream file(pending,std::ios::trunc);
    for(const auto& option:PersistentSettings())file<<option.key<<'='<<option.value<<'\n';
    file.close();
    if(file&&MoveFileExW(pending.wstring().c_str(),settingsPath_.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
        settingsDirty_=false;
        return;
    }
    std::filesystem::remove(pending,ec);
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
    stickLatched_=mixStickLatched_=true;
    avionics_=false;
    wake_.Clear();
    dustTravel_=0.f;
    rotorDirection_={};
    rotorReversals_=0u;
    ResetNeutral();
}

void ShuttlePilot::Leave() {
    occupied_=avionics_=false;
    wake_.Clear();
    dustTravel_=0.f;
    rotorDirection_={};
    rotorReversals_=0u;
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

void ShuttlePilot::ResetNeutral(bool requireUpright) {
    requireUpright_=requireUpright;
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
                    theme_=(theme_+step+count)%count;
                    break;
                }
                case Setting::ReactorLimit:reactorLimit_=std::clamp(reactorLimit_+5*step,0,125);break;
                case Setting::FanRpm:fanRpmSteps_=std::clamp(fanRpmSteps_+step,12,120);break;
                case Setting::FanRise:fanRiseTenths_=std::clamp(fanRiseTenths_+step,1,50);break;
                case Setting::Seat:seat_=std::clamp(seat_+step,0,10);break;
                default:break;
            }
            ApplyPropulsionSettings();
            settingsDirty_=true;
            lastSettingEdit_=input.time;
        }
    }
    if(setting_==Setting::Calibrate&&input.Pressed(actions.recenter,0u))
        ResetNeutral(false);
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
    const auto model=XMLoadFloat4x4(&modelToTracking_);
    const auto headward=TransformDirection({1,0,0},model);
    auto fore=Sub(headward,Scale(restUp,Dot(headward,restUp)));
    if(Length(fore)<.001f)fore=CrossDirection(restUp,TransformDirection({0,0,1},model));
    const auto forward=Unit(fore);
    const auto left=CrossDirection(forward,restUp);
    const double keepUp=std::clamp(double(Dot(up,restUp)),-1.,1.);
    if(keepUp<=0.)return {};

    // Swing the neutral grip to the current shaft direction before measuring twist.
    const auto axis=CrossDirection(restUp,up);
    auto neutralSide=TransformDirection({1,0,0},neutral);
    if(Length(axis)>.00001f)
        neutralSide=TransformDirection(neutralSide,
            XMMatrixRotationAxis(XMLoadFloat3(&axis),float(std::atan2(Length(axis),keepUp))));
    const auto side=TransformDirection({1,0,0},current);
    const double twist=std::atan2(Dot(up,CrossDirection(neutralSide,side)),Dot(neutralSide,side));
    return {Shape(std::atan2(Dot(up,forward),keepUp),24.*Degrees,deadZone_),
            Shape(std::atan2(Dot(up,left),keepUp),24.*Degrees,deadZone_),
            Shape(twist*(slot==0?1.:-1.),30.*Degrees,deadZone_)};
}

void ShuttlePilot::ReadMixer(const FrameInput& input,const VrInputActions& actions,bool toolOwnsStick) {
    if(!input.focused||toolOwnsStick||!avionics_){
        mixStickLatched_=true;
        return;
    }
    const auto stick=input.Vector(actions.turn);
    const float extent=std::max(std::abs(stick.x),std::abs(stick.y));
    if(extent<StickRelease)mixStickLatched_=false;
    const bool step=!mixStickLatched_&&extent>=StickEngage;
    if(step){
        mixStickLatched_=true;
        if(std::abs(stick.y)>std::abs(stick.x)){
            const int count=int(Mixer::Count);
            mixer_=Mixer((int(mixer_)+(stick.y>0.f?count-1:1))%count);
            return;
        }
    }
    const auto index=uint32_t(mixer_);
    int& value=mix_[index];
    const bool reset=input.Pressed(actions.shuttleMixReset,1u);
    if(reset||step){
        const int minimum=mixer_==Mixer::JetMax||mixer_==Mixer::FanMax?mix_[index-1u]:0;
        const int maximum=mixer_==Mixer::JetMin||mixer_==Mixer::FanMin?mix_[index+1u]:100;
        const int next=std::clamp(reset?DefaultMix[index]:value+(stick.x>0.f?5:-5),minimum,maximum);
        if(next!=value){
            value=next;
            settingsDirty_=true;
            lastSettingEdit_=input.time;
        }
    }
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
    for(uint32_t hand=0;hand<2;++hand)aligned_[hand]=hands[hand].tracked&&(!requireUpright_||Upright(hands[hand],hand));
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
        avionics_=true;
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
    // The shields affect horizontal velocity and trim, independently of the jets.
    demand.liftBalance=std::min(brake_[0],brake_[1]);
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
        if(braking>0.f){
            const auto horizontal=Sub(velocity_,Scale(up,Dot(velocity_,up)));
            const float speed=Length(horizontal);
            if(speed>0.f){
                const float stop=float(braking*std::min(8.f,3.f*speed)*dt);
                velocity_=Sub(velocity_,Scale(horizontal,std::min(1.f,stop/speed)));
            }
        }
        velocity_=Add(velocity_,Scale(acceleration,float(dt)));
        if(density>0.f)velocity_=Scale(velocity_,float(1./(1.+.09*Length(velocity_)*dt)));
        translation=Add(translation,Scale(velocity_,float(dt)));

        const Vec3 momentum{Inertia.x*angularVelocity_.x,Inertia.y*angularVelocity_.y,Inertia.z*angularVelocity_.z};
        // Trim the fore/aft centre-of-mass offset without removing the commanded lift bias.
        const Vec3 trim{0.,Centre.x*wrench.forceN.z,0.};
        const auto torque=wrench.torqueNm-trim-Cross(angularVelocity_,momentum);
        angularVelocity_=angularVelocity_*std::exp(-(1.4+8.*demand.liftBalance)*dt)+
            Vec3{torque.x/Inertia.x,torque.y/Inertia.y,torque.z/Inertia.z}*dt;
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
    for(uint32_t side=0;side<2;++side){
        const auto& rotor=loads_[4u+2u*side];
        if(rotor.rpm<80.||rotor.rotorDirection==0)continue;
        auto& previous=rotorDirection_[side];
        if(previous!=0&&previous!=rotor.rotorDirection)rotorReversals_|=1u<<side;
        previous=rotor.rotorDirection;
    }
    dustTravel_=std::fmod(dustTravel_+float(seconds)*std::min(Length(velocity_),180.f),40.f);
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
    if(Ready()){
        for(auto& pose:neutral_)pose=FromMatrix(Matrix(pose)*change);
        demand_={};
    }else{
        hasAlignedSince_=false;
        heldFraction_=0.f;
    }
}

void ShuttlePilot::Publish(FXMMATRIX trackingToWorld,const FrameInput& input) {
    if(!Active())return;
    XMStoreFloat4x4(&modelToWorld_,XMLoadFloat4x4(&modelToTracking_)*trackingToWorld);
    const auto model=RotationOnly(XMLoadFloat4x4(&modelToTracking_));
    const auto frame=input.insideSphere==Invalid?trackingToWorld:trackingToWorld*
        XMMatrixInverse(nullptr,SolarBodyToCosmos(input.insideSphere,SolarBody(input.insideSphere,input.solarTime)));
    const auto orientation=Matrix(FromMatrix(model*frame));
    const auto root=XMLoadFloat4x4(&modelToTracking_)*frame;
    localVelocity_=TransformDirection(velocity_,XMMatrixTranspose(RotationOnly(orientation)));
    const auto forward=TransformDirection({1.f,0.f,0.f},orientation);
    pitchDegrees_=XMConvertToDegrees(std::asin(std::clamp(forward.y,-1.f,1.f)));
    activeBody_=input.insideSphere;
    wake_.Step(input,root,localVelocity_,loads_,Mix(Mixer::DustDensity),canopy_);
    map_.Refresh(input.solarTime,{input.head.p.x,input.head.p.z},activeBody_,[&](size_t owner){
        const auto position=SolarCenter(uint32_t(owner),input.solarTime);
        return Vaisseaux::MapDot{position.x,position.z};
    });
}

XMFLOAT3 ShuttlePilot::HandleAngles(uint32_t hand) const noexcept {
    const auto& axes=demand_.hands[hand];
    return {float(axes.longitudinal)*24.f*Degrees,float(axes.lateral)*24.f*Degrees,
            float(axes.vertical)*(hand==0u?30.f:-30.f)*Degrees};
}

bool ShuttlePilot::ConsumeIgnition() noexcept {return std::exchange(ignition_,false);}
uint32_t ShuttlePilot::ConsumeRotorReversals() noexcept {return std::exchange(rotorReversals_,0u);}

Pose ShuttlePilot::ParkingPose(double time) const noexcept {
    auto eye=ShuttleLayout::Eye;
    eye.y+=(seatPosition_-8.f)*SeatStepMetres;
    const auto spot=Add(ShuttleLayout::Parking,
        Scale({-eye.z,eye.y,eye.x},SpectacleInteriorUnitsPerMeter));
    const auto oasis=SolarBody(0u,time);
    return FromMatrix(XMMatrixTranslation(spot.x,spot.y,spot.z)*SolarBodyToCosmos(0u,oasis));
}

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
    if(!avionics_)return 0;
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
        for(uint32_t i=0;i<PropulsorCount;++i){
            const auto& force=loads_[i].forceN;
            uint32_t direction=0;
            if(i<4u&&std::hypot(force.x,force.y)>4.8)
                direction=force.y>.08*std::abs(force.x)?3u:force.y<-.08*std::abs(force.x)?1u:2u;
            else if(i>=4u&&std::abs(force.z)>1.)
                direction=force.z>0.?1u:2u;
            payload|=direction<<(2u*i);
        }
    }else if(page==2u){
        const auto channel=uint32_t(mixer_);
        payload=channel|(uint32_t(mix_[channel])<<3u);
    }else if(page==0u){
        payload=activeBody_==Invalid?15u:activeBody_;
    }else if(page==5u){
        const auto horizontal=std::hypot(localVelocity_.x,localVelocity_.z);
        const float vertical=std::abs(localVelocity_.y);
        // Keep finer resolution around zero, with a wider range for free flight.
        const auto speed=uint32_t(std::round(std::clamp(horizontal<=25.5f?horizontal*10.f:
            255.f+(horizontal-25.5f)*2.f,0.f,1023.f)));
        const auto climb=uint32_t(std::round(std::clamp(vertical<=63.f?vertical:
            63.f+(vertical-63.f)*.25f,0.f,127.f)));
        payload=speed|(climb<<10u)|(uint32_t(localVelocity_.y<0.f&&climb!=0u)<<17u);
    }
    // RenderObject::visualPose arrives at the shader as a finite FP32 integer.
    return 1u+uint32_t(theme_)+(page<<3u)+(payload<<6u);
}
}
