#pragma once
#include "Assets.h"
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
    std::vector<Placement> placements_;
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
        placements_.clear();dryArea_=0.;
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
    }
};
}
