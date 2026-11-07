#include "HomeWorld.h"
#include "ClientAssets.h"
#include "ClientVisuals.h"
#include "ContentIds.h"
#include "LabInputMapping.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <cstdio>
#include <cmath>
namespace lab {
void HomeWorld::Load(WorldServices& services){
    if(loaded_)Fail("HomeWorld already loaded");
    scene_=CreateClientAssetSet();
    auto& materials=scene_.materials;
    materials[uint32_t(Material::HandLeft)]={{.05f,.45f,.8f,.44f},{.01f,.02f,.04f,.1f}};
    materials[uint32_t(Material::HandRight)]={{.86f,.27f,.06f,.44f},{.035f,.01f,0,.1f}};
    materials[uint32_t(Material::HomeBlack)]={{.018f,.023f,.032f,.86f},{0,0,0,0}};
    materials[uint32_t(Material::Floor)]={{.020f,.025f,.032f,.24f},{0,0,0,.2f}};
    auto box=[&](XMFLOAT3 p,XMFLOAT3 s){RenderObject o;o.current.p=p;o.previous=o.current;o.scale=s;o.material=uint32_t(Material::HomeBlack);scene_.objects.push_back(o);};
    box({0,-.05f,0},{8,.10f,8});scene_.objects.back().material=uint32_t(Material::Floor);
    box({0,3.80f,0},{8,.10f,8});scene_.objects.back().material=uint32_t(Material::Ceiling);
    box({0,1.875f,-4},{8,3.75f,.10f});scene_.objects.back().material=uint32_t(Material::WallZMinus);
    box({-4,1.875f,0},{.10f,3.75f,8});scene_.objects.back().material=uint32_t(Material::WallXMinus);
    box({4,1.875f,0},{.10f,3.75f,8});scene_.objects.back().material=uint32_t(Material::WallXPlus);
    // Fixed furniture shares exactly the same boxes with the PhysX scene.
    auto furniture=[&](XMFLOAT3 pos,XMFLOAT3 size,Material mat){box(pos,size);scene_.objects.back().material=uint32_t(mat);};
    materials[uint32_t(Material::Wood)]={{.13f,.066f,.026f,.8f},{0,0,0,0}};
    furniture({-.85f,1.08f,1.15f},{1.5f,.08f,.95f},Material::Wood);
    for(float x:{-1.47f,-.23f})for(float z:{.77f,1.53f})furniture({x,.52f,z},{.08f,1.04f,.08f},Material::Wood);
    physics_.Load(services.Require<LabHostApiV1>(),scene_);
    const auto& host=services.Require<LabHostApiV1>();terminalGpu_=static_cast<const LabGpuServiceV1*>(host.QueryService(host.hostContext,LABOVR_SERVICE_GPU_V1,1));
    if(!terminalGpu_||terminalGpu_->structSize<sizeof(*terminalGpu_)||!terminalGpu_->PublishTerminal||!terminalGpu_->PublishTerminalV2||!terminalGpu_->HitTestTerminalV2)Fail("Home requires Core terminal ABI");
    loaded_=true;ambientVisible_=true;ResetMenu();
}
void HomeWorld::Unload(WorldServices&){if(terminalGpu_){LabTerminalFrameV1 empty{};empty.structSize=sizeof(empty);terminalGpu_->PublishTerminal(terminalGpu_->serviceContext,&empty);LabTerminalFrameV2 panel{};panel.structSize=sizeof(panel);panel.pose.orientation.w=1;terminalGpu_->PublishTerminalV2(terminalGpu_->serviceContext,&panel);terminalGpu_=nullptr;}physics_.Unload();scene_={};loaded_=false;}
void HomeWorld::ResetMenu(){selectedNode_=3;inspectorTab_=0;treeScroll_=0;expanded_={37,1,3,21};panelHover_=0;heldControl_={};repeatControl_={};repeatAt_={};panelTouch_={};panelDistance_={};lastPanelAt_=0;worldPage_=visualPage_=false;worldPageIndex_=0;hover_=Invalid;command_={};lastListenerAt_=panelStartSeconds_=0;listenerVelocity_={};lastListener_={};physics_.SelectNode(selectedNode_);BuildPanel();}
void HomeWorld::Reset(WorldServices& services){Unload(services);Load(services);}
const SceneData& HomeWorld::Scene()const{if(!loaded_)Fail("HomeWorld is not loaded");return scene_;}
void HomeWorld::SetWorldNames(std::vector<std::string> names){worlds_=std::move(names);worldPageIndex_=0;}
PoseActionState HomeWorld::AimState(const FrameInput& input,uint32_t hand)const{
    auto aim=input.PoseState(StandardVrActions().aimPose,hand);
    const auto& grip=input.PoseState(StandardVrActions().handPose,hand);auto stabilized=physics_.HandState(hand);
    if(aim.tracked&&grip.tracked&&stabilized.tracked)aim.pose=FromMatrix(Matrix(aim.pose)*XMMatrixInverse(nullptr,Matrix(grip.pose))*Matrix(stabilized.pose));
    return aim;
}
void HomeWorld::UpdateMenu(const FrameInput& input){
    if(!menuVisible_)return;
    constexpr uint32_t pagedWorldsPerPage=HomeMenuMaxLines-3;
    const bool paged=worlds_.size()+1>HomeMenuMaxLines;
    const uint32_t pageCount=paged?uint32_t((worlds_.size()+pagedWorldsPerPage-1)/pagedWorldsPerPage):1u;
    if(worldPageIndex_>=pageCount)worldPageIndex_=pageCount-1;
    const uint32_t start=paged?worldPageIndex_*pagedWorldsPerPage:0u;
    const uint32_t visible=worldPage_?uint32_t((std::min)(worlds_.size()-start,size_t(paged?pagedWorldsPerPage:HomeMenuMaxLines-1))):0u;
    const uint32_t count=visualPage_?2u:worldPage_?visible+(paged?3u:1u):7u;
    hover_=Invalid;

    // Reuse the existing aim pose and primary menu action. No actionset,
    // profile, binding or hand gesture is introduced for the lamp switch.
    for(uint32_t hand=0;hand<2;hand++){
        const auto aim=AimState(input,hand);
        if(!aim.tracked||panelDistance_[hand]>0||panelTouch_[hand])continue;
        XMFLOAT3 direction=Sub(Transform({0,0,1},aim.pose),aim.pose.p);
        if(std::abs(direction.z)<1e-4f)continue;
        float distance=(1.72f-aim.pose.p.z)/direction.z;
        if(distance<=0)continue;
        XMFLOAT3 hit=Add(aim.pose.p,Scale(direction,distance));
        uint32_t hitRow=Invalid;
        for(uint32_t row=0;row<count;row++){
            float y=2.02f-float(row)*.115f;
            if(std::abs(hit.x)<=.40f&&std::abs(hit.y-y)<=.045f){hitRow=row;break;}
        }
        if(hitRow!=Invalid)hover_=hitRow;
        if(hitRow==Invalid||input.Down(StandardVrActions().grab,hand)||!input.Pressed(StandardVrActions().primary,hand))continue;
        if(visualPage_){
            if(hitRow==0)SetLampEnabled(!physics_.LampEnabled());
            else visualPage_=false;
        }else if(worldPage_){
            if(hitRow<visible){command_={HomeCommandKind::LoadWorld,start+hitRow};worldPage_=false;}
            else if(paged&&hitRow==visible){if(worldPageIndex_>0)--worldPageIndex_;}
            else if(paged&&hitRow==visible+1){if(worldPageIndex_+1<pageCount)++worldPageIndex_;}
            else worldPage_=false;
        }else switch(hitRow){
            case 0:command_={HomeCommandKind::NewSession,Invalid};break;
            case 1:worldPage_=true;worldPageIndex_=0;break;
            case 3:visualPage_=true;break;
            case 6:command_={HomeCommandKind::Quit,Invalid};break;
            default:break;
        }
        break;
    }
}
void HomeWorld::Update(const FrameInput& input,double now,std::span<const SceneLight> lights){if(!loaded_)return;auto aim=AimState(input,1);if(!aim.tracked)aim=AimState(input,0);
    physics_.SetPopulationView(input.head.p,aim.pose.p,Sub(Transform({0,0,1},aim.pose),aim.pose.p),aim.tracked);
    physics_.Update(input.frameId,now,lights);if(!panelStartSeconds_)panelStartSeconds_=input.time;UpdateCaptureButtons(input);UpdatePanel(input);UpdateMenu(input);}
HomeCommand HomeWorld::ConsumeCommand()noexcept{auto c=command_;command_={};return c;}
HomeMenuSnapshot HomeWorld::MenuSnapshot()const{
    HomeMenuSnapshot out;

    auto set=[&](uint32_t row,const char* label){auto n=(std::min)(std::strlen(label),size_t(HomeMenuMaxChars-1));std::copy_n(label,n,out.labels[row].data());out.labels[row][n]='\0';};
    if(!menuVisible_){out.count=0;}else if(visualPage_){
        out.count=2;set(0,physics_.LampEnabled()?"Lamp: On":"Lamp: Off");set(1,"Back");
    }else if(worldPage_){
        constexpr uint32_t pagedWorldsPerPage=HomeMenuMaxLines-3;
        const bool paged=worlds_.size()+1>HomeMenuMaxLines;
        const uint32_t start=paged?worldPageIndex_*pagedWorldsPerPage:0u;
        const uint32_t visible=uint32_t((std::min)(worlds_.size()-start,size_t(paged?pagedWorldsPerPage:HomeMenuMaxLines-1)));
        out.count=visible+(paged?3u:1u);
        for(uint32_t i=0;i<visible;i++)set(i,worlds_[start+i].c_str());
        if(paged){set(visible,"Prev");set(visible+1,"Next");set(visible+2,"Back");}else set(visible,"Back");
    }else{
        static constexpr const char* root[]{"New","World","Graphics","Visual","Audio","Controls","Quit"};
        out.count=uint32_t(std::size(root));
        for(uint32_t i=0;i<out.count;i++)set(i,root[i]);
    }
    out.hover=hover_<out.count?hover_:Invalid;
    out.time=float(panelStartSeconds_?lastPanelAt_-panelStartSeconds_:0);
    return out;
}
void HomeWorld::PublishMenu()const{
    if(!terminalGpu_)return;auto snapshot=MenuSnapshot();LabTerminalFrameV1 frame{};frame.structSize=sizeof(frame);frame.timeSeconds=snapshot.time;frame.lineCount=snapshot.count;
    for(uint32_t i=0;i<snapshot.count;i++){
        auto& line=frame.lines[i];line.center={0,2.02f-float(i)*.115f,1.716f};line.halfWidth=.42f;line.halfHeight=.046f;line.curveHalfWidth=.435f;line.hovered=snapshot.hover==i;
        std::copy_n(snapshot.labels[i].data(),32,line.text);for(auto& ch:line.text)if(uint8_t(ch)>127)ch='?';
    }
    if(!terminalGpu_->PublishTerminal(terminalGpu_->serviceContext,&frame))Fail("Home gpu_v1: terminal frame rejected");
    PublishPanel();
}
void HomeWorld::FillAudio(AudioFrame& a,const FrameInput& input,double now)noexcept{
    a={};a.inputFrameId=input.frameId;a.time=input.time>0?input.time:now;a.listener=input.head;
    if(lastListenerAt_>0&&input.time>lastListenerAt_){listenerVelocity_=Scale(Sub(input.head.p,lastListener_.p),1.f/std::max(.001f,float(input.time-lastListenerAt_)));float speed=Length(listenerVelocity_);if(speed>10)listenerVelocity_=Scale(listenerVelocity_,10/speed);}
    lastListener_=input.head;lastListenerAt_=input.time;a.velocity=listenerVelocity_;
}
void HomeWorld::Append(RenderScene& r,const FrameInput& input,double)const{
    if(diagnosticHands_){
        std::array<PoseActionState,2> hands{};
        for(uint32_t hand=0;hand<2;hand++){
            const float side=hand?1.f:-1.f;
            const XMMATRIX local=XMMatrixRotationX(-.12f)*XMMatrixRotationY(-side*.16f)*XMMatrixTranslation(side*.22f,-.20f,.68f);
            hands[hand].pose=FromMatrix(local*Matrix(diagnosticCamera_));hands[hand].tracked=true;
        }
        AppendClientHands(r,input,&hands);
        for(uint32_t hand=0;hand<2;hand++){
            const auto& pose=hands[hand].pose;
            RenderObject beam;beam.current=pose;beam.current.p=Transform({0,0,.65f},pose);beam.previous=beam.current;
            beam.scale={.003f,.003f,1.2f};beam.mesh=uint32_t(Mesh::Box);beam.material=uint32_t(hand?Material::HandRight:Material::HandLeft);
            beam.id=0xa0000100u+hand;beam.flags=DynamicObject|NoCollision|NoShadow|NoRayTracing;r.objects.push_back(beam);++r.count;
            RenderObject point=beam;point.current.p=Transform({0,0,1.25f},pose);point.previous=point.current;
            point.scale={.012f,.012f,.012f};point.mesh=uint32_t(Mesh::Sphere);point.id=0xa0000110u+hand;r.objects.push_back(point);++r.count;
        }
        return;
    }
    std::array<PoseActionState,2> hands{physics_.HandState(0),physics_.HandState(1)};AppendClientHands(r,input,&hands);
    if(!ambientVisible_&&!menuVisible_)return;
    for(uint32_t hand=0;hand<2;hand++){
        const auto aim=AimState(input,hand);if(!aim.tracked||(panelTouch_[hand]&&panelDistance_[hand]<=0))continue;
        auto direction=Sub(Transform({0,0,1},aim.pose),aim.pose.p);float distance=2.f;
        if(menuVisible_&&std::abs(direction.z)>1e-4f){float hit=(1.72f-aim.pose.p.z)/direction.z;if(hit>0&&hit<6)distance=hit;}
        if(panelDistance_[hand]>0)distance=std::min(distance,panelDistance_[hand]);
        RenderObject beam;beam.current=aim.pose;beam.current.p=Transform({0,0,distance*.5f},aim.pose);beam.previous=beam.current;beam.scale={.002f,.002f,distance};beam.mesh=uint32_t(Mesh::Box);beam.material=uint32_t(hand?Material::HandRight:Material::HandLeft);beam.id=0xa0000100u+hand;beam.flags=DynamicObject|NoCollision|NoShadow|NoRayTracing;r.objects.push_back(beam);++r.count;
        RenderObject point=beam;point.current.p=Transform({0,0,distance},aim.pose);point.previous=point.current;point.scale={.012f,.012f,.012f};point.mesh=uint32_t(Mesh::Sphere);point.id=0xa0000110u+hand;r.objects.push_back(point);++r.count;
    }
}
void HomeWorld::AppendCallback(void* context,RenderScene& r,const FrameInput& input,double sceneTime){static_cast<HomeWorld*>(context)->Append(r,input,sceneTime);}
RenderContributions HomeWorld::Contributions(){RenderContributions out;out.context=this;out.appendContext=AppendCallback;out.streamMaterials=physics_.StreamMaterials();return out;}
}
