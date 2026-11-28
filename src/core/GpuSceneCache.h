#pragma once
#include "GpuMeshTypes.h"
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OASIS {
inline constexpr double TerritorialTileMeters=10.0;
inline constexpr int32_t TerrainTileLayer=66;
// Expansion canopy keeps 10m logical cells and individual instance routes.
// A 4x4-cell ownership page contains about 128 routes at 400 trees/ha,
// before density variation; it is not a limit on route count or visibility.
inline constexpr int32_t ExpansionCanopyTileLayer=68;
inline constexpr int64_t ExpansionCanopyPageCells=4;
struct GpuSceneDirtyRange {
    uint32_t first{}, count{};
};

struct GpuSceneCacheStats {
    uint64_t replacements{}, replacementBytes{};
    uint64_t frameCopies{}, frameCopyBytes{};
};


struct GpuTileKey {
    int64_t x{},z{};
    int32_t layer{};
    bool operator==(const GpuTileKey&)const=default;
    bool operator<(const GpuTileKey& other)const noexcept{
        if(x!=other.x)return x<other.x;
        if(z!=other.z)return z<other.z;
        return layer<other.layer;
    }
};
inline constexpr int64_t ExpansionCanopyPageOrigin(int64_t cell) noexcept {
    const auto remainder=cell%ExpansionCanopyPageCells;
    return cell-(remainder<0?remainder+ExpansionCanopyPageCells:remainder);
}
inline constexpr GpuTileKey ExpansionCanopyPageKey(int64_t x,int64_t z) noexcept {
    return {ExpansionCanopyPageOrigin(x),ExpansionCanopyPageOrigin(z),ExpansionCanopyTileLayer};
}
struct GpuResidentTileObject {
    uint64_t identity{};
    uint32_t renderId{},mesh{},material{},flags{};
    GpuFloat4 position{},previousPosition{},rotation{},previousRotation{},scale{};
    uint32_t cell{};

    float cellX{},cellZ{},cellRadius{};
    uint32_t enabled{1};
    uint64_t revision{1};
};
struct GpuResidentTileGpuInstance {
    GpuInstance instance{};
    GpuFloat4 sector{};
    // Keep the 96-byte GPU record and enabled offset stable. The first
    // two words carry the persistent 64-bit MG2 instance identity.
    uint32_t padding0{},padding1{},enabled{1},padding2{};
};
static_assert(sizeof(GpuResidentTileGpuInstance)==96);

struct GpuResidentTile {
    static constexpr uint32_t Cells=1;
    GpuTileKey key{};
    uint64_t revision{1};
    uint64_t topologyGeneration{};
    std::vector<GpuResidentTileObject> objects;

    mutable std::vector<uint32_t> modifiedIndices;
    mutable std::vector<uint8_t> modifiedFlags;


    mutable std::array<std::vector<uint32_t>,3> rayModifiedIndices;
    mutable std::array<std::vector<uint8_t>,3> rayModifiedFlags;
    std::map<uint32_t,uint32_t> materialCounts;
    struct DeformationUse {uint32_t total{},ray{};};
    std::map<uint32_t,DeformationUse> deformableMeshes;

    std::array<std::vector<uint64_t>,Cells> cellObjects{};


    std::array<uint64_t,Cells> shadowCellSignatures{};
    std::array<uint64_t,Cells> shadowCellRevisions{};
    std::array<uint32_t,Cells> shadowCasterCounts{};
    struct ShadowCellVolume {
        float x{},z{},radius{};
        std::vector<uint32_t> materials;
    };
    std::array<ShadowCellVolume,Cells> shadowVolumes;
    std::map<uint64_t,uint32_t> byIdentity;
    std::map<uint32_t,uint64_t> byRenderId;
};


class GpuSceneCache {
public:
    static constexpr uint32_t FrameSlots = 3;

    void Reset();
    void Replace(std::vector<GpuInstance> instances);
    bool Patch(uint32_t first, std::span<const GpuInstance> instances);
    void InvalidateFrame(uint32_t frameSlot);
    std::span<const GpuSceneDirtyRange> Pending(uint32_t frameSlot) const;
    void Commit(uint32_t frameSlot);


    void UpsertTile(GpuResidentTile tile);
    bool PatchTileObject(GpuTileKey key,uint64_t identity,
                         const GpuResidentTileObject& value);
    void RemoveTile(GpuTileKey key);
    // Existing resident ownership index; no secondary world registry.
    const GpuResidentTile* FindResidentOwner(uint64_t identity,uint32_t& index) const noexcept;
    void CommitTileChanges(GpuTileKey key,uint64_t topologyGeneration,
        std::span<const std::pair<uint32_t,uint64_t>> updates) const;
    void CommitRayTileChanges(uint32_t frameSlot,GpuTileKey key,
        uint64_t topologyGeneration,
        std::span<const std::pair<uint32_t,uint64_t>> updates) const;
    void ClearTileRecords() noexcept {
        residentTiles_.clear();
        residentIdentityOwners_.clear();
        residentRenderOwners_.clear();
        forestDirtyTiles_.clear();
        forestTileReset_=true;
        forestAdmissionDirtyTiles_.clear();
        forestAdmissionReset_=true;
        residentSlotCount_=0;
        residentMaterials_.clear();
        residentNonCampusMaterials_.clear();
        residentDeformableMeshes_.clear();
    }
    // Independent MG2 consumer; raster and DXR acknowledgements do not
    // consume this journal. Access is confined to the render owner.
    const std::set<GpuTileKey>& ForestDirtyTiles()const noexcept{return forestDirtyTiles_;}
    bool ForestTileResetPending()const noexcept{return forestTileReset_;}
    void CommitForestTiles()const noexcept{forestDirtyTiles_.clear();forestTileReset_=false;}
    // Independent publication journal: selection may acknowledge its inputs
    // before the matching generation is ready for raster/RTX.
    const std::set<GpuTileKey>& ForestAdmissionDirtyTiles()const noexcept{return forestAdmissionDirtyTiles_;}
    bool ForestAdmissionResetPending()const noexcept{return forestAdmissionReset_;}
    void CommitForestAdmissionTiles()const noexcept{forestAdmissionDirtyTiles_.clear();forestAdmissionReset_=false;}
    const std::map<GpuTileKey,GpuResidentTile>& ResidentTiles()const noexcept{
        return residentTiles_;
    }
    // Exact summaries of the existing owner map, maintained at mutation.
    // Slot count includes vacant slots: raster and DXR ordinals depend on it.
    // Material and deformation use preserve tile usage semantics, including
    // disabled records with an identity and the existing campus filter.
    struct ResidentDeformationUse {uint64_t total{},ray{};};
    uint64_t ResidentSlotCount()const noexcept{return residentSlotCount_;}
    const std::map<uint32_t,uint64_t>& ResidentMaterials(bool campusVisible)const noexcept{
        return campusVisible?residentMaterials_:residentNonCampusMaterials_;
    }
    const std::map<uint32_t,ResidentDeformationUse>& ResidentDeformableMeshes()const noexcept{
        return residentDeformableMeshes_;
    }
    void SetTileViewer(double x,double z) noexcept{tileViewerX_=x;tileViewerZ_=z;}
    double TileViewerX()const noexcept{return tileViewerX_;}
    double TileViewerZ()const noexcept{return tileViewerZ_;}

    uint32_t StaticCount() const noexcept { return uint32_t(instances_.size()); }
    uint64_t ContentGeneration() const noexcept { return contentGeneration_; }
    std::span<const GpuInstance> StaticInstances() const noexcept { return instances_; }
    const GpuSceneCacheStats& Stats() const noexcept { return stats_; }

private:
    std::vector<GpuInstance> instances_;
    std::map<GpuTileKey,GpuResidentTile> residentTiles_;
    uint64_t residentSlotCount_{};
    std::map<uint32_t,uint64_t> residentMaterials_,residentNonCampusMaterials_;
    std::map<uint32_t,ResidentDeformationUse> residentDeformableMeshes_;
    void ChangeResidentUsage(const GpuResidentTile& tile,bool add);
    std::unordered_map<uint64_t,GpuTileKey> residentIdentityOwners_;
    std::unordered_map<uint32_t,GpuTileKey> residentRenderOwners_;
    mutable std::set<GpuTileKey> forestDirtyTiles_;
    mutable bool forestTileReset_{true};
    mutable std::set<GpuTileKey> forestAdmissionDirtyTiles_;
    mutable bool forestAdmissionReset_{true};
    uint64_t nextTileTopologyGeneration_{};
    double tileViewerX_{},tileViewerZ_{};
    std::array<std::vector<GpuSceneDirtyRange>, FrameSlots> pending_;
    GpuSceneCacheStats stats_{};
    uint64_t contentGeneration_{};

    void MarkRange(uint32_t first, uint32_t count);
    void RefreshGeneration();
    static void MergeRange(std::vector<GpuSceneDirtyRange>& ranges, GpuSceneDirtyRange range);
};
}
