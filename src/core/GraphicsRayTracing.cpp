#include "GraphicsInternal.h"
#include <set>

namespace OASIS {
    uint32_t Graphics::Impl::ReserveRaySlots(const RaySlotOwner& owner,uint32_t count){
        constexpr uint32_t limit=1u<<24;
        auto& lease=raySlotLeases[owner];
        if(count<=lease.capacity)return lease.first;
        if(count>limit)Fail("DXR InstanceID is 24 bits: ray slot capacity exceeded");
        const uint32_t wanted=std::min(limit,count+count/4u);
        std::vector<RaySlotLease> live;
        for(const auto& [key,span]:raySlotLeases)
            if(key!=owner&&span.capacity)live.push_back(span);
        std::sort(live.begin(),live.end(),[](auto a,auto b){return a.first<b.first;});
        auto find=[&](uint32_t capacity){
            uint32_t first=0;
            for(auto used:live){
                if(used.first-first>=capacity)return first;
                first=used.first+used.capacity;
            }
            return limit-first>=capacity?first:Invalid;
        };
        auto capacity=wanted;
        auto first=find(capacity);
        if(first==Invalid){
            capacity=count;
            first=find(capacity);
        }
        if(first==Invalid)Fail("DXR InstanceID is 24 bits: ray slot capacity exceeded");
        lease={first,capacity};
        raySlotCount=std::max(raySlotCount,first+capacity);
        return first;
    }

