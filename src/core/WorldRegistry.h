#pragma once
#include "GpuSceneCache.h"
#include "TerritorialPatrimony.h"
#include "ArchitecturePatrimony.h"
#include "RenderContributions.h"
#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>

namespace OASIS {
class Graphics;


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
    ArchitectureAttachment architecture{};
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
    Graphics* forestAdmission_{}; // session owner, not owned
    std::unordered_set<uint64_t> reservedMutationIdentities_;
    bool MutationIdentityAllowed(uint64_t)const noexcept;
    void FinishForestMutation(uint64_t);
    TerritorialPatrimony vegetationPatrimony_,forestPatrimony_;
    ArchitecturePatrimony architecturePatrimony_;
    std::unordered_map<uint64_t,CanopyPlant> canopyPlants_;
    void ForgetCanopyRoute(uint64_t);
    void RefreshPatrimony();
    uint64_t EraseBatchImpl(std::span<const uint64_t>,bool forgetCanopy);
    std::unordered_map<uint64_t,GpuTileKey> owners_;
    uint64_t nextIdentity_{1};
    uint32_t nextRenderId_{1};
    static GpuTileKey TileOf(const WorldObject&);
    static GpuResidentTileObject Record(const WorldObject&,uint64_t,uint32_t);
    GpuResidentTile CopyTile(GpuTileKey) const;
public:
    void BindForestAdmission(Graphics* graphics)noexcept{forestAdmission_=graphics;}
    uint64_t ForestAdmissionEpoch()const noexcept;
    // Reserve logical identities before an asynchronous insertion. The caller
    // owns the batch and retries it until committed, or explicitly cancels it.
    std::vector<uint64_t> ReserveMutationIdentities(size_t count);
    void CancelMutation(std::span<const uint64_t> identities);
    uint64_t Create(const WorldObject&);


    struct Change {uint64_t identity;WorldObject object;};
    std::vector<uint64_t> CreateBatch(std::span<const WorldObject>,std::span<const uint64_t> stableIdentities={});
    // Pairs are opaque bark and cutout foliage, in that order.
    void CreateCanopyBatch(std::span<const WorldObject>,std::span<const uint32_t> assemblies,
                           std::span<const uint64_t> stableIdentities={});
    // Atomic two-route creation; false means pending, no plant published.
    bool TryCreateCanopyBatch(std::span<const WorldObject>,std::span<const uint32_t>,
                              std::span<const uint64_t> stableIdentities);
    const CanopyPlant* FindCanopyPlant(uint64_t identity) const noexcept;
    bool UpdateCanopyPlant(uint64_t identity,uint32_t assembly,
                           const std::array<WorldObject,2>&,bool morphology,bool transform,bool state);
    bool EraseCanopyPlant(uint64_t identity);
    std::vector<uint64_t> ApplyBatch(std::span<const Change>,std::span<const uint64_t> removed,
                                     std::span<const WorldObject> added,
                                     std::span<const uint64_t> stableIdentities={});
    // nullopt: producer pending; no changes/removals/insertions applied.
    // Deferred additions must use stable or pre-reserved identities.
    std::optional<std::vector<uint64_t>> TryApplyBatch(std::span<const Change>,std::span<const uint64_t> removed,
                                     std::span<const WorldObject> added,std::span<const uint64_t> stableIdentities={});
    uint32_t RenderId(uint64_t identity)const;
    bool Update(uint64_t identity,const WorldObject&);
    bool Erase(uint64_t identity);
    uint64_t EraseBatch(std::span<const uint64_t> identities);
    uint64_t Count()const noexcept{return owners_.size();}
    uint64_t TileCount()const noexcept{return cache_.ResidentTiles().size();}
    const GpuSceneCache& Resident()const noexcept{return cache_;}
    TerritorialPatrimony& VegetationPatrimony() noexcept{return vegetationPatrimony_;}
    const TerritorialPatrimony& VegetationPatrimony()const noexcept{return vegetationPatrimony_;}
    TerritorialPatrimony& ForestPatrimony() noexcept{return forestPatrimony_;}
    const TerritorialPatrimony& ForestPatrimony()const noexcept{return forestPatrimony_;}
    const ArchitecturePatrimony& Architecture()const noexcept{return architecturePatrimony_;}
    ArchitecturePatrimony& Architecture()noexcept{return architecturePatrimony_;}
    void SetViewer(double x,double z)noexcept{cache_.SetTileViewer(x,z);}
    RenderContributions Contributions()noexcept;
};
}
