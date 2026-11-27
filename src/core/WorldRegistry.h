#pragma once
#include "GpuSceneCache.h"
#include "RenderContributions.h"
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace OASIS {


// Identity is independent of the resident tile, publication order and GPU slot.
// High-half namespace: producer (3 bits), signed lattice X/Z (29 bits each),
// and up to four subroutes. Adjacent bark/foliage routes remain consecutive.
enum class TerritorialProducer : uint32_t { Tree=1, Floor=2, Understory=3, Insect=4 };
inline uint64_t TerritorialIdentity(TerritorialProducer producer,int32_t x,int32_t z,uint32_t sub=0){
    constexpr int32_t limit=1<<28;
    if(x<-limit||x>=limit||z<-limit||z>=limit||sub>=4u)Fail("Territorial identity range exhausted");
    const auto encode=[](int32_t v){return uint64_t(int64_t(v)+(1ll<<28));};
    return (1ull<<63)|(uint64_t(producer)<<60)|(encode(x)<<31)|(encode(z)<<2)|sub;
}
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
    std::vector<uint64_t> CreateBatch(std::span<const WorldObject>,std::span<const uint64_t> stableIdentities={});
    // Pairs are opaque bark and cutout foliage, in that order.
    void CreateCanopyBatch(std::span<const WorldObject>,std::span<const uint32_t> assemblies,
                           std::span<const uint64_t> stableIdentities={});
    const CanopyPlant* FindCanopyPlant(uint64_t identity) const noexcept;
    bool UpdateCanopyPlant(uint64_t identity,uint32_t assembly,
                           const std::array<WorldObject,2>&,bool morphology,bool transform,bool state);
    bool EraseCanopyPlant(uint64_t identity);
    std::vector<uint64_t> ApplyBatch(std::span<const Change>,std::span<const uint64_t> removed,
                                     std::span<const WorldObject> added,
                                     std::span<const uint64_t> stableIdentities={});
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
