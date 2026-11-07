#pragma once
#include "GpuMeshTypes.h"
#include <array>
#include <cstdint>
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

    uint32_t StaticCount() const noexcept { return uint32_t(instances_.size()); }
    uint64_t ContentGeneration() const noexcept { return contentGeneration_; }
    std::span<const GpuInstance> StaticInstances() const noexcept { return instances_; }
    const GpuSceneCacheStats& Stats() const noexcept { return stats_; }

private:
    std::vector<GpuInstance> instances_;
    std::array<std::vector<GpuSceneDirtyRange>, FrameSlots> pending_;
    GpuSceneCacheStats stats_{};
    uint64_t contentGeneration_{};

    void MarkRange(uint32_t first, uint32_t count);
    void RefreshGeneration();
    static void MergeRange(std::vector<GpuSceneDirtyRange>& ranges, GpuSceneDirtyRange range);
};
}
