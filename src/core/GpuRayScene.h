#pragma once
#include "GpuMeshTypes.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace lab {
inline constexpr uint32_t GpuRayNoTracingFlag = 1u << 5;

struct GpuRayDirtyRange { uint32_t first{}, count{}; };
struct GpuRayPublicationState { uint64_t generation{}; uint32_t base{}, capacity{}; };

// CPU-side dirty tracker for the persistent ray scene. It deliberately owns no
// D3D objects: Graphics keeps the TLAS/descriptor buffers resident while this
// class decides which descriptor ranges and stream generations actually changed.
class GpuRayScene {
public:
    static constexpr uint32_t FrameSlots = 3;

    void Reset();
    void ReplaceStatic(std::span<const GpuInstance> instances);
    void UpdateDynamic(uint32_t staticCount, std::span<const GpuInstance> instances);
    void InvalidateFrame(uint32_t frameSlot);
    void StagePublications(uint32_t frameSlot, std::span<const GpuRayPublicationState> publications);
    void MarkGeometryDirty(uint32_t frameSlot);

    std::span<const GpuRayDirtyRange> PendingCpu(uint32_t frameSlot) const;
    std::span<const uint32_t> PendingPublications(uint32_t frameSlot) const;
    bool NeedsTlas(uint32_t frameSlot) const;
    void Commit(uint32_t frameSlot);

    uint32_t CpuCount() const noexcept { return uint32_t(keys_.size()); }
    uint32_t StaticCount() const noexcept { return staticCount_; }
    uint64_t StaticGeneration() const noexcept { return staticGeneration_; }

private:
    struct Key {
        uint32_t bits[16]{};
        bool operator==(const Key&) const = default;
    };
    struct FrameState {
        std::vector<GpuRayDirtyRange> cpuDirty;
        std::vector<GpuRayPublicationState> publications;
        std::vector<GpuRayPublicationState> stagedPublications;
        std::vector<uint32_t> publicationDirty;
        bool cpuTopologyDirty{}, publicationTopologyDirty{}, geometryDirty{};
    };

    uint32_t staticCount_{};
    uint64_t staticGeneration_{};
    std::vector<Key> keys_;
    std::array<FrameState, FrameSlots> frames_{};

    static Key MakeKey(const GpuInstance& instance);
    static void MergeRange(std::vector<GpuRayDirtyRange>& ranges, GpuRayDirtyRange range);
    void MarkRange(uint32_t first, uint32_t count);
    void ClipRanges(uint32_t count);
};
}
