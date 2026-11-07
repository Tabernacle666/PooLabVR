#include "GpuSceneCache.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace lab {
void GpuSceneCache::RefreshGeneration() {
    uint64_t hash=1469598103934665603ull;
    const auto* bytes=reinterpret_cast<const uint8_t*>(instances_.data());
    for(size_t i=0;i<instances_.size()*sizeof(GpuInstance);++i){hash^=bytes[i];hash*=1099511628211ull;}
    contentGeneration_=hash?hash:1ull;
}

void GpuSceneCache::Reset() {
    instances_.clear();
    for (auto& ranges : pending_) ranges.clear();
    stats_ = {};
    contentGeneration_ = 0;
}

void GpuSceneCache::Replace(std::vector<GpuInstance> instances) {
    instances_ = std::move(instances);
    for (auto& ranges : pending_) {
        ranges.clear();
        if (!instances_.empty()) ranges.push_back({0, uint32_t(instances_.size())});
    }
    RefreshGeneration();
    ++stats_.replacements;
    stats_.replacementBytes += uint64_t(instances_.size()) * sizeof(GpuInstance);
}

void GpuSceneCache::MergeRange(std::vector<GpuSceneDirtyRange>& ranges, GpuSceneDirtyRange range) {
    if (!range.count) return;
    uint64_t first = range.first;
    uint64_t end = first + range.count;
    auto it = ranges.begin();
    while (it != ranges.end() && uint64_t(it->first) + it->count < first) ++it;
    while (it != ranges.end() && uint64_t(it->first) <= end) {
        first = std::min<uint64_t>(first, it->first);
        end = std::max<uint64_t>(end, uint64_t(it->first) + it->count);
        it = ranges.erase(it);
    }
    ranges.insert(it, {uint32_t(first), uint32_t(end - first)});
}

void GpuSceneCache::MarkRange(uint32_t first, uint32_t count) {
    if (!count) return;
    for (auto& ranges : pending_) MergeRange(ranges, {first, count});
}

bool GpuSceneCache::Patch(uint32_t first, std::span<const GpuInstance> instances) {
    if (uint64_t(first) + instances.size() > instances_.size()) return false;
    uint32_t runFirst = 0, runCount = 0;
    bool changed = false;
    auto flush = [&] {
        if (runCount) { MarkRange(runFirst, runCount); runCount = 0; }
    };
    for (uint32_t i = 0; i < instances.size(); ++i) {
        uint32_t index = first + i;
        if (std::memcmp(&instances_[index], &instances[i], sizeof(GpuInstance)) == 0) {
            flush();
            continue;
        }
        instances_[index] = instances[i];
        if (!runCount) runFirst = index;
        ++runCount;
        changed = true;
    }
    flush();
    if(changed)RefreshGeneration();
    return changed;
}

void GpuSceneCache::InvalidateFrame(uint32_t frameSlot) {
    if (frameSlot >= FrameSlots) return;
    auto& ranges = pending_[frameSlot];
    ranges.clear();
    if (!instances_.empty()) ranges.push_back({0, uint32_t(instances_.size())});
}

std::span<const GpuSceneDirtyRange> GpuSceneCache::Pending(uint32_t frameSlot) const {
    if (frameSlot >= FrameSlots) return {};
    return pending_[frameSlot];
}

void GpuSceneCache::Commit(uint32_t frameSlot) {
    if (frameSlot >= FrameSlots) return;
    auto& ranges = pending_[frameSlot];
    if (!ranges.empty()) {
        ++stats_.frameCopies;
        for (const auto& range : ranges) stats_.frameCopyBytes += uint64_t(range.count) * sizeof(GpuInstance);
        ranges.clear();
    }
}
}
