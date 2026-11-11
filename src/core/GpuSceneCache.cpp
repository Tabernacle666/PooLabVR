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
    nextTileTopologyGeneration_=0;
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
uint64_t HashShadowPart(uint64_t hash,const void* bytes,size_t count){
    const auto* p=static_cast<const uint8_t*>(bytes);
    for(size_t i=0;i<count;++i){hash^=p[i];hash*=1099511628211ull;}
    return hash;
}
template<class T>uint64_t HashShadowPart(uint64_t hash,const T& value){
    return HashShadowPart(hash,&value,sizeof(value));
}
void UpdateShadowCell(GpuResidentTile& tile,uint32_t cell){
    uint64_t key=1469598103934665603ull;
    uint32_t casters=0;
    for(uint64_t identity:tile.cellObjects[cell]){
        const auto found=tile.byIdentity.find(identity);
        if(found==tile.byIdentity.end())Fail("Resident shadow cell has unknown object identity");
        const auto& o=tile.objects[found->second];
        if(!o.enabled||(o.flags&(DynamicObject|DeformedObject|NoShadow)))continue;
        ++casters;
        key=HashShadowPart(key,o.identity);
        key=HashShadowPart(key,o.mesh);
        key=HashShadowPart(key,o.material);
        key=HashShadowPart(key,o.flags);
        key=HashShadowPart(key,o.position);
        key=HashShadowPart(key,o.previousPosition);
        key=HashShadowPart(key,o.rotation);
        key=HashShadowPart(key,o.previousRotation);
        key=HashShadowPart(key,o.scale);
    }
    const uint64_t signature=casters?key:0ull;
    if(signature!=tile.shadowCellSignatures[cell]){
        if(tile.shadowCellRevisions[cell]==UINT64_MAX)
            Fail("Resident shadow cell revision exhausted");
        ++tile.shadowCellRevisions[cell];
    }
    tile.shadowCellSignatures[cell]=signature;
    tile.shadowCasterCounts[cell]=casters;
}
bool SameResidentState(const GpuResidentTileObject& a,const GpuResidentTileObject& b){
    return a.renderId==b.renderId&&a.mesh==b.mesh&&a.material==b.material&&
        a.flags==b.flags&&a.cell==b.cell&&
        a.cellX==b.cellX&&a.cellZ==b.cellZ&&a.cellRadius==b.cellRadius&&
        a.farMesh==b.farMesh&&a.hideFar==b.hideFar&&a.enabled==b.enabled&&
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
    bool topologyChanged=previous==residentTiles_.end()||
        previous->second.objects.size()!=tile.objects.size();
    if(!topologyChanged)
        for(uint32_t i=0;i<tile.objects.size();++i)
            if(previous->second.objects[i].identity!=tile.objects[i].identity){
                topologyChanged=true;
                break;
            }
    bool changed=topologyChanged;
    if(previous!=residentTiles_.end()){
        tile.shadowCellSignatures=previous->second.shadowCellSignatures;
        tile.shadowCellRevisions=previous->second.shadowCellRevisions;
    }
    if(topologyChanged){
        if(nextTileTopologyGeneration_==UINT64_MAX)
            Fail("Resident tile topology generation exhausted");
        tile.topologyGeneration=++nextTileTopologyGeneration_;
        tile.modifiedIndices.resize(tile.objects.size());
        for(uint32_t i=0;i<tile.objects.size();++i)tile.modifiedIndices[i]=i;
        tile.modifiedFlags.assign(tile.objects.size(),1u);
    }else{
        tile.topologyGeneration=previous->second.topologyGeneration;
        tile.modifiedIndices=previous->second.modifiedIndices;
        tile.modifiedFlags=previous->second.modifiedFlags;
    }
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
                    if(!tile.modifiedFlags[i]){
                        tile.modifiedFlags[i]=1u;
                        tile.modifiedIndices.push_back(i);
                    }
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
    for(uint32_t cell=0;cell<GpuResidentTile::Cells;++cell)
        UpdateShadowCell(tile,cell);
    residentTiles_.insert_or_assign(tile.key,std::move(tile));
}

bool GpuSceneCache::PatchTileObject(GpuTileKey key,uint64_t identity,
                                   const GpuResidentTileObject& value){
    auto found=residentTiles_.find(key);
    if(found==residentTiles_.end())Fail("GPU resident tile patch for missing key");
    auto& tile=found->second;
    auto pos=tile.byIdentity.find(identity);
    if(pos==tile.byIdentity.end())Fail("GPU resident tile patch for missing identity");
    auto& object=tile.objects[pos->second];
    if(object.identity!=value.identity||object.renderId!=value.renderId||
       object.cell!=value.cell)
        Fail("GPU resident tile patch attempted to change identity or cell");
    if(SameResidentState(object,value))return false;
    if(object.revision==UINT64_MAX||tile.revision==UINT64_MAX)
        Fail("GPU resident tile revision exhausted");
    const uint64_t nextRevision=object.revision+1;
    object=value;object.revision=nextRevision;
    ++tile.revision;
    if(!tile.modifiedFlags[pos->second]){
        tile.modifiedFlags[pos->second]=1u;
        tile.modifiedIndices.push_back(pos->second);
    }
    UpdateShadowCell(tile,object.cell);
    return true;
}

void GpuSceneCache::RemoveTile(GpuTileKey key){
    // The absent tile is retired by Graphics, after its GPU readers finish.
    residentTiles_.erase(key);
}

void GpuSceneCache::CommitTileChanges(GpuTileKey key,uint64_t topologyGeneration,
    std::span<const std::pair<uint32_t,uint64_t>> updates) const {
    const auto found=residentTiles_.find(key);
    if(found==residentTiles_.end()||
       found->second.topologyGeneration!=topologyGeneration)return;
    const auto& tile=found->second;
    for(const auto& [index,revision]:updates){
        if(index>=tile.objects.size()||index>=tile.modifiedFlags.size())
            Fail("Resident tile acknowledgement index out of range");
        // A newer mutation, even to the same index, must remain pending.
        if(tile.objects[index].revision==revision)tile.modifiedFlags[index]=0u;
    }
    std::erase_if(tile.modifiedIndices,[&](uint32_t index){
        return !tile.modifiedFlags[index];
    });
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
