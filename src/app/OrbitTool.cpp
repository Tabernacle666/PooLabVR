#include "OrbitTool.h"
#include "BuildingSession.h"
#include "Graphics.h"
#include "LocalProfile.h"
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
struct Setting {
    std::string_view key;
    float minimum,maximum,step;
    float MembraneSettings::* member{};
};
constexpr std::array<Setting,OrbitFeatureCount> Settings{{
    {"fog_density",0.f,FogMax,FogStep},
    {"dlss_scale",0.f,1.f,0.f},
    {"membrane_pattern_speed",0.f,3.f,.10f,&MembraneSettings::patternSpeed},
    {"membrane_opacity",0.f,.45f,.01f,&MembraneSettings::opacity},
    {"membrane_deformation",0.f,20.f,1.f,&MembraneSettings::deformation},
    {"membrane_glitter",0.f,2.f,.05f,&MembraneSettings::glitter},
    {"membrane_glitter_width",5.f,35.f,1.f,&MembraneSettings::glitterWidth},
    {"membrane_wave_strength",0.f,2.f,.05f,&MembraneSettings::waveStrength},
    {"membrane_wave_speed",.25f,2.5f,.05f,&MembraneSettings::waveSpeed},
    {"membrane_wave_duration",.25f,2.f,.05f,&MembraneSettings::waveDuration},
    {"scene_reflections",0.f,2.f,1.f},
    {"scene_antialiasing",0.f,3.f,1.f},
    {"peripheral_shading",0.f,1.f,1.f},
    {"solar_visibility",0.f,2.f,1.f},
}};
constexpr MembraneSettings FactoryFilm{};

bool IsGraphicsChoice(OrbitFeature feature){
    return feature>=OrbitFeature::SceneReflections&&feature<OrbitFeature::Count;
}

float GraphicsChoice(OrbitFeature feature,const RenderingOptions& values){
    switch(feature){
        case OrbitFeature::SceneReflections:return float(values.reflections);
        case OrbitFeature::Antialiasing:return float(values.antialiasing);
        case OrbitFeature::PeripheralShading:return float(values.peripheral);
        case OrbitFeature::SolarVisibility:return float(values.solarVisibility);
        default:return 0.f;
    }
}

void ChangeGraphicsChoice(OrbitFeature feature,float value,RenderingOptions& values){
    switch(feature){
        case OrbitFeature::SceneReflections:values.reflections=SceneReflections(uint32_t(value));break;
        case OrbitFeature::Antialiasing:values.antialiasing=SceneAntialiasing(uint32_t(value));break;
        case OrbitFeature::PeripheralShading:values.peripheral=PeripheralShading(uint32_t(value));break;
        case OrbitFeature::SolarVisibility:values.solarVisibility=SceneSolarVisibility(uint32_t(value));break;
        default:break;
    }
}

std::string_view GraphicsChoiceLabel(OrbitFeature feature,float value){
    constexpr std::array reflections{"COMPLET","SELECTIF","AMBIANCE"};
    constexpr std::array antialiasing{"NATIF","FXAA LEGER","FXAA FORT","MSAA 4X"};
    constexpr std::array peripheral{"AUTO","1X1"};
    constexpr std::array solar{"ADAPTATIF","EXACT DXR","SANS OMBRES"};
    const auto index=uint32_t(value);
    switch(feature){
        case OrbitFeature::SceneReflections:return reflections[index];
        case OrbitFeature::Antialiasing:return antialiasing[index];
        case OrbitFeature::PeripheralShading:return peripheral[index];
        case OrbitFeature::SolarVisibility:return solar[index];
        default:return {};
    }
}

std::string_view DlssStatusLabel(DlssRenderStatus state){
    switch(state){
        case DlssRenderStatus::Pending:return "EN ATTENTE";
        case DlssRenderStatus::Disabled:return "NATIF";
        case DlssRenderStatus::Unavailable:return "NGX INDISPO";
        case DlssRenderStatus::SceneBlocked:return "NATIF / SCENE";
        case DlssRenderStatus::MsaaBlocked:return "NATIF / MSAA";
        case DlssRenderStatus::Active:return "DLSS ACTIF";
        case DlssRenderStatus::EvaluationFailed:return "ECHEC DLSS";
    }
    return {};
}

