#include "OrbitTool.h"
#include "Graphics.h"
#include "VrInputMapping.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string_view>

namespace OASIS {
namespace {
constexpr uint32_t Right=1;
constexpr float FogMax=.40f,FogStep=.01f;
constexpr float Neutral=.16f,SameStep=.0001f;
constexpr float Detent=XM_2PI/16.f;
constexpr double ColorReturnTime=.13;
constexpr auto FogBit=OrbitFeatureBit(OrbitFeature::FogDensity);
constexpr auto DlssBit=OrbitFeatureBit(OrbitFeature::DlssScale);
constexpr std::array<std::string_view,2> ProfileKeys{"fog_density","dlss_scale"};

bool Crossed(float before,float after,float mark){
    return (before<mark-SameStep&&after>=mark-SameStep)||
           (before>mark+SameStep&&after<=mark+SameStep);
}
}

OrbitProfile::OrbitProfile(const std::wstring& runtimeConfig){
    if(runtimeConfig.empty())return;
    namespace fs=std::filesystem;
    auto folder=fs::path(runtimeConfig).parent_path();
    for(;folder.has_relative_path();folder=folder.parent_path()){
        std::error_code ec;
        if(fs::exists(folder/".git",ec)&&fs::is_regular_file(folder/"compile and run.bat",ec)){
            path_=folder/".oasis-local"/"orbit.ini";
            break;
        }
    }
    if(path_.empty())return;
    std::ifstream file(path_);
    for(std::string line;std::getline(file,line);){
        std::istringstream entry(line);
        entry.imbue(std::locale::classic());
        std::string key;
        float value{};
        if(!std::getline(entry,key,'=')||!(entry>>value)||!std::isfinite(value))continue;
        entry>>std::ws;
        if(!entry.eof())continue;
        if(key==ProfileKeys[0]&&value>=0.f&&value<=FogMax)values_[0]=value;
        if(key==ProfileKeys[1]&&value>0.f&&value<=1.f)values_[1]=value;
    }
}

std::optional<float> OrbitProfile::Get(OrbitFeature feature)const{
    return values_[uint32_t(feature)-1u];
}

void OrbitProfile::Set(OrbitFeature feature,std::optional<float> value){
    auto& slot=values_[uint32_t(feature)-1u];
    if(slot==value)return;
    slot=value;
    dirty_=true;
}

bool OrbitProfile::Save()const{
    if(!dirty_||path_.empty())return true;
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(),ec);
    if(ec)return false;
    auto pending=path_;
    pending+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
    std::ofstream file(pending,std::ios::trunc);
    file.imbue(std::locale::classic());
    file<<std::setprecision(std::numeric_limits<float>::max_digits10);
    for(size_t i=0;i<values_.size();++i)
        if(values_[i])file<<ProfileKeys[i]<<'='<<*values_[i]<<'\n';
    file.close();
    if(file&&MoveFileExW(pending.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        return true;
    std::filesystem::remove(pending,ec);
    return false;
}

void OrbitTool::Restore(Graphics& graphics,AtmosphereSettings& atmosphere){
    if(atmosphere.radius>0.f){
        if(const auto density=profile_.Get(OrbitFeature::FogDensity)){
            atmosphere.density=*density;
            graphics.SetAtmosphere(atmosphere);
        }
    }
    if(logo_){
        const auto scale=profile_.Get(OrbitFeature::DlssScale);
        if(scale&&*scale<1.f)graphics.SetDlssScale(*scale);
    }
    Refresh(graphics.GetDlssScaleControl(),atmosphere);
}

void OrbitTool::UpdatePointer(const PoseActionState& left,const PoseActionState& aim){
    const auto previous=display_.hover;
    display_.hover=OrbitControl::None;
    display_.pointer.reset();
    if(!aim.tracked)return;
    const auto glove=VrGlovePose(left,0);
    if(!glove)return;
    const auto inverse=XMMatrixInverse(nullptr,PhosphorOrbitModel(*glove));
    const auto forward=Sub(Transform({0,0,1},aim.pose),aim.pose.p);
    XMFLOAT3 origin,direction;
    XMStoreFloat3(&origin,XMVector3TransformCoord(XMLoadFloat3(&aim.pose.p),inverse));
    XMStoreFloat3(&direction,XMVector3TransformNormal(XMLoadFloat3(&forward),inverse));
    if(direction.z<=.15f*Length(direction)||origin.z>=-.44f)return;
    const XMFLOAT2 slope{direction.x/direction.z,direction.y/direction.z};
    const XMFLOAT2 intercept{origin.x-slope.x*origin.z,origin.y-slope.y*origin.z};
    const float faceDistance=(-.64f-origin.z)/direction.z;
    if(faceDistance>=0.f&&faceDistance<=1.2f)
        display_.pointer=XMFLOAT4{intercept.x,intercept.y,slope.x,slope.y};

    float best=1.f;
    for(const auto& zone:OrbitControlZones){
        if(!display_.features&&zone.control!=OrbitControl::Color)continue;
        const float t=(zone.front-origin.z)/direction.z;
        if(t<0.f||t>1.2f)continue;
        const float dx=(intercept.x+slope.x*zone.front-zone.center.x)/zone.halfSize.x;
        const float dy=(intercept.y+slope.y*zone.front-zone.center.y)/zone.halfSize.y;
        const float distance=dx*dx+dy*dy;
        if(zone.control==previous&&distance<=1.35f){
            display_.hover=previous;
            break;
        }
        if(distance<best){
            best=distance;
            display_.hover=zone.control;
        }
    }
}

void OrbitTool::Drop(){
    if(display_.locked!=OrbitControl::None)awaitNeutral_=true;
    display_.locked=OrbitControl::None;
    turning_=false;
    accumulatedAngle_=0.f;
    snapDirection_=0;
}

void OrbitTool::ReadGesture(XMFLOAT2 stick,double time){
    const float radius=std::hypot(stick.x,stick.y);
    if(radius<=Neutral)gestureArmed_=true;
    if(!gestureArmed_)return;
    if(display_.locked==OrbitControl::Select||display_.locked==OrbitControl::Adjust){
        if(radius<(turning_?.28f:.40f)){
            turning_=false;
            return;
        }
        const float angle=std::atan2(stick.y,stick.x);
        const float delta=turning_?std::remainder(previousAngle_-angle,XM_2PI):0.f;
        previousAngle_=angle;
        turning_=true;
        if(std::abs(delta)>XM_PIDIV2)return;
        auto& dial=display_.locked==OrbitControl::Select?display_.selectAngle:display_.adjustAngle;
        dial=std::remainder(dial+delta,XM_2PI);
        accumulatedAngle_+=delta;
        steps_=int(accumulatedAngle_/Detent);
        accumulatedAngle_-=float(steps_)*Detent;
        return;
    }
    if(std::abs(stick.y)<.52f||std::abs(stick.y)<1.1f*std::abs(stick.x))return;
    if(display_.locked==OrbitControl::Color&&stick.y<0.f)return;
    steps_=stick.y>0.f?1:-1;
    gestureArmed_=false;
    if(display_.locked==OrbitControl::Color)colorReturn_=time+ColorReturnTime;
}

bool OrbitTool::ReadInput(const FrameInput& input){
    steps_=0;
    display_.colorTravel=.13f*std::clamp(float((colorReturn_-input.time)/ColorReturnTime),0.f,1.f);
    const auto& actions=StandardVrActions();
    if(input.focused&&input.Pressed(actions.equipRightTool,Right))
        rightTool_=rightTool_==RightHandTool::Basalt?RightHandTool::Scoria:
            rightTool_==RightHandTool::Scoria?RightHandTool::Empty:RightHandTool::Basalt;
    const auto& left=input.PoseState(actions.handPose,0);
    const auto& right=input.PoseState(actions.aimPose,Right);
    const auto& turn=input.State(actions.turn);
    const auto stick=turn.vector[Right];
    const bool neutral=turn.active[Right]&&std::hypot(stick.x,stick.y)<=Neutral;
    const bool canPoint=input.focused&&rightTool_==RightHandTool::Empty&&left.tracked&&right.tracked;
    if(canPoint)UpdatePointer(left,right);
    else{
        display_.hover=OrbitControl::None;
        display_.pointer.reset();
    }
    if(!canPoint||!input.Down(actions.grab,Right))Drop();
    if(canPoint&&input.Pressed(actions.grab,Right)&&display_.hover!=OrbitControl::None){
        display_.locked=display_.hover;
        gestureArmed_=neutral;
        awaitNeutral_=false;
    }
    if(display_.locked!=OrbitControl::None){
        if(turn.active[Right])ReadGesture(stick,input.time);
        else{
            gestureArmed_=false;
            turning_=false;
        }
        return true;
    }
    const bool consume=awaitNeutral_;
    if(input.focused&&neutral)awaitNeutral_=false;
    return consume;
}

std::optional<float> OrbitTool::UserValue(OrbitFeature feature)const{
    auto value=profile_.Get(feature);
    if(feature==OrbitFeature::DlssScale&&value&&*value<1.f&&dlssMinimum_>0.f)
        value=std::clamp(*value,dlssMinimum_,dlssMaximum_);
    return value;
}

void OrbitTool::Refresh(const DlssScaleControl& dlss,const AtmosphereSettings& atmosphere){
    if(dlss.minimum>0.f&&dlss.minimum<=dlss.maximum){
        dlssMinimum_=dlss.minimum;
        dlssMaximum_=dlss.maximum;
    }
    const bool fog=atmosphere.radius>0.f;
    const bool scale=logo_&&dlss.available;
    display_.features=(fog?FogBit:0u)|(scale?DlssBit:0u);
    if(!(display_.features&OrbitFeatureBit(display_.selected)))
        display_.selected=fog?OrbitFeature::FogDensity:scale?OrbitFeature::DlssScale:OrbitFeature::None;

    display_.rangeKnown=false;
    display_.userMark=-1.f;
    display_.setTravel=0.f;
    if(display_.selected==OrbitFeature::None)return;
    const bool adjustingFog=display_.selected==OrbitFeature::FogDensity;
    const float minimum=adjustingFog?0.f:dlssMinimum_;
    const float maximum=adjustingFog?FogMax:1.f;
    const float factory=adjustingFog?factoryFog_:1.f;
    const float live=adjustingFog?atmosphere.density:dlss.value;
    const auto user=UserValue(display_.selected);
    auto normalize=[&](float v){return std::clamp((v-minimum)/(maximum-minimum),0.f,1.f);};
    display_.value=adjustingFog?live:100.f*live;
    display_.rangeKnown=adjustingFog||minimum>0.f;
    display_.liveMark=normalize(live);
    display_.factoryMark=normalize(factory);
    if(user){
        display_.userMark=normalize(*user);
        if(std::abs(live-*user)<=SameStep)display_.setTravel=.17f;
    }
}

void OrbitTool::Apply(Graphics& graphics,AtmosphereSettings& atmosphere){
    const auto dlss=graphics.GetDlssScaleControl();
    if(steps_){
        const auto feature=display_.selected;
        const bool fog=feature==OrbitFeature::FogDensity;
        switch(display_.locked){
        case OrbitControl::Select:
            if(display_.features==(FogBit|DlssBit)&&steps_%2!=0)
                display_.selected=fog?OrbitFeature::DlssScale:OrbitFeature::FogDensity;
            break;
        case OrbitControl::Adjust:{
            if(feature==OrbitFeature::None)break;
            display_.lastTouched=feature;
            const int direction=steps_>0?1:-1;
            // Hold a crossed marker for the next detent in that direction.
            // Reversing or grabbing again moves away immediately.
            const int count=std::abs(steps_)-(snapDirection_==direction?1:0);
            snapDirection_=0;
            if(!count)break;
            if(fog){
                const float before=atmosphere.density;
                const float grid=std::round(before/FogStep)+float(direction*count);
                float next=std::clamp(grid*FogStep,0.f,FogMax);
                if(Crossed(before,next,factoryFog_)){
                    next=factoryFog_;
                    snapDirection_=direction;
                }
                if(const auto user=UserValue(feature);user&&Crossed(before,next,*user)){
                    next=*user;
                    snapDirection_=direction;
                }
                if(next!=before){
                    atmosphere.density=next;
                    graphics.SetAtmosphere(atmosphere);
                }
            }else{
                for(int i=0;i<count;++i){
                    const float before=graphics.GetDlssScaleControl().value;
                    graphics.UpdateDlss(direction);
                    const auto after=graphics.GetDlssScaleControl();
                    if(!after.available||std::abs(after.value-before)<=SameStep)break;
                    if(const auto user=UserValue(feature);user&&Crossed(before,after.value,*user)){
                        graphics.SetDlssScale(*user==1.f?0.f:*user);
                        snapDirection_=direction;
                        break;
                    }
                    if(after.value==1.f){
                        snapDirection_=direction;
                        break;
                    }
                }
            }
            break;
        }
        case OrbitControl::Color:
            display_.color=(display_.color+1u)%5u;
            break;
        case OrbitControl::Set:
            if(feature==OrbitFeature::None)break;
            display_.lastTouched=feature;
            profile_.Set(feature,steps_>0?std::optional{fog?atmosphere.density:dlss.value}:std::nullopt);
            break;
        default:break;
        }
        steps_=0;
    }
    Refresh(graphics.GetDlssScaleControl(),atmosphere);
}
}
