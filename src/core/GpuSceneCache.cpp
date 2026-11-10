#include "GpuSceneCache.h"
#include "Core.h"
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
    residentTiles_.clear();
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

namespace {
bool SameResidentState(const GpuResidentTileObject& a,const GpuResidentTileObject& b){
    return a.renderId==b.renderId&&a.mesh==b.mesh&&a.material==b.material&&
        a.flags==b.flags&&a.cell==b.cell&&
        std::memcmp(&a.position,&b.position,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.previousPosition,&b.previousPosition,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.rotation,&b.rotation,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.previousRotation,&b.previousRotation,sizeof(GpuFloat4))==0&&
        std::memcmp(&a.scale,&b.scale,sizeof(GpuFloat4))==0;
}
}

void GpuSceneCache::UpsertTile(GpuResidentTile tile) {
    if(tile.objects.size()>UINT32_MAX)Fail("GPU resident tile object count exhausted");
    auto previous=residentTiles_.find(tile.key);
    tile.byIdentity.clear();
    tile.byRenderId.clear();
    for(auto& cell:tile.cellObjects)cell.clear();
    bool changed=previous==residentTiles_.end();
    for(uint32_t i=0;i<tile.objects.size();++i){
        auto& object=tile.objects[i];
        if(object.cell>=GpuResidentTile::Cells)
            Fail("GPU resident tile has invalid spatial cell");
        if(!tile.byIdentity.emplace(object.identity,i).second)
            Fail("GPU resident tile has duplicate 64-bit object identity");
        if(!tile.byRenderId.emplace(object.renderId,object.identity).second)
            Fail("GPU resident tile has collision in 32-bit render IDs");
        for(const auto& [key,other]:residentTiles_)if(!(key==tile.key)){
            if(other.byIdentity.contains(object.identity))
                Fail("GPU resident tiles have colliding 64-bit object identities");
            if(other.byRenderId.contains(object.renderId))
                Fail("GPU resident tiles have colliding 32-bit render IDs");
        }
        tile.cellObjects[object.cell].push_back(object.identity);
        if(previous!=residentTiles_.end()){
            const auto lookup=previous->second.byIdentity.find(object.identity);
            if(lookup==previous->second.byIdentity.end()){
                object.revision=1;changed=true;
            }else{
                const auto& old=previous->second.objects[lookup->second];
                if(SameResidentState(old,object))object.revision=old.revision;
                else{
                    if(old.revision==UINT64_MAX)Fail("GPU resident object revision exhausted");
                    object.revision=old.revision+1;changed=true;
                }
            }
        }else object.revision=1;
    }
    if(previous!=residentTiles_.end()){
        if(previous->second.objects.size()!=tile.objects.size())changed=true;
        if(changed){
            if(previous->second.revision==UINT64_MAX)Fail("GPU resident tile revision exhausted");
            tile.revision=previous->second.revision+1;
        }else tile.revision=previous->second.revision;
    }else tile.revision=1;
    residentTiles_.insert_or_assign(tile.key,std::move(tile));
}

void GpuSceneCache::RemoveTile(GpuTileKey key){
    residentTiles_.erase(key);
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