XMUINT4 PackOrbitText(std::string_view text){
    std::array<uint32_t,4> bits{};
    for(size_t i=0;i<std::min(text.size(),size_t(16));++i)
        bits[i/4]|=uint32_t(static_cast<unsigned char>(text[i]))<<(8*(i%4));
    return {bits[0],bits[1],bits[2],bits[3]};
}

bool KnownFeature(OrbitFeature feature){
    return feature!=OrbitFeature::None&&feature<OrbitFeature::Count;
}

const Setting& Definition(OrbitFeature feature){
    return Settings[uint32_t(feature)-1u];
}

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
    if(path_.empty())path_=fs::path(runtimeConfig).parent_path()/".oasis-local"/"orbit.ini";
    std::ifstream file(path_);
    for(std::string line;std::getline(file,line);){
        std::istringstream entry(line);
        entry.imbue(std::locale::classic());
        std::string key;
        float value{};
        if(!std::getline(entry,key,'=')||!(entry>>value)||!std::isfinite(value))continue;
        entry>>std::ws;
        if(!entry.eof())continue;
        for(size_t i=0;i<Settings.size();++i){
            const auto& setting=Settings[i];
            if(key==setting.key&&value>=setting.minimum&&value<=setting.maximum&&
               (i!=uint32_t(OrbitFeature::DlssScale)-1u||value>0.f)&&
               (!IsGraphicsChoice(OrbitFeature(i+1u))||std::trunc(value)==value))
                values_[i]=value;
        }
    }
}

std::optional<float> OrbitProfile::Get(OrbitFeature feature)const{
    return KnownFeature(feature)?values_[uint32_t(feature)-1u]:std::nullopt;
}

std::optional<float> OrbitProfile::Effective(OrbitFeature feature)const{
    if(!KnownFeature(feature))return std::nullopt;
    const auto live=live_[uint32_t(feature)-1u];
    return live?live:Get(feature);
}

void OrbitProfile::Remember(OrbitFeature feature,float value){
    if(KnownFeature(feature))live_[uint32_t(feature)-1u]=value;
}

bool OrbitProfile::Set(OrbitFeature feature,std::optional<float> value){
    if(!KnownFeature(feature))return false;
    auto& slot=values_[uint32_t(feature)-1u];
    if(slot==value)return true;
    slot=value;
    dirty_=true;
    return true;
}

bool OrbitProfile::Flush(){
    if(!dirty_)return true;
    if(!WriteLocalProfile(path_,[&](std::ostream& out){
        out.imbue(std::locale::classic());
        out<<std::setprecision(std::numeric_limits<float>::max_digits10);
        for(size_t i=0;i<values_.size();++i)
            if(values_[i])out<<Settings[i].key<<'='<<*values_[i]<<'\n';
    }))return false;
    dirty_=false;
    return true;
}

void OrbitProfile::RestoreAtmosphere(AtmosphereSettings& atmosphere)const{
    if(atmosphere.radius<=0.f)return;
    if(const auto density=Effective(OrbitFeature::FogDensity))atmosphere.density=*density;
    if(!atmosphere.membrane)return;
    for(uint32_t i=0;i<Settings.size();++i){
        const auto member=Settings[i].member;
        if(member)if(const auto value=Effective(OrbitFeature(i+1u)))atmosphere.film.*member=*value;
    }
}

void OrbitProfile::RestoreRendering(Graphics& graphics)const{
    const auto defaults=graphics.GetRenderingOptions();
    auto options=defaults;
    for(uint32_t i=uint32_t(OrbitFeature::SceneReflections);i<uint32_t(OrbitFeature::Count);++i){
        const auto feature=OrbitFeature(i);
        if(const auto value=Effective(feature))ChangeGraphicsChoice(feature,*value,options);
    }
    if(!graphics.SetRenderingOptions(options)){
        options.antialiasing=defaults.antialiasing;
        graphics.SetRenderingOptions(options);
    }
}

