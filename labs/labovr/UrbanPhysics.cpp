#include "HomePhysics.h"
#include "ClientAssets.h"
#include "UrbanLayout.h"
#include "SessionLog.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace lab {
namespace {
void NormalizeWind(LabFogWindV1& w) {
    const float length=std::sqrt(w.direction.x*w.direction.x+
        w.direction.y*w.direction.y+w.direction.z*w.direction.z);
    if(length<=0)Fail("Urban wind direction cannot be zero");
    w.direction.x/=length;w.direction.y/=length;w.direction.z/=length;
}
void RequireUrban(LabU32 ok,const char* what) { if(!ok)Fail(what); }
LabVec3V1 Uv(XMFLOAT3 p) {return {p.x,p.y,p.z};}
LabPoseV1 UrbanPose(XMFLOAT3 p) {return {Uv(p),{0,0,0,1}};}
}

// An exterior scene has no room objects, rotor, chain, vase, or editor actors.
// It uses a few non-rendered static light supports for the existing PhysX
// population ABI. All street assets remain render-only geometry.
void HomePhysics::BuildUrban(SceneData& assets) {
    SceneData flyMeshes;
    flyMeshes.meshes=LoadGLTF(AssetPath(L"assets/urban/flies/flies.gltf"));
    if(flyMeshes.meshes.ranges.size()!=12u)Fail("Urban fly mesh library changed");
    const uint32_t flyBase=uint32_t(assets.meshes.ranges.size());
    AppendClientScene(assets,flyMeshes);
    auto& m=assets.materials;
    m[uint32_t(Material::Fly)]={{.045f,.070f,.046f,.28f},{0,0,0,.65f}};
    m[uint32_t(Material::Charcoal)]={{.008f,.012f,.010f,.55f},{0,0,0,.15f}};
    m[uint32_t(Material::Glue)]={{.20f,.26f,.32f,.25f},{0,0,0,.60f}};
    m[uint32_t(Material::Glue)].doubleSided=1;

    // One source body per nearby streetlight: no physical body per decorative
    // brick, tree, snow patch or road stripe.
    constexpr std::array<XMFLOAT3,4> positions{{
        {10.f,6.35f,8.8f},{30.f,6.35f,-8.8f},
        {8.8f,6.35f,10.f},{-8.8f,6.35f,30.f}
    }};
    std::array<LabPhysicsLightV1,positions.size()> lights{};
    for(uint32_t i=0;i<positions.size();++i) {
        LabPhysicsBodyDescV1 d{};
        d.structSize=sizeof(d);
        d.kind=LAB_PHYSICS_KINEMATIC;
        d.mass=.01f;
        d.shape=LAB_PHYSICS_SPHERE;
        d.layer=1;
        d.collisionMask=0;
        d.pose=UrbanPose(positions[i]);
        d.shapePose=UrbanPose({});
        d.dimensions={.04f,0,0};
        d.staticFriction=.5f;d.dynamicFriction=.4f;
        d.contactOffset=.002f;
        auto body=Body(d);
        entities_.back().id="urban-light-"+std::to_string(i);
        entities_.back().name="Exterior light support";
        LabPhysicsLightV1 light{};
        light.structSize=sizeof(light);
        light.sceneLightIndex=i;
        light.body=body;
        light.localPosition={0,0,0};
        light.localDirection={0,-1,0};
        light.radius=18.f;
        light.color={1.f,.65f,.31f};
        light.power=17.f;
        light.outerCos=-.9f;light.innerCos=.3f;
        light.reserved[0]=1800; // Thermal attraction strength, millipower.
        light.reserved[1]=6500; // Thermal radius in millimeters.
        lights[i]=light;
        constexpr uint32_t ordinals[4]{0,1,6,7};
        urbanLampIds_[i]=uint32_t(urban::Identity(urban::Kind::Lamp,{0,0},ordinals[i]));
    }
    lampLight_=lights[0];lampPower_=lights[0].power;

    LabPopulationDescV1 population{};
    population.structSize=sizeof(population);
    population.capacity=8192;
    population.initialCount=populationCount_;
    population.layer=4;
    population.collisionMask=1;
    population.spawnCenter={10.f,6.0f,8.8f};
    population.spawnHalfExtent={5.f,2.2f,5.f};
    // Compact fly-only library: thorax/head/abdomen/eye/left wing/veins/
    // right wing/veins/leg/foot/antennae/proxy.
    population.proxyMesh=flyBase+11;
    population.bodyMesh=flyBase+0;
    population.headMesh=flyBase+1;
    population.abdomenMesh=flyBase+2;
    population.leftWingMesh=flyBase+4;
    population.rightWingMesh=flyBase+6;
    population.bodyMaterial=uint32_t(Material::Fly);
    population.headMaterial=uint32_t(Material::Charcoal);
    population.wingMaterial=uint32_t(Material::Glue);
    population.flight=flight_;
    RequireUrban(physics_->CreatePopulation(physics_->serviceContext,scene_,&population),
                 "urban physics: CUDA insect population rejected");
    RequireUrban(physics_->SetVisuals(physics_->serviceContext,scene_,nullptr,0,
                     lights.data(),uint32_t(lights.size())),
                 "urban physics: light supports rejected");

    populationSpace_.enabled=1;
    populationSpace_.center={10.f,6.0f,8.8f};
    populationSpace_.halfExtent={15.f,7.f,15.f};
    populationSpace_.returnStrength=1.5f;
    RequireUrban(physics_->SetPopulationSpace(physics_->serviceContext,scene_,&populationSpace_),
                 "urban physics: flight volume rejected");
    RequireUrban(physics_->SetPopulationEffects(physics_->serviceContext,scene_,&effects_),
                 "urban physics: population effects rejected");
    fog_.center={10.f,5.5f,5.65f};
    fog_.halfExtent={22.f,10.f,22.f};
    fog_.density=.085f;
    fog_.height=2.5f;
    fog_.quality=1;
    fog_.enabled=1;
    fog_.lightIndex=0;
    transport_.enabled=1;
    wind_.position={10.f,.15f,5.65f};
    wind_.direction={.30f,.10f,-.93f};
    NormalizeWind(wind_);
    wind_.speed=1.3f;
    wind_.radius=1.5f;
    wind_.length=8.f;
    wind_.paused=0;
    optical_.enabled=0; // The home bulb is gone: no dangling optical source.
    RequireUrban(gpu_->SetFogSettings(gpu_->serviceContext,&fog_),
                 "urban fog: volume rejected");
    RequireUrban(gpu_->SetFogWind(gpu_->serviceContext,&wind_),
                 "urban fog: wind rejected");
    PublishOptical();
    LogEvent(0,"urban.physics","actors=%u flies=%u fog=1 fan=0",
             uint32_t(entities_.size()),populationCount_);
}

