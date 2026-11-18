#include "GraphicsInternal.h"

namespace OASIS {
    Graphics::Impl::RayFrameData Graphics::Impl::PrepareRayScene(
        Frame& f,const GpuSceneCache* tileCatalog,uint32_t frameSlot) {
        RayFrameData result;
        // Step 5b: harvest finished Direct-queue snapshots, never wait for
        // one. Per-block revisions allow completions to be merged even when
        // their frame slots are polled in a different order.
        const UINT64 completed=direct.fence->GetCompletedValue();
        for(uint32_t slot=0;slot<GpuSceneCache::FrameSlots;++slot){
            const UINT64 pending=cutRayReadbackFences[slot];
            if(!pending||pending>completed||!cutRayReadbacks[slot])continue;
            D3D12_RANGE readable{0,SIZE_T(cutRayMetadataBytes)};
            void* mapped=nullptr;
            Hr(cutRayReadbacks[slot]->Map(0,&readable,&mapped),
               "Cut DXR changed-block metadata readback");
            const auto* words=static_cast<const uint32_t*>(mapped);
            const uint32_t count=words[0],generation=words[1],cacheSlot=words[2];
            if(count>cutRayCpuBlocks.size()||
               (count!=0u&&(generation==0u||cacheSlot>1u)))
                Fail("Cut DXR metadata publication invalid");
            for(uint32_t i=0;i<count;++i){
                const uint32_t* record=words+4u+4u*i;
                const uint32_t id=record[0],state=record[1],
                               first=record[2],triangles=record[3];
                if(id>=cutRayCpuBlocks.size()||state>2u||
                   (state==0u&&(first>cutInitialCount||
                                  triangles>cutInitialCount-first))||
                   (state==1u&&(first>cutCapacity||
                                  triangles>cutCapacity-first))||
                   (state==2u&&triangles!=0u))
                    Fail("Cut DXR changed-block range invalid");
                auto& block=cutRayCpuBlocks[id];
                if(generation>=block.revision){
                    block={state,first,triangles,generation};
                    cutRaySceneRevision=std::max(cutRaySceneRevision,generation);
                }
            }
            D3D12_RANGE noWrite{0,0};
            cutRayReadbacks[slot]->Unmap(0,&noWrite);
            cutRayReadbackFences[slot]=0;
        }
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


        uint64_t rayCompactCount=f.cpuCount;
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
            const uint32_t compactFirst=uint32_t(rayCompactCount);
            if(rayCompactCount+physicalCount>0x1000000ull)
                Fail("DXR InstanceID is 24 bits: compact ray instance capacity exceeded");
            rayCompactCount+=physicalCount;
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
        if(rayCompactCount>0x1000000ull)
            Fail("DXR InstanceID is 24 bits: compact ray instance capacity exceeded");
        f.legacyRayInstanceCount=uint32_t(rayCompactCount);
        f.rayInstanceCount=f.legacyRayInstanceCount;
        if(f.legacyRayInstanceCount)
            EnsureRayInstanceMap(f,f.legacyRayInstanceCount);


        f.rayMapSegments.clear();
        if(f.cpuCount)f.rayMapSegments.push_back({0,f.cpuCount,0});
        for(const auto& range:rayPublicationRanges)
            if(range.count)f.rayMapSegments.push_back({
                range.compactFirst,range.count,range.renderFirst});
        f.rayMapBuild=f.rayMapSegments!=f.committedRayMapSegments;
        if(f.rayMapBuild)rayScene.InvalidateFrame(frameSlot);

        if(surfacesDirty)
            for(uint32_t slot=0;slot<GpuRayScene::FrameSlots;++slot)
                rayScene.InvalidateFrame(slot);
        rayScene.StageTiles(frameSlot,f.snapshotCount,tileCatalog);
        rayScene.StagePublications(frameSlot,rayPublicationScratch);
        if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);
        result.cpuDirty=rayScene.PendingCpu(frameSlot);
        result.publicationDirty=rayScene.PendingPublications(frameSlot);
        result.dirty=rayScene.NeedsTlas(frameSlot);
        return result;
    }
}