void OrbitTool::Restore(Graphics& graphics,AtmosphereSettings& atmosphere){
    profile_.RestoreAtmosphere(atmosphere);
    graphics.SetAtmosphere(atmosphere);
    factoryAntialiasing_=uint32_t(graphics.GetRenderingOptions().antialiasing);
    profile_.RestoreRendering(graphics);
    if(logo_)pendingDlss_=profile_.Effective(OrbitFeature::DlssScale);
    Refresh(graphics.GetDlssScaleControl(),graphics.GetRenderingOptions(),graphics.GetRenderingState(),atmosphere);
}

void OrbitTool::UpdatePointer(const PoseActionState& aim,FXMMATRIX model){
    const auto previous=display_.hover;
    display_.hover=OrbitControl::None;
    display_.pointer.reset();
    const auto inverse=XMMatrixInverse(nullptr,model);
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
    if(display_.locked==OrbitControl::Color&&stick.y<0.f&&!building_)return;
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
    const auto left=VrGlovePose(input.PoseState(actions.handPose,0),0);
    const auto& right=input.PoseState(actions.aimPose,Right);
    const auto& turn=input.State(actions.turn);
    const auto stick=turn.vector[Right];
    const bool neutral=turn.active[Right]&&std::hypot(stick.x,stick.y)<=Neutral;
    const bool canPoint=input.focused&&right.tracked&&rightTool_==RightHandTool::Empty&&left;
    const auto grab=actions.grab;
    if(canPoint)UpdatePointer(right,PhosphorOrbitModel(*left));
    else{
        display_.hover=OrbitControl::None;
        display_.pointer.reset();
    }
    if(!canPoint||!input.Down(grab,Right))Drop();
    if(canPoint&&input.Pressed(grab,Right)&&display_.hover!=OrbitControl::None){
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

void OrbitTool::Refresh(const DlssScaleControl& dlss,const RenderingOptions& options,
                        const RenderingState& rendered,const AtmosphereSettings& atmosphere){
    if(dlss.minimum>0.f&&dlss.minimum<=dlss.maximum){
        dlssMinimum_=dlss.minimum;
        dlssMaximum_=dlss.maximum;
    }
    display_.features=0u;
    std::array<OrbitFeature,OrbitFeatureCount> active{};
    uint32_t count=0,selected=0;
    for(uint32_t i=1;i<uint32_t(OrbitFeature::Count);++i){
        const auto feature=OrbitFeature(i);
        const bool enabled=feature==OrbitFeature::FogDensity?atmosphere.radius>0.f:
            feature==OrbitFeature::DlssScale?logo_:IsGraphicsChoice(feature)||
            (atmosphere.membrane&&atmosphere.radius>0.f);
        if(!enabled)continue;
        if(feature==display_.selected)selected=count;
        active[count++]=feature;
        display_.features|=OrbitFeatureBit(feature);
    }
    display_.selected=active[selected];
    display_.rows=0u;
    const uint32_t first=std::min(selected,count>OrbitVisibleRows?count-OrbitVisibleRows:0u);
    for(uint32_t i=0;i<OrbitVisibleRows&&first+i<count;++i)
        display_.rows|=uint32_t(active[first+i])<<(4u*i);

    display_.rangeKnown=false;
    display_.userMark=-1.f;
    display_.setTravel=0.f;
    display_.graphicsText={};
    if(display_.selected==OrbitFeature::None)return;
    const auto feature=display_.selected;
    const auto& setting=Definition(feature);
    const bool scaling=feature==OrbitFeature::DlssScale;
    const float minimum=scaling?dlssMinimum_:setting.minimum;
    const float maximum=setting.maximum;
    const bool discrete=IsGraphicsChoice(feature);
    const float factory=setting.member?FactoryFilm.*setting.member:scaling?1.f:
        discrete?(feature==OrbitFeature::Antialiasing?float(factoryAntialiasing_):0.f):factoryFog_;
    const float live=setting.member?atmosphere.film.*setting.member:
        scaling?pendingDlss_.value_or(dlss.value):discrete?GraphicsChoice(feature,options):atmosphere.density;
    if(discrete){
        auto applied=GraphicsChoiceLabel(feature,GraphicsChoice(feature,rendered.applied));
        if(rendered.dlss==DlssRenderStatus::Pending)applied="EN ATTENTE";
        else if(feature==OrbitFeature::Antialiasing&&rendered.msaa4AllocationFailed)
            applied="MSAA MEMOIRE";
        display_.graphicsText[0]=PackOrbitText(GraphicsChoiceLabel(feature,live));
        display_.graphicsText[1]=PackOrbitText(applied);
    }else if(scaling){
        display_.graphicsText[0]=PackOrbitText(std::to_string(std::lround(100.f*live))+"%");
        auto status=DlssStatusLabel(rendered.dlss);
        display_.graphicsText[1]=PackOrbitText(status);
        const auto dimensions=[](uint32_t width,uint32_t height){
            return width&&height?std::to_string(width)+"X"+std::to_string(height):std::string("-");
        };
        display_.graphicsText[2]=PackOrbitText(dimensions(rendered.width,rendered.height));
        display_.graphicsText[3]=PackOrbitText(dimensions(rendered.outputWidth,rendered.outputHeight));
    }
    const auto user=UserValue(feature);
    auto normalize=[&](float v){return std::clamp((v-minimum)/(maximum-minimum),0.f,1.f);};
    const bool percent=scaling||feature==OrbitFeature::Opacity||feature==OrbitFeature::Glitter||
        feature==OrbitFeature::WaveStrength;
    display_.value=percent?100.f*live:live;
    display_.rangeKnown=!scaling||minimum>0.f;
    display_.liveMark=normalize(live);
    display_.factoryMark=normalize(factory);
    if(user){
        display_.userMark=normalize(*user);
        if(std::abs(live-*user)<=SameStep)display_.setTravel=.17f;
    }
}

void OrbitTool::SetBuilding(BuildingSession* building){
    if(building_==building)return;
    building_=building;
    joint_.reset();shown_={};shownRevision_=0;
}
void OrbitTool::ClearBuildingContext(){
    joint_.reset();shown_={};shownRevision_=0;
    display_.jointPreview=false;
    if(display_.buildingPage||display_.plasterPage)steps_=0;
    display_.buildingText={PackOrbitText("HORS EDIFICE")};
}
void OrbitTool::BuildingContext(const FrameInput& input){
    if(!building_)return;
    using namespace Buildings;
    const auto target=building_->Locate(input.head.p);
    const auto revision=building_->Plan().revision;
    const auto scope=Scope(scope_);
    const bool same=target.inside==shown_.inside&&revision==shownRevision_&&
        (scope==Scope::Building||target.level==shown_.level)&&
        (scope!=Scope::Room||target.room==shown_.room);
    if(steps_&&display_.locked==OrbitControl::Color&&steps_>0){
        const bool wasBuilding=display_.buildingPage;
        const bool wasPlaster=display_.plasterPage;
        display_.buildingPage=!wasBuilding&&!wasPlaster;
        display_.plasterPage=wasBuilding;
        joint_.reset();
        steps_=0;
    }else if(display_.plasterPage){
        if(jointRevision_!=revision||input.insideSphere!=0u)joint_.reset();
        jointRevision_=revision;
        const auto& aim=input.PoseState(StandardVrActions().aimPose,1);
        if(display_.locked==OrbitControl::None&&display_.hover==OrbitControl::None&&
           aim.tracked&&input.focused&&rightTool_==RightHandTool::Empty&&input.insideSphere==0u)
            if(const auto* hit=building_->Inspect(aim.pose))joint_=*hit;
        if(steps_&&display_.locked==OrbitControl::Adjust)
            patchQuality_=(patchQuality_-1+3+(steps_>0?1:-1))%3+1;
        if(steps_&&joint_&&display_.locked==OrbitControl::Set)
            building_->Repair(joint_->id,joint_->kind,PatchQuality(patchQuality_),jointRevision_);
        steps_=0;
        auto& text=display_.buildingText;
        text[0]=PackOrbitText("PLATRIER");
        text[1]=PackOrbitText(joint_?(joint_->kind==JointKind::PartitionFacade?"JOINT OUVERT":"RECOUVREMENT"):"VISER UN JOINT");
        text[2]=PackOrbitText(joint_?(joint_->approved?"JOINT CANNE":std::to_string(building_->JointCount(joint_->kind))+" JOINTS LIBRES"):"");
        constexpr std::array quality{"TRIANGLES MIN","COMPROMIS 80-20","REALISME MAX"};
        text[3]=PackOrbitText(quality[patchQuality_-1]);
        text[4]=PackOrbitText(joint_&&!joint_->approved?"S : VALIDER":"");
        text[5]=PackOrbitText(building_->Status());
        display_.jointPreview=joint_.has_value();
        if(joint_){
            std::array<XMFLOAT2,16> projected;
            float left=0.f,right=0.f,bottom=0.f,top=0.f;
            for(unsigned box=0;box<2;++box)for(unsigned i=0;i<8;++i){
                const auto& b=joint_->sources[box];
                const float x=((i&1u)?b.hi.x:b.lo.x)-joint_->seam.lo.x;
                const float y=((i&2u)?b.hi.y:b.lo.y)-joint_->seam.lo.y;
                const float z=((i&4u)?b.hi.z:b.lo.z)-joint_->seam.lo.z;
                const XMFLOAT2 p{.76f*x-.66f*z,-.83f*y+.32f*(x+z)};
                projected[box*8+i]=p;
                left=std::min(left,p.x);right=std::max(right,p.x);
                bottom=std::min(bottom,p.y);top=std::max(top,p.y);
            }
            const float scale=std::min(186.f/std::max(.01f,right-left),217.f/std::max(.01f,top-bottom));
            auto place=[&](XMFLOAT2 p){return XMFLOAT2{484.f+(p.x-(left+right)*.5f)*scale,
                147.f+(p.y-(bottom+top)*.5f)*scale};};
            for(unsigned i=0;i<8;++i){
                const auto a=place(projected[i]),b=place(projected[8+i]);
                display_.jointWire[i]={a.x,a.y,b.x,b.y};
            }
        }
        return;
    }else if(steps_&&display_.buildingPage){
        const int direction=steps_>0?1:-1;
        if(display_.locked==OrbitControl::Select)scope_=(scope_+3+direction)%3;
        if(display_.locked==OrbitControl::Adjust)operation_=(operation_+5+direction)%5;
        if(display_.locked==OrbitControl::Set){
            if(operation_==3)variantStep_=std::clamp(variantStep_+direction,1,99);
            else if(same&&target.inside){
                if(operation_==4)building_->FixRoom(target);
                else building_->Request({target,scope,Action(operation_),variantStep_,revision});
            }
        }
        if(display_.locked!=OrbitControl::Color)steps_=0;
    }
    shown_=target;shownRevision_=revision;
    auto& text=display_.buildingText;
    text[0]=PackOrbitText(target.inside?std::string("OASIS ")+building_->Definition().letter:"HORS EDIFICE");
    text[1]=PackOrbitText(target.inside?(target.level<0?"ETAGE B1":"ETAGE "+std::to_string(target.level)):"ETAGE -");
    const auto room=target.room&&target.room<BathroomTarget?
        " "+std::to_string(target.room&0xffffu):std::string{};
    text[2]=PackOrbitText(std::string(building_->RoomLabel(target))+room);
    constexpr std::array scopes{"SALLE","ETAGE","EDIFICE"};
    constexpr std::array operations{"REGENERER","CANNER","DECANNER","VARIANTE","FIX-ROOM"};
    text[3]=PackOrbitText(std::string(scopes[scope_])+
        (building_->Plan().Locked(Scope(scope_),target)?" / BLOQUE":""));
    text[4]=PackOrbitText(std::string(operations[operation_])+
        ((operation_==0||operation_==3)?" "+std::to_string(variantStep_):""));
    text[5]=PackOrbitText(building_->Status().empty()?
        (operation_==4?"S : CORRIGER SALLE":"S POUR APPLIQUER"):building_->Status());
}

bool OrbitTool::Apply(Graphics& graphics,AtmosphereSettings& atmosphere){
    const auto rendered=graphics.GetRenderingState();
    if(pendingDlss_&&rendered.canAdjustDlss){
        graphics.SetDlssScale(*pendingDlss_==1.f?0.f:*pendingDlss_);
        pendingDlss_.reset();
    }
    const auto dlss=graphics.GetDlssScaleControl();
    auto options=graphics.GetRenderingOptions();
    bool saved=true;
    if(steps_){
        const auto feature=display_.selected;
        const bool scaling=feature==OrbitFeature::DlssScale;
        switch(display_.locked){
        case OrbitControl::Select:{
            const int direction=steps_>0?1:-1;
            int index=int(feature)-1;
            for(int i=0;display_.features&&i<std::abs(steps_);++i){
                do{index=(index+direction+int(OrbitFeatureCount))%int(OrbitFeatureCount);}
                while(!(display_.features&OrbitFeatureBit(OrbitFeature(index+1))));
            }
            if(display_.features)display_.selected=OrbitFeature(index+1);
            break;
        }
        case OrbitControl::Adjust:{
            if(feature==OrbitFeature::None)break;
            display_.lastTouched=feature;
            const int direction=steps_>0?1:-1;
            // Hold a crossed marker for the next detent in that direction.
            // Reversing or grabbing again moves away immediately.
            const int count=std::abs(steps_)-(snapDirection_==direction?1:0);
            snapDirection_=0;
            if(!count)break;
            if(IsGraphicsChoice(feature)){
                const auto& setting=Definition(feature);
                const float maximum=feature==OrbitFeature::Antialiasing&&!rendered.msaa4Supported?
                    float(SceneAntialiasing::FxaaStrong):setting.maximum;
                const float next=std::clamp(GraphicsChoice(feature,options)+float(direction*count),
                                            setting.minimum,maximum);
                ChangeGraphicsChoice(feature,next,options);
                graphics.SetRenderingOptions(options);
            }else if(!scaling){
                const auto& setting=Definition(feature);
                auto& live=setting.member?atmosphere.film.*setting.member:atmosphere.density;
                const float factory=setting.member?FactoryFilm.*setting.member:factoryFog_;
                const float grid=std::round((live-setting.minimum)/setting.step)+float(direction*count);
                const float target=std::clamp(setting.minimum+grid*setting.step,setting.minimum,setting.maximum);
                float next=target;
                if(Crossed(live,target,factory))next=factory;
                if(const auto user=UserValue(feature);user&&Crossed(live,target,*user)&&
                   std::abs(*user-live)<=std::abs(next-live))
                    next=*user;
                if(next!=target)snapDirection_=direction;
                if(next!=live){
                    live=next;
                    profile_.Remember(feature,live);
                    graphics.SetAtmosphere(atmosphere);
                }
            }else if(rendered.canAdjustDlss){
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
        case OrbitControl::Set:{
            if(feature==OrbitFeature::None)break;
            display_.lastTouched=feature;
            const auto member=Definition(feature).member;
            const float live=member?atmosphere.film.*member:scaling?pendingDlss_.value_or(dlss.value):
                IsGraphicsChoice(feature)?GraphicsChoice(feature,options):atmosphere.density;
            profile_.Remember(feature,live);
            saved=profile_.Set(feature,steps_>0?std::optional{live}:std::nullopt);
            break;
        }
        default:break;
        }
        steps_=0;
    }
    const auto updated=graphics.GetDlssScaleControl();
    if(rendered.canAdjustDlss&&!pendingDlss_&&updated.available)
        profile_.Remember(OrbitFeature::DlssScale,updated.value);
    const auto requested=graphics.GetRenderingOptions();
    for(uint32_t i=uint32_t(OrbitFeature::SceneReflections);i<uint32_t(OrbitFeature::Count);++i){
        const auto feature=OrbitFeature(i);
        profile_.Remember(feature,GraphicsChoice(feature,requested));
    }
    Refresh(updated,requested,rendered,atmosphere);
    return saved;
}
}
