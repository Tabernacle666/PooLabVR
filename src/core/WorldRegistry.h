#pragma once
#include "GpuSceneCache.h"
#include "RenderContributions.h"
#include <array>
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
    bool enabled{true};
    int32_t tileLayer{};
};

// Logical plant identity is the first of its two existing resident routes.
// This is an ownership relation, not another population or GPU instance table.
struct CanopyPlant {
    uint64_t identity{};
    std::array<uint64_t,2> routes{};
    uint32_t assembly{};
    uint64_t morphologyRevision{1},transformRevision{1},stateRevision{1};
};
class WorldRegistry {
    GpuSceneCache cache_;
    std::unordered_map<uint64_t,CanopyPlant> canopyPlants_;
    void ForgetCanopyRoute(uint64_t);
    uint64_t EraseBatchImpl(std::span<const uint64_t>,bool forgetCanopy);
    std::unordered_map<uint64_t,GpuTileKey> owners_;
    uint64_t nextIdentity_{1};
    uint32_t nextRenderId_{1};
    static GpuTileKey TileOf(const WorldObject&);
    static GpuResidentTileObject Record(const WorldObject&,uint64_t,uint32_t);
    GpuResidentTile CopyTile(GpuTileKey) const;
public:
    uint64_t Create(const WorldObject&);


    struct Change {uint64_t identity;WorldObject object;};
    std::vector<uint64_t> CreateBatch(std::span<const WorldObject>);
    // Pairs are opaque bark and cutout foliage, in that order.
    void CreateCanopyBatch(std::span<const WorldObject>,std::span<const uint32_t> assemblies);
    const CanopyPlant* FindCanopyPlant(uint64_t identity) const noexcept;
    bool UpdateCanopyPlant(uint64_t identity,uint32_t assembly,
                           const std::array<WorldObject,2>&,bool morphology,bool transform,bool state);
    bool EraseCanopyPlant(uint64_t identity);
    std::vector<uint64_t> ApplyBatch(std::span<const Change>,std::span<const uint64_t> removed,
                                     std::span<const WorldObject> added);
    uint32_t RenderId(uint64_t identity)const;
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
