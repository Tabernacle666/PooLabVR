#pragma once
#include "GpuMeshTypes.h"
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lab {
struct GpuSceneDirtyRange {
    uint32_t first{}, count{};
};

struct GpuSceneCacheStats {
    uint64_t replacements{}, replacementBytes{};
    uint64_t frameCopies{}, frameCopyBytes{};
};


struct GpuTileKey {
    int64_t x{},z{};
    bool operator==(const GpuTileKey&)const=default;
    bool operator<(const GpuTileKey& other)const noexcept{
        return x<other.x||(x==other.x&&z<other.z);
    }
};
struct GpuResidentTileObject {
    uint64_t identity{};
    uint32_t renderId{},mesh{},material{},flags{};
    GpuFloat4 position{},previousPosition{},rotation{},previousRotation{},scale{};
    uint32_t cell{};

    float cellX{},cellZ{},cellRadius{};
    uint32_t farMesh{UINT32_MAX},hideFar{},enabled{1};
    uint64_t revision{1};
};
struct GpuResidentTileGpuInstance {
    GpuInstance instance{};
    GpuFloat4 sector{};
    uint32_t farMesh{UINT32_MAX},hideFar{},enabled{1},reserved{};
};
static_assert(sizeof(GpuResidentTileGpuInstance)==96);

struct GpuResidentTile {
    static constexpr uint32_t Cells=36;
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
    void CommitTileChanges(GpuTileKey key,uint64_t topologyGeneration,
        std::span<const std::pair<uint32_t,uint64_t>> updates) const;
    void CommitRayTileChanges(uint32_t frameSlot,GpuTileKey key,
        uint64_t topologyGeneration,
        std::span<const std::pair<uint32_t,uint64_t>> updates) const;
    void ClearTileRecords() noexcept {
        residentTiles_.clear();
        residentIdentityOwners_.clear();
        residentRenderOwners_.clear();
    }
    const std::map<GpuTileKey,GpuResidentTile>& ResidentTiles()const noexcept{
        return residentTiles_;
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
    std::unordered_map<uint64_t,GpuTileKey> residentIdentityOwners_;
    std::unordered_map<uint32_t,GpuTileKey> residentRenderOwners_;
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
