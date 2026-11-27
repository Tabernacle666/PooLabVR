#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <cmath>
#include <algorithm>
#include <map>
#include <limits>
#include <vector>

namespace OASIS {
// The source catalog is immutable; WorldRegistry alone owns live routes.
// Terrain is ordinary rigid geometry, not a GPU cutting source. Building
// damage, collision queries, materials, and MG2 continue through their
// existing paths.
class TerrainTerritory {
    struct Source {std::vector<WorldObject> objects;std::vector<uint64_t> identities;};
    std::map<GpuTileKey,Source> sources_;
    int64_t lastX_{INT64_MIN},lastZ_{INT64_MIN};
    double lastReach_{};
    static uint64_t Identity(GpuTileKey key,size_t ordinal){
        constexpr int64_t limit=1ll<<23;
        if(key.x<-limit||key.x>=limit||key.z<-limit||key.z>=limit||ordinal>=4096u)
            Fail("Terrain territorial identity overflow");
        return (1ull<<63)|(5ull<<60)|(uint64_t(key.x+limit)<<36)|
            (uint64_t(key.z+limit)<<12)|uint64_t(ordinal);
    }
    static int64_t Coordinate(double p){return int64_t(std::floor(p/TerritorialTileMeters));}
public:
    void Install(SceneData& scene,WorldRegistry& world,XMFLOAT3 initialViewer){
        sources_.clear();lastX_=lastZ_=INT64_MIN;lastReach_=0.;
        if(!scene.solarSystem||scene.terrainSpans.empty())return;
        std::vector<const TerrainTileSpan*> ownership(scene.meshes.ranges.size(),nullptr);
        for(const auto& span:scene.terrainSpans){
            if(span.mesh>=ownership.size()||ownership[span.mesh])
                Fail("Terrain mesh crosses territorial ownership");
            ownership[span.mesh]=&span;
        }
        std::vector<RenderObject> staticObjects;
        staticObjects.reserve(scene.objects.size());
        for(const auto& source:scene.objects){
            const auto* span=ownership.at(source.mesh);
            if(!span){staticObjects.push_back(source);continue;}
            GpuTileKey key{span->tileX,span->tileZ,TerrainTileLayer};
            if(Coordinate(source.current.p.x)!=key.x||Coordinate(source.current.p.z)!=key.z)
                Fail("Terrain mesh origin outside its territory");
            auto& tile=sources_[key];
            WorldObject object{};
            object.current=source.current;object.previous=source.previous;
            object.scale=source.scale;object.mesh=source.mesh;object.material=source.material;
            object.flags=source.flags;object.tileLayer=TerrainTileLayer;
            const auto& bounds=scene.meshes.ranges[source.mesh].bounds;
            object.boundRadius=std::max(.001f,bounds.w+Length({bounds.x,bounds.y,bounds.z}));
            tile.identities.push_back(Identity(key,tile.objects.size()));
            tile.objects.push_back(object);
        }
        if(sources_.empty())return;
        scene.objects=std::move(staticObjects);
        scene.territorialTerrain=true;
        // Publish only the initial viewer's reachable territory. Keep the
        // authored geometry catalog separate from the resident world.
        Update(world,initialViewer,400.f);
    }
    void Update(WorldRegistry& world,XMFLOAT3 viewer,float farMeters){
        if(sources_.empty()||!std::isfinite(viewer.x)||!std::isfinite(viewer.z)||
           !std::isfinite(farMeters))return;
        const auto x=Coordinate(viewer.x),z=Coordinate(viewer.z);
        // Keep the full visible and ray-reachable landscape. This is not
        // the future pedestrian prefetch ring: far shadows must not vanish.
        const double reach=std::max(400.,2.*double(farMeters))+30.;
        if(lastX_==x&&lastZ_==z&&lastReach_==reach)return;
        lastX_=x;lastZ_=z;lastReach_=reach;
        const auto& resident=world.Resident().ResidentTiles();
        std::vector<uint64_t> removed,identities;
        std::vector<WorldObject> added;
        const auto distance=[&](const GpuTileKey& key){
            return std::hypot(double(key.x-x),double(key.z-z))*TerritorialTileMeters;
        };
        // Eviction visits resident routes only. No second live registry.
        for(const auto& [key,tile]:resident){
            if(key.layer!=TerrainTileLayer||distance(key)<=reach+2.*TerritorialTileMeters)continue;
            for(const auto& object:tile.objects)removed.push_back(object.identity);
        }
        // The sorted immutable source is indexed by X/Z, so the selection
        // cost depends on the reachable rectangle, not the continent area.
        const auto radius=int64_t(std::ceil(reach/TerritorialTileMeters));
        for(int64_t ix=x-radius;ix<=x+radius;++ix){
            auto it=sources_.lower_bound({ix,z-radius,TerrainTileLayer});
            for(;it!=sources_.end()&&it->first.x==ix&&it->first.z<=z+radius;++it){
                const auto& [key,tile]=*it;
                if(resident.contains(key)||distance(key)>reach)continue;
                identities.insert(identities.end(),tile.identities.begin(),tile.identities.end());
                added.insert(added.end(),tile.objects.begin(),tile.objects.end());
            }
        }
        if(!added.empty()||!removed.empty())world.ApplyBatch({},removed,added,identities);
    }
};
}