void HomePhysics::UpdateUrban(uint64_t frame,double now) {
    if(!scene_)return;
    LabPhysicsSnapshotV1 snap{sizeof(snap),uint32_t(states_.size()),states_.data()};
    if(physics_->PollSnapshot(physics_->serviceContext,scene_,&snap)) {
        sourceFrame_=snap.sourceFrame;
        simulationSeconds_=snap.simulationSeconds;
        stateSeconds_=snap.simulationSeconds;
        stateCount_=snap.stateCount;
    }
    LabFogInputV1 fogInput{};
    fogInput.structSize=sizeof(fogInput);
    fogInput.enabled=visible_?1u:0u;
    RequireUrban(gpu_->SetFogInput(gpu_->serviceContext,&fogInput),
                 "urban fog: input rejected");
    populationInput_.enabled=1;
    populationInput_.hands[0]=0;populationInput_.hands[1]=0;
    RequireUrban(physics_->SetPopulationInput(physics_->serviceContext,scene_,&populationInput_),
                 "urban flies: population view rejected");
    physics_->PollPopulation(physics_->serviceContext,scene_,&population_);
    // Four light source identities follow the streetlights nearest the
    // observer without resetting the 500 GPU insects or their audio IDs.
    if(now>=urbanNextLightSelection_) {
        urbanNextLightSelection_=now+1.;
        const urban::Point p{populationInput_.playerPosition.x,
                             populationInput_.playerPosition.z};
        const auto centerTile=urban::TileAt(p);
        std::vector<std::pair<double,urban::Feature>> ranked;
        ranked.reserve(120);
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            for(const auto& f:urban::GenerateTile({centerTile.x+dx,centerTile.z+dz}))
                if(f.kind==urban::Kind::Lamp){
                    const double x=f.position.x-p.x,z=f.position.z-p.z;
                    ranked.emplace_back(x*x+z*z,f);
                }
        std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){
            if(a.first!=b.first)return a.first<b.first;
            return a.second.id<b.second.id;
        });
        std::array<LabPhysicsCommandV1,4> commands{};
        uint32_t moving=0;
        for(uint32_t i=0;i<urbanLampIds_.size()&&i<ranked.size();++i) {
            const auto& feature=ranked[i].second;
            const uint32_t identity=uint32_t(feature.id);
            if(urbanLampIds_[i]==identity)continue;
            auto& cmd=commands[moving++];
            cmd.structSize=sizeof(cmd);
            cmd.kind=LAB_PHYSICS_KINEMATIC_TARGET;
            cmd.body=entities_[i].body;
            cmd.target=UrbanPose({float(feature.position.x),
                                   float(urban::GroundHeight(feature.position)+6.35),
                                   float(feature.position.z)});
            urbanLampIds_[i]=identity;
            urbanLightOff_[i]=!urban::LampOff(identity,now);
            LogEvent(frame,"urban.light.retarget",
                     "slot=%u id=%u x=%.2f z=%.2f distance=%.2f",
                     i,identity,feature.position.x,feature.position.z,
                     std::sqrt(ranked[i].first));
        }
        if(moving)
            RequireUrban(physics_->SubmitCommands(physics_->serviceContext,scene_,
                       commands.data(),moving),
                       "Urban lights: kinematic targets rejected");
        LabPopulationSpaceV1 movingSpace=populationSpace_;
        movingSpace.center={float(p.x),5.5f,float(p.z)};
        movingSpace.halfExtent={22.f,9.f,22.f};
        RequireUrban(physics_->SetPopulationSpace(physics_->serviceContext,scene_,&movingSpace),
                     "Urban flies: following flight volume rejected");
        populationSpace_=movingSpace;
    }
    // Visible power uses the same stable schedule as crackle events; the
    // independent thermal field stays warm throughout short outages.
    for(uint32_t i=0;i<urbanLampIds_.size();++i) {
        const auto id=urbanLampIds_[i];
        const bool off=urban::LampOff(id,now);
        if(off!=urbanLightOff_[i]) {
            RequireUrban(physics_->SetLightPower(physics_->serviceContext,scene_,i,
                                                off?0.f:17.f),
                         "Urban physics: physical lamp power rejected");
            urbanLightOff_[i]=off;
            LogEvent(frame,"urban.light.physical",
                     "index=%u id=%u time=%.3f power=%.2f heat_mw=1800",
                     i,id,now,off?0.f:17.f);
        }
    }
    if(now>=urbanNextLightLog_) {
        urbanNextLightLog_=now+1.;
        LogEvent(frame,"urban.thermal","lights=4 heat_mw_per_light=1800 thermal_radius_m=6.5 independent_of_power=1");
    }
    // World wind varies continuously, and never reads the defunct fan rotor.
    // The persistent fog grid follows the observer while preserving density
    // through world-space reprojection. Its inlet tracks the nearest drain.
    if(now>=urbanNextFogUpdate_) {
        urbanNextFogUpdate_=now+.5;
        const urban::Point viewer{double(populationInput_.playerPosition.x),
                                   double(populationInput_.playerPosition.z)};
        fog_.center={float(viewer.x),5.0f,float(viewer.z)};
        const auto homeTile=urban::TileAt(viewer);
        double best=1e300;
        urban::Feature nearest{};
        bool found=false;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx)
            for(const auto& f:urban::GenerateTile({homeTile.x+dx,homeTile.z+dz}))
                if(f.kind==urban::Kind::Drain) {
                    const double x=f.position.x-viewer.x,z=f.position.z-viewer.z;
                    const double distance=x*x+z*z;
                    if(distance<best){best=distance;nearest=f;found=true;}
                }
        if(found) {
            wind_.position={float(nearest.position.x),
                            float(nearest.height)+.17f,
                            float(nearest.position.z)};
            LogEvent(frame,"urban.fog",
                     "center=%.2f,%.2f source_id=%llu drain=%.2f,%.2f distance=%.2f quality=%u",
                     fog_.center.x,fog_.center.z,
                     static_cast<unsigned long long>(nearest.id),
                     nearest.position.x,nearest.position.z,std::sqrt(best),fog_.quality);
        }
        RequireUrban(gpu_->SetFogSettings(gpu_->serviceContext,&fog_),
                     "urban fog: moving exterior grid rejected");
    }
    wind_.speed=1.1f+.4f*std::sin(float(now)*.17f);
    wind_.direction={.26f+.1f*std::sin(float(now)*.07f),.10f,-.91f};
    NormalizeWind(wind_);
    RequireUrban(gpu_->SetFogWind(gpu_->serviceContext,&wind_),
                 "urban fog: outdoor airflow rejected");
    // Airflow descriptor changes retire the previous CUDA stream. Publish
    // an authored wind descriptor at 1 Hz; fog advection remains per frame.
    if(now>=urbanNextAirflow_) {
        urbanNextAirflow_=now+1.;
        LabPhysicsAirflowV1 airflow{};
        airflow.structSize=sizeof(airflow);
        airflow.enabled=visible_?1u:0u;
        airflow.emitter=entities_.empty()?0:entities_[0].body;
        const float nx=wind_.direction.x, ny=wind_.direction.y, nz=wind_.direction.z;
        const float inv=1.f/std::sqrt(nx*nx+ny*ny+nz*nz);
        airflow.localDirection={nx*inv,ny*inv,nz*inv};
        airflow.radius=4.f;
        airflow.length=16.f;
        airflow.speedAtReferenceRpm=wind_.speed;
        airflow.referenceRpm=0.f; // Static, rotor-independent ambient wind.
        airflow.turbulence=.22f;
        RequireUrban(physics_->SetAirflows(physics_->serviceContext,scene_,&airflow,1),
                     "urban flies: ambient wind rejected");
    }

    std::array<LabAudioEmitterV1,LAB_PHYSICS_POPULATION_AUDIO_MAX_V1+2> emitters{};
    uint32_t count=0;
    if(visible_)for(uint32_t i=0;i<population_.voiceCount&&count<emitters.size();++i) {
        const auto& voice=population_.voices[i];
        if(voice.insect>=flyAudioIds_.size())continue;
        auto& e=emitters[count++];
        e.structSize=sizeof(e);
        e.id=0x7f000000u+flyAudioIds_[voice.insect];
        e.flags=LABOVR_AUDIO_EMITTER_DIFFUSE_V1;
        e.wave=LABOVR_AUDIO_WAVE_BUZZ_V1;
        e.position[0]=voice.position.x;
        e.position[1]=voice.position.y;
        e.position[2]=voice.position.z;
        e.velocity[0]=voice.velocity.x;
        e.velocity[1]=voice.velocity.y;
        e.velocity[2]=voice.velocity.z;
        e.frequency=voice.frequency;
        e.gain=.045f*std::sqrt(std::max(0.f,voice.activity))/
            std::sqrt(float(std::max(1u,population_.voiceCount)));
    }
    if(visible_){
        auto& wind=emitters[count++];
        wind.structSize=sizeof(wind);
        wind.id=0x7d000001u;
        wind.flags=LABOVR_AUDIO_EMITTER_DIFFUSE_V1;
        wind.wave=LABOVR_AUDIO_WAVE_NOISE_V1;
        wind.position[0]=populationInput_.playerPosition.x;
        wind.position[1]=populationInput_.playerPosition.y;
        wind.position[2]=populationInput_.playerPosition.z;
        wind.gain=.007f+.003f*wind_.speed;
        wind.frequency=150.f;
        auto& drain=emitters[count++];
        drain.structSize=sizeof(drain);
        drain.id=0x7d000002u;
        drain.wave=LABOVR_AUDIO_WAVE_NOISE_V1;
        drain.position[0]=wind_.position.x;
        drain.position[1]=wind_.position.y;
        drain.position[2]=wind_.position.z;
        drain.gain=.011f;
        drain.frequency=270.f;
    }
    LabAudioEmitterBatchV1 batch{1,sizeof(batch),frame,emitters.data(),count};
    RequireUrban(audio_->PublishEmitters(audio_->serviceContext,&batch),
                 "urban audio: insect emitter batch rejected");
}
} // namespace lab
