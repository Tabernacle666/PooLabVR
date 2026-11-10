#pragma once
#include "GpuMeshTypes.h"
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace lab {
struct GpuSceneDirtyRange {
    uint32_t first{}, count{};
};

struct GpuSceneCacheStats {
    uint64_t replacements{}, replacementBytes{};
    uint64_t frameCopies{}, frameCopyBytes{};
};

// Metadata-only resident tile catalog. This is not a GPU upload in step 10;
// the existing static scene and per-frame rendering are unchanged.
struct GpuTileKey {
    int64_t x{},z{};
    bool operator==(const GpuTileKey&)const=default;
    bool operator<(const GpuTileKey& other)const noexcept{
        return x<other.x||(x==other.x&&z<other.z);
    }
};
struct GpuResidentTileObject {
    uint64_t identity{}; // Authoritative world/tile identity; never an array offset.
    uint32_t renderId{},mesh{},material{},flags{};
    GpuFloat4 position{},previousPosition{},rotation{},previousRotation{},scale{};
    uint32_t cell{};
    uint64_t revision{1};
};
struct GpuResidentTile {
    static constexpr uint32_t Cells=36; // Six-by-six twenty-metre world cells.
    GpuTileKey key{};
    uint64_t revision{1};
    std::vector<GpuResidentTileObject> objects;
    // Links are persistent 64-bit identities, not transient render indices.
    std::array<std::vector<uint64_t>,Cells> cellObjects{};
    std::map<uint64_t,uint32_t> byIdentity;
    std::map<uint32_t,uint64_t> byRenderId;
};

// CPU mirror + per-frame residency state for the persistent static MeshSolid region.
// The canonical static data is uploaded by Graphics only at scene replacement; frame
// instance buffers receive only pending dirty ranges and keep them until invalidated.
class GpuSceneCache {
public:
    static constexpr uint32_t FrameSlots = 3;

    void Reset();
    void Replace(std::vector<GpuInstance> instances);
    bool Patch(uint32_t first, std::span<const GpuInstance> instances);
    void InvalidateFrame(uint32_t frameSlot);
    std::span<const GpuSceneDirtyRange> Pending(uint32_t frameSlot) const;
    void Commit(uint32_t frameSlot);

    // Tile records are independent of the temporary visible-object list.
    // Upsert preserves revisions for unchanged objects and rejects collisions.
    void UpsertTile(GpuResidentTile tile);
    void RemoveTile(GpuTileKey key);
    void ClearTileRecords() noexcept { residentTiles_.clear(); }
    const std::map<GpuTileKey,GpuResidentTile>& ResidentTiles()const noexcept{
        return residentTiles_;
    }

    uint32_t StaticCount() const noexcept { return uint32_t(instances_.size()); }
    uint64_t ContentGeneration() const noexcept { return contentGeneration_; }
    std::span<const GpuInstance> StaticInstances() const noexcept { return instances_; }
    const GpuSceneCacheStats& Stats() const noexcept { return stats_; }

private:
    std::vector<GpuInstance> instances_;
    std::map<GpuTileKey,GpuResidentTile> residentTiles_;
    std::array<std::vector<GpuSceneDirtyRange>, FrameSlots> pending_;
    GpuSceneCacheStats stats_{};
    uint64_t contentGeneration_{};

    void MarkRange(uint32_t first, uint32_t count);
    void RefreshGeneration();
    static void MergeRange(std::vector<GpuSceneDirtyRange>& ranges, GpuSceneDirtyRange range);
};
}
