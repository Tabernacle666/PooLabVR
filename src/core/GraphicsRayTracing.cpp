#include "GraphicsInternal.h"

namespace lab {
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


        uint64_t rayCompactCount=f.cpuCount;
        rayPublicationScratch.clear();rayPublicationRanges.clear();
        rayPublicationScratch.reserve(meshDomain->Bindings().size());
        rayPublicationRanges.reserve(meshDomain->Bindings().size());
        for(const auto& bound:meshDomain->Bindings()){
            const auto* instances=bound.publication.Find(GpuMeshPublicationAbi::Instances);
            const auto* physical=PhysicalRayMetadata(bound.publication);
            const bool nativePhysics=
                bound.publication.FindGroup(GpuMeshPublicationGroupId::PhysicsVisual)||
                bound.publication.FindGroup(GpuMeshPublicationGroupId::Population)||
                bound.publication.FindGroup(GpuMeshPublicationGroupId::Snow);
            uint32_t physicalFirst=0,physicalCount=nativePhysics?0u:bound.capacity;
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
                if(physical){
                    if(!physical->rayRevision)Fail("Physical ray publication missing revision");
                    rayRevision=physical->rayRevision;
                }else{
                    rayRevision=bound.publication.generation;
                    rayRevision=HashMemory(&instances->generation,sizeof(instances->generation),
                        rayRevision?rayRevision:1469598103934665603ull);
                }
            }
            const uint64_t sourceOwner=physicalCount?bound.publication.sourceOwner:0;
            const uint64_t sceneIdentity=physicalCount&&physical?physical->sceneIdentity:0;
            const uint32_t sourceBase=physicalCount?bound.base+physicalFirst:0;
            rayPublicationScratch.push_back({rayRevision,sourceOwner,sceneIdentity,
                compactFirst,physicalCount,sourceBase});
        }
        if(rayCompactCount>0x1000000ull)
            Fail("DXR InstanceID is 24 bits: compact ray instance capacity exceeded");
        f.rayInstanceCount=uint32_t(rayCompactCount);
        if(f.rayInstanceCount)EnsureRayInstanceMap(f,f.rayInstanceCount);


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
