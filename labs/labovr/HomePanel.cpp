#include "HomeWorld.h"
#include "LabInputMapping.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
namespace lab {
namespace {
LabU64 ControlId(LabU64 node,uint32_t action,uint32_t index,float sign){
    LabU64 h=14695981039346656037ull;const LabU64 words[]{node,action,index,sign<0?1ull:2ull};
    for(auto w:words)for(uint32_t b=0;b<8;b++){h^=(w>>(b*8))&255;h*=1099511628211ull;}return h?h:1;
}
LabVec3V1 Vec(XMFLOAT3 v){return {v.x,v.y,v.z};}
}
void HomeWorld::BuildPanel(){
    panelNodes_=physics_.EditorNodes();panelElements_.clear();panelControls_.clear();
    auto selected=std::find_if(panelNodes_.begin(),panelNodes_.end(),[&](const auto& n){return n.id==selectedNode_;});
    if(selected==panelNodes_.end()){selectedNode_=13;selected=std::find_if(panelNodes_.begin(),panelNodes_.end(),[](const auto& n){return n.id==13;});inspectorTab_=2;}
    uint32_t caps=selected->capabilities;
    if(inspectorTab_>2||!(caps&(1u<<inspectorTab_))){inspectorTab_=0;while(inspectorTab_<3&&!(caps&(1u<<inspectorTab_)))++inspectorTab_;}
    for(auto& n:panelNodes_)n.flags=(std::find(expanded_.begin(),expanded_.end(),n.id)!=expanded_.end()?LAB_TERMINAL_NODE_OPEN_V2:0)|(n.id==selectedNode_?LAB_TERMINAL_NODE_SELECTED_V2:0);
    auto element=[&](LabU64 id,LabU64 node,uint32_t kind,const char* text,float x,float y,float hw,float hh,uint32_t flags=0,float alpha=.035f){
        LabTerminalElementV2 e{};e.id=id;e.nodeId=node;e.kind=kind;e.flags=flags|(id==panelHover_?LAB_TERMINAL_HOVERED_V2:0);e.x=x;e.y=y;e.halfWidth=hw;e.halfHeight=hh;
        snprintf(e.text,sizeof(e.text),"%s",text);for(auto& ch:e.text)if(uint8_t(ch)>127)ch='?';e.pixelMeters=std::min(.010f,hw*2/std::max(6.f,float(std::strlen(e.text))*6.f));e.backgroundAlpha=alpha;panelElements_.push_back(e);
    };
    auto control=[&](LabU64 node,PanelAction action,uint32_t index,float sign,const char* text,float x,float y,float hw,float hh=.042f,uint32_t extra=0){
        LabU64 id=ControlId(node,uint32_t(action),index,sign);panelControls_.push_back({id,node,action,index,sign});element(id,node,action==PanelAction::Tab?LAB_TERMINAL_TAB_V2:action==PanelAction::Expand?LAB_TERMINAL_BRANCH_V2:LAB_TERMINAL_CONTROL_V2,text,x,y,hw,hh,LAB_TERMINAL_INTERACTIVE_V2|extra,.05f);
    };
    auto label=[&](uint32_t i,const char* text,float x,float y,float hw=.96f){element(0x100000000ull+i,0,LAB_TERMINAL_LABEL_V2,text,x,y,hw,.041f,0,.015f);};
    element(0x1000000010ull,0,LAB_TERMINAL_PANEL_V2,"",-2.55f,-.04f,1.05f,1.28f,0,.025f);
    element(0x1000000011ull,0,LAB_TERMINAL_PANEL_V2,"",-.02f,-.04f,1.37f,1.28f,0,.025f);
    element(0x1000000012ull,0,LAB_TERMINAL_PANEL_V2,"",2.55f,-.04f,1.05f,1.28f,0,.025f);
    control(0,PanelAction::Freeze,0,1,physics_.Frozen()?"FROZEN / UNFREEZE":"RUNNING / FREEZE",-2.75f,1.55f,.85f,.06f,physics_.Frozen()?LAB_TERMINAL_SELECTED_V2:0);
    control(0,PanelAction::Theme,0,1,physics_.PanelTheme()?"AMBRE / VERT":"VERT / AMBRE",-.9f,1.55f,.85f,.06f,physics_.PanelTheme()?LAB_TERMINAL_SELECTED_V2:0);
    control(0,PanelAction::Face,0,1,physics_.PanelFace()?"FACE EXTERIEURE / INTERIEURE":"FACE INTERIEURE / EXTERIEURE",.95f,1.55f,.85f,.06f,LAB_TERMINAL_BOTH_FACES_V2|(physics_.PanelFace()?LAB_TERMINAL_SELECTED_V2:0));
    label(1,"CONSOLEVR HOME",2.75f,1.55f,.85f);
    label(2,"ARBRE",-2.75f,1.36f,.75f);control(0,PanelAction::TreeScroll,0,-1,"<",-1.95f,1.36f,.09f);control(0,PanelAction::TreeScroll,0,1,">",-1.7f,1.36f,.09f);
    label(3,"INSPECTEUR",0,1.36f,1.32f);label(4,"TELEMETRY",2.55f,1.36f,1.0f);
    std::vector<std::pair<size_t,uint32_t>> rows;
    std::function<void(LabU64,uint32_t)> visit=[&](LabU64 parent,uint32_t depth){for(size_t i=0;i<panelNodes_.size();i++){const auto& n=panelNodes_[i];if(n.parentId!=parent)continue;rows.emplace_back(i,depth);if(n.flags&LAB_TERMINAL_NODE_OPEN_V2)visit(n.id,depth+1);}};visit(0,0);
    constexpr uint32_t rowCapacity=24;treeScroll_=std::min(treeScroll_,rows.size()>rowCapacity?uint32_t(rows.size())-rowCapacity:0);
    for(uint32_t row=0;row<rowCapacity&&row+treeScroll_<rows.size();row++){
        auto [i,depth]=rows[row+treeScroll_];const auto& n=panelNodes_[i];float y=1.14f-row*.101f,indent=.10f*std::min(depth,5u);
        bool children=std::any_of(panelNodes_.begin(),panelNodes_.end(),[&](const auto& ch){return ch.parentId==n.id;});
        if(children)control(n.id,PanelAction::Expand,0,1,(n.flags&LAB_TERMINAL_NODE_OPEN_V2)?"-":"+",-3.46f+indent,y,.055f);
        control(n.id,PanelAction::Select,0,1,n.label,-2.48f+indent*.5f,y,.85f-indent*.5f,.042f,n.id==selectedNode_?LAB_TERMINAL_SELECTED_V2:0);
    }
    label(5,selected->label,0,1.15f,1.31f);
    const char* tabs[]{"TRANSFORM","ACTION","PARAMETERS"};uint32_t tabCount=0;for(uint32_t i=0;i<3;i++)if(caps&(1u<<i))++tabCount;
    uint32_t at=0;for(uint32_t i=0;i<3;i++)if(caps&(1u<<i)){float width=2.60f/tabCount;control(selectedNode_,PanelAction::Tab,i,1,tabs[i],-1.30f+width*(at+++.5f),.99f,width*.47f,.045f,i==inspectorTab_?LAB_TERMINAL_SELECTED_V2:0);}
    float y=.81f;uint32_t info=100;
    auto text=[&](const char* value){label(info++,value,0,y,1.30f);y-=.12f;};
    auto button=[&](PanelAction action,uint32_t index,const char* name){
        bool on=false;
        switch(action){
        case PanelAction::Freeze:on=physics_.Frozen();break;
        case PanelAction::Fan:on=index==0&&physics_.FanEnabled();break;
        case PanelAction::FanCCD:on=physics_.FanCCD();break;
        case PanelAction::Lamp:on=physics_.LampEnabled();break;
        case PanelAction::Fog:on=index==0&&physics_.Fog().enabled;break;
        case PanelAction::Optical:on=index==0&&physics_.Optical().enabled;break;
        case PanelAction::Transport:on=index==0&&physics_.Transport().enabled;break;
        case PanelAction::CaptureInterface:on=physics_.CaptureInterface()!=0;break;
        case PanelAction::Antialiasing:on=index==physics_.Antialiasing();break;
        case PanelAction::Multisampling:on=index==physics_.Multisampling();break;
        case PanelAction::PopulationSpace:on=index==0&&physics_.PopulationSpace().enabled;break;
        case PanelAction::Theme:on=physics_.PanelTheme()!=0;break;
        case PanelAction::Face:on=physics_.PanelFace()!=0;break;
        default:break;
        }
        control(selectedNode_,action,index,1,name,0,y,1.30f,.042f,on?LAB_TERMINAL_SELECTED_V2:0);y-=.12f;
    };
    auto parameter=[&](PanelAction action,uint32_t index,const char* name,float value){char valueText[96];snprintf(valueText,sizeof(valueText),"%s %.3f",name,value);label(info++,valueText,0,y,1.04f);control(selectedNode_,action,index,-1,"-",-1.20f,y,.09f);control(selectedNode_,action,index,1,"+",1.20f,y,.09f);y-=.12f;};
    const auto& f=physics_.Flight();const auto& e=physics_.Effects();const auto& fog=physics_.Fog();const auto& o=physics_.Optical();const auto& stats=physics_.Population();char b[96];
    if(inspectorTab_==0){
        auto pos=physics_.SelectionPosition();snprintf(b,sizeof(b),"X %.3f Y %.3f Z %.3f M",pos.x,pos.y,pos.z);text(b);
        text(physics_.Frozen()?"TRANSLATION WORLD / HOLD":"FREEZE TO TRANSLATE");
        for(uint32_t axis=0;axis<3;axis++){char minus[16],plus[16];snprintf(minus,sizeof(minus),"-%c",'X'+axis);snprintf(plus,sizeof(plus),"+%c",'X'+axis);
            uint32_t disabled=physics_.Frozen()?0:LAB_TERMINAL_DISABLED_V2;
            control(selectedNode_,PanelAction::Translate,axis,-1,minus,-.66f,y,.63f,.045f,disabled);control(selectedNode_,PanelAction::Translate,axis,1,plus,.66f,y,.63f,.045f,disabled);y-=.14f;}
        const char* speeds[]{"FINE .01 M/S","NORMAL .15 M/S","FAST .75 M/S"};button(PanelAction::Speed,0,speeds[physics_.EditSpeed()]);
        if(selectedNode_==25)parameter(PanelAction::Rotate,0,"YAW STEP / DEGREES",15);
        if(selectedNode_==3)parameter(PanelAction::FanYaw,0,"FAN YAW STEP / DEGREES",5);
        button(PanelAction::LocalReset,0,"RESET TO LAST VALID");text("INVALID LAYOUT BLOCKS RESUME / SAVE");
    }else if(inspectorTab_==1){
        if(selectedNode_==28){button(PanelAction::Capture,0,"CAPTURE PNG");button(PanelAction::CaptureEye,0,physics_.CaptureEye()?"EYE RIGHT / LEFT":"EYE LEFT / RIGHT");button(PanelAction::CaptureInterface,0,physics_.CaptureInterface()?"INTERFACE INCLUDED / HIDE":"INTERFACE HIDDEN / INCLUDE");text("LEFT X OR Y / PRESS ONCE");const auto& capture=physics_.CaptureStatus();text(capture.message);text("DESTINATION");std::string dir=capture.directoryUtf8;for(size_t i=0;i<dir.size();i+=42)text(dir.substr(i,42).c_str());}
        if(selectedNode_==3||selectedNode_==4)button(PanelAction::Fan,0,physics_.FanEnabled()?"FAN ON / OFF":"FAN OFF / ON");
        if(selectedNode_==3)button(PanelAction::FanCCD,0,physics_.FanCCD()?"SPECULATIVE CCD ON / OFF":"SPECULATIVE CCD OFF / ON");
        if(selectedNode_==8||selectedNode_==11)button(PanelAction::Lamp,0,physics_.LampEnabled()?"LAMP ON / OFF":"LAMP OFF / ON");
        if(selectedNode_==13||selectedNode_==14){button(PanelAction::Population,0,"+500");button(PanelAction::Population,1,"+5000");button(PanelAction::Population,2,"-500");button(PanelAction::Population,3,"-5000");}
        if(selectedNode_==13||selectedNode_==15)button(PanelAction::Flight,99,"RESET FLIGHT");
        if(selectedNode_==19)button(PanelAction::Optical,0,o.enabled?"OPTICAL SWARM ON / OFF":"OPTICAL SWARM OFF / ON");
        if(selectedNode_==21||selectedNode_==22)button(PanelAction::Fog,0,fog.enabled?"FOG ON / OFF":"FOG OFF / ON");
        if(selectedNode_==25){button(PanelAction::Theme,0,physics_.PanelTheme()?"AMBRE / VERT":"VERT / AMBRE");button(PanelAction::Face,0,physics_.PanelFace()?"EXTERIEURE / INTERIEURE":"INTERIEURE / EXTERIEURE");}
    }else if(inspectorTab_==2){
        if(selectedNode_==37||selectedNode_==38){
            text("FXAA / LIVE / BOTH EYES");button(PanelAction::Antialiasing,0,"FXAA OFF");button(PanelAction::Antialiasing,1,"FXAA LOW");button(PanelAction::Antialiasing,2,"FXAA QUALITY");
            text("SCENE MULTISAMPLING / LIVE");button(PanelAction::Multisampling,1,"MSAA OFF / 1X");button(PanelAction::Multisampling,4,"MSAA 4X");
        }
        if(selectedNode_==14){const auto& v=physics_.PopulationSpace();button(PanelAction::PopulationSpace,0,"RETURN TO FLIGHT VOLUME");
            const char* names[]{"CENTER X M","CENTER Y M","CENTER Z M","HALF WIDTH M","HALF HEIGHT M","HALF DEPTH M","RETURN STRENGTH"};
            const float values[]{v.center.x,v.center.y,v.center.z,v.halfExtent.x,v.halfExtent.y,v.halfExtent.z,v.returnStrength};for(uint32_t i=0;i<7;i++)parameter(PanelAction::PopulationSpace,i+1,names[i],values[i]);
        }
        if(selectedNode_>=32&&selectedNode_<=36){const auto& s=physics_.Surface(uint32_t(selectedNode_-32));const char* names[]{"RED / LINEAR","GREEN / LINEAR","BLUE / LINEAR","ROUGHNESS","MICRO NORMAL","FREQUENCY / M","JUNCTION WIDTH UV","JUNCTION CONTRAST"};const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};for(uint32_t i=0;i<8;i++)parameter(PanelAction::Surface,i,names[i],v[i]);text("PBR / SHADOWS / FOG / REFLECTIONS");}
        if(selectedNode_==20){parameter(PanelAction::Size,0,"DIAMETER MM",physics_.DiameterMm());text("STEP .05 MM / HOLD .5 MM PER SECOND");text("COLLISION / PROXY / DETAIL / SHADOW");text("AREA / COMPENSATED MASS SCALE TOGETHER");}
        if(selectedNode_==30){const char* names[]{"TRIGGER TOUCH","PRIMARY TOUCH","SECONDARY TOUCH","STICK TOUCH","THUMBREST TOUCH"};
            for(uint32_t i=0;i<5;i++){label(info++,names[i],0,y,.65f);for(uint32_t h=0;h<2;h++){bool available=(physics_.TouchAvailable(h)&(1u<<i))!=0,chosen=(physics_.TouchMask(h)&(1u<<i))!=0,down=(physics_.TouchDown(h)&(1u<<i))!=0;char source[48];snprintf(source,sizeof(source),"%c %s %s",h?'R':'L',available?chosen?"ON":"OFF":"N/A",down?"TOUCH":"");control(selectedNode_,PanelAction::Touch,i*2+h,1,source,h?.99f:-.99f,y,.29f,.042f,(available?0:LAB_TERMINAL_DISABLED_V2)|(chosen?LAB_TERMINAL_SELECTED_V2:0));}y-=.12f;}
            parameter(PanelAction::HandResponse,0,"POSITION DAMP S",physics_.HandPositionTau());parameter(PanelAction::HandResponse,1,"ROTATION DAMP S",physics_.HandRotationTau());text("NO TOUCH = FREE HAND / GRIP = GRAB");}
        if(selectedNode_==3||selectedNode_==4)parameter(PanelAction::Fan,1,"RPM SETPOINT",physics_.FanRpm());
        if(selectedNode_==3||selectedNode_==6||selectedNode_==24){const auto& t=physics_.Transport();const auto& w=physics_.Wind();button(PanelAction::Transport,0,t.enabled?"ADVECTION ON / OFF":"ADVECTION OFF / ON");const char* names[]{"RPM INFLUENCE","JET RADIUS M","JET RANGE M","DENSITY CONTRAST","STRUCTURES / M","TURBULENCE","RELAXATION / S"};const float v[]{physics_.FanWind(),w.radius,w.length,t.contrast,t.frequency,fog.turbulence,w.dissipation};for(uint32_t i=0;i<7;i++)parameter(PanelAction::Transport,i+1,names[i],v[i]);}
        if(selectedNode_==13||selectedNode_==15){const char* names[]{"SPEED","MAX SPEED","RESPONSE S","TURN / FOURIER","TURN RADIUS","LIGHT ATTRACTION","COMFORT","ANTICIPATION","WANDER / FOURIER","CLIMB / DIVE","FAIL CHANCE"};
            const float v[]{f.nominalSpeed,f.maxSpeed,f.responseSeconds,f.turnIntensity,f.turnRadius,f.lightAttraction,f.comfortDistance,f.anticipationDistance,f.wander,f.vertical,f.avoidanceFailure};for(uint32_t i=0;i<11;i++)parameter(PanelAction::Flight,i,names[i],v[i]);}
        if(selectedNode_==16){parameter(PanelAction::Flight,11,"HIT DURATION S",f.impactSeconds);parameter(PanelAction::Flight,12,"HIT MULTIPLIER",f.impactMultiplier);}
        if(selectedNode_==17){const char* names[]{"SWEEP RADIUS M","IMPULSE","FLEE S","AVOID RADIUS M","RESIDUAL AVERSION"};const float v[]{e.sweepRadius,e.impulse,e.fleeSeconds,e.avoidanceRadius,e.residualAversion};for(uint32_t i=0;i<5;i++)parameter(PanelAction::Effects,i,names[i],v[i]);}
        if(selectedNode_==18){parameter(PanelAction::Effects,5,"SHADOW DENSITY",e.shadowDensity);parameter(PanelAction::Effects,6,"SHADOW POWER",e.shadowIntensity);}
        if(selectedNode_==19){parameter(PanelAction::Optical,1,"QUALITY",float(o.quality));parameter(PanelAction::Optical,2,"STABILIZATION S",o.stabilizationSeconds);parameter(PanelAction::Optical,3,"COVERAGE",o.coverage);parameter(PanelAction::Optical,4,"OPTICAL VIEW",float(o.visualization));text("0 NORMAL / 1 DEPTH / 2 TRANSMISSION");text("Q0 96X96X48 / Q1 192X192X64");text("Q2 256X256X96");}
        if(selectedNode_==21||selectedNode_==22){const char* names[]{"DENSITY","HEIGHT M","FALLOFF","SCATTER","ANISOTROPY","ABSORPTION","TURBULENCE","EVOLUTION","HAND RESPONSE","VOLUME LIGHT"};const float v[]{fog.density,fog.height,fog.falloff,fog.scattering,fog.anisotropy,fog.absorption,fog.turbulence,fog.evolution,fog.handResponse,fog.lightIntensity};for(uint32_t i=0;i<10;i++)parameter(PanelAction::Fog,i+1,names[i],v[i]);parameter(PanelAction::Fog,12,"FOG QUALITY",float(fog.quality));}

        if(selectedNode_==25){text("MONOCHROME / CONTENT ALPHA");text("EMPTY SPACE IS TRANSPARENT");text("NO SOLID COLLISION");text("FACE SWITCH ACCESSIBLE FROM BOTH SIDES");}
    }else{
        if(selectedNode_==7){snprintf(b,sizeof(b),"FAN ACTUAL %.0f RPM",physics_.FanActualRpm());text(b);text("ROTOR COUPLED AUDIO / THREE EMITTERS");text("PITCH / GAIN FOLLOW ACTUAL ROTATION");}
        else if(selectedNode_==20){snprintf(b,sizeof(b),"BODY R %.5f / WING HZ %.1f",f.bodyRadius,f.wingFrequency);text(b);snprintf(b,sizeof(b),"DETAIL %.2f M / NEAR %.2f M",f.detailDistance,f.nearDistance);text(b);snprintf(b,sizeof(b),"MASS %.8f / WING AREA %.8f",f.mass,f.wingArea);text(b);text("GPU MESHES / TRANSLUCENT WINGS");}
        else if(selectedNode_==26){snprintf(b,sizeof(b),"GPU TARGET ID %u",stats.targetId);text(b);snprintf(b,sizeof(b),"STABLE ID %llu",static_cast<unsigned long long>(physics_.TargetIdentity()));text(b);snprintf(b,sizeof(b),"HIT LEFT %.2f S / ACTIVITY %.2f",stats.targetImpactRemaining,stats.targetActivity);text(b);text("LIVE TARGET / COMPACT GPU PROBE");}
    }
    uint32_t telemetry=200;float ty=1.14f;
    auto tele=[&](const char* value){label(telemetry++,value,2.55f,ty,1.0f);ty-=.12f;};
    tele(physics_.Frozen()?"SIMULATION FROZEN":"SIMULATION RUNNING");snprintf(b,sizeof(b),"SIM TIME %.2f S",physics_.SimulationSeconds());tele(b);
    snprintf(b,sizeof(b),"FLIES %u",physics_.PopulationCount());tele(b);snprintf(b,sizeof(b),"NEAR %u / DETAIL %u",stats.nearCount,stats.detailedCount);tele(b);snprintf(b,sizeof(b),"MASS %u / CAPACITY %u",stats.simplifiedCount,stats.capacity);tele(b);
    if(stats.targetId==Invalid)tele("TARGET -");else{snprintf(b,sizeof(b),"TARGET %u",stats.targetId);tele(b);}
    snprintf(b,sizeof(b),"HIT LEFT %.2f S",stats.targetImpactRemaining);tele(b);snprintf(b,sizeof(b),"ACTIVITY %.2f",stats.targetActivity);tele(b);
    snprintf(b,sizeof(b),"FAN SET %.0f / REAL %.0f RPM",physics_.FanRpm(),physics_.FanActualRpm());tele(b);snprintf(b,sizeof(b),"JET %.2f M/S",physics_.Wind().speed);tele(b);
    snprintf(b,sizeof(b),"HANDS L %s / R %s",physics_.HandPhysical(0)?"PHYSICAL":"FREE",physics_.HandPhysical(1)?"PHYSICAL":"FREE");tele(b);
    snprintf(b,sizeof(b),"FOG %s / QUALITY %u",fog.enabled?"ON":"OFF",fog.quality);tele(b);tele("Q0 1/4 16 / Q1 1/4 32 / Q2 1/2 48");
    snprintf(b,sizeof(b),"OPTICAL %s Q%u / VIEW %u",o.enabled?"ON":"OFF",o.quality,o.visualization);tele(b);snprintf(b,sizeof(b),"STABLE %.0f MS / COVERAGE %.2f",o.stabilizationSeconds*1000,o.coverage);tele(b);
    tele(selected->label);if(caps&1){auto p=physics_.SelectionPosition();snprintf(b,sizeof(b),"POS %.3f %.3f %.3f M",p.x,p.y,p.z);tele(b);}tele(physics_.LampEnabled()?"LAMP ON":"LAMP OFF");tele(physics_.CaptureStatus().message);
    const char* profiles[]{"SAVE AS SYSTEM-DEFAULT","SAVE AS USER-DEFAULT","RELOAD SYSTEM-DEFAULT","RELOAD USER-DEFAULT"};
    for(uint32_t i=0;i<4;i++)control(0,PanelAction::Profile,i,1,profiles[i],-2.73f+1.82f*i,-1.47f,.86f,.051f);
    label(6,physics_.Status(),0,-1.64f,3.60f);
}
void HomeWorld::PublishPanel()const{
    if(!terminalGpu_)return;LabTerminalFrameV2 frame{};frame.structSize=sizeof(frame);frame.pose=physics_.PanelReadingPose();frame.theme=physics_.PanelTheme();frame.timeSeconds=float(lastPanelAt_-panelStartSeconds_);
    if(ambientVisible_){frame.nodeCount=uint32_t(panelNodes_.size());frame.nodes=panelNodes_.data();frame.elementCount=uint32_t(panelElements_.size());frame.elements=panelElements_.data();}
    if(!terminalGpu_->PublishTerminalV2(terminalGpu_->serviceContext,&frame))Fail("Home gpu_v1: panel V2 rejected");
}
void HomeWorld::InvokePanel(const PanelControl& c,double dt){
    switch(c.action){
    case PanelAction::Select:selectedNode_=c.node;physics_.SelectNode(c.node);inspectorTab_=0;heldControl_={};break;
    case PanelAction::Expand:{auto it=std::find(expanded_.begin(),expanded_.end(),c.node);if(it==expanded_.end())expanded_.push_back(c.node);else expanded_.erase(it);break;}
    case PanelAction::TreeScroll:treeScroll_=uint32_t(std::max(0,int(treeScroll_)+(c.sign>0?6:-6)));break;
    case PanelAction::Tab:inspectorTab_=c.index;heldControl_={};break;
    case PanelAction::Freeze:physics_.SetFrozen(!physics_.Frozen());heldControl_={};break;
    case PanelAction::Theme:physics_.TogglePanelTheme();break;
    case PanelAction::Face:physics_.TogglePanelFace();heldControl_={};break;
    case PanelAction::Profile:physics_.ProfileCommand(c.index);heldControl_={};break;
    case PanelAction::Translate:physics_.SelectNode(c.node);physics_.TranslateSelection(c.index,c.sign,dt);break;
    case PanelAction::Speed:physics_.CycleEditSpeed();break;
    case PanelAction::Flight:physics_.AdjustFlight(c.index,c.sign);break;
    case PanelAction::Effects:physics_.AdjustEffects(c.index,c.sign);break;
    case PanelAction::Fog:physics_.AdjustFog(c.index,c.sign);break;
    case PanelAction::Fan:physics_.AdjustFan(c.index,c.sign);break;
    case PanelAction::FanCCD:physics_.ToggleFanCCD();heldControl_={};break;
    case PanelAction::Optical:physics_.AdjustOptical(c.index,c.sign);break;
    case PanelAction::Population:{const int deltas[]{500,5000,-500,-5000};physics_.AddPopulation(deltas[c.index]);break;}
    case PanelAction::Lamp:physics_.SetLampEnabled(!physics_.LampEnabled());break;
    case PanelAction::Rotate:physics_.RotatePanel(c.sign);break;
    case PanelAction::LocalReset:physics_.SelectNode(c.node);physics_.ResetSelection();break;
    case PanelAction::Size:physics_.AdjustSize(c.sign,dt);break;
    case PanelAction::Capture:physics_.Capture();break;
    case PanelAction::CaptureEye:physics_.ToggleCaptureEye();break;
    case PanelAction::CaptureInterface:physics_.ToggleCaptureInterface();break;
    case PanelAction::Touch:physics_.ToggleTouch(c.index%2,c.index/2);break;
    case PanelAction::Antialiasing:physics_.SetAntialiasingMode(c.index);break;
    case PanelAction::Multisampling:physics_.SetMultisampling(c.index);break;
    case PanelAction::PopulationSpace:physics_.AdjustPopulationSpace(c.index,c.sign);break;
    case PanelAction::Transport:physics_.AdjustTransport(c.index,c.sign);break;
    case PanelAction::Surface:physics_.AdjustSurface(uint32_t(c.node-32),c.index,c.sign);break;
    case PanelAction::FanYaw:physics_.RotateFan(c.sign);break;
    case PanelAction::HandResponse:physics_.AdjustHandResponse(c.index,c.sign);break;
    default:break;
    }
}
void HomeWorld::UpdatePanel(const FrameInput& input){
    panelHover_=0;panelDistance_={};panelDt_=lastPanelAt_>0?std::clamp(input.time-lastPanelAt_,0.,.04):0;lastPanelAt_=input.time;
    BuildPanel();PublishPanel();
    if(!ambientVisible_||!input.focused){heldControl_={};repeatControl_={};panelTouch_={};return;}
    for(uint32_t hand=0;hand<2;hand++){
        auto grip=physics_.HandState(hand),aim=AimState(input,hand);bool primary=input.Down(StandardVrActions().primary,hand),grab=input.Down(StandardVrActions().grab,hand);
        if(heldControl_[hand]){auto it=std::find_if(panelControls_.begin(),panelControls_.end(),[&](const auto& c){return c.id==heldControl_[hand];});
            if(primary&&!grab&&aim.tracked&&it!=panelControls_.end()&&it->action==PanelAction::Translate&&physics_.Frozen()){auto control=*it;InvokePanel(control,panelDt_);panelHover_=control.id;break;}heldControl_[hand]=0;}
        LabTerminalHitV2 hit{sizeof(hit)};bool touched=false;
        if(grip.tracked){auto finger=Transform({0,0,.055f},grip.pose);LabTerminalHitQueryV2 q{sizeof(q),LAB_TERMINAL_HIT_TOUCH_V2,Vec(finger),{0,0,0},0,.028f};
            if(!terminalGpu_->HitTestTerminalV2(terminalGpu_->serviceContext,&q,&hit))Fail("Home terminal touch rejected");touched=hit.hit!=0;}
        if(!touched&&aim.tracked){auto d=Sub(Transform({0,0,1},aim.pose),aim.pose.p);float len=Length(d);if(len>0){LabTerminalHitQueryV2 q{sizeof(q),LAB_TERMINAL_HIT_RAY_V2,Vec(aim.pose.p),Vec(Scale(d,1/len)),12,0};
            if(!terminalGpu_->HitTestTerminalV2(terminalGpu_->serviceContext,&q,&hit))Fail("Home terminal ray rejected");if(hit.hit)panelDistance_[hand]=hit.distance;}}
        bool touchEdge=touched&&!panelTouch_[hand];panelTouch_[hand]=touched;
        if(!hit.hit){repeatControl_[hand]=0;continue;}panelHover_=hit.elementId;
        auto it=std::find_if(panelControls_.begin(),panelControls_.end(),[&](const auto& c){return c.id==hit.elementId;});if(it==panelControls_.end()||grab){repeatControl_[hand]=0;continue;}auto control=*it;
        if(control.action==PanelAction::Translate&&(touched||primary)&&physics_.Frozen()){
            InvokePanel(control,panelDt_);if(primary&&!touched)heldControl_[hand]=control.id;break;
        }
        if(control.action==PanelAction::Size){
            if(touchEdge||input.Pressed(StandardVrActions().primary,hand)){InvokePanel(control,0);repeatControl_[hand]=control.id;}
            else if((touched||primary)&&repeatControl_[hand]==control.id)InvokePanel(control,panelDt_);else repeatControl_[hand]=0;break;
        }
        bool repeatable=(control.action==PanelAction::Flight&&control.index!=99)||control.action==PanelAction::Effects||(control.action==PanelAction::Fog&&control.index>0)||(control.action==PanelAction::Fan&&control.index>0)||(control.action==PanelAction::Optical&&control.index>0)||control.action==PanelAction::Rotate||control.action==PanelAction::HandResponse||(control.action==PanelAction::Transport&&control.index>0)||control.action==PanelAction::Surface||control.action==PanelAction::FanYaw||(control.action==PanelAction::PopulationSpace&&control.index>0);
        if(touchEdge||input.Pressed(StandardVrActions().primary,hand)){
            InvokePanel(control,panelDt_);if(repeatable){repeatControl_[hand]=control.id;repeatAt_[hand]=input.time+.35;}break;
        }
        if(repeatable&&(touched||primary)&&repeatControl_[hand]==control.id){
            if(input.time>=repeatAt_[hand]){InvokePanel(control,panelDt_);repeatAt_[hand]=input.time+.10;break;}
        }else repeatControl_[hand]=0;
    }
    BuildPanel();
}
void HomeWorld::UpdateCaptureButtons(const FrameInput& input){
    const auto& a=StandardVrActions();
    if(input.focused&&(input.Pressed(a.mode,0)||input.Pressed(a.next,0)))physics_.Capture();
}

}
