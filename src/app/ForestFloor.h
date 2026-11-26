#pragma once
#include "Assets.h"
#include "Understory.h"
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
    struct Placement { Pose pose; uint32_t model; };
    std::vector<Placement> placements_,understory_;
    double dryArea_{};
    static constexpr float Units=SpectacleInteriorUnitsPerMeter;
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
    void Prepare(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites){
        placements_.clear();understory_.clear();dryArea_=0.;
        if(terrain.soil.empty())return;
        struct Bin {std::vector<const TerrainTriangle*> soil,wet;};
        std::unordered_map<uint64_t,Bin> bins;
        constexpr float binWidth=.15f;
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
            if(std::abs(det)<1.e-10f)return {};
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
        // 10 botanical individuals per patch / 2.4 individuals per m2.
        // The 2.04 m spacing is derived from reference density, not a cap.
        const float spacing=std::sqrt(10.f/2.4f)*Units;
        constexpr float radius=1.25f*Units;
        for(int32_t iz=int32_t(std::floor(front/spacing));float(iz)*spacing<back;++iz)
            for(int32_t ix=int32_t(std::floor(left/spacing));float(ix)*spacing<right;++ix){
                const auto seed=Seed(ix,iz);
                const float x=(float(ix)+.5f+.20f*(Random(seed^0x11u)-.5f))*spacing;
                const float z=(float(iz)+.5f+.20f*(Random(seed^0x22u)-.5f))*spacing;
                const auto y=dry(x,z);if(!y)continue;
                dryArea_+=double(spacing/Units)*(spacing/Units);
                // The shared patch must fit on dry ground and outside foundations.
                const bool building=std::any_of(sites.begin(),sites.end(),[&](const auto& s){
                    const float dx=x-std::clamp(x,s.origin.x,s.origin.x+s.width);
                    const float dz=z-std::clamp(z,s.origin.z,s.origin.z+s.depth);
                    return std::hypot(dx,dz)<radius;
                });
                if(building)continue;
                bool fits=true;
                for(unsigned k=0;k<8;++k){
                    const float a=XM_2PI*float(k)/8.f;
                    const auto edge=dry(x+radius*std::cos(a),z+radius*std::sin(a));
                    if(!edge||std::abs(*edge-*y)>.10f*Units){fits=false;break;}
                }
                if(!fits)continue;
                const float field=Field(x,z),roll=Random(seed^0x33u);
                const unsigned family=field<.34f?3u:field<.52f?1u:field>.76f&&roll<.65f?2u:0u;
                const float a=XM_PI*Random(seed^0x44u);
                const Pose pose{{x,*y+.005f*Units,z},{0.f,std::sin(a),0.f,std::cos(a)}};
                placements_.push_back({pose,family*2u+unsigned(Random(seed^0x55u)>.5f)});
                // Fallen wood has its own real geometry and rare ecological
                // occurrence (~one candidate per 200 square metres).
                if(Random(seed^0x66u)<.02f)placements_.push_back({pose,8u});
            }

        // ARB-5.2 is additive: the validated 0-30 cm carpet is untouched.
        // Colony acceptance is deterministic in world coordinates, never
        // driven by the camera. 4 plants at 1.95 m and ~70% occupancy
        // corresponds to ~0.71 plants/m2 before geographical exclusions.
        constexpr float patchSpacing=1.95f*Units,patchRadius=1.48f*Units;
        for(int32_t iz=int32_t(std::floor(front/patchSpacing));float(iz)*patchSpacing<back;++iz)
            for(int32_t ix=int32_t(std::floor(left/patchSpacing));float(ix)*patchSpacing<right;++ix){
                const auto seed=Seed(ix,iz)^0x55325242u;
                const float x=(float(ix)+.5f+.58f*(Random(seed^0x11u)-.5f))*patchSpacing;
                const float z=(float(iz)+.5f+.58f*(Random(seed^0x22u)-.5f))*patchSpacing;
                const auto y=dry(x,z);if(!y)continue;
                const float cluster=Field(x+17.f*Units,z-13.f*Units);
                const float habitat=Field(x-41.f*Units,z+22.f*Units);
                // Sparse linked openings, with an irregular width, provide
                // low-eye-level sightlines without deleting nearby plants.
                const float xm=(x-SpectacleCenter.x)/Units,zm=(z-SpectacleCenter.z)/Units;
                const float corridorA=std::abs(xm-7.f*std::sin(zm*.090f)-3.f*std::sin(zm*.23f));
                const float corridorB=std::abs(zm-6.f*std::sin(xm*.084f)+2.f*std::sin(xm*.21f));
                if(std::min(corridorA,corridorB)<2.4f||cluster<.24f)continue;
                const float occupation=.48f+.43f*cluster;
                if(Random(seed^0x33u)>occupation)continue;
                const bool nearBuilding=std::any_of(sites.begin(),sites.end(),[&](const auto& site){
                    const float dx=x-std::clamp(x,site.origin.x,site.origin.x+site.width);
                    const float dz=z-std::clamp(z,site.origin.z,site.origin.z+site.depth);
                    return std::hypot(dx,dz)<patchRadius;
                });
                if(nearBuilding)continue;
                bool fits=true;
                for(unsigned k=0;k<8u;++k){
                    const float a=XM_2PI*float(k)/8.f;
                    const auto edge=dry(x+patchRadius*std::cos(a),z+patchRadius*std::sin(a));
                    if(!edge||std::abs(*edge-*y)>.10f*Units){fits=false;break;}
                }
                if(!fits)continue;
                // Preserve a visible contribution from all five families.
                // The correlated field makes adjacent cells favor the same
                // species, while the individual seed breaks up repetition.
                const float roll=Random(seed^0x45u);
                const unsigned family=roll<.17f?0u:roll<.45f?1u:roll<.845f?2u:roll<.96f?3u:4u;
                constexpr std::array<unsigned,5> first{0,8,13,23,26};
                constexpr std::array<unsigned,5> count{8,5,10,3,4};
                const float choice=.68f*habitat+.32f*Random(seed^0x56u);
                const unsigned species=first[family]+std::min(count[family]-1u,unsigned(choice*count[family]));
                // Independent occasional immigrants preserve the rare ends
                // of each species range in addition to colony-dominant forms.
                const unsigned chosen=Random(seed^0x58u)<.18f?
                    first[family]+std::min(count[family]-1u,unsigned(Random(seed^0x59u)*count[family])):species;
                unsigned variant=std::min(3u,unsigned(Random(seed^0x67u)*4.f));
                // The floor already contains short grass, ferns and shrubs.
                // In those communities favor the taller morphology rather
                // than simply doubling the existing 30-50 cm stratum.
                const float floorField=Field(x,z);
                if(floorField>=.34f&&floorField<.52f&&family==1u)variant=std::max(variant,1u);
                if(floorField>.76f&&family==0u)variant=std::max(variant,1u);
                const float a=XM_PI*Random(seed^0x78u);
                const Pose pose{{x,*y+.005f*Units,z},{0.f,std::sin(a),0.f,std::cos(a)}};
                understory_.push_back({pose,chosen*Understory::Variants+variant});
            }
    }
    void Install(SceneData& scene,WorldRegistry& world)const{
        if(placements_.empty())return;
        const auto models=BuildForestFloorSources(scene);
        std::vector<WorldObject> objects;objects.reserve(placements_.size());
        std::array<size_t,5> counts{};
        for(const auto& p:placements_){
            auto object=models.at(p.model);
            object.current=object.previous=p.pose;
            object.scale={Units,Units,Units};
            object.tileLayer=64; // same resident forest domain, no new pass
            objects.push_back(object);
            ++counts[p.model/2u];
        }
        world.CreateBatch(objects);
        const auto plants=10u*(counts[0]+counts[1]+counts[2]+counts[3]);
        char line[384];std::snprintf(line,sizeof(line),
            "arb5_floor grass_patches=%zu fern_patches=%zu shrub_patches=%zu moss_patches=%zu deadwood=%zu live_plants=%zu routes=%zu dry_candidate_m2=%.2f plants_per_m2=%.3f\r\n",
            counts[0],counts[1],counts[2],counts[3],counts[4],plants,objects.size(),dryArea_,
            dryArea_>0.?double(plants)/dryArea_:0.);
        DiagnosticLine(line);

        if(understory_.empty())return;
        std::array<bool,Understory::ModelCount> used{};
        for(const auto& p:understory_)used.at(p.model)=true;
        const auto sources=BuildUnderstorySources(scene,used);
        objects.clear();objects.reserve(understory_.size());
        std::array<size_t,5> population{};
        std::array<bool,Understory::SpeciesList.size()> present{};
        for(const auto& p:understory_){
            auto object=sources.at(p.model);
            object.current=object.previous=p.pose;
            object.scale={Units,Units,Units};
            object.tileLayer=64;
            objects.push_back(object);
            const unsigned species=p.model/Understory::Variants;
            const auto& definition=Understory::SpeciesList[species];
            population[size_t(definition.family)]+=Understory::Individuals(species);
            present[species]=true;
        }
        world.CreateBatch(objects);
        const auto total=population[0]+population[1]+population[2]+population[3]+population[4];
        char summary[512];std::snprintf(summary,sizeof(summary),
            "arb5_2_understory shrubs=%zu ferns=%zu herbs=%zu grasses=%zu saplings=%zu species=%zu forms=%zu plants=%zu routes=%zu added_plants_m2=%.3f\r\n",
            population[0],population[1],population[2],population[3],population[4],
            size_t(std::count(present.begin(),present.end(),true)),size_t(std::count(used.begin(),used.end(),true)),
            total,objects.size(),dryArea_>0.?double(total)/dryArea_:0.);
        DiagnosticLine(summary);
    }
};
}
