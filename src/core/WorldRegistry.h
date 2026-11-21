#pragma once
#include "GpuSceneCache.h"
#include "RenderContributions.h"
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace OASIS {


struct WorldObject {
    Pose current{},previous{};
    XMFLOAT3 scale{1.f,1.f,1.f};
    uint32_t mesh{},material{},flags{};
    float boundRadius{1.f};
    uint32_t farMesh{UINT32_MAX},hideFar{};
    bool enabled{true};
    int32_t tileLayer{};
};

class WorldRegistry {
    GpuSceneCache cache_;
    std::unordered_map<uint64_t,GpuTileKey> owners_;
    uint64_t nextIdentity_{1};
    uint32_t nextRenderId_{1};
    static GpuTileKey TileOf(const WorldObject&);
    static GpuResidentTileObject Record(const WorldObject&,uint64_t,uint32_t);
    GpuResidentTile CopyTile(GpuTileKey) const;
public:
    uint64_t Create(const WorldObject&);


    std::vector<uint64_t> CreateBatch(std::span<const WorldObject>);
    bool Update(uint64_t identity,const WorldObject&);
    bool Erase(uint64_t identity);
    uint64_t EraseBatch(std::span<const uint64_t> identities);
    uint64_t Count()const noexcept{return owners_.size();}
    uint64_t TileCount()const noexcept{return cache_.ResidentTiles().size();}
    const GpuSceneCache& Resident()const noexcept{return cache_;}
    void SetViewer(double x,double z)noexcept{cache_.SetTileViewer(x,z);}
    RenderContributions Contributions()noexcept;
};
}
