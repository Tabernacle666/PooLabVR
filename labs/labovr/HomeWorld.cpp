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
    // Preserve the proven 1950s lab and its full dynamic PhysX scene.
    // The missing positive-Z wall becomes the brick exit toward the street.
    materials[uint32_t(Material::HomeBlack)]={{.018f,.023f,.032f,.86f},{0,0,0,0}};
    materials[uint32_t(Material::Floor)]={{.020f,.025f,.032f,.24f},{0,0,0,.2f}};
    materials[uint32_t(Material::Wood)]={{.13f,.066f,.026f,.8f},{0,0,0,0}};
    auto roomBox=[&](XMFLOAT3 p,XMFLOAT3 scale,Material mat){
        RenderObject o{};o.current.p=p;o.previous=o.current;o.scale=scale;
        o.mesh=uint32_t(Mesh::Box);o.material=uint32_t(mat);
        scene_.objects.push_back(o);
    };
    roomBox({0,-.05f,0},{8,.10f,8},Material::Floor);
    roomBox({0,3.80f,0},{8,.10f,8},Material::Ceiling);
    roomBox({0,1.875f,-4},{8,3.75f,.10f},Material::WallZMinus);
    roomBox({-4,1.875f,0},{.10f,3.75f,8},Material::WallXMinus);
    roomBox({4,1.875f,0},{.10f,3.75f,8},Material::WallXPlus);
    roomBox({-.85f,1.08f,1.15f},{1.5f,.08f,.95f},Material::Wood);
    for(float x:{-1.47f,-.23f})for(float z:{.77f,1.53f})
        roomBox({x,.52f,z},{.08f,1.04f,.08f},Material::Wood);
    physics_.SetUrbanMode(false);
    physics_.Load(services.Require<LabHostApiV1>(),scene_);
    urbanLibrary_=urban::InstallLibrary(scene_);
    const auto& host=services.Require<LabHostApiV1>();
    urbanAudio_=static_cast<const LabAudioServiceV1*>(
        host.QueryService(host.hostContext,LABOVR_SERVICE_AUDIO_V1,1));
    if(!urbanAudio_||!urbanAudio_->PublishEvents)
        Fail("Urban world requires audio_v1 one-shot events");
    // The original lamp retains its PhysX-tracked light slot. Street
    // lights use the independent per-frame light slots after that lamp.
    // Exterior shares the existing renderer with the fully simulated room.
    // Never call ReplaceScene when opening the physical exit.
    auto brick=[&](XMFLOAT3 p,XMFLOAT3 scale,uint32_t index){
        RenderObject o{};o.current.p=p;o.previous=o.current;
        o.mesh=uint32_t(Mesh::Box);o.scale=scale;
        o.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::Brick);
        o.id=0x66000000u+index;o.flags=NoCollision;
        scene_.objects.push_back(o);
    };
    // North-facing (+Z) facade with two physical apertures:
    // door x=-.9..+.9, y=0..2.75; window x=1.5..2.9, y=.9..2.25.
    // All masonry openings have matching static PhysX collider cutouts.
    brick({-2.45f,2.05f,4.10f},{3.10f,4.10f,.24f},0);
    brick({1.20f,2.05f,4.10f},{.60f,4.10f,.24f},1);
    brick({3.45f,2.05f,4.10f},{1.10f,4.10f,.24f},2);
    brick({2.20f,.45f,4.10f},{1.40f,.90f,.24f},3);
    brick({2.20f,3.175f,4.10f},{1.40f,1.85f,.24f},4);
    brick({0,3.425f,4.10f},{1.80f,1.35f,.24f},5);
    // Low parapet: the previous 1.4-m projection made the small lab
    // resemble a three-storey wall when seen from the sidewalk.
    brick({0,3.96f,4.18f},{8.f,.35f,.28f},6);
    // Exterior brick cladding of all three previously black walls.
    // The original indoor walls and their PhysX collision bodies remain.
    brick({-4.14f,1.875f,0},{.24f,3.75f,8.35f},7);
    brick({4.14f,1.875f,0},{.24f,3.75f,8.35f},8);
    brick({0,1.875f,-4.14f},{8.35f,3.75f,.24f},9);
    // Weatherproof roof is independent of the indoor ceiling.
    RenderObject roof{};
    roof.current.p={0,3.92f,0};roof.previous=roof.current;
    roof.scale={8.35f,.14f,8.35f};
    roof.mesh=uint32_t(Mesh::Box);
    roof.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::AsphaltPatch);
    roof.id=0x6600000au;roof.flags=NoCollision;
    scene_.objects.push_back(roof);
    for(uint32_t patch=0;patch<8;++patch){
        const uint64_t h=urban::Hash(urban::WorldSeed^uint64_t(patch)*797ull);
        RenderObject snow{};
        snow.current.p={-2.75f+float(h&0xffffu)/65535.f*5.5f,4.02f,
                        -2.75f+float((h>>16)&0xffffu)/65535.f*5.5f};
        snow.previous=snow.current;
        snow.scale={.65f+.30f*float(patch%3),.4f,.65f+.25f*float(patch%2)};
        snow.mesh=urbanLibrary_.meshBase+uint32_t(urban::MeshId::SnowPatch);
        snow.material=urbanLibrary_.materialBase+uint32_t(urban::SurfaceMaterial::Snow);
        snow.id=0x68000000u+patch;snow.flags=NoCollision|NoShadow;
        scene_.objects.push_back(snow);
    }
    // Open-view wooden window frame and central mullion, no opaque infill.
    roomBox({1.50f,1.575f,4.30f},{.085f,1.35f,.12f},Material::Wood);
    roomBox({2.90f,1.575f,4.30f},{.085f,1.35f,.12f},Material::Wood);
    roomBox({2.20f,.90f,4.30f},{1.49f,.075f,.12f},Material::Wood);
    roomBox({2.20f,2.25f,4.30f},{1.49f,.075f,.12f},Material::Wood);
    roomBox({2.20f,1.575f,4.30f},{.040f,1.35f,.09f},Material::Wood);
    // Real geometric masonry courses on the street face. Each brick is a
    // shared-mesh instance with millimetric protrusion and a mortar recess:
    // stereo depth and grazing-angle shadows remain consistent without
    // pretending the unconnected POM shader is active in the home renderer.
    // Both apertures remain open in the raised masonry brick courses.
    for(unsigned row=0;row<38;++row) {
        const float centerY=.055f+float(row)*.107f;
        const float phase=(row&1u)?-.18f:0.f;
        for(unsigned column=0;column<24;++column) {
            const float centerX=-4.f+.18f+float(column)*.36f+phase;
            if(centerX>3.98f)continue;
            if(centerY<2.76f&&std::abs(centerX)<1.03f)continue;
            if(centerY>.84f&&centerY<2.32f&&centerX>1.33f&&centerX<3.07f)continue;
            const uint64_t seed=urban::Hash(uint64_t(row)*941u+column*97u);
            const float protrusion=float(seed&255u)/255.f*.014f;
            RenderObject block{};
            block.current.p={centerX,centerY,4.252f+protrusion};
            block.previous=block.current;
            block.mesh=uint32_t(Mesh::Box);
            block.material=urbanLibrary_.materialBase+
                            uint32_t(urban::SurfaceMaterial::Brick);
            block.scale={.346f,.095f,.055f};
            block.id=0x67000000u+row*24u+column;
            block.flags=NoCollision;
            scene_.objects.push_back(block);
        }
    }
    // Glowing lamps and urban street tiles remain outside the new exit.
    terminalGpu_=static_cast<const LabGpuServiceV1*>(
        host.QueryService(host.hostContext,LABOVR_SERVICE_GPU_V1,1));
    if(!terminalGpu_||terminalGpu_->structSize<sizeof(*terminalGpu_)||
       !terminalGpu_->PublishTerminal||!terminalGpu_->PublishTerminalV2)
        Fail("Urban world requires Core terminal ABI");
    urbanTiles_.clear();
    urbanPositionKnown_=false;
    lastUrbanUpdate_=lastUrbanLog_=urbanSeconds_=urbanOrigin_=urbanFogNextUpdate_=0;
    urbanFogActive_=false;
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
        result->features=urban::GenerateTile(key);
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
    // The diagnostic camera, not the unattended headset on the table,
    // owns tile residency throughout an automated capture. Otherwise the
    // two viewpoints fight over opposite sides of the tile seam each frame.
    const urban::Point view=diagnosticHands_?
        urban::Point{diagnosticCamera_.p.x,diagnosticCamera_.p.z}:
        urban::Point{double(input.head.p.x),double(input.head.p.z)};
    EnsureUrbanTiles(view);
    lastUrbanUpdate_=now;
    // Outside, reuse the existing GPU density-transport volume with the
    // nearest warm street drain as its inlet; inside, the original fan fog
    // remains authoritative. This is a true moving density field, not a
    // column of transparent sphere meshes.
    const bool outside=std::max(std::abs(view.x),std::abs(view.z))>4.6;
    if(outside&&(now>=urbanFogNextUpdate_||!urbanFogActive_)) {
        urbanFogNextUpdate_=now+.5;
        urban::Feature nearest{};
        double closest=1.e30;
        bool found=false;
        for(const auto& [key,tile]:urbanTiles_)
            for(const auto& f:tile->features)if(f.kind==urban::Kind::Drain) {
                const double dx=f.position.x-view.x,dz=f.position.z-view.z;
                const double dd=dx*dx+dz*dz;
                if(dd<closest){closest=dd;nearest=f;found=true;}
            }
        if(found) {
            auto settings=physics_.Fog();
            settings.structSize=sizeof(settings);
            settings.enabled=1;
            settings.quality=1;
            // One camera-centred 3D grid, not a city-sized fog simulation.
            // The plume widens into an approximately 9-m-high tree canopy.
            settings.center={float(view.x),4.8f,float(view.z)};
            settings.halfExtent={15.f,9.f,15.f};
            settings.density=.059f;
            settings.height=8.8f;
            settings.falloff=.48f;
            // The laboratory bulb is slot 0; Append() sorts local sodium
            // lamps into slots 1 onward. Light the outdoor volume from
            // those lamps rather than the bulb behind a masonry wall.
            settings.lightIndex=1;
            LabFogWindV1 inlet{};
            inlet.structSize=sizeof(inlet);
            inlet.position={float(nearest.position.x),float(nearest.height)+.12f,
                            float(nearest.position.z)};
            inlet.direction={.141f,.896f,-.421f};
            inlet.radius=1.25f;inlet.length=9.0f;
            inlet.speed=.95f;
            inlet.dissipation=.45f;
            urbanFogWind_=inlet;
            if(!terminalGpu_->SetFogSettings(terminalGpu_->serviceContext,&settings))
                Fail("urban fog: exterior volume rejected");
            if(!urbanFogActive_)
                LogEvent(input.frameId,"urban.fog",
                    "exterior=1 source=%llu distance=%.2f density=%.4f",
                    static_cast<unsigned long long>(nearest.id),
                    std::sqrt(closest),settings.density);
            urbanFogActive_=true;
        }
    } else if(!outside&&urbanFogActive_){
        auto settings=physics_.Fog();
        settings.structSize=sizeof(settings);
        if(!terminalGpu_->SetFogSettings(terminalGpu_->serviceContext,&settings))
            Fail("urban fog: restore original laboratory settings rejected");
        urbanFogActive_=false;
        urbanFogNextUpdate_=0;
        LogEvent(input.frameId,"urban.fog","exterior=0 restored_lab=1");
    }
    // HomePhysics::UpdateFan() still publishes its indoor wind every frame.
    // Reapply the outdoor inlet every frame, not merely every .5 seconds.
    if(outside&&urbanFogActive_&&
       !terminalGpu_->SetFogWind(terminalGpu_->serviceContext,&urbanFogWind_))
        Fail("urban fog: outdoor source rejected");
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
        for(const auto& feature:tile->features){
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
            // Thin irregular wet-asphalt films immediately upstream of
            // the warm drain. Reuse the existing shallow puddle mesh and
            // ray-query material, but never submit these beyond 10 m in VR.
            // State comes from the same SurfaceTile as snow/melt/runoff.
            if(dx*dx+dz*dz<=10.*10.){
                const float damp=std::clamp(
                    wet.wetness*.55f+wet.water*40.f+wet.heat*.095f,0.f,1.f);
                if(damp>.12f)for(uint32_t patch=0;patch<2;++patch){
                    const float shift=(patch==0?-1.45f:1.25f);
                    RenderObject film{};
                    film.current.p={
                        float(feature.position.x)
                            + (feature.axis==urban::Axis::East?shift:-
                                float(feature.side)*.58f),
                        float(feature.height)+.061f,
                        float(feature.position.z)
                            + (feature.axis==urban::Axis::North?shift:-
                                float(feature.side)*.58f)};
                    XMStoreFloat4(&film.current.q,XMQuaternionRotationRollPitchYaw(
                        0,feature.axis==urban::Axis::North?XM_PIDIV2:0.f,0));
                    film.previous=film.current;
                    const float scale=(.76f+.28f*damp)*
                        (patch==0?1.f:.83f);
                    film.scale={scale,.85f,scale};
                    film.mesh=urbanLibrary_.meshBase+
                              uint32_t(urban::MeshId::Puddle);
                    film.material=urbanLibrary_.materialBase+
                                  uint32_t(urban::SurfaceMaterial::WetAsphalt);
                    film.id=uint32_t(urban::Hash(feature.id^
                            (0x7a1943b1ull+uint64_t(patch))));
                    film.flags=DynamicObject|NoCollision|NoShadow;
                    r.objects.push_back(film);++r.count;
                }
            }
            // Steam remains a transported volume in AtmosphereDomain,
            // never sphere meshes or a forest of transparent cards.
        }
    }
}
void HomeWorld::AppendCallback(void* context,RenderScene& r,const FrameInput& input,double sceneTime){static_cast<HomeWorld*>(context)->Append(r,input,sceneTime);}
RenderContributions HomeWorld::Contributions(){RenderContributions out;out.context=this;out.appendContext=AppendCallback;out.streamMaterials=physics_.StreamMaterials();return out;}
}