    Graphics::Impl::RayFrameData Graphics::Impl::PrepareRayScene(
        Frame& f,const GpuSceneCache* tileCatalog,uint32_t frameSlot) {
        RayFrameData result;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        auto& rayTileAcks=result.acks;
        if(tiles)for(const auto& [key,tile]:*tiles){
            const auto& indices=tile.rayModifiedIndices[frameSlot];
            if(indices.empty())continue;
            RayTileAck ack{};
            ack.key=key;
            ack.topologyGeneration=tile.topologyGeneration;
            ack.updates.reserve(indices.size());
            for(uint32_t index:indices){
                if(index>=tile.objects.size())
                    Fail("Resident tile ray update index out of range");
                ack.updates.emplace_back(index,tile.objects[index].revision);
            }
            rayTileAcks.push_back(std::move(ack));
        }


        auto ownerOf=[](const auto& bound){
            const auto* physical=PhysicalRayMetadata(bound.publication);
            return RaySlotOwner{2u,bound.publication.sourceOwner,
                physical?physical->sceneIdentity:0u,uint64_t(bound.publication.domain)};
        };
        std::set<RaySlotOwner> owners;
        if(f.cpuCount)owners.insert({0,0,0,0});
        if(tiles)for(const auto& [key,tile]:*tiles)
            owners.insert({1,uint64_t(key.x),uint64_t(key.z),uint64_t(uint32_t(key.layer))});
        for(const auto& bound:meshDomain->Bindings())
            if(PhysicalRayMetadata(bound.publication))owners.insert(ownerOf(bound));
        std::erase_if(raySlotLeases,[&](const auto& it){return !owners.contains(it.first);});

        f.rayMapSegments.clear();
        auto addSegment=[&](uint32_t first,uint32_t count,uint32_t render){
            if(count)f.rayMapSegments.push_back({first,count,render});
        };
        const uint32_t worldCount=sceneCache.StaticCount()+f.snapshotCount;
        if(worldCount)addSegment(ReserveRaySlots({0,0,0,0},worldCount),worldCount,0u);
        uint32_t tileFirst=worldCount;
        if(tiles)for(const auto& [key,tile]:*tiles){
            const uint32_t count=uint32_t(tile.objects.size());
            if(count)addSegment(ReserveRaySlots({1,uint64_t(key.x),uint64_t(key.z),uint64_t(uint32_t(key.layer))},count),
                count,tileFirst);
            tileFirst+=count;
        }
        rayPublicationScratch.clear();rayPublicationRanges.clear();
        rayPublicationScratch.reserve(meshDomain->Bindings().size());
        rayPublicationRanges.reserve(meshDomain->Bindings().size());
        for(const auto& bound:meshDomain->Bindings()){
            const auto* physical=PhysicalRayMetadata(bound.publication);
            uint32_t physicalFirst=0,physicalCount=0;
            if(bound.publication.FindGroup(GpuMeshPublicationGroupId::PhysicsVisual)){
                if(!physical)Fail("Physical ray publication missing physical range metadata");
                const auto* source=bound.publication.Find(GpuMeshPublicationAbi::PhysicsInstances);
                if(physical->physicalCount&&(!source||
                    physical->sourceFirst>source->bytes/sizeof(GpuInstance)||
                    physical->physicalCount>source->bytes/sizeof(GpuInstance)-physical->sourceFirst))
                    Fail("Physical ray publication source range exceeds its storage");
                physicalFirst=physical->sourceFirst;physicalCount=physical->physicalCount;
            }
            if(physicalFirst>bound.capacity||physicalCount>bound.capacity-physicalFirst)
                Fail("Physical ray publication exceeds raster instance range");
            const uint32_t compactFirst=physicalCount?
                ReserveRaySlots(ownerOf(bound),physicalCount):0u;
            addSegment(compactFirst,physicalCount,bound.base+physicalFirst);
            rayPublicationRanges.push_back({
                compactFirst,physicalCount,physicalCount?bound.base+physicalFirst:0u});


            uint64_t rayRevision=0;
            if(physicalCount){
                if(!physical->rayRevision)Fail("Physical ray publication missing revision");
                rayRevision=physical->rayRevision;
            }
            const uint64_t sourceOwner=physicalCount?bound.publication.sourceOwner:0;
            const uint64_t sceneIdentity=physicalCount&&physical?physical->sceneIdentity:0;
            const uint32_t sourceBase=physicalCount?bound.base+physicalFirst:0;
            rayPublicationScratch.push_back({rayRevision,sourceOwner,sceneIdentity,
                compactFirst,physicalCount,sourceBase});
        }
        f.rayInstanceCount=raySlotCount;
        if(f.rayInstanceCount){
            EnsureFrameInstanceCapacity(f,f.rayInstanceCount);
            EnsureRayInstanceMap(f,f.rayInstanceCount);
        }

        auto& map=f.rayMapSegments;
        std::sort(map.begin(),map.end(),[](auto a,auto b){return a.compactFirst<b.compactFirst;});
        std::vector<Frame::RayMapSegment> complete;
        uint32_t end=0;
        for(const auto& segment:map){
            if(segment.compactFirst<end)Fail("Overlapping DXR ray ranges");
            if(segment.compactFirst>end)
                complete.push_back({end,segment.compactFirst-end,Invalid});
            complete.push_back(segment);
            end=segment.compactFirst+segment.count;
        }
        if(end<f.rayInstanceCount)
            complete.push_back({end,f.rayInstanceCount-end,Invalid});
        map=std::move(complete);

        // Only update slots whose raster route changed for this frame resource.
        f.dirtyRayMapSegments.clear();
        const auto& committed=f.committedRayMapSegments;
        size_t oldIndex=0;
        for(const auto& segment:map){
            const uint32_t finish=segment.compactFirst+segment.count;
            uint32_t first=segment.compactFirst;
            while(first<finish){
                while(oldIndex<committed.size()&&
                      committed[oldIndex].compactFirst+committed[oldIndex].count<=first)
                    ++oldIndex;
                const auto* prior=oldIndex<committed.size()?&committed[oldIndex]:nullptr;
                uint32_t next=finish;
                const uint32_t render=segment.renderFirst==Invalid?Invalid:
                    segment.renderFirst+(first-segment.compactFirst);
                bool same=false;
                if(prior){
                    next=std::min(next,prior->compactFirst+prior->count);
                    const uint32_t oldRender=prior->renderFirst==Invalid?Invalid:
                        prior->renderFirst+(first-prior->compactFirst);
                    same=oldRender==render;
                }
                if(!same)f.dirtyRayMapSegments.push_back({first,next-first,render});
                first=next;
            }
        }
        f.rayMapBuild=!f.dirtyRayMapSegments.empty();

        rayScene.StageTiles(frameSlot,f.snapshotCount,tileCatalog);
        rayScene.StagePublications(frameSlot,rayPublicationScratch);
        if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);
        for(const auto& segment:f.dirtyRayMapSegments)
            result.cpuDirty.push_back({segment.compactFirst,segment.count});
        const auto pending=rayScene.PendingCpu(frameSlot);
        for(const auto& segment:map){
            if(segment.renderFirst>=f.cpuCount)continue;
            auto it=std::lower_bound(pending.begin(),pending.end(),segment.renderFirst,
                [](auto range,uint32_t first){return range.first+range.count<=first;});
            for(;it!=pending.end()&&it->first<segment.renderFirst+segment.count;++it){
                const uint32_t first=std::max(it->first,segment.renderFirst);
                const uint32_t end=std::min(it->first+it->count,segment.renderFirst+segment.count);
                result.cpuDirty.push_back({segment.compactFirst+(first-segment.renderFirst),end-first});
            }
        }
        for(uint32_t index:rayScene.PendingPublications(frameSlot)){
            const auto& range=rayPublicationRanges[index];
            if(range.count)result.cpuDirty.push_back({range.compactFirst,range.count});
        }
        // Build each descriptor at most once, even if content and mapping both changed.
        auto& dirty=result.cpuDirty;
        std::sort(dirty.begin(),dirty.end(),[](auto a,auto b){return a.first<b.first;});
        size_t written=0;
        for(const auto range:dirty){
            if(written&&dirty[written-1].first+dirty[written-1].count>=range.first){
                auto& previous=dirty[written-1];
                previous.count=std::max(previous.count,range.first+range.count-previous.first);
            }else dirty[written++]=range;
        }
        dirty.resize(written);
        result.dirty=f.rayMapBuild||rayScene.NeedsTlas(frameSlot);
        return result;
    }
}
