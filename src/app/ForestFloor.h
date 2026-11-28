#pragma once
#include "Assets.h"
#include "OasisMetric.h"
#include "Understory.h"
#include "ExpansionFlora.h"
#include "BotanicalVariation.h"
#include "ForestFogField.h"
#include "WorldRegistry.h"
#include "SphereGeometry.h"
#include "AppSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <unordered_map>

namespace OASIS {
// Shared 10-plant patches, not ten resident routes per patch. The ninth
// source is a separate fallen trunk; all are ordinary forest mesh sources.
std::array<WorldObject,9> BuildForestFloorSources(SceneData& scene);

class ForestFloor {
    struct Placement { Pose pose; uint32_t model; XMFLOAT3 scale{OasisSceneUnitsPerMeter,OasisSceneUnitsPerMeter,OasisSceneUnitsPerMeter}; uint64_t identity{}; };
    std::vector<Placement> placements_,understory_;
    ForestFogField fogField_;
    double dryArea_{};
    static constexpr float Units=OasisSceneUnitsPerMeter;
    static float Random(uint32_t n){
        n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;
        return float(n&0x00ffffffu)/16777215.f;
    }
    static uint32_t Seed(int32_t x,int32_t z){
        return uint32_t(x)*0x9e3779b9u^uint32_t(z)*0x85ebca6bu^0x41524235u;
    }
    static float Field(float x,float z){
        const float u=x/(8.f*Units),v=z/(8.f*Units);
        const auto i=int32_t(std::floor(u)),j=int32_t(std::floor(v));
        float a=u-float(i),b=v-float(j);a=a*a*(3.f-2.f*a);b=b*b*(3.f-2.f*b);
        return std::lerp(std::lerp(Random(Seed(i,j)),Random(Seed(i+1,j)),a),
                         std::lerp(Random(Seed(i,j+1)),Random(Seed(i+1,j+1)),a),b);
    }
public:
    void Prepare(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,
                 std::span<const XMFLOAT3> treeRoots){
        placements_.clear();understory_.clear();dryArea_=0.;
        fogField_.Build(terrain,treeRoots);
        if(terrain.soil.empty())return;
        struct Bin {std::vector<const TerrainTriangle*> soil,wet;};
        std::unordered_map<uint64_t,Bin> bins;
        constexpr float binWidth=4.36893203f;
        const auto cell=[](float p){return int32_t(std::floor(p/binWidth));};
        const auto key=[](int32_t x,int32_t z){return (uint64_t(uint32_t(x))<<32)|uint32_t(z);};
        float left=std::numeric_limits<float>::max(),right=-left,front=left,back=-left;
        const auto insert=[&](const auto& triangles,bool wet){
            for(const auto& t:triangles){
                const float lx=std::min({t[0].x,t[1].x,t[2].x}),rx=std::max({t[0].x,t[1].x,t[2].x});
                const float lz=std::min({t[0].z,t[1].z,t[2].z}),rz=std::max({t[0].z,t[1].z,t[2].z});
                if(!wet){
                    left=std::min(left,lx);right=std::max(right,rx);
                    front=std::min(front,lz);back=std::max(back,rz);
                }
                for(int32_t z=cell(lz);z<=cell(rz);++z)for(int32_t x=cell(lx);x<=cell(rx);++x){
                    auto& bin=bins[key(x,z)];(wet?bin.wet:bin.soil).push_back(&t);
                }
            }
        };
        insert(terrain.soil,false);insert(terrain.banks,true);insert(terrain.waterway,true);
        const auto surface=[](const TerrainTriangle& t,float x,float z)->std::optional<float>{
            const auto& [a,b,c]=t;
            const auto ab=Sub(b,a),ac=Sub(c,a);
            const float det=ab.x*ac.z-ab.z*ac.x;
            if(std::abs(det)<8.48336316e-8f)return {};
            const float dx=x-a.x,dz=z-a.z;
            const float u=(dx*ac.z-dz*ac.x)/det,v=(ab.x*dz-ab.z*dx)/det;
            if(u<0.f||v<0.f||u+v>1.f)return {};
            return a.y+u*ab.y+v*ac.y;
        };
        const auto dry=[&](float x,float z)->std::optional<float>{
            const auto it=bins.find(key(cell(x),cell(z)));if(it==bins.end())return {};
            for(const auto* t:it->second.wet)if(surface(*t,x,z))return {};
            std::optional<float> best;
            for(const auto* t:it->second.soil)if(auto y=surface(*t,x,z);y&&(!best||*y>*best))best=y;
            return best;
        };
        // ARB-6 is part of placement: actual mature tree roots define
        // shade and trunk exclusion; wet surfaces determine riparian habitat.
        std::unordered_map<uint64_t,std::vector<XMFLOAT3>> treeBins;
        for(const auto& root:treeRoots)treeBins[key(cell(root.x),cell(root.z))].push_back(root);
        const auto habitatAt=[&](float x,float z){
            float nearest=8.f;
            for(int j=-2;j<=2;++j)for(int i=-2;i<=2;++i){
                const auto it=treeBins.find(key(cell(x)+i,cell(z)+j));
                if(it==treeBins.end())continue;
                for(const auto& root:it->second)
                    nearest=std::min(nearest,std::hypot(x-root.x,z-root.z)/Units);
            }
            float wetness=0.f;
            for(unsigned k=0;k<8u;++k){
                const float a=XM_2PI*float(k)/8.f;
                for(float distance:{2.f,4.f,6.f}){
                    const float px=x+distance*Units*std::cos(a),pz=z+distance*Units*std::sin(a);
                    const auto it=bins.find(key(cell(px),cell(pz)));
                    if(it==bins.end())continue;
                    for(const auto* t:it->second.wet)if(surface(*t,px,pz)){
                        wetness=std::max(wetness,1.f-distance/8.f);break;
                    }
                }
            }
            return std::array<float,3>{nearest,std::clamp(1.f-nearest/5.f,0.f,1.f),wetness};
        };
        const auto fitPatch=[&](float x,float z,float y,float radius,float yaw)->std::optional<Pose>{
            if(std::any_of(sites.begin(),sites.end(),[&](const auto& site){
                const float dx=x-std::clamp(x,site.origin.x,site.origin.x+site.width);
                const float dz=z-std::clamp(z,site.origin.z,site.origin.z+site.depth);
                return std::hypot(dx,dz)<radius;
            }))return {};
            std::array<float,8> heights{};
            for(unsigned k=0;k<8u;++k){
                const float a=XM_2PI*float(k)/8.f;
                const auto h=dry(x+radius*std::cos(a),z+radius*std::sin(a));
                if(!h)return {};
                heights[k]=*h;
            }
            const float gx=(heights[0]-heights[4])/(2.f*radius);
            const float gz=(heights[2]-heights[6])/(2.f*radius);
            for(unsigned k=0;k<8u;++k){
                const float a=XM_2PI*float(k)/8.f;
                const float predicted=y+radius*(gx*std::cos(a)+gz*std::sin(a));
                if(std::abs(heights[k]-predicted)>.16f*Units)return {};
            }
            // Fit a rigid patch to the terrain tangent instead of rejecting
            // every slope. Yaw is applied before the up-to-normal rotation.
            const auto normal=XMVector3Normalize(XMVectorSet(-gx,1.f,-gz,0.f));
            const auto up=XMVectorSet(0.f,1.f,0.f,0.f);
            const auto axis=XMVector3Cross(up,normal);
            const float sine=XMVectorGetX(XMVector3Length(axis));
            const float cosine=XMVectorGetX(XMVector3Dot(up,normal));
            const auto tilt=sine>1.e-5f?XMQuaternionRotationAxis(axis,std::atan2(sine,cosine)):XMQuaternionIdentity();
            const auto spin=XMQuaternionRotationAxis(up,yaw);
            Pose pose{{x,y+.008f*Units,z}};
            XMStoreFloat4(&pose.q,XMQuaternionMultiply(spin,tilt));
            return pose;
        };
        // A nearly continuous, overlapping ground carpet. The spacing follows
        // the measured patch footprint, not a memory or population ceiling.
        constexpr float spacing=1.65f*Units;
        constexpr float radius=1.48f*Units;
        for(int32_t iz=int32_t(std::floor(front/spacing));float(iz)*spacing<back;++iz)
            for(int32_t ix=int32_t(std::floor(left/spacing));float(ix)*spacing<right;++ix){
                const auto seed=Seed(ix,iz);
                const float x=(float(ix)+.5f+.20f*(Random(seed^0x11u)-.5f))*spacing;
                const float z=(float(iz)+.5f+.20f*(Random(seed^0x22u)-.5f))*spacing;
                const auto y=dry(x,z);if(!y)continue;
                dryArea_+=double(spacing/Units)*(spacing/Units);
                const auto pose=fitPatch(x,z,*y,radius,XM_2PI*Random(seed^0x44u));
                if(!pose)continue;
                const auto habitat=habitatAt(x,z);
                const float shade=habitat[1],wet=habitat[2],field=Field(x,z),roll=Random(seed^0x33u);
                const float moss=.18f+.25f*shade+.12f*wet;
                const float fern=.16f+.17f*wet+.10f*shade;
                const float shrub=.12f+.12f*field;
                const unsigned family=roll<moss?3u:roll<moss+fern?1u:roll<moss+fern+shrub?2u:0u;
                const auto& span=BotanicalVariation::Floor[family];
                const float size=BotanicalVariation::Plant(Random(seed^0x5154u),span[0],span[1]);
                // Root carpet footprint stays fixed; size diversity is vertical.
                placements_.push_back({*pose,family*2u+unsigned(Random(seed^0x55u)>.5f),
                    {Units,Units*size,Units},TerritorialIdentity(TerritorialProducer::Floor,ix,iz)});
                // Fallen wood has its own real geometry and rare ecological
                // occurrence (~one candidate per 200 square metres).
                if(Random(seed^0x66u)<.013f&&habitat[0]>.6f){
                    const float size=BotanicalVariation::Plant(Random(seed^0x5155u),.50f,3.f);
                    // Keep the old attractor/root. A larger log needs its own
                    // footprint check; otherwise retain the valid baseline.
                    float fitted=size;
                    auto woodPose=pose;
                    if(size>1.f){
                        woodPose=fitPatch(x,z,*y,radius*size,XM_2PI*Random(seed^0x44u));
                        if(!woodPose){woodPose=pose;fitted=1.f;}
                    }
                    placements_.push_back({*woodPose,8u,{Units*fitted,Units*fitted,Units*fitted},TerritorialIdentity(TerritorialProducer::Floor,ix,iz,1u)});
                }
            }

        // Colonies overlap across a mosaic of fern brakes, berry shrubs,
        // open herb/grass glades and regenerating young trees. No synthetic
        // corridors or camera-driven empty areas.
        constexpr float patchSpacing=1.65f*Units,patchRadius=1.65f*Units;
        for(int32_t iz=int32_t(std::floor(front/patchSpacing));float(iz)*patchSpacing<back;++iz)
            for(int32_t ix=int32_t(std::floor(left/patchSpacing));float(ix)*patchSpacing<right;++ix){
                const auto seed=Seed(ix,iz)^0x55325242u;
                const float x=(float(ix)+.5f+.40f*(Random(seed^0x11u)-.5f))*patchSpacing;
                const float z=(float(iz)+.5f+.40f*(Random(seed^0x22u)-.5f))*patchSpacing;
                const auto y=dry(x,z);if(!y)continue;
                const auto habitat=habitatAt(x,z);
                const float shade=habitat[1],wet=habitat[2];
                if(habitat[0]<.42f)continue;
                const float cluster=Field(x+17.f*Units,z-13.f*Units);
                const float community=Field(x-41.f*Units,z+22.f*Units);
                const float occupation=.83f+.16f*cluster;
                if(Random(seed^0x33u)>occupation)continue;
                const auto pose=fitPatch(x,z,*y,patchRadius,XM_2PI*Random(seed^0x78u));
                if(!pose)continue;
                // Weights are ecological affinities, not family quotas.
                const std::array<float,5> weights{
                    .30f+.55f*cluster+.15f*(1.f-shade),
                    .45f+.65f*shade+.85f*wet+.30f*(1.f-community),
                    .37f+.30f*(1.f-shade)+.24f*community,
                    .16f+.65f*(1.f-shade)+.12f*wet,
                    .09f+.20f*(1.f-shade)*community
                };
                float sum=0.f;for(float weight:weights)sum+=weight;
                float roll=Random(seed^0x45u)*sum;
                unsigned family=0;
                while(family+1u<weights.size()&&roll>=weights[family]){roll-=weights[family];++family;}
                std::array<unsigned,Understory::SpeciesList.size()> candidates{};
                unsigned count=0;
                for(unsigned i=0;i<Understory::SpeciesList.size();++i)
                    if(unsigned(Understory::SpeciesList[i].family)==family)candidates[count++]=i;
                if(!count)continue;
                const float choice=.55f*community+.45f*Random(seed^0x56u);
                const float immigrant=Random(seed^0x58u);
                const float pick=immigrant<.28f?Random(seed^0x59u):choice;
                const unsigned chosen=candidates[std::min(count-1u,unsigned(pick*count))];
                // All eight ages/form seeds participate. Open glades favor
                // shorter individuals, not missing plants; no uniform wall.
                const float maturity=std::clamp(.70f*Random(seed^0x67u)+.30f*cluster
                    +.12f*shade-.12f*(1.f-shade),0.f,.9999f);
                const unsigned variant=std::min(Understory::Variants-1u,unsigned(maturity*Understory::Variants));
                const auto& span=BotanicalVariation::Understory[family];
                const float size=BotanicalVariation::Plant(Random(seed^0x5156u),span[0],span[1]);
                // No gaps introduced by scaling the spacing of rooted plants.
                understory_.push_back({*pose,chosen*Understory::Variants+variant,
                    {Units,Units*size,Units},TerritorialIdentity(TerritorialProducer::Understory,ix,iz)});
            }
    }
    // A shared immutable two-meshlet particle source. Colonies use the
    // resident world registry; flight is evaluated from stable IDs on GPU.
    void InstallInsects(SceneData& scene,WorldRegistry& world,ExpansionFlora* expansion=nullptr)const{
        if(understory_.empty())return;
        constexpr uint32_t Count=32u,GroupSize=21u,ColonyCount=128u;
        auto& mesh=scene.meshes;
        const uint32_t firstVertex=uint32_t(mesh.vertices.size()),firstIndex=uint32_t(mesh.indices.size());
        const uint32_t firstMeshlet=uint32_t(mesh.meshlets.size());
        constexpr std::array<XMFLOAT2,3> corners{{{-.85f,-.65f},{.85f,-.65f},{0.f,1.f}}};
        for(uint32_t start=0;start<Count;start+=GroupSize){
            const uint32_t count=std::min(GroupSize,Count-start);
            mesh.meshlets.push_back({uint32_t(mesh.remap.size()),uint32_t(mesh.triangles.size()),count*3u,count});
            for(uint32_t particle=0;particle<count;++particle){
                const uint32_t seed=(start+particle+1u)*0x9e3779b9u;
                const float angle=XM_2PI*Random(seed),radius=.85f*std::sqrt(Random(seed^0x31u));
                const XMFLOAT3 point{std::cos(angle)*radius,.45f+1.65f*Random(seed^0x73u),std::sin(angle)*radius};
                for(const auto& corner:corners){
                    const uint32_t index=uint32_t(mesh.vertices.size());
                    mesh.vertices.push_back({point,{0,1,0},corner,{Random(seed^0x89u),Random(seed^0x51u)}});
                    mesh.indices.push_back(index);mesh.remap.push_back(index);
                }
                mesh.triangles.push_back({particle*3u,particle*3u+1u,particle*3u+2u});
            }
        }
        WorldObject source{};
        source.mesh=uint32_t(mesh.ranges.size());source.material=uint32_t(scene.materials.size());
        source.flags=BuildingObject|OASIS_INSTANCE_FOREST|OASIS_INSTANCE_FOREST_INSECTS|NoCollision|NoRayTracing|NoShadow|AlphaBlendObject;
        source.boundRadius=3.5f;source.tileLayer=65;
        mesh.ranges.push_back({firstMeshlet,uint32_t(mesh.meshlets.size())-firstMeshlet,
            firstVertex,Count*3u,firstIndex,Count*3u,{0,0,0,3.5f}});
        mesh.deformations.emplace_back();mesh.motions.emplace_back();mesh.skin.resize(mesh.vertices.size());
        if(!scene.triangleSolids.empty())scene.triangleSolids.resize(mesh.indices.size()/3u,Invalid);
        AssetMaterial material{};
        material.colorRoughness={.065f,.052f,.032f,1.f};
        material.emissionMetallic={.035f,.030f,.019f,0.f};
        material.alphaMode=2u;material.alpha=.78f;material.doubleSided=1u;material.unlit=1u;
        scene.materials.push_back(material);scene.materialNames.emplace_back("Forest microfauna");
        if(expansion)expansion->insects=source;
        const auto colonies=std::min<size_t>(ColonyCount,understory_.size());
        std::vector<WorldObject> objects;objects.reserve(colonies);
        std::vector<uint64_t> identities;identities.reserve(colonies);
        for(size_t i=0;i<colonies;++i){
            auto object=source;
            const auto& plant=understory_[(i*understory_.size()+understory_.size()/2u)/colonies];
            object.current=object.previous=plant.pose;object.scale={Units,Units,Units};
            objects.push_back(object);
            identities.push_back((plant.identity&~(7ull<<60))|(uint64_t(TerritorialProducer::Insect)<<60));
        }
        world.CreateBatch(objects,identities);
        char line[160];std::snprintf(line,sizeof(line),"atm3 colonies=%zu insects=%zu gpu_meshes=1\r\n",colonies,colonies*Count);
        DiagnosticLine(line);
    }
    void Install(SceneData& scene,WorldRegistry& world,ExpansionFlora* expansion=nullptr,bool lodDiagnostic=false)const{
        fogField_.Install(scene);
        if(placements_.empty())return;
        auto models=BuildForestFloorSources(scene);
        if(lodDiagnostic)for(auto& model:models)model.flags|=OASIS_INSTANCE_LOD_VEGETATION;
        if(expansion){expansion->floor=models;expansion->hasFloor=true;}
        std::vector<WorldObject> objects;objects.reserve(placements_.size());
        std::vector<uint64_t> identities;identities.reserve(placements_.size());
        std::array<size_t,5> counts{};
        for(const auto& p:placements_){
            auto object=models.at(p.model);
            object.current=object.previous=p.pose;
            object.scale=p.scale;
            object.tileLayer=64; // same resident forest domain, no new pass
            objects.push_back(object);
            identities.push_back(p.identity);
            ++counts[p.model/2u];
        }
        world.CreateBatch(objects,identities);
        const auto plants=10u*(counts[0]+counts[1]+counts[2]+counts[3]);
        char line[384];std::snprintf(line,sizeof(line),
            "arb5_floor grass_patches=%zu fern_patches=%zu shrub_patches=%zu moss_patches=%zu deadwood=%zu live_plants=%zu routes=%zu dry_candidate_m2=%.2f plants_per_m2=%.3f\r\n",
            counts[0],counts[1],counts[2],counts[3],counts[4],plants,objects.size(),dryArea_,
            dryArea_>0.?double(plants)/dryArea_:0.);
        DiagnosticLine(line);

        if(understory_.empty())return;
        std::array<bool,Understory::ModelCount> used{};
        for(const auto& p:understory_)used.at(p.model)=true;
        auto sources=BuildUnderstorySources(scene,used);
        if(lodDiagnostic)for(auto& source:sources)source.flags|=OASIS_INSTANCE_LOD_VEGETATION;
        if(expansion){expansion->understory=sources;expansion->used=used;}
        objects.clear();objects.reserve(understory_.size());
        identities.clear();identities.reserve(understory_.size());
        std::array<size_t,5> population{};
        std::array<bool,Understory::SpeciesList.size()> present{};
        for(const auto& p:understory_){
            auto object=sources.at(p.model);
            object.current=object.previous=p.pose;
            object.scale=p.scale;
            object.tileLayer=64;
            objects.push_back(object);
            identities.push_back(p.identity);
            const unsigned species=p.model/Understory::Variants;
            const auto& definition=Understory::SpeciesList[species];
            population[size_t(definition.family)]+=Understory::Individuals(species);
            present[species]=true;
        }
        world.CreateBatch(objects,identities);
        const auto total=population[0]+population[1]+population[2]+population[3]+population[4];
        char summary[512];std::snprintf(summary,sizeof(summary),
            "arb5_3_understory shrubs=%zu ferns=%zu herbs=%zu grasses=%zu saplings=%zu species=%zu forms=%zu plants=%zu routes=%zu added_plants_m2=%.3f\r\n",
            population[0],population[1],population[2],population[3],population[4],
            size_t(std::count(present.begin(),present.end(),true)),size_t(std::count(used.begin(),used.end(),true)),
            total,objects.size(),dryArea_>0.?double(total)/dryArea_:0.);
        DiagnosticLine(summary);
        InstallInsects(scene,world,expansion);
    }
};
}
