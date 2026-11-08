#include "HomeWorld.h"
#include "ClientAssets.h"
#include "ClientVisuals.h"
#include "ContentIds.h"
#include "LabInputMapping.h"
#include "SessionLog.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <cstdio>
#include <cmath>
namespace lab {
void HomeWorld::Load(WorldServices& services){
    if(loaded_)Fail("Urban world already loaded");
    scene_=CreateClientAssetSet();
    auto& materials=scene_.materials;
    materials[uint32_t(Material::HandLeft)]={{.05f,.45f,.8f,.44f},{.01f,.02f,.04f,.1f}};
    materials[uint32_t(Material::HandRight)]={{.86f,.27f,.06f,.44f},{.035f,.01f,0,.1f}};
    urbanLibrary_=urban::InstallLibrary(scene_);
    const auto& host=services.Require<LabHostApiV1>();
    urbanAudio_=static_cast<const LabAudioServiceV1*>(
        host.QueryService(host.hostContext,LABOVR_SERVICE_AUDIO_V1,1));
    if(!urbanAudio_||!urbanAudio_->PublishEvents)
        Fail("Urban world requires audio_v1 one-shot events");
    // The four PhysX-tracked light supports own the first scene-light slots.
    // Additional streetlights are provided as ordinary per-frame lights.
    constexpr std::array<XMFLOAT3,4> sourcePositions{{
        {10.f,6.35f,8.8f},{30.f,6.35f,-8.8f},
        {8.8f,6.35f,10.f},{-8.8f,6.35f,30.f}
    }};
    for(uint32_t i=0;i<sourcePositions.size();++i) {
        SceneLight light{};
        light.position=sourcePositions[i];
        light.radius=18.f;light.power=17.f;
        light.color={1.f,.65f,.31f};
        light.direction={0,-1,0};
        light.outerCos=-.9f;light.innerCos=.3f;
        light.id=0x401u+i;light.flags=5u;
        scene_.lights.push_back(light);
    }
    // Street instances and lights are supplied per XR frame by Append():
    // the static mesh/texture libraries are uploaded exactly once.
    physics_.SetUrbanMode(true);
    physics_.Load(services.Require<LabHostApiV1>(),scene_);
    terminalGpu_=static_cast<const LabGpuServiceV1*>(
        host.QueryService(host.hostContext,LABOVR_SERVICE_GPU_V1,1));
    if(!terminalGpu_||terminalGpu_->structSize<sizeof(*terminalGpu_)||
       !terminalGpu_->PublishTerminal||!terminalGpu_->PublishTerminalV2)
        Fail("Urban world requires Core terminal ABI");
    urbanTiles_.clear();
    urbanPositionKnown_=false;
    lastUrbanUpdate_=lastUrbanLog_=urbanSeconds_=urbanOrigin_=0;
    loaded_=true;
    ambientVisible_=true;
    menuVisible_=false;
    ResetMenu();
    EnsureUrbanTiles({0,0});
    LogEvent(0,"urban.world","loaded=1 base_meshes=%u materials=%u textures=%u tiles=%u",
        uint32_t(scene_.meshes.ranges.size()),uint32_t(scene_.materials.size()),
        uint32_t(scene_.textures.size()),uint32_t(urbanTiles_.size()));
}
void HomeWorld::Unload(WorldServices&){if(terminalGpu_){LabTerminalFrameV1 empty{};empty.structSize=sizeof(empty);terminalGpu_->PublishTerminal(terminalGpu_->serviceContext,&empty);LabTerminalFrameV2 panel{};panel.structSize=sizeof(panel);panel.pose.orientation.w=1;terminalGpu_->PublishTerminalV2(terminalGpu_->serviceContext,&panel);terminalGpu_=nullptr;}physics_.Unload();urbanAudio_=nullptr;urbanTiles_.clear();urbanPositionKnown_=false;scene_={};loaded_=false;}
void HomeWorld::ResetMenu(){selectedNode_=3;inspectorTab_=0;treeScroll_=0;expanded_={37,1,3,21};panelHover_=0;heldControl_={};repeatControl_={};repeatAt_={};panelTouch_={};panelDistance_={};lastPanelAt_=0;worldPage_=visualPage_=false;worldPageIndex_=0;hover_=Invalid;command_={};lastListenerAt_=panelStartSeconds_=0;listenerVelocity_={};lastListener_={};}
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
    hover_=Invalid;
    if(!menuVisible_)return;
    for(uint32_t hand=0;hand<2;++hand){
        const auto aim=AimState(input,hand);
        if(!aim.tracked)continue;
        const auto dir=Sub(Transform({0,0,1},aim.pose),aim.pose.p);
        if(std::abs(dir.z)<1e-5f)continue;
        const float distance=(1.72f-aim.pose.p.z)/dir.z;
        if(distance<=0||distance>8)return;
        const auto p=Add(aim.pose.p,Scale(dir,distance));
        if(std::abs(p.x)>.40f)continue;
        const uint32_t row=std::abs(p.y-2.02f)<.05f?0u:
                           std::abs(p.y-1.905f)<.05f?1u:Invalid;
        if(row==Invalid)continue;
        hover_=row;
        if(input.Pressed(StandardVrActions().primary,hand)){
            if(row==0)menuVisible_=false;
            else command_={HomeCommandKind::Quit,Invalid};
        }
        break;
    }
}
void HomeWorld::EnsureUrbanTiles(urban::Point p) const{
    const auto tile=urban::TileAt(p);
    if(urbanPositionKnown_&&tile==activeUrbanTile_)return;
    activeUrbanTile_=tile;urbanPositionKnown_=true;
    for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx){
        const urban::Tile key{tile.x+dx,tile.z+dz};
        const UrbanTileKey id{key.x,key.z};
        if(urbanTiles_.find(id)!=urbanTiles_.end())continue;
        auto result=std::make_unique<UrbanTile>(key);
        const auto second=static_cast<int64_t>(std::floor(urbanSeconds_));
        if(second>0)result->surface.Advance(double(second),.00000016,-1.5);
        result->simulatedSeconds=second;
        SceneData instances;
        urban::AppendTile(instances,urbanLibrary_,key);
        result->objects=std::move(instances.objects);
        result->lights=std::move(instances.lights);
        urbanTiles_.emplace(id,std::move(result));
    }
    for(auto it=urbanTiles_.begin();it!=urbanTiles_.end();){
        const auto dx=it->first.first-tile.x;
        const auto dz=it->first.second-tile.z;
        if(dx< -1||dx>1||dz< -1||dz>1)it=urbanTiles_.erase(it);
        else ++it;
    }
    LogEvent(0,"urban.stream","center_tile=%lld,%lld resident=%u",
        static_cast<long long>(tile.x),static_cast<long long>(tile.z),
        uint32_t(urbanTiles_.size()));
}
void HomeWorld::Update(const FrameInput& input,double now,std::span<const SceneLight> lights){
    if(!loaded_)return;
    auto aim=AimState(input,1);
    if(!aim.tracked)aim=AimState(input,0);
    physics_.SetPopulationView(input.head.p,aim.pose.p,
        Sub(Transform({0,0,1},aim.pose),aim.pose.p),aim.tracked);
    if(!urbanOrigin_)urbanOrigin_=now;
    const double previousSeconds=urbanSeconds_;
    urbanSeconds_=std::max(0.,now-urbanOrigin_);
    physics_.Update(input.frameId,urbanSeconds_,lights);
    const urban::Point view{double(input.head.p.x),double(input.head.p.z)};
    EnsureUrbanTiles(view);
    lastUrbanUpdate_=now;
    // Weather always advances on global one-second ticks, independent of
    // rendering cadence and tile loading order. Reload reproduces its state
    // from the same seed and absolute simulation second.
    const int64_t seconds=static_cast<int64_t>(std::floor(urbanSeconds_));
    for(auto& [key,tile]:urbanTiles_)if(seconds>tile->simulatedSeconds){
        tile->surface.Advance(double(seconds-tile->simulatedSeconds),.00000016,-1.5);
        tile->simulatedSeconds=seconds;
    }
    // One-shot electrical crackles share the exact deterministic event IDs
    // and intervals with render-visible streetlight failures.
    std::vector<LabAudioSoundEventV1> crackles;
    if(urbanAudio_&&urbanSeconds_>previousSeconds)
        for(const auto& [key,tile]:urbanTiles_)for(const auto& light:tile->lights)
            for(const auto& flash:urban::FlashEvents(light.id,previousSeconds,urbanSeconds_)){
                LabAudioSoundEventV1 event{};
                event.structSize=sizeof(event);
                event.kind=uint32_t(flash.id);
                event.frameId=input.frameId;
                event.position[0]=light.position.x;
                event.position[1]=light.position.y;
                event.position[2]=light.position.z;
                event.gain=.06f;
                event.frequency=350.f+float(flash.id%600);
                event.decay=.975f;
                event.wave=LABOVR_AUDIO_WAVE_NOISE_V1;
                event.flags=1;
                crackles.push_back(event);
                LogEvent(input.frameId,"urban.lamp.glitch",
                    "light=%u event=%llu begin_s=%.3f end_s=%.3f audible=1",
                    light.id,static_cast<unsigned long long>(flash.id),
                    flash.begins,flash.ends);
            }
    if(!crackles.empty()){
        LabAudioEventBatchV1 batch{};
        batch.version=LABOVR_AUDIO_SERVICE_VERSION_V1;
        batch.structSize=sizeof(batch);
        batch.detailedEvents=crackles.data();
        batch.detailedEventCount=uint32_t(crackles.size());
        if(!urbanAudio_->PublishEvents(urbanAudio_->serviceContext,&batch))
            Fail("Urban lamp crackle audio queue rejected");
    }
    if(lastUrbanLog_==0||now-lastUrbanLog_>=1.0){
        double water=0,drained=0;
        for(const auto& [key,tile]:urbanTiles_){
            water+=tile->surface.StoredWater();
            drained+=tile->surface.DischargedWater();
        }
        LogEvent(input.frameId,"urban.weather",
            "winter=1 resident=%u surface_water_equivalent=%.6f drained=%.6f wind=%.3f flies=%u",
            uint32_t(urbanTiles_.size()),water,drained,physics_.Wind().speed,
            physics_.PopulationCount());
        lastUrbanLog_=now;
    }
    UpdateMenu(input);
}
HomeCommand HomeWorld::ConsumeCommand()noexcept{auto c=command_;command_={};return c;}
HomeMenuSnapshot HomeWorld::MenuSnapshot()const{
    HomeMenuSnapshot out{};
    if(!menuVisible_)return out;
    out.count=2;
    std::snprintf(out.labels[0].data(),HomeMenuMaxChars,"Resume");
    std::snprintf(out.labels[1].data(),HomeMenuMaxChars,"Quit");
    out.hover=hover_;
    out.time=float(urbanSeconds_);
    return out;
}
void HomeWorld::PublishMenu()const{
    if(!terminalGpu_)return;auto snapshot=MenuSnapshot();LabTerminalFrameV1 frame{};frame.structSize=sizeof(frame);frame.timeSeconds=snapshot.time;frame.lineCount=snapshot.count;
    for(uint32_t i=0;i<snapshot.count;i++){
        auto& line=frame.lines[i];line.center={0,2.02f-float(i)*.115f,1.716f};line.halfWidth=.42f;line.halfHeight=.046f;line.curveHalfWidth=.435f;line.hovered=snapshot.hover==i;
        std::copy_n(snapshot.labels[i].data(),32,line.text);for(auto& ch:line.text)if(uint8_t(ch)>127)ch='?';
    }
    if(!terminalGpu_->PublishTerminal(terminalGpu_->serviceContext,&frame))Fail("Home gpu_v1: terminal frame rejected");
}
void HomeWorld::FillAudio(AudioFrame& a,const FrameInput& input,double now)noexcept{
    a={};a.inputFrameId=input.frameId;a.time=input.time>0?input.time:now;a.listener=input.head;
    if(lastListenerAt_>0&&input.time>lastListenerAt_){listenerVelocity_=Scale(Sub(input.head.p,lastListener_.p),1.f/std::max(.001f,float(input.time-lastListenerAt_)));float speed=Length(listenerVelocity_);if(speed>10)listenerVelocity_=Scale(listenerVelocity_,10/speed);}
    lastListener_=input.head;lastListenerAt_=input.time;a.velocity=listenerVelocity_;
}
void HomeWorld::Append(RenderScene& r,const FrameInput& input,double /*sceneTime*/)const{
    if(!loaded_||!ambientVisible_)return;
    const double sceneTime=urbanSeconds_;
    const auto point=diagnosticHands_?
        urban::Point{diagnosticCamera_.p.x,diagnosticCamera_.p.z}:
        urban::Point{input.head.p.x,input.head.p.z};
    EnsureUrbanTiles(point);
    std::vector<std::pair<double,SceneLight>> nearestLights;
    nearestLights.reserve(9*12);
    for(const auto& [key,tile]:urbanTiles_){
        const auto origin=urban::Origin({key.first,key.second});
        constexpr unsigned resolution=urban::SurfaceTile::Resolution;
        for(const auto& authored:tile->objects) {
            RenderObject object=authored;
            const uint32_t meshId=object.mesh-urbanLibrary_.meshBase;
            const double ddx=double(object.current.p.x)-point.x;
            const double ddz=double(object.current.p.z)-point.z;
            const double distanceSquared=ddx*ddx+ddz*ddz;
            if(distanceSquared>30.*30.) {
                switch(urban::MeshId(meshId)){
                case urban::MeshId::DeciduousTrunk:
                    object.mesh=urbanLibrary_.meshBase+
                                uint32_t(urban::MeshId::DeciduousLOD);break;
                case urban::MeshId::Conifer:
                    object.mesh=urbanLibrary_.meshBase+
                                uint32_t(urban::MeshId::ConiferLOD);break;
                case urban::MeshId::LampPole:
                    object.mesh=urbanLibrary_.meshBase+
                                uint32_t(urban::MeshId::LampLOD);break;
                case urban::MeshId::Vines:
                    object.mesh=urbanLibrary_.meshBase+
                                uint32_t(urban::MeshId::VinesLOD);break;
                case urban::MeshId::VineLeaves:
                case urban::MeshId::DeciduousLeaves:
                case urban::MeshId::Grass:
                    continue; // Fine foliage is absent at the far LOD.
                default:break;
                }
            }
            if(meshId==uint32_t(urban::MeshId::LampDiffuser)){
                for(const auto& light:tile->lights){
                    const double x=double(object.current.p.x)-light.position.x;
                    const double z=double(object.current.p.z)-light.position.z;
                    if(x*x+z*z<.1){
                        object.material=urbanLibrary_.materialBase+
                            uint32_t(urban::LampOff(light.id,sceneTime)?
                                urban::SurfaceMaterial::LampEnamel:
                                urban::SurfaceMaterial::LampGlow);
                        break;
                    }
                }
            }
            if(meshId==uint32_t(urban::MeshId::SnowPatch)||
               meshId==uint32_t(urban::MeshId::Puddle)) {
                const auto x=static_cast<unsigned>(std::clamp(
                    int((double(object.current.p.x)-origin.x)*
                        double(resolution)/urban::BlockMeters),0,int(resolution)-1));
                const auto z=static_cast<unsigned>(std::clamp(
                    int((double(object.current.p.z)-origin.z)*
                        double(resolution)/urban::BlockMeters),0,int(resolution)-1));
                const auto& surface=tile->surface.At(x,z);
                if(meshId==uint32_t(urban::MeshId::SnowPatch)) {
                    const float cover=std::clamp(surface.snow/.012f,0.f,2.4f);
                    if(cover<.04f)continue;
                    object.scale=Scale(object.scale,.2f+.8f*cover);
                } else {
                    const float puddle=std::clamp(surface.water*120.f+
                        surface.wetness*.32f,.035f,1.2f);
                    object.scale=Scale(object.scale,puddle);
                    object.material=urbanLibrary_.materialBase+
                        uint32_t(puddle>.22f?urban::SurfaceMaterial::Water:
                                               urban::SurfaceMaterial::Slush);
                }
            }
            r.objects.push_back(object);++r.count;
        }
        for(const auto& light:tile->lights){
            const auto& phys=physics_.UrbanLampIds();
            if(std::find(phys.begin(),phys.end(),light.id)!=phys.end())continue;
            const double dx=double(light.position.x)-point.x;
            const double dz=double(light.position.z)-point.z;
            nearestLights.emplace_back(dx*dx+dz*dz,light);
        }
    }
    std::sort(nearestLights.begin(),nearestLights.end(),[](const auto& a,const auto& b){
        if(a.first!=b.first)return a.first<b.first;
        return a.second.id<b.second.id;
    });
    for(const auto& candidate:nearestLights){
        if(r.lightCount>=MaxLights)break;
        SceneLight light=candidate.second;
        light.power=urban::LampOff(light.id,sceneTime)?0.f:light.power;
        light.flags=candidate.first<(27.*27.)?1u:0u;
        r.lights[r.lightCount++]=light;
    }
    // Inward-facing overcast dome: moves with the viewer, remains inside
    // the XR far clip and never participates in shadows or reflections.
    RenderObject sky{};
    sky.current.p={float(point.x),diagnosticHands_?diagnosticCamera_.p.y:input.head.p.y,
                   float(point.z)};
    sky.previous=sky.current;
    sky.scale={1,1,1};
    sky.mesh=urbanLibrary_.meshBase+uint32_t(urban::MeshId::SkyDome);
    sky.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::OvercastSky);
    sky.id=0x62000001u;
    sky.flags=DynamicObject|NoCollision|NoShadow|NoRayTracing;
    r.objects.push_back(sky);++r.count;
    // Fixed-seed particles are a camera-centred precipitation volume.
    // Falling is analytic, hence deterministic and stereo-consistent.
    constexpr uint32_t Snowflakes=360;
    for(uint32_t i=0;i<Snowflakes;++i){
        const uint64_t id=urban::Hash(urban::WorldSeed^uint64_t(i)*0x9e3779b97f4a7c15ull);
        const double dx=double(id&0xffffu)/65535.*44.-22.;
        const double dz=double((id>>16)&0xffffu)/65535.*44.-22.;
        const double initial=double((id>>32)&0xffffu)/65535.*13.;
        const double fall=std::fmod(initial-sceneTime*(.65+double(i%11)*.055),13.);
        const float height=float(fall<0?fall+13.:fall);
        const double drift=std::sin(sceneTime*.43+double(i)*.37)*.85;
        const double sway=std::cos(sceneTime*.29+double(i)*.21)*.52;
        RenderObject flake{};
        flake.current.p={float(point.x+dx+drift),height,float(point.z+dz+sway)};
        flake.previous=flake.current;
        flake.scale={.018f+.006f*float(i%4),.027f,.018f};
        flake.mesh=uint32_t(Mesh::Box);
        flake.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::Snow);
        flake.id=0x61000000u+i;
        flake.flags=DynamicObject|NoCollision|NoShadow|NoRayTracing;
        r.objects.push_back(flake);++r.count;
    }
    // Additional drain vapour is authored as translucent streaks; the
    // existing density-transport fog provides the volumetric background.
    for(const auto& [key,tile]:urbanTiles_){
        const urban::Tile tk{key.first,key.second};
        const auto features=urban::GenerateTile(tk);
        for(const auto& feature:features){
            if(feature.kind!=urban::Kind::Drain)continue;
            const double dx=feature.position.x-point.x,dz=feature.position.z-point.z;
            if(dx*dx+dz*dz>23.*23.)continue;
            // Directional runoff uses the known road/drain axis rather
            // than a full 3D fluid solver. The segment phase is world-time
            // based, so it stays continuous across tile residency changes.
            const auto origin=urban::Origin(tk);
            const int ix=std::clamp(int((feature.position.x-origin.x)*
                   urban::SurfaceTile::Resolution/urban::BlockMeters),0,
                   int(urban::SurfaceTile::Resolution)-1);
            const int iz=std::clamp(int((feature.position.z-origin.z)*
                   urban::SurfaceTile::Resolution/urban::BlockMeters),0,
                   int(urban::SurfaceTile::Resolution)-1);
            const auto& wet=tile->surface.At(unsigned(ix),unsigned(iz));
            const float flowStrength=std::clamp(
                wet.water*80.f+wet.wetness*.5f+wet.heat*.09f,.1f,1.f);
            const float directionX=feature.axis==urban::Axis::North?float(feature.side):0.f;
            const float directionZ=feature.axis==urban::Axis::East?float(feature.side):0.f;
            const float yaw=std::atan2(-directionZ,directionX);
            for(uint32_t stripe=0;stripe<4;++stripe) {
                const double stagger=double(stripe)*.31;
                for(uint32_t segment=0;segment<4;++segment){
                    const double fraction=std::fmod(
                        sceneTime*(.19+.04*stripe)+double(segment)*.25+
                        double(feature.id%31)*.017,1.);
                    const float upstream=float((1.-fraction)*3.7+.32);
                    const float lateral=float((double(stripe)-1.5)*.26);
                    const float offsetX=-directionX*upstream-directionZ*lateral;
                    const float offsetZ=-directionZ*upstream+directionX*lateral;
                    RenderObject trickle{};
                    trickle.current.p={
                        float(feature.position.x)+offsetX,
                        float(feature.height)+.025f,
                        float(feature.position.z)+offsetZ};
                    XMStoreFloat4(&trickle.current.q,
                                  XMQuaternionRotationRollPitchYaw(0,yaw,0));
                    trickle.previous=trickle.current;
                    trickle.scale={flowStrength,.9f,flowStrength};
                    trickle.mesh=urbanLibrary_.meshBase+
                                 uint32_t(urban::MeshId::RunoffStrip);
                    trickle.material=urbanLibrary_.materialBase+
                                     uint32_t(urban::SurfaceMaterial::Water);
                    trickle.id=uint32_t(urban::Hash(feature.id^
                                (uint64_t(stripe)*16+segment+1)*0x9e3779b9ull));
                    trickle.flags=DynamicObject|NoCollision|NoShadow|
                                  NoRayTracing|AlphaBlendObject;
                    r.objects.push_back(trickle);++r.count;
                }
            }
            for(uint32_t i=0;i<10;++i){
                const float u=(float(i)+.5f)/10.f;
                const double phase=sceneTime*.24+double(feature.id%509)*.031;
                RenderObject steam{};
                steam.current.p={
                    float(feature.position.x+u*1.3+.24*std::sin(phase+i)),
                    float(feature.height+.18+u*6.5),
                    float(feature.position.z-u*1.9+.20*std::cos(phase+i))};
                steam.previous=steam.current;
                steam.scale={.24f+u*.60f,.32f+u*.18f,.19f+u*.42f};
                steam.mesh=uint32_t(Mesh::Sphere);
                steam.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::Steam);
                steam.id=uint32_t(urban::Hash(feature.id^i^0xaceu));
                steam.flags=DynamicObject|NoCollision|NoShadow|NoRayTracing|AlphaBlendObject;
                r.objects.push_back(steam);++r.count;
            }
        }
    }
}
void HomeWorld::AppendCallback(void* context,RenderScene& r,const FrameInput& input,double sceneTime){static_cast<HomeWorld*>(context)->Append(r,input,sceneTime);}
RenderContributions HomeWorld::Contributions(){RenderContributions out;out.context=this;out.appendContext=AppendCallback;out.streamMaterials=physics_.StreamMaterials();return out;}
}
