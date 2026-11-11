#include "GraphicsInternal.h"

namespace lab {
    void Graphics::Impl::Render(XRFrame& xr,const GpuProducerCoordinator& coordinator){
        traceFrame=xr.input.frameId;if(!active)return;if(xr.viewCount!=viewCount||!xr.viewCount||xr.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(xr.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    if(bound.deferred){
                        if(!coordinator.finishFallback)Fail("Deferred GPU producer cancellation without worker join");
                        coordinator.finishFallback();
                    }
                    // All native group slots and the legacy slot must be returned.
                    ReturnUnusedPublication(publication);
                }
                bound.stream->Release(publication);bound.stream=nullptr;bound.publication={};
            }
        };
        for(auto* stream:streams){
            auto* domain=FindDomain(stream->Domain());if(!domain)Fail("GPU stream domain is not registered");
            auto reserved=std::find_if(reservedStreams.begin(),reservedStreams.end(),[&](const auto& r){return r.stream==stream;});
            GpuPublication publication;
            const bool deferred=reserved!=reservedStreams.end();
            if(deferred){publication=std::move(reserved->publication);reserved->stream=nullptr;}
            else {
                // No advance reservation exists; the provider may need to
                // complete its worker before Acquire can expose its buffers.
                if(coordinator.finishFallback)coordinator.finishFallback();
                publication=stream->Acquire();
            }
            // A provider with no data contributes nothing to this frame. Other
            // streams, the room/menu and OpenXR presentation remain operative.
            // Physics publishes through queued consumer waits when its ring is
            // occupied, so saturation does not reach this no-data branch.
            if(!publication){LogEvent(traceFrame,"render.publication","state=unavailable domain=%u frame_continues=1",uint32_t(stream->Domain()));continue;}
            if(publication.domain!=stream->Domain())Fail("GPU publication changed stream domain");if(!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)Fail("GPU stream publication missing producer/consumer fence");if(!domain->ValidatePublication(publication))Fail("GPU publication rejected by domain contract");
            DescribePublication(publication);publications.push_back({stream,domain,std::move(publication),deferred});domain->BindPublication(publications.back().publication);
        }
        auto& f=*active;
        const bool coreSceneWork=f.cpuCount!=0||!meshDomain->Bindings().empty();
        f.instanceCount=meshDomain->TotalInstances();
        EnsureInstances(f,sceneCache.StaticCount()+f.snapshotCount,f.cpuCount,f.instanceCount);
        EnsureLightBuffers(f);
        EnsureLightBvhBuffers(f);
        const GpuSceneCache* tileCatalog=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        const uint32_t tileCount=tiles?uint32_t(tiles->size()):0;
        if(tileCount>UINT32_MAX-2)Fail("Too many GPU resident tile ranges");
        EnsureInstanceRoutes(f,tileCount+2);
        if(!f.instanceRangeData)Fail("Frame instance source table unavailable");
        f.tileTransfers.clear();f.tileRunTransfers.clear();f.heldTileSources.clear();
        // Eviction releases the registry entry, while prior frame slots still
        // retain their own COM references until their queue fences retire.
        for(auto it=residentTileGpu.begin();it!=residentTileGpu.end();){
            if(!tiles||!tiles->contains(it->first)){
                ReleaseDescriptor(it->second.srv);
                ReleaseDescriptor(it->second.runSrv);
                passGraph.Forget(it->second.buffer.Get());
                passGraph.Forget(it->second.runBuffer.Get());
                it=residentTileGpu.erase(it);
            }else ++it;
        }
        const uint32_t worldCount=sceneCache.StaticCount()+f.snapshotCount;
        uint32_t first=worldCount;
        f.instanceRangeData[0]={0,worldCount,Physical(f.worldInstanceSRV),
                                 tileCount+2};
        uint32_t route=1;
        struct TileBatchDispatch {
            GpuTileKey key{};
            uint32_t first{},count{},blockFirst{},blockCount{},
                superFirst{},superCount{},runFirst{},runCount{},runDescriptor{};
        };
        std::vector<TileBatchDispatch> tileBatchDispatch;
        uint32_t tileBatchBlocks=0,tileBatchSuperBlocks=0;
        if(tiles)for(const auto& [key,tile]:*tiles){
            if(tile.objects.size()>UINT32_MAX-first)
                Fail("Resident tile range exceeds 32-bit instance address space");
            auto& gpu=residentTileGpu[key];
            const uint32_t count=uint32_t(tile.objects.size());
            const bool rebuild=!gpu.buffer||gpu.objectCount!=count||
                gpu.topologyGeneration!=tile.topologyGeneration;
            if(rebuild){
                if(gpu.srv.Valid())ReleaseDescriptor(gpu.srv);
                if(gpu.buffer)passGraph.Forget(gpu.buffer.Get());
                gpu.buffer=Temporary(Buffer(UINT64(std::max(1u,count))*
                    sizeof(GpuResidentTileGpuInstance)),D3D12_RESOURCE_STATE_COMMON);
                gpu.srv=Srv(gpu.buffer.Get(),std::max(1u,count),
                            sizeof(GpuResidentTileGpuInstance));
                gpu.objectCount=count;
                gpu.topologyGeneration=tile.topologyGeneration;
            }
            // The texture-mask partition is persistent and changes only
            // when a tile's topology or a material mask changes.
            auto maskAt=[&](uint32_t index){
                const uint32_t material=tile.objects[index].material;
                if(material>=surfaceData.size())
                    Fail("Resident tile object material out of range");
                return MaterialTextureMask(surfaceData[material]);
            };
            bool runsChanged=rebuild||gpu.materialMasks.size()!=count;
            if(rebuild||gpu.materialMasks.size()!=count){
                gpu.materialMasks.resize(count);
                gpu.runs.clear();
                for(uint32_t index=0;index<count;++index){
                    const uint32_t mask=maskAt(index);
                    gpu.materialMasks[index]=mask;
                    if(gpu.runs.empty()||gpu.runs.back().mask!=mask)
                        gpu.runs.push_back({index,1u,mask,0});
                    else ++gpu.runs.back().count;
                }
            }else{
                auto changeMask=[&](uint32_t index){
                    if(index>=count)Fail("Resident material edit outside tile");
                    const uint32_t mask=maskAt(index);
                    if(mask==gpu.materialMasks[index])return;
                    gpu.materialMasks[index]=mask;
                    auto it=std::upper_bound(gpu.runs.begin(),gpu.runs.end(),index,
                        [](uint32_t value,const ResidentMaterialRun& run){
                            return value<run.first;
                        });
                    if(it==gpu.runs.begin())Fail("Resident material run missing");
                    --it;
                    if(index>=it->first+it->count)
                        Fail("Resident material run corrupt");
                    const ResidentMaterialRun old=*it;
                    const size_t offset=size_t(it-gpu.runs.begin());
                    gpu.runs.erase(it);
                    std::vector<ResidentMaterialRun> pieces;
                    if(index>old.first)
                        pieces.push_back({old.first,index-old.first,old.mask,0});
                    pieces.push_back({index,1u,mask,0});
                    if(index+1u<old.first+old.count)
                        pieces.push_back({index+1u,old.first+old.count-index-1u,old.mask,0});
                    gpu.runs.insert(gpu.runs.begin()+offset,pieces.begin(),pieces.end());
                    for(size_t j=offset?offset-1u:0u;j+1u<gpu.runs.size();){
                        if(gpu.runs[j].mask==gpu.runs[j+1u].mask){
                            gpu.runs[j].count+=gpu.runs[j+1u].count;
                            gpu.runs.erase(gpu.runs.begin()+j+1u);
                        }else ++j;
                        if(j>offset+3u)break;
                    }
                    runsChanged=true;
                };
                if(surfacesDirty)
                    for(uint32_t index=0;index<count;++index)changeMask(index);
                else
                    for(uint32_t index:tile.modifiedIndices)changeMask(index);
            }
            if(runsChanged){
                uint64_t covered=0;
                for(const auto& run:gpu.runs){
                    if(run.first!=covered||!run.count||run.mask>=32u)
                        Fail("Resident material partition invalid");
                    covered+=run.count;
                }
                if(covered!=count)
                    Fail("Resident material partition size mismatch");
            }
            if(runsChanged||(!gpu.runBuffer&&!gpu.runsDirty)){
                if(gpu.runRevision==UINT64_MAX)
                    Fail("Resident material run revision exhausted");
                ++gpu.runRevision;
                gpu.runsDirty=true;
            }
            if(gpu.runsDirty){
                if(gpu.runs.size()>=UINT32_MAX)
                    Fail("Resident run table index space exhausted");
                const uint32_t required=uint32_t(gpu.runs.size())+1u;
                if(!gpu.runBuffer||required>gpu.runCapacity){
                    const uint32_t capacity=GrowCapacity(gpu.runCapacity,required);
                    if(capacity<required)Fail("Resident run buffer capacity exhausted");
                    if(gpu.runSrv.Valid())ReleaseDescriptor(gpu.runSrv);
                    if(gpu.runBuffer)passGraph.Forget(gpu.runBuffer.Get());
                    gpu.runBuffer=Temporary(Buffer(uint64_t(capacity)*
                        sizeof(ResidentMaterialRun)),D3D12_RESOURCE_STATE_COMMON);
                    gpu.runSrv=Srv(gpu.runBuffer.Get(),capacity,
                        sizeof(ResidentMaterialRun));
                    gpu.runCapacity=capacity;
                }
                Frame::TileRunTransfer transfer{};
                transfer.key=key;
                transfer.revision=gpu.runRevision;
                transfer.buffer=gpu.runBuffer;
                transfer.upload=Host(uint64_t(required)*
                    sizeof(ResidentMaterialRun),D3D12_HEAP_TYPE_UPLOAD);
                D3D12_RANGE range{0,0};
                void* ptr{};
                Hr(transfer.upload->Map(0,&range,&ptr),"tile run staging map");
                auto* dst=static_cast<ResidentMaterialRun*>(ptr);
                dst[0]={uint32_t(gpu.runs.size()),0,0,0};
                std::copy(gpu.runs.begin(),gpu.runs.end(),dst+1);
                transfer.upload->Unmap(0,nullptr);
                f.tileRunTransfers.push_back(std::move(transfer));
            }
            f.heldTileSources.push_back(gpu.buffer);
            f.heldTileSources.push_back(gpu.runBuffer);
            f.instanceRangeData[route++]={first,count,Physical(gpu.srv),1};
            if(gpu.runs.size()>UINT32_MAX)
                Fail("Resident material run ordinal overflow");
            const uint32_t blockCount=(count+255u)/256u;
            const uint32_t superCount=(blockCount+255u)/256u;
            tileBatchDispatch.push_back({key,first,count,tileBatchBlocks,
                blockCount,tileBatchSuperBlocks,superCount,0,
                uint32_t(gpu.runs.size()),Physical(gpu.runSrv)});
            if(tileBatchBlocks>UINT32_MAX-blockCount||
               tileBatchSuperBlocks>UINT32_MAX-superCount)
                Fail("Resident tile batch prefix capacity exhausted");
            tileBatchBlocks+=blockCount;
            tileBatchSuperBlocks+=superCount;
            first+=count;
            // Upload the full tile on topology changes, and only actual
            // edits on subsequent frames.
            std::vector<uint32_t> indices;
            if(rebuild){
                indices.resize(count);
                for(uint32_t i=0;i<count;++i)indices[i]=i;
            }else indices=tile.modifiedIndices;
            if(indices.empty())continue;
            std::sort(indices.begin(),indices.end());
            indices.erase(std::unique(indices.begin(),indices.end()),indices.end());
            Frame::TileTransfer transfer{};
            transfer.key=key;
            transfer.topologyGeneration=tile.topologyGeneration;
            transfer.buffer=gpu.buffer;
            uint32_t runStart=indices.front(),runCount=0;
            for(uint32_t index:indices){
                if(index>=count)Fail("Resident tile dirty index out of range");
                if(runCount&&uint64_t(runStart)+runCount!=index){
                    transfer.ranges.push_back({runStart,runCount});
                    runStart=index;runCount=0;
                }
                ++runCount;
                transfer.updates.emplace_back(index,tile.objects[index].revision);
            }
            if(runCount)transfer.ranges.push_back({runStart,runCount});
            // Pack the upload by modified objects, retaining original
            // destination offsets for all GPU readers.
            transfer.upload=Host(UINT64(indices.size())*
                                 sizeof(GpuResidentTileGpuInstance),
                                 D3D12_HEAP_TYPE_UPLOAD);
            D3D12_RANGE empty{0,0};void* mapped{};
            Hr(transfer.upload->Map(0,&empty,&mapped),"resident tile staging map");
            auto* output=static_cast<GpuResidentTileGpuInstance*>(mapped);
            for(size_t n=0;n<transfer.updates.size();++n){
                const auto& o=tile.objects[transfer.updates[n].first];
                auto& record=output[n];
                record.instance=ConvertResidentBase(o);
                record.sector={o.cellX,o.cellZ,o.cellRadius,0};
                record.farMesh=o.farMesh;
                record.hideFar=o.hideFar;
                record.enabled=o.enabled;
                record.reserved=0;
            }
            transfer.upload->Unmap(0,nullptr);
            f.tileTransfers.push_back(std::move(transfer));
        }
        if(first!=f.cpuCount)Fail("GPU tile source size changed after preparation");
        f.instanceRangeData[route]={f.cpuCount,f.instanceCount-f.cpuCount,
                                    Physical(f.publishedInstanceSRV),2};
        EnsureVisibilityBuffers(f,f.cpuCount,f.instanceCount-f.cpuCount);
        // Nonresident snapshot instances retain the existing CPU grouping.
        // Residents use the stored material-mask partition for each tile.
        struct OpaqueMaterialRun {uint32_t first{},count{},mask{};};
        std::vector<OpaqueMaterialRun> opaqueRuns;
        auto appendMaterial=[&](uint32_t material){
            if(material>=surfaceData.size())Fail("Opaque instance references missing material");
            uint32_t mask=MaterialTextureMask(surfaceData[material]);
            if(opaqueRuns.empty()||opaqueRuns.back().mask!=mask)
                opaqueRuns.push_back({uint32_t(opaqueRuns.empty()?0u:
                    opaqueRuns.back().first+opaqueRuns.back().count),1u,mask});
            else ++opaqueRuns.back().count;
        };
        for(uint32_t i=0;i<worldCount;++i)appendMaterial(CpuInstance(f,i).material);
        if(worldCount){
            if(opaqueRuns.empty()||
               opaqueRuns.back().first+opaqueRuns.back().count!=worldCount)
                Fail("Material run partition does not cover the nonresident scene");
        }
        uint64_t totalRuns=opaqueRuns.size();
        for(auto& tileBatch:tileBatchDispatch){
            tileBatch.runFirst=uint32_t(totalRuns);
            totalRuns+=tileBatch.runCount;
        }
        if(totalRuns>UINT32_MAX)
            Fail("Resident opaque run address space exhausted");
        EnsureBatchRunCounters(f,uint32_t(totalRuns));
        if(tileBatchBlocks>UINT32_MAX-tileBatchSuperBlocks)
            Fail("Resident tile scan hierarchy capacity exhausted");
        EnsureTileBatchBuffers(f,f.cpuCount,tileBatchBlocks+tileBatchSuperBlocks);
        EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);bool copyStatic=!pendingStatic.empty();
        struct RayTileAck {
            GpuTileKey key{};
            uint64_t topologyGeneration{};
            std::vector<std::pair<uint32_t,uint64_t>> updates;
        };
        std::vector<RayTileAck> rayTileAcks;
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
        // The raster publication stays unchanged. Ray descriptors use a compact
        // CPU-first range followed only by each producer's authored physical
        // visuals. Providers without the native group retain legacy ray coverage.
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
            // Native PhysX metadata describes only the physical geometry
            // actually consumed by the compact ray scene. Insect/snow frame
            // sequences and the combined raster buffer are irrelevant here.
            // Legacy producers have no physical revision: retain their safe
            // sequence-based invalidation until they publish one.
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
        EnsureRayInstanceMap(f,f.rayInstanceCount);
        // Keep only compact source ranges on CPU. The mapping contents
        // are constructed on GPU when their disposition changes.
        f.rayMapSegments.clear();
        if(f.cpuCount)f.rayMapSegments.push_back({0,f.cpuCount,0});
        for(const auto& range:rayPublicationRanges)
            if(range.count)f.rayMapSegments.push_back({
                range.compactFirst,range.count,range.renderFirst});
        f.rayMapBuild=f.rayMapSegments!=f.committedRayMapSegments;
        if(f.rayMapBuild)rayScene.InvalidateFrame(frameSlot);
        // Material alpha changes invalidate every frame slot's ray flags.
        if(surfacesDirty)
            for(uint32_t slot=0;slot<GpuRayScene::FrameSlots;++slot)
                rayScene.InvalidateFrame(slot);
        rayScene.StageTiles(frameSlot,f.snapshotCount,tileCatalog);
        rayScene.StagePublications(frameSlot,rayPublicationScratch);
        if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);
        auto rayCpuDirty=rayScene.PendingCpu(frameSlot);
        auto rayPublicationDirty=rayScene.PendingPublications(frameSlot);
        bool rayDirty=rayScene.NeedsTlas(frameSlot);
        if(coreSceneWork||!publications.empty()){
            if(!PrepareResidency(xr,f,coreSceneWork)){releasePublications(true);publications.clear();endDomains();return;}
        }else{
            std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);requiredResidencyPages.clear();
        }
        if(xr.input.trackingRevision!=lastTrackingRevision||(lastDisplayTime&&xr.displayTime-lastDisplayTime>xr.displayPeriod*2))historyReady=false;lastTrackingRevision=xr.input.trackingRevision;lastDisplayTime=xr.displayTime;
        f.submittedFrame=xr.input.frameId;LogEvent(traceFrame,"render.scene","source_input=%llu simulation_tick=%llu instance_capacity=%u streams=%u tlas_dirty=%u ray_cpu_ranges=%u ray_streams=%u",static_cast<unsigned long long>(f.sourceInput),static_cast<unsigned long long>(f.sourceTick),f.instanceCount,uint32_t(publications.size()),unsigned(rayDirty),uint32_t(rayCpuDirty.size()),uint32_t(rayPublicationDirty.size()));
        constexpr float resolutionScale=1.0f;renderWidth=width;renderHeight=height;renderMips=std::bit_width(std::max(renderWidth,renderHeight));constexpr uint32_t quality=3u;uint32_t output=uint32_t(frameNumber&1),previous=1-output;
        uint32_t staticCount=sceneCache.StaticCount();f.transparentCount=0;bool hasReflections=false;
        if(coreSceneWork){
            for(uint32_t index:staticTransparent)f.transparent[f.transparentCount++]=index;hasReflections=staticReflectionCandidates[quality];
            for(uint32_t i=0;i<f.dynamicCount;i++){const auto& object=f.objects[i];
                if(i<f.snapshotCount&&(object.flags&AlphaBlendObject))
                    f.transparent[f.transparentCount++]=staticCount+i;
                if(!(object.flags&128u)&&!hasReflections&&object.material<surfaceData.size())
                    hasReflections=SurfaceMayReflect(surfaceData[object.material],quality);
            }
            if(!hasReflections&&tileCatalog)
                for(const auto& [key,tile]:tileCatalog->ResidentTiles()){
                    for(const auto& [material,count]:tile.materialCounts){
                        if(!count)continue;
                        if(material>=surfaceData.size())
                            Fail("Resident tile reflection material out of range");
                        if(SurfaceMayReflect(surfaceData[material],quality)){
                            hasReflections=true;break;
                        }
                    }
                    if(hasReflections)break;
                }
            if(!hasReflections)for(uint32_t material:contributions.streamMaterials){if(material>=surfaceData.size()){hasReflections=true;break;}if(SurfaceMayReflect(surfaceData[material],quality)){hasReflections=true;break;}}
            // MeshSolid publications do not expose a per-instance material summary. If a
            // producer has not supplied the existing streamMaterials contract, keep the
            // reflection path rather than guessing that GPU-only instances are matte.
            if(!hasReflections&&!meshDomain->Bindings().empty()&&contributions.streamMaterials.empty())hasReflections=true;
        }else{
            hasReflections=false;historyReady=false;
        }
        RenderPassPlan passPlan{};
        if(coreSceneWork){
            passPlan.occlusion=f.instanceCount>SmallSceneOcclusionThreshold;
            passPlan.depthHierarchy=passPlan.occlusion||hasReflections;
            passPlan.clusteredLights=f.lightCount>DirectLightLimit;
            passPlan.reflections=hasReflections;
            passPlan.shadowAtlas=f.hasShadowAtlasLights;
            passPlan.rayQueries=passPlan.reflections||f.hasShadowedLights;
            passPlan.rayWork=rayDirty&&passPlan.rayQueries&&f.rayInstanceCount>0;
        }
        auto& c=*f.constants;c={};
        if(tileCatalog)c.tileViewer={
            float(tileCatalog->TileViewerX()),float(tileCatalog->TileViewerZ()),0,0};
        bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        for(uint32_t view=0;view<xr.viewCount;view++){c.vp[view]=xr.vp[view];c.inverseVP[view]=xr.inverseVP[view];c.previousVP[view]=reflectionHistoryReady?lastVP[view]:xr.vp[view];c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};lastVP[view]=xr.vp[view];}
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV});
        SetDomain(f,ResourceDomain::Frame,{f.worldInstanceSRV,{}, {},f.worldInstanceUAV,{}, {},{}, {},f.cpuVisibility.depthArgumentsUAV,f.cpuVisibility.depthCountUAV,f.publishedVisibility.depthArgumentsUAV,f.publishedVisibility.depthCountUAV,f.cpuVisibility.cullArgumentsUAV,f.cpuVisibility.cullCountUAV,f.publishedVisibility.cullArgumentsUAV,f.publishedVisibility.cullCountUAV,f.cpuVisibility.shadowArgumentsUAV,f.cpuVisibility.shadowCountUAV,f.publishedInstanceSRV,f.instanceRangesSRV,
            f.cpuVisibility.batchVisibilityUAV,f.cpuVisibility.batchVisibilitySRV,
            f.cpuVisibility.batchArgumentsUAV,f.cpuVisibility.batchCountUAV,
            f.cpuVisibility.tileMarksUAV,f.cpuVisibility.tileMarksSRV,
            f.cpuVisibility.tilePrefixesUAV,f.cpuVisibility.tilePrefixesSRV,
            f.cpuVisibility.tileTotalsUAV,f.cpuVisibility.tileTotalsSRV,
            f.cpuVisibility.tileOffsetsUAV,f.cpuVisibility.tileOffsetsSRV});
        SetDomain(f,ResourceDomain::Lighting,{
            f.lightsSRV,f.clusterSRV,f.clusterUAV,shadowSRV,
            f.clusterIndicesSRV,f.clusterIndicesUAV,
            f.clusterGroupCountsSRV,f.clusterGroupCountsUAV,
            f.clusterGroupOffsetsSRV,f.clusterGroupOffsetsUAV,
            f.clusterDirectionalIndicesSRV,f.clusterDirectionalIndicesUAV,
            f.clusterDirectionalCountSRV,f.clusterDirectionalCountUAV,
            f.clusterBoundsSRV,f.clusterBoundsUAV,
            f.lightBvhNodesSRV,f.lightBvhNodesUAV,
            f.lightBvhPairsSRV,f.lightBvhPairsUAV,
            f.lightBvhBoundsSRV,f.lightBvhBoundsUAV});
        SetDomain(f,ResourceDomain::History,{hierarchySRV,depthSRV,hdrResolvedSRV,reflectionsResolvedSRV,historySRV[previous],historyDepthSRV[previous],historyUAV[output],historyDepthUAV[output],historySRV[output],hierarchySRV});
        for(uint32_t mip=0;mip<hierarchyUAV.size();mip++)f.resourceTableData[uint32_t(ResourceDomain::History)][HistoryHiZMipBase+mip]={Physical(hierarchyUAV[mip]),Generation(hierarchyUAV[mip])};
        SetDomain(f,ResourceDomain::Animation,{f.dynamicSRV,f.dynamicUAV,deformationsSRV,f.motionSRV,f.boneSRV,f.weightSRV,skinSRV,morphSRV,f.deformedBoundsSRV,f.deformedBoundsUAV});
        SetDomain(f,ResourceDomain::Visibility,{f.hotBoundsSRV,f.hotBoundsUAV,f.hotMeshesSRV,f.hotMeshesUAV});
        SetDomain(f,ResourceDomain::RayTracing,{f.tlasSRV,f.rayUAV,f.blasAddressesSRV,f.rayMapSRV});
        c.resourceAbi={DescriptorIndex(f.resourceDirectorySRV),ResourceAbiVersion,0,0};c.dimensions={renderWidth,renderHeight,renderMips,0};c.outputDimensions={width,height,renderWidth,renderHeight};c.counts={coreSceneWork?f.instanceCount:0u,coreSceneWork?f.additionalCount:0u,coreSceneWork?f.lightCount:0u,coreSceneWork&&reflectionHistoryReady?1u:0u};c.topology={xr.viewCount,MsaaSamples,passPlan.reflections?1u:0u,f.rayInstanceCount};c.timing={NearZ,FarZ,float(f.simulationTime),float(quality)};
        c.lightBvhLayout={f.lightBvhLeaves,0,0,0};
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](uint32_t a,uint32_t b){auto distance=[&](uint32_t i){auto p=CpuInstance(f,i).positionRadius;auto delta=Sub({p.x,p.y,p.z},center);return Dot(delta,delta);};return distance(a)>distance(b);});
        }

        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useTexture=[&](uint32_t node,TextureResource& resource,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){frameGraph.Texture(node,&resource,resource.resource.Get(),resource.layout,mode,layout,sync,access);};
        auto useTileSources=[&](uint32_t node,D3D12_BARRIER_SYNC sync){
            for(const auto& tile:f.heldTileSources)
                useBuffer(node,tile.Get(),GraphMode::Read,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        };

        if(surfacesDirty&&materials&&!surfaceData.empty()){
            UINT64 bytes=surfaceData.size()*sizeof(Surface),offset=Reserve(f,bytes);memcpy(f.uploadGeneration->mapped+offset,surfaceData.data(),size_t(bytes));
            auto node=frameGraph.AddPass("Materials.Update",GraphQueue::Direct,9,[&,offset,bytes](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyBufferRegion(materials.Get(),0,f.uploadGeneration->resource.Get(),offset,bytes);surfacesDirty=false;});
            useBuffer(node,materials.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        uint32_t coreColorNode=UINT32_MAX;
        if(coreSceneWork){
            auto uploadNode=frameGraph.AddPass("Upload",GraphQueue::Copy,10,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                if(!f.uploadGeneration)Fail("frame upload generation missing");auto uploadResource=f.uploadGeneration->resource.Get();auto copyAnimation=[&](ID3D12Resource* destination,UINT64 offset){l->CopyBufferRegion(destination,0,uploadResource,offset,destination->GetDesc().Width);};copyAnimation(f.motionBuffer.Get(),f.motionOffset);copyAnimation(f.boneBuffer.Get(),f.boneOffset);copyAnimation(f.weightBuffer.Get(),f.weightOffset);if(f.lightCount)l->CopyBufferRegion(f.lights.Get(),0,uploadResource,f.lightsOffset,UINT64(f.lightCount)*sizeof(Light));
                if(copyStatic){if(!residentStaticInstances)Fail("persistent static scene source missing");for(const auto& range:pendingStatic){UINT64 offset=UINT64(range.first)*sizeof(GpuInstance),bytes=UINT64(range.count)*sizeof(GpuInstance);l->CopyBufferRegion(f.worldInstances.Get(),offset,residentStaticInstances.Get(),offset,bytes);}}if(f.snapshotCount)l->CopyBufferRegion(f.worldInstances.Get(),UINT64(staticCount)*sizeof(GpuInstance),uploadResource,f.objectsOffset,UINT64(f.snapshotCount)*sizeof(GpuInstance));
                // Tile data is copied only for new/edited instance revisions.
                // Other frames read the same persistent GPU source directly.
                for(const auto& transfer:f.tileTransfers){
                    UINT64 sourceOffset=0;
                    for(const auto& range:transfer.ranges){
                        const UINT64 destinationOffset=UINT64(range.first)*
                            sizeof(GpuResidentTileGpuInstance);
                        const UINT64 bytes=UINT64(range.count)*
                            sizeof(GpuResidentTileGpuInstance);
                        l->CopyBufferRegion(transfer.buffer.Get(),destinationOffset,
                            transfer.upload.Get(),sourceOffset,bytes);
                        sourceOffset+=bytes;
                    }
                }
                for(const auto& transfer:f.tileRunTransfers)
                    l->CopyBufferRegion(transfer.buffer.Get(),0,
                        transfer.upload.Get(),0,transfer.upload->GetDesc().Width);

            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})if(resource)useBuffer(uploadNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic||f.snapshotCount)useBuffer(uploadNode,f.worldInstances.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic)useBuffer(uploadNode,residentStaticInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            for(const auto& transfer:f.tileTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            for(const auto& transfer:f.tileRunTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
 if(f.lightCount)useBuffer(uploadNode,f.lights.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }

        frameResources.Clear();auto frameAddress=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;
        frameResources.Set({GpuFrameResourceId::SceneDepth,depth.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,depth.layout,depthSRV,{}, {},Dsv(depthDSV),0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::DepthResolved,hierarchy.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,hierarchy.layout,hierarchySRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::HdrColor,hdr.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,hdr.layout,MsaaSamples==1?hdrResolvedSRV:GpuResourceHandle{},MsaaSamples==1?hdrResolvedUAV:GpuResourceHandle{},Rtv(hdrRTV),{},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedColor=MsaaSamples>1?hdrResolved:hdr;frameResources.Set({GpuFrameResourceId::ResolvedColor,resolvedColor.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedColor.layout,hdrResolvedSRV,hdrResolvedUAV,MsaaSamples==1?Rtv(hdrRTV):Rtv(hdrResolvedRTV), {},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedReflection=MsaaSamples>1?reflectionsResolved:reflections;frameResources.Set({GpuFrameResourceId::Reflections,resolvedReflection.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedReflection.layout,reflectionsResolvedSRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::ShadowAtlas,shadow.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,shadow.layout,shadowSRV,{}, {},Dsv(shadowDSV),0,0,0,3072,2048,1});
        frameResources.Set({GpuFrameResourceId::Lighting,f.lights.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightsSRV,{}, {},{},0,f.lights?f.lights->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::Clusters,f.clusters.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterSRV,f.clusterUAV,{}, {},0,f.clusters?f.clusters->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::ClusterIndices,f.clusterIndices.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterIndicesSRV,f.clusterIndicesUAV,{}, {},0,f.clusterIndices?f.clusterIndices->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::LightBvhNodes,f.lightBvhNodes.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightBvhNodesSRV,f.lightBvhNodesUAV,{}, {},0,f.lightBvhNodes?f.lightBvhNodes->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::TLAS,f.tlas.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.tlasSRV,{}, {},{},0,f.tlas?f.tlas->GetDesc().Width:0,f.tlas?f.tlas->GetGPUVirtualAddress():0,0,0,0});
        GpuFrameResource constants{GpuFrameResourceId::FrameConstants,f.uploadGeneration->resource.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,{}, {}, {},{},f.constantsOffset,sizeof(Constants),frameAddress,0,0,0};frameResources.Set(constants);constants.id=GpuFrameResourceId::ViewConstants;frameResources.Set(constants);
        for(const auto& bound:publications)publicationCatalog.Add(bound.publication);
        GpuFrameContext domainContext{&frameResources,xr.input.frameId,renderWidth,renderHeight,xr.viewCount,frameAddress,&publicationCatalog,frameSlot,uint32_t(frames.size()),f.simulationTime};

        // Deformation only uses the uploaded animation state, so submit it
        // independently of the current frame's physical instance publication.
        if(coreSceneWork&&!f.deformationWork.Empty()){
            auto deformNode=frameGraph.AddPass("Deform",GraphQueue::Compute,15,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetPipelineState(deformPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch((meshes.ranges[mesh].vertexCount+63)/64,1,1);}
                BBarrier(l,f.dynamicVertices.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                l->SetPipelineState(deformBoundsPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch(1,1,1);}
                BBarrier(l,f.deformedBounds.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})useBuffer(deformNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.dynamicVertices.Get(),f.deformedBounds.Get()})useBuffer(deformNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }

        // The CPU-owned instance range is available before the GPU producer's
        // publication. Keep its unoccluded depth list independent of the
        // legacy mixed-scene cull until all consumers are migrated.
        if(coreSceneWork&&f.cpuCount){
            auto cpuVisibilityNode=frameGraph.AddPass("CpuScene.Visibility",GraphQueue::Compute,20,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);
                l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetComputeRoot32BitConstant(1,0,0);
                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                l->SetPipelineState(hotDataPSO.Get());
                l->Dispatch((f.cpuCount+63)/64,1,1);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                ClearCount(l,f.cpuVisibility.depthCount.Get(),f.cpuVisibility.depthCountUAV);
                l->SetPipelineState(cullPSO.Get());
                l->SetComputeRoot32BitConstant(1,3,1);
                l->Dispatch((f.cpuCount+63)/64,1,1);
                ArgumentsReady(l,f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get());
            });
            useTileSources(cpuVisibilityNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            useBuffer(cpuVisibilityNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuVisibilityNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(cpuVisibilityNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }

        // Render the CPU scene depth before the published GPU instance range
        // becomes available. The full-scene Depth pass will append to this same
        // DSV after the publication rather than clear the early contribution.
        if(coreSceneWork&&f.cpuCount){
            auto cpuDepthNode=frameGraph.AddPass("CpuScene.Depth",GraphQueue::Direct,21,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,true);
                Viewport(l,renderWidth,renderHeight);
                auto depthHandle=Dsv(depthDSV);
                l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
                l->SetPipelineState(depthPSO.Get());
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.depthArguments.Get(),0,f.cpuVisibility.depthCount.Get(),0);
            });
            useTileSources(cpuDepthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(cpuDepthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuDepthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useTexture(cpuDepthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        }

        // Stream copies logically precede the renderer consumers of instances/control.
        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);
        domainBuilder.SetDomain(opticalDomain->Identity());opticalDomain->BuildFields(domainBuilder,domainContext,frameResources);domainBuilder.SetDomain(GpuDomainId::Invalid);
        c.resourceAbi.z=opticalDomain->FieldDescriptor();c.resourceAbi.w=opticalDomain->FieldCount();



        // The CPU light snapshot determines whether the original Morton sort
        // needs refresh. GPU PhysX publications can move between snapshots.
        bool bvhGpuLightsMayMove=false;
        for(const auto& light:f.lightData)
            bvhGpuLightsMayMove|=(uint32_t(light.parameters.z)&4u)!=0u;
        for(const auto& bound:meshDomain->Bindings())
            for(const auto& resource:bound.publication.resources)
                if(resource.binding>=GpuMeshPublicationAbi::LightBase&&
                   resource.binding<GpuMeshPublicationAbi::LightBase+MaxLights)
                    bvhGpuLightsMayMove=true;
        const bool bvhSourceChanged=f.lightBvhSortSnapshot.size()!=f.lightData.size()||
            (!f.lightData.empty()&&std::memcmp(f.lightBvhSortSnapshot.data(),
                f.lightData.data(),f.lightData.size()*sizeof(Light))!=0);
        const uint64_t bvhAge=frameNumber>=f.lightBvhLastSortFrame?
            frameNumber-f.lightBvhLastSortFrame:UINT64_MAX;
        const bool bvhSortNeeded=f.lightBvhPendingSort||!f.lightBvhSorted||
            f.lightBvhSortedCount!=f.lightCount||
            ((bvhSourceChanged||bvhGpuLightsMayMove)&&bvhAge>=256u);
        f.lightBvhPendingSort=bvhSortNeeded;
        if(coreSceneWork){
            const uint32_t leaves=f.lightBvhLeaves;
            const uint32_t boundGroups=uint32_t(
                (uint64_t(std::max(1u,f.lightCount))+255u)/256u);
            const auto bvhNode=frameGraph.AddPass(
                "Light.BVH",GraphQueue::Compute,29,
                [&,leaves,boundGroups,bvhSortNeeded]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    uint32_t args[5]{leaves,0u,boundGroups,0u,0u};
                    l->SetComputeRoot32BitConstants(1,5,args,0);
                    auto dispatch256=[&](uint32_t count){
                        const uint64_t groups=(uint64_t(count)+255u)/256u;
                        if(groups>uint64_t(65535u)*65535u)
                            Fail("Light BVH dispatch dimensions exhausted");
                        l->Dispatch(uint32_t(std::min<uint64_t>(groups,65535u)),
                            uint32_t((groups+65534u)/65535u),1u);
                    };
                    auto uavBarrier=[&](ID3D12Resource* buffer){
                        BBarrier(l,buffer,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    };
                    if(bvhSortNeeded){
                        l->SetPipelineState(lightBvhPartialPSO.Get());
                        dispatch256(std::max(1u,f.lightCount));
                        uavBarrier(f.lightBvhBounds.Get());
                        l->SetComputeRoot32BitConstant(1,boundGroups,2);
                        l->SetPipelineState(lightBvhBoundsPSO.Get());
                        l->Dispatch(1,1,1);
                        uavBarrier(f.lightBvhBounds.Get());
                        l->SetPipelineState(lightBvhMortonPSO.Get());
                        dispatch256(leaves);
                        uavBarrier(f.lightBvhPairs.Get());
                        for(uint64_t size=2u;size<=leaves;size*=2u)
                            for(uint32_t stride=uint32_t(size/2u);
                                stride>0u;stride>>=1u){
                                l->SetComputeRoot32BitConstant(1,uint32_t(size),1);
                                l->SetComputeRoot32BitConstant(1,stride,2);
                                l->SetPipelineState(lightBvhSortPSO.Get());
                                dispatch256(leaves);
                                uavBarrier(f.lightBvhPairs.Get());
                            }
                    }
                    // Refit current GPU light spheres every image, even
                    // when sorting is skipped for unchanged cardinality.
                    l->SetPipelineState(lightBvhLeavesPSO.Get());
                    dispatch256(leaves);
                    uavBarrier(f.lightBvhNodes.Get());
                    for(uint32_t level=leaves/2u;level>0u;level>>=1u){
                        l->SetComputeRoot32BitConstant(1,level,1);
                        l->SetComputeRoot32BitConstant(1,level,2);
                        l->SetPipelineState(lightBvhParentsPSO.Get());
                        dispatch256(level);
                        uavBarrier(f.lightBvhNodes.Get());
                    }
                });
            useBuffer(bvhNode,f.lights.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.lightBvhNodes.Get(),f.lightBvhPairs.Get(),
                                f.lightBvhBounds.Get()})
                useBuffer(bvhNode,resource,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }

        if(coreSceneWork){

            // The publication ranges start at their bound.base; never rebase
            // their indirect commands to a local index or touch CPU visibility.
            if(f.instanceCount>f.cpuCount){
                auto publishedNode=frameGraph.AddPass("MeshSolid.Visibility",GraphQueue::Compute,25,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    l->SetComputeRoot32BitConstants(1,4,params,0);
                    auto dispatchGpu=[&](uint32_t passIndex){
                        l->SetComputeRoot32BitConstant(1,passIndex,1);
                        for(const auto& bound:meshDomain->Bindings()){
                            l->SetComputeRoot32BitConstant(1,bound.base,0);
                            l->SetComputeRoot32BitConstant(1,Invalid,2);
                            uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);
                        }
                        l->SetComputeRoot32BitConstants(1,4,params,0);
                    };
                    l->SetPipelineState(hotDataPSO.Get());
                    dispatchGpu(0);
                    for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                        BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    ClearCount(l,f.publishedVisibility.depthCount.Get(),f.publishedVisibility.depthCountUAV);
                    l->SetPipelineState(cullPSO.Get());
                    dispatchGpu(4);
                    ArgumentsReady(l,f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get());
                });
                useBuffer(publishedNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                useBuffer(publishedNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(publishedNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    useBuffer(publishedNode,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})
                    useBuffer(publishedNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }

            if(passPlan.rayWork||f.shadowDirty){
            auto computeNode=frameGraph.AddPass("DeformCull",GraphQueue::Compute,30,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);if(passPlan.rayWork){
                if(f.rayMapBuild){
                    l->SetPipelineState(rayMapFillPSO.Get());
                    for(const auto& segment:f.rayMapSegments){
                        uint32_t mapArgs[4]{
                            segment.compactFirst,segment.renderFirst,segment.count,
                            Physical(f.rayMapUAV)};
                        l->SetComputeRoot32BitConstants(1,4,mapArgs,0);
                        const uint32_t groups=(segment.count+63u)/64u;
                        l->Dispatch(std::min(groups,65535u),
                                    (groups+65534u)/65535u,1);
                    }
                    BBarrier(l,f.rayInstanceMap.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                }
                l->SetPipelineState(rayBuildPSO.Get());for(const auto& range:rayCpuDirty){l->SetComputeRoot32BitConstant(1,range.first,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch(std::min((range.count+63u)/64u,65535u),
                    (((range.count+63u)/64u)+65534u)/65535u,1);}for(uint32_t bindingIndex:rayPublicationDirty){if(bindingIndex>=rayPublicationRanges.size())Fail("compact ray publication dirty index");const auto& range=rayPublicationRanges[bindingIndex];if(!range.count)continue;l->SetComputeRoot32BitConstant(1,range.compactFirst,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch(std::min((range.count+63u)/64u,65535u),
                    (((range.count+63u)/64u)+65534u)/65535u,1);}l->SetComputeRoot32BitConstants(1,4,params,0);}l->SetComputeRoot32BitConstants(1,4,params,0);if(f.shadowDirty){ClearCount(l,f.cpuVisibility.shadowCount.Get(),f.cpuVisibility.shadowCountUAV);uint32_t shadowParams[4]={0,7,f.cpuCount,0};l->SetComputeRoot32BitConstants(1,4,shadowParams,0);if(f.cpuCount)l->Dispatch((f.cpuCount+63)/64,1,1);ArgumentsReady(l,f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get());}});
            if(passPlan.rayWork)useBuffer(computeNode,f.rayInstanceMap.Get(),
                f.rayMapBuild?GraphMode::ReadWrite:GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                f.rayMapBuild?(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                               D3D12_BARRIER_ACCESS_SHADER_RESOURCE):
                              D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayWork)useBuffer(computeNode,materials.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useTileSources(computeNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            if((passPlan.rayWork&&f.cpuCount)||f.shadowDirty)
                useBuffer(computeNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayWork&&f.instanceCount>f.cpuCount)
                useBuffer(computeNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(computeNode,f.rayDescriptors.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);if(f.shadowDirty)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }

            auto depthNode=frameGraph.AddPass("Depth",GraphQueue::Direct,40,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);if(passPlan.rayQueries&&!f.blasWork.Empty())BuildDynamic(l,f);if(passPlan.rayWork){AliasMemory(l);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=f.rayInstanceCount;build.Inputs.InstanceDescs=f.rayDescriptors->GetGPUVirtualAddress();build.DestAccelerationStructureData=f.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();if(f.tlasBuilt&&f.lastTlasCount==f.rayInstanceCount){build.SourceAccelerationStructureData=build.DestAccelerationStructureData;build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;}l->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(l,f.tlas.Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.lastTlasCount=f.rayInstanceCount;f.tlasBuilt=true;}if(f.shadowDirty){auto dsv=Dsv(shadowDSV);l->OMSetRenderTargets(0,nullptr,FALSE,&dsv);l->SetPipelineState(shadowPSO.Get());for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot)){uint32_t x=(slot%2)*1536,y=(slot/2)*1024;D3D12_RECT rectangle{LONG(x),LONG(y),LONG(x+1536),LONG(y+1024)};l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,1,&rectangle);for(uint32_t face=0;face<6;face++){Viewport(l,512,512,x+(face%3)*512,y+(face/3)*512);l->SetGraphicsRoot32BitConstant(1,slot*6+face,1);l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.shadowArguments.Get(),0,f.cpuVisibility.shadowCount.Get(),0);}}EmitBarriers(l,compiled.barriersDuring[0]);}Viewport(l,renderWidth,renderHeight);auto depthHandle=Dsv(depthDSV);if(!f.cpuCount)l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);l->SetPipelineState(depthPSO.Get());l->SetGraphicsRoot32BitConstants(1,4,params,0);if(f.instanceCount>f.cpuCount)l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,f.publishedVisibility.depthArguments.Get(),0,f.publishedVisibility.depthCount.Get(),0);});
            useTileSources(depthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(depthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.shadowDirty&&f.cpuCount)useBuffer(depthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(depthNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(depthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(depthNode,f.rayDescriptors.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.instanceCount>f.cpuCount)for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(passPlan.rayWork)useBuffer(depthNode,f.tlas.Get(),GraphMode::ReadWrite,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);useTexture(depthNode,depth,f.cpuCount?GraphMode::ReadWrite:GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);if(f.shadowDirty)useTexture(depthNode,shadow,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);

            if(f.shadowDirty)frameGraph.TransitionTexture(depthNode,shadow,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
            frameGraph.TransitionTexture(depthNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            uint32_t visibilityNode=UINT32_MAX;
            if(passPlan.depthHierarchy||passPlan.clusteredLights){
                visibilityNode=frameGraph.AddPass("Visibility",GraphQueue::Compute,50,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                    if(passPlan.depthHierarchy){
                        l->SetPipelineState(depthResolvePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);
                        if(passPlan.occlusion){
                            auto mipReady=[&](uint32_t mip){EmitBarriers(l,compiled.barriersDuring[mip]);};
                            mipReady(0);l->SetPipelineState(hizPSO.Get());for(uint32_t mip=1;mip<renderMips;mip++){l->SetComputeRoot32BitConstant(1,mip,1);l->Dispatch((std::max(1u,renderWidth>>mip)+7)/8,(std::max(1u,renderHeight>>mip)+7)/8,viewCount);mipReady(mip);}for(uint32_t mip=renderMips;mip<mips;mip++)mipReady(mip);
                            ClearCount(l,f.cpuVisibility.cullCount.Get(),f.cpuVisibility.cullCountUAV);
                            ClearCount(l,f.publishedVisibility.cullCount.Get(),f.publishedVisibility.cullCountUAV);
                            l->SetPipelineState(cullPSO.Get());
                            if(f.cpuCount){
                                l->SetComputeRoot32BitConstant(1,0,0);
                                l->SetComputeRoot32BitConstant(1,5,1);
                                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                                l->Dispatch((f.cpuCount+63)/64,1,1);
                            }
                            if(f.instanceCount>f.cpuCount){
                                l->SetComputeRoot32BitConstant(1,6,1);
                                for(const auto& bound:meshDomain->Bindings()){
                                    l->SetComputeRoot32BitConstant(1,bound.base,0);
                                    l->SetComputeRoot32BitConstant(1,Invalid,2);
                                    uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;
                                    l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);
                                }
                            }
                            l->SetComputeRoot32BitConstants(1,4,params,0);
                            ArgumentsReady(l,f.cpuVisibility.cullArguments.Get(),f.cpuVisibility.cullCount.Get());
                            ArgumentsReady(l,f.publishedVisibility.cullArguments.Get(),f.publishedVisibility.cullCount.Get());
                        }else EmitBarriers(l,compiled.barriersDuring[0]);
                    }

                
                });
                if(passPlan.depthHierarchy){useTexture(visibilityNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(visibilityNode,hierarchy,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.occlusion){useTileSources(visibilityNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                if(f.cpuCount)useBuffer(visibilityNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(f.instanceCount>f.cpuCount)useBuffer(visibilityNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.streamControl)useBuffer(visibilityNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);for(auto* resource:{f.cpuVisibility.cullArguments.Get(),f.cpuVisibility.cullCount.Get(),f.publishedVisibility.cullArguments.Get(),f.publishedVisibility.cullCount.Get()})useBuffer(visibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}
                if(passPlan.occlusion&&f.cpuCount)
                    useBuffer(visibilityNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Write,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(passPlan.clusteredLights){
                    // One invocation owns one existing 64x64/24-depth cluster.
                    // The 33rd candidate marks exact per-point BVH fallback.
                    const uint64_t groups=(uint64_t(clustersCount)+63u)/64u;
                    if(groups>65535u)
                        Fail("Light spatial clusters exceed DX12 dispatch X");
                    const auto clusterNode=frameGraph.AddPass(
                        "Light.Clusters",GraphQueue::Compute,51,
                        [&,groups](const CompiledNode&,
                                   ID3D12GraphicsCommandList7* l){
                            Bind(l,f,false);
                            l->SetComputeRoot32BitConstants(1,4,params,0);
                            l->SetPipelineState(clusterPSO.Get());
                            l->Dispatch(uint32_t(groups),1u,1u);
                        });
                    for(auto* resource:{f.lights.Get(),f.lightBvhNodes.Get()})
                        useBuffer(clusterNode,resource,GraphMode::Read,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    for(auto* resource:{f.clusters.Get(),f.clusterBounds.Get(),
                        f.clusterIndices.Get(),
                        f.clusterDirectionalIndices.Get(),
                        f.clusterDirectionalCount.Get()})
                        useBuffer(clusterNode,resource,GraphMode::Write,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                }
                if(passPlan.depthHierarchy){
                    if(passPlan.occlusion)for(uint32_t mip=0;mip<mips;++mip){
                        D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=mip;range.NumMipLevels=1;range.NumArraySlices=viewCount;range.NumPlanes=1;
                        frameGraph.TransitionTextureSubresource(visibilityNode,hierarchy,range,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,mip+1==mips);
                    }
                    else frameGraph.TransitionTexture(visibilityNode,hierarchy,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
                }
            }
            // Only opaque submission is grouped. Depth, shadow and ray
            // descriptors remain per-object, preserving their global IDs.
            if(f.cpuCount){
                auto batchNode=frameGraph.AddPass("CpuScene.BatchOpaque",GraphQueue::Compute,59,
                    [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        Bind(l,f,false);
                        ClearCount(l,f.cpuVisibility.batchCount.Get(),f.cpuVisibility.batchCountUAV);
                        l->SetPipelineState(batchOpaquePSO.Get());
                        for(uint32_t runIndex=0;runIndex<opaqueRuns.size();++runIndex){
                            const auto& run=opaqueRuns[runIndex];
                            const uint32_t batchParams[4]{run.first,0u,run.count,runIndex};
                            l->SetComputeRoot32BitConstants(1,4,batchParams,0);
                            l->Dispatch((run.count+1023u)/1024u,1,1);
                        }
                        // Each resident tile is handled independently on GPU.
                        // The mark, local scan, block-prefix and emit passes
                        // preserve the original authored order, not atomic order.
                        auto uavBarrier=[&](ID3D12Resource* resource){
                            BBarrier(l,resource,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        };
                        for(const auto& tileBatch:tileBatchDispatch){
                            if(!tileBatch.count)continue;
                            const uint32_t args[5]{
                                tileBatch.first,tileBatch.runDescriptor,
                                tileBatch.count,tileBatch.blockFirst,
                                tileBatch.runFirst};
                            l->SetComputeRoot32BitConstants(1,5,args,0);
                            const uint32_t groups=(tileBatch.count+63u)/64u;
                            l->SetPipelineState(tileBatchMarkPSO.Get());
                            l->Dispatch(std::min(groups,65535u),
                                (groups+65534u)/65535u,1);
                            uavBarrier(f.cpuVisibility.tileBatchMarks.Get());
                            l->SetPipelineState(tileBatchScanPSO.Get());
                            l->Dispatch(std::min(tileBatch.blockCount,65535u),
                                (tileBatch.blockCount+65534u)/65535u,1);
                            uavBarrier(f.cpuVisibility.tileBatchPrefixes.Get());
                            uavBarrier(f.cpuVisibility.tileBatchTotals.Get());
                            const uint32_t superFirst=tileBatchBlocks+
                                tileBatch.superFirst;
                            l->SetComputeRoot32BitConstant(1,superFirst,1);
                            l->SetComputeRoot32BitConstant(1,tileBatch.blockCount,2);
                            l->SetPipelineState(tileBatchOffsetScanPSO.Get());
                            l->Dispatch(tileBatch.superCount,1,1);
                            uavBarrier(f.cpuVisibility.tileBatchTotals.Get());
                            uavBarrier(f.cpuVisibility.tileBatchOffsets.Get());
                            l->SetPipelineState(tileBatchOffsetFinalizePSO.Get());
                            l->Dispatch((tileBatch.blockCount+63u)/64u,1,1);
                            uavBarrier(f.cpuVisibility.tileBatchOffsets.Get());
                            l->SetComputeRoot32BitConstants(1,5,args,0);
                            l->SetPipelineState(tileBatchEmitPSO.Get());
                            l->Dispatch(std::min(groups,65535u),
                                (groups+65534u)/65535u,1);
                        }
                        ArgumentsReady(l,f.cpuVisibility.batchArguments.Get(),f.cpuVisibility.batchCount.Get());
                    });
                useTileSources(batchNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                useBuffer(batchNode,f.worldInstances.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(batchNode,f.hotMeshes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(batchNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.cpuVisibility.tileBatchMarks.Get(),
                    f.cpuVisibility.tileBatchPrefixes.Get(),
                    f.cpuVisibility.tileBatchTotals.Get(),
                    f.cpuVisibility.tileBatchOffsets.Get()})
                    useBuffer(batchNode,resource,GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.cpuVisibility.batchArguments.Get(),f.cpuVisibility.batchCount.Get()})
                    useBuffer(batchNode,resource,GraphMode::Write,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }
            ID3D12Resource* cpuOpaqueArguments=f.cpuVisibility.batchArguments.Get();
            ID3D12Resource* cpuOpaqueCount=f.cpuVisibility.batchCount.Get();
            ID3D12Resource* publishedOpaqueArguments=passPlan.occlusion?f.publishedVisibility.cullArguments.Get():f.publishedVisibility.depthArguments.Get();
            ID3D12Resource* publishedOpaqueCount=passPlan.occlusion?f.publishedVisibility.cullCount.Get():f.publishedVisibility.depthCount.Get();

            // Each frame slot owns closed direct lists, one per resident tile
            // and reflection mode. Re-record only when the actual command state
            // or authored material partition changes; reuse GPU-generated args.
            for(auto it=f.tileDrawCache.begin();it!=f.tileDrawCache.end();){
                if(!tiles||!tiles->contains(it->first))
                    it=f.tileDrawCache.erase(it);
                else ++it;
            }
            std::vector<ID3D12CommandList*> residentOpaqueLists;
            residentOpaqueLists.reserve(tileBatchDispatch.size());
            const bool reflectedMode=passPlan.reflections;
            const uint32_t modeIndex=reflectedMode?1u:0u;
            // A constant 32-entry check replaces a per-tile per-run PSO walk.
            uint64_t pipelineSignature=1469598103934665603ull;
            for(uint32_t mask=0;mask<32;++mask){
                const auto* pso=(reflectedMode?sceneVariantPSO[mask]:
                    sceneDirectVariantPSO[mask]).Get();
                const auto address=reinterpret_cast<uintptr_t>(pso);
                pipelineSignature=HashMemory(&address,sizeof(address),
                    pipelineSignature);
            }
            for(const auto& tileBatch:tileBatchDispatch){
                const auto found=residentTileGpu.find(tileBatch.key);
                if(found==residentTileGpu.end())
                    Fail("Resident material batch lost its GPU tile");
                const auto& tile=found->second;
                if(!tileBatch.count||tile.runs.empty())continue;
                auto& cached=f.tileDrawCache[tileBatch.key][modeIndex];
                const uintptr_t pointers[]{
                    reinterpret_cast<uintptr_t>(f.cpuVisibility.batchArguments.Get()),
                    reinterpret_cast<uintptr_t>(f.cpuVisibility.batchCount.Get()),
                    reinterpret_cast<uintptr_t>(f.uploadGeneration->resource.Get()),
                    reinterpret_cast<uintptr_t>(hdr.resource.Get()),
                    reinterpret_cast<uintptr_t>(reflections.resource.Get()),
                    reinterpret_cast<uintptr_t>(depth.resource.Get()),
                    reinterpret_cast<uintptr_t>(root.Get()),
                    reinterpret_cast<uintptr_t>(descriptors.Get()),
                    reinterpret_cast<uintptr_t>(samplers.Get()),
                    reinterpret_cast<uintptr_t>(indirect.Get())
                };
                const uint64_t commandState[]{
                    tile.runRevision,tile.topologyGeneration,
                    tileBatch.first,tileBatch.runFirst,tileBatch.count,
                    f.constantsOffset,renderWidth,renderHeight,MsaaSamples,
                    modeIndex,HashMemory(pointers,sizeof(pointers)),
                    hdrRTV,reflectionsRTV,depthDSV,pipelineSignature
                };
                const uint64_t signature=HashMemory(
                    commandState,sizeof(commandState));
                if(!cached.initialized||cached.signature!=signature){
                    if(!cached.initialized)
                        CommandsInit(cached.commands,D3D12_COMMAND_LIST_TYPE_DIRECT);
                    cached.commands.Reset();
                    auto* l=cached.commands.list.Get();
                    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                    l->SetDescriptorHeaps(2,heaps);
                    l->SetGraphicsRootSignature(root.Get());
                    l->SetGraphicsRootConstantBufferView(0,
                        f.uploadGeneration->resource->GetGPUVirtualAddress()+
                        f.constantsOffset);
                    const uint32_t drawParams[4]{};
                    l->SetGraphicsRoot32BitConstants(1,4,drawParams,0);
                    Viewport(l,renderWidth,renderHeight);
                    auto dsv=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]{Rtv(hdrRTV),
                        Rtv(reflectionsRTV)};
                    l->OMSetRenderTargets(reflectedMode?2u:1u,targets,FALSE,&dsv);
                    D3D12_SHADING_RATE_COMBINER combiners[]{
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
                    l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);
                    for(uint32_t r=0;r<tile.runs.size();++r){
                        const auto& run=tile.runs[r];
                        auto* pso=(reflectedMode?sceneVariantPSO[run.mask]:
                            sceneDirectVariantPSO[run.mask]).Get();
                        if(!pso)
                            Fail("Resident opaque material specialization missing");
                        l->SetPipelineState(pso);
                        l->ExecuteIndirect(indirect.Get(),run.count,
                            cpuOpaqueArguments,
                            uint64_t(tileBatch.first+run.first)*16,
                            cpuOpaqueCount,
                            uint64_t(tileBatch.runFirst+r)*4);
                    }
                    cached.commands.Close();
                    cached.signature=signature;
                    cached.initialized=true;
                }
                residentOpaqueLists.push_back(cached.commands.list.Get());
            }

            // GPU publications do not advertise contiguous per-material runs.
            // Preserve the generic PSO for all external providers rather than
            // assume a physics-only material summary describes their content.
            auto opaqueNode=frameGraph.AddPass("Opaque",GraphQueue::Direct,60,
                [&,cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,true);
                    l->SetGraphicsRoot32BitConstants(1,4,params,0);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]={Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    float clear[]={.008f,.013f,.021f,1},black[]={0,0,0,0};
                    l->ClearRenderTargetView(targets[0],clear,0,nullptr);
                    if(passPlan.reflections)l->ClearRenderTargetView(targets[1],black,0,nullptr);
                    l->OMSetRenderTargets(passPlan.reflections?2:1,targets,FALSE,&depthHandle);
                    D3D12_SHADING_RATE_COMBINER combiners[]={
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
                    l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);
                    for(uint32_t i=0;i<opaqueRuns.size();++i){
                        const auto& run=opaqueRuns[i];
                        auto* pso=(passPlan.reflections?
                            sceneVariantPSO[run.mask]:sceneDirectVariantPSO[run.mask]).Get();
                        if(!pso)Fail("Opaque material specialization unavailable");
                        l->SetPipelineState(pso);
                        l->ExecuteIndirect(indirect.Get(),run.count,cpuOpaqueArguments,
                            uint64_t(run.first)*16,cpuOpaqueCount,uint64_t(i)*4);
                    }
                });
            auto opaqueTileNode=frameGraph.AddPass(
                "Opaque.Resident",GraphQueue::Direct,61);
            frameGraph.Node(opaqueTileNode).reusableDirectLists=
                residentOpaqueLists;
            frameGraph.Dependency(opaqueNode,opaqueTileNode);
            auto opaqueTailNode=frameGraph.AddPass("Opaque.Tail",GraphQueue::Direct,62,
                [&,publishedOpaqueArguments,publishedOpaqueCount]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,true);
                    l->SetGraphicsRoot32BitConstants(1,4,params,0);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]{
                        Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    l->OMSetRenderTargets(passPlan.reflections?2u:1u,targets,
                        FALSE,&depthHandle);
                    D3D12_SHADING_RATE_COMBINER combiners[]{
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
                    l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);
                    if(f.instanceCount>f.cpuCount){
                        l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());
                        l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,
                            publishedOpaqueArguments,0,publishedOpaqueCount,0);
                    }
                    if(f.transparentCount){
                        l->SetPipelineState((passPlan.reflections?
                                            transparentPSO:transparentDirectPSO).Get());
                        for(uint32_t index=0;index<f.transparentCount;index++){
                            uint32_t object=f.transparent[index];
                            l->SetGraphicsRoot32BitConstant(1,object,0);
                            l->DispatchMesh(meshes.ranges[CpuInstance(f,object).mesh].meshletCount,1,1);
                        }
                    }
                });
            frameGraph.Dependency(opaqueTileNode,opaqueTailNode);
            if(f.rayInstanceCount)useBuffer(opaqueNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(const auto* fields=frameResources.Find(GpuFrameResourceId::PopulationOpticalFields))useBuffer(opaqueNode,fields->resource,GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:domainContext.lightOcclusionTextures)frameGraph.Texture(opaqueNode,nullptr,field->resource,D3D12_BARRIER_LAYOUT_COMMON,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueNode;
            useTileSources(opaqueNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            // Reflection hit queries need live BVH even for <=4 lights.
            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            // The three sequential opaque nodes each advertise their actual
            // reads/writes, retaining cross-frame UAV and RT dependencies.
            if(f.rayInstanceCount)useBuffer(opaqueTileNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(const auto* fields=frameResources.Find(GpuFrameResourceId::PopulationOpticalFields))useBuffer(opaqueTileNode,fields->resource,GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:domainContext.lightOcclusionTextures)frameGraph.Texture(opaqueTileNode,nullptr,field->resource,D3D12_BARRIER_LAYOUT_COMMON,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTileNode;
            useTileSources(opaqueTileNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTileNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTileNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTileNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTileNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTileNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTileNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            // Reflection hit queries need live BVH even for <=4 lights.
            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTileNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries)useBuffer(opaqueTileNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTileNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTileNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTileNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTileNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(f.rayInstanceCount)useBuffer(opaqueTailNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(const auto* fields=frameResources.Find(GpuFrameResourceId::PopulationOpticalFields))useBuffer(opaqueTailNode,fields->resource,GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:domainContext.lightOcclusionTextures)frameGraph.Texture(opaqueTailNode,nullptr,field->resource,D3D12_BARRIER_LAYOUT_COMMON,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTailNode;
            useTileSources(opaqueTailNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTailNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTailNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTailNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTailNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTailNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTailNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            // Reflection hit queries need live BVH even for <=4 lights.
            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTailNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries)useBuffer(opaqueTailNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTailNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTailNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTailNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTailNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(MsaaSamples>1){frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);}else{frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}frameGraph.TransitionTexture(opaqueTailNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(MsaaSamples>1){
                auto resolveNode=frameGraph.AddPass("ResolveColor",GraphQueue::Direct,70,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){for(uint32_t view=0;view<viewCount;view++){l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);if(passPlan.reflections)l->ResolveSubresource(reflectionsResolved.resource.Get(),view,reflections.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);}});
                coreColorNode=resolveNode;
                useTexture(resolveNode,hdr,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);if(passPlan.reflections){useTexture(resolveNode,reflections,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,reflectionsResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);}
                frameGraph.TransitionTexture(resolveNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(resolveNode,reflectionsResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }

            if(passPlan.reflections){
                auto postNode=frameGraph.AddPass("Post",GraphQueue::Direct,90,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(denoisePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(postNode,resolvedReflection,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,hierarchy,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(reflectionHistoryReady){useTexture(postNode,history[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,historyDepth[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}useTexture(postNode,history[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);useTexture(postNode,historyDepth[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                frameGraph.TransitionTexture(postNode,history[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);frameGraph.TransitionTexture(postNode,historyDepth[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                auto mergeNode=frameGraph.AddPass("Reflection.Composite",GraphQueue::Direct,91,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t mergeParams[]{0,0,Physical(hdrResolvedUAV),0};l->SetComputeRoot32BitConstants(1,4,mergeParams,0);l->SetPipelineState(mergeReflectionPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(mergeNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(mergeNode,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);coreColorNode=mergeNode;
            }

        }else{
            auto clearNode=frameGraph.AddPass("Core.ClearVisualTarget",GraphQueue::Direct,60,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Viewport(l,renderWidth,renderHeight);
                const float color[]={.008f,.013f,.021f,1.0f};auto rtv=Rtv(hdrRTV);auto dsv=Dsv(depthDSV);
                l->ClearRenderTargetView(rtv,color,0,nullptr);
                l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);
                EmitBarriers(l,compiled.barriersDuring[0]);
                EmitBarriers(l,compiled.barriersDuring[1]);
                if(MsaaSamples>1){
                    EmitBarriers(l,compiled.barriersDuring[2]);
                    for(uint32_t view=0;view<viewCount;++view)l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
            });
            useTexture(clearNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
            useTexture(clearNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
            if(MsaaSamples>1)useTexture(clearNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);
            frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET,true);
            frameGraph.TransitionTexture(clearNode,depth,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,true);
            if(MsaaSamples>1){
                frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,true);
                frameGraph.TransitionTexture(clearNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
            coreColorNode=clearNode;
        }

        if(!captureTicket.request)capture.Take(captureTicket);
        if(captureTicket.request&&captureTicket.options.eye>=viewCount){capture.Failed(captureTicket,"SELECTED EYE UNAVAILABLE");captureTicket={};}
        if(terminalDomain)terminalDomain->SetSuppressed(captureTicket.request&&!captureTicket.options.includeInterface);
        // Scene products have now been declared. External domains consume the
        // current versions and publish subsequent mutations before Present.
        // Cross-domain consumers registered first name their content version.
        if(aaMode){
            if(!aaSourceLease){GpuTextureDesc d{};d.width=renderWidth;d.height=renderHeight;d.depthOrArraySize=viewCount;d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.uav=false;d.name=L"FXAA.SpatialSource";aaSourceLease=CreateManagedTexture2D(d);if(!aaSourceLease)Fail("FXAA source allocation");aaSource.resource=aaSourceLease->resource;aaSource.layout=D3D12_BARRIER_LAYOUT_COMMON;}
            auto copy=frameGraph.AddPass("FXAA.Snapshot",GraphQueue::Direct,92,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyResource(aaSource.resource.Get(),resolvedColor.resource.Get());});
            useTexture(copy,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);useTexture(copy,aaSource,GraphMode::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            auto filter=frameGraph.AddPass("FXAA.Spatial",GraphQueue::Direct,93,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t p[]{DescriptorIndex(aaSourceLease->srv),Physical(hdrResolvedUAV),aaMode,0};l->SetComputeRoot32BitConstants(1,4,p,0);l->SetPipelineState(fxaaPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
            useTexture(filter,aaSource,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(filter,resolvedColor,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
        for(auto& domain:externalDomains){domainBuilder.SetDomain(domain->Identity());domain->BuildPasses(domainBuilder,domainContext);}domainBuilder.SetDomain(GpuDomainId::Invalid);

        void* captureRecord{};
        auto presentNode=frameGraph.AddPass("Present",GraphQueue::Direct,100,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Viewport(l,width,height);auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);l->OMSetRenderTargets(1,&target,FALSE,&ds);Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);l->SetGraphicsRoot32BitConstant(1,passPlan.reflections?1u:0u,3);l->SetPipelineState(presentPSO.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->DrawInstanced(3,1,0,0);
            if(captureRecord){
                auto* slot=static_cast<TextureReadback*>(captureRecord);
                D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource=slot->source.Get();barrier.Transition.Subresource=slot->subresource;
                barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                l->ResourceBarrier(1,&barrier);Graphics::RecordTextureReadback(captureRecord,l);
                std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);l->ResourceBarrier(1,&barrier);
            }
        });useTexture(presentNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)useTexture(presentNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(presentNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

        frameGraph.Dependency(coreColorNode,presentNode);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom)){LogEvent(traceFrame,"render.skip","reason=managed_domain_residency_budget");releasePublications(true);publications.clear();managedMemory.EndFrame(SubmittedFences());endDomains();active=nullptr;historyReady=false;return;}
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());
        if(captureTicket.request){
            // The copy is part of Present itself. READBACK heaps stay COPY_DEST;
            // they require no enhanced barrier or separate frame-graph pass.
            auto handle=facade->RequestTextureReadback(xrColorTargets[xr.color].Get(),captureTicket.options.eye,xr.input.frameId,&captureRecord);
            if(handle)captureReadbacks.push_back({captureTicket,handle,QpcNowSeconds()});
            else capture.Failed(captureTicket,"GPU CAPTURE FORMAT / ALLOCATION FAILED");
            captureTicket={};
        }
        for(const auto& bound:publications)frameGraph.PublicationComplete(bound.publication,presentNode);
        frameGraph.CompileExecutionBatches();
        auto batchCommands=[&](GraphQueue queue,uint32_t index)->Commands&{
            auto& slots=f.batchCommands[QI(queue)];while(slots.size()<=index){auto commands=std::make_unique<Commands>();CommandsInit(*commands,queue==GraphQueue::Copy?D3D12_COMMAND_LIST_TYPE_COPY:queue==GraphQueue::Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT);slots.push_back(std::move(commands));}return *slots[index];
        };
        const auto progress=coordinator.progress?
            coordinator.progress:std::make_shared<GpuSubmissionProgress>();
        // Observation is installed before checking submission gates. A signal
        // raced with Observe is still visible through Enqueued() and its epoch.
        for(const auto& bound:publications)if(bound.deferred){
            if(bound.publication.submission)bound.publication.submission->Observe(progress);
            for(const auto& group:bound.publication.groups)
                if(group.submission)group.submission->Observe(progress);
        }
        auto canSubmit=[&](const GraphPass& pass)->bool{
            for(const auto& wait:pass.externalWaits)
                for(const auto& bound:publications)if(bound.deferred){
                    const auto& pub=bound.publication;
                    if(wait.fence==pub.producerFence.Get()){
                        if(wait.value>pub.producerValue)Fail("GPU publication wait exceeds reserved signal");
                        if(pub.submission){
                            if(!pub.submission->Enqueued(wait.value)){
                                if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                    Fail("GPU producer completed without enqueueing publication signal");
                                return false;
                            }
                        }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                    }
                    for(const auto& group:pub.groups)
                        if(wait.fence==group.producerFence.Get()){
                            if(wait.value>group.producerValue)Fail("GPU group wait exceeds reserved signal");
                            if(group.submission){
                                if(!group.submission->Enqueued(wait.value)){
                                    if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                        Fail("GPU producer completed without enqueueing group signal");
                                    return false;
                                }
                            }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                        }
                }
            return true;
        };
        if(performanceProfiler.active)
            frameGraph.ExecuteProfiled(performanceProfiler,*jobs,xr.input.frameId,batchCommands,canSubmit,progress);
        else frameGraph.Execute(*jobs,xr.input.frameId,batchCommands,canSubmit,progress);
        if(captureRecord){
            auto* slot=static_cast<TextureReadback*>(captureRecord);auto fence=frameGraph.Signal(presentNode);
            if(!slot->recorded||!fence){slot->failed=true;LogEvent(traceFrame,"capture.error","stage=copy_not_submitted");}
            else {slot->fenceValue=fence;slot->recorded=false;LogEvent(traceFrame,"capture.gpu","frame=%llu eye=%u fence=%llu",static_cast<unsigned long long>(slot->frameId),slot->subresource,static_cast<unsigned long long>(fence));}
        }
        FinishTextureReadbacks();if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");
        // The Morton order is committed only after successful submission.
        // A cancelled frame must leave the same update pending next time.
        if(coreSceneWork&&bvhSortNeeded){
            f.lightBvhSorted=true;
            f.lightBvhPendingSort=false;
            f.lightBvhSortedCount=f.lightCount;
            f.lightBvhSortSnapshot=f.lightData;
            f.lightBvhLastSortFrame=frameNumber;
        }
        // Acquit dirty indices only if the whole graph was submitted.
        if(copyStatic)sceneCache.Commit(frameSlot);
        // Keep metadata edits pending when a frame is skipped or cancelled.
        for(const auto& transfer:f.tileRunTransfers){
            auto it=residentTileGpu.find(transfer.key);
            if(it!=residentTileGpu.end()&&
               it->second.runRevision==transfer.revision&&
               it->second.runBuffer.Get()==transfer.buffer.Get())
                it->second.runsDirty=false;
        }
        if(coreSceneWork&&(!rayDirty||passPlan.rayWork))rayScene.Commit(frameSlot);
        if(coreSceneWork&&tileCatalog){
            for(const auto& transfer:f.tileTransfers)
                tileCatalog->CommitTileChanges(transfer.key,
                    transfer.topologyGeneration,transfer.updates);
            if(passPlan.rayWork)
                for(const auto& ack:rayTileAcks)
                    tileCatalog->CommitRayTileChanges(frameSlot,ack.key,
                        ack.topologyGeneration,ack.updates);
        }
        if(passPlan.rayWork&&f.rayMapBuild)
            f.committedRayMapSegments=f.rayMapSegments;
        residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;xr.submitted=true;LogEvent(traceFrame,"gpu.ray_scene","tlas_update=%u cpu_dirty_ranges=%u renewed_streams=%u",unsigned(passPlan.rayWork),uint32_t(rayCpuDirty.size()),uint32_t(rayPublicationDirty.size()));
    }

    void Graphics::Impl::Bind(ID3D12GraphicsCommandList7* l,Frame& f,bool graphics){if(!f.uploadGeneration)Fail("frame upload generation missing");ID3D12DescriptorHeap* heaps[]={descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);auto address=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;if(graphics){l->SetGraphicsRootSignature(root.Get());l->SetGraphicsRootConstantBufferView(0,address);}else{l->SetComputeRootSignature(root.Get());l->SetComputeRootConstantBufferView(0,address);}}

    const GpuInstance& Graphics::Impl::CpuInstance(const Frame& f,uint32_t index)const{uint32_t staticCount=sceneCache.StaticCount();if(index<staticCount)return sceneCache.StaticInstances()[index];index-=staticCount;if(index>=f.dynamicCount)Fail("CPU instance index");return f.objects[index];}

    void Graphics::Impl::BuildDynamic(ID3D12GraphicsCommandList7* list,Frame& f) {
        
        uint32_t ordinal=0;
        for(uint32_t dynamicIndex:f.blasWork.Active()){
            if(dynamicIndex>=dynamicMeshes.size()||dynamicIndex>=f.dynamicBLAS.size()||dynamicIndex>=f.dynamicBlasBuilt.size())Fail("dynamic work index");
            if(ordinal++)BBarrier(list,f.dynamicScratch.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            uint32_t mesh=dynamicMeshes[dynamicIndex];auto geometry=DynamicGeometry(f,mesh);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.pGeometryDescs=&geometry;build.DestAccelerationStructureData=f.dynamicBLAS[dynamicIndex]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.dynamicScratch->GetGPUVirtualAddress();bool built=f.dynamicBlasBuilt[dynamicIndex]!=0;if(built){build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;build.SourceAccelerationStructureData=build.DestAccelerationStructureData;}BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),built?D3D12_BARRIER_SYNC_ALL_SHADING:D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,built?D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ:D3D12_BARRIER_ACCESS_NO_ACCESS,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);list->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.dynamicBlasBuilt[dynamicIndex]=1;
        }
    }

    void Graphics::Impl::PrepareLights(Frame& f,const RenderScene& snapshot) {
        if(snapshot.lightCount!=snapshot.lights.size())Fail("Render scene light count mismatch");
        f.lightCount=snapshot.lightCount;f.lightData.resize(f.lightCount);f.shadowDirty=0;f.hasShadowedLights=false;f.hasShadowAtlasLights=false;f.shadowIDs.fill(Invalid);f.shadowKeys.fill(0);
        // Geometry is keyed per light, not by the entire camera-selected
        // scene. Include off-screen casters whose conservative bounds intersect
        // a light's influence; never use the current view to invalidate shadows.
        const GpuSceneCache* tiles=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto shadowCasterNear=[&](const SceneLight& light,const GpuInstance& o){
            if(o.flags&(DynamicObject|DeformedObject|NoShadow))return false;
            if(o.mesh>=meshes.ranges.size())
                Fail("Shadow caster references missing mesh");
            const auto bounds=meshes.ranges[o.mesh].bounds;
            const double maximumScale=std::max({
                std::abs(double(o.scale.x)),std::abs(double(o.scale.y)),
                std::abs(double(o.scale.z))});
            // Include mesh-local centres that are offset from the pivot.
            // Same conservative inflation as UrbanScene's spatial indexing.
            const double localReach=std::sqrt(double(bounds.x)*bounds.x+
                double(bounds.y)*bounds.y+double(bounds.z)*bounds.z)+
                double(bounds.w);
            const double radius=double(light.radius)+
                localReach*maximumScale*2.5+2.;
            const double dx=double(o.positionRadius.x)-double(light.position.x);
            const double dz=double(o.positionRadius.z)-double(light.position.z);
            return dx*dx+dz*dz<=radius*radius;
        };
        std::array<bool,ShadowSlots> assigned{};
        for(uint32_t i=0;i<f.lightCount;i++) {
            auto light=snapshot.lights[i];if(light.animationNode!=Invalid){auto world=animation.NodeMatrix(light.animationNode)*XMMatrixScaling(1,1,-1);XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));}if(contributions.lightContext)contributions.lightContext(contributions.context,light,f.simulationTime);int slot=-1;
            if((light.flags&1)&&!(light.flags&(2|4))) {
                for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]&&shadowCache[j].id==light.id){slot=int(j);break;}
                if(slot<0)for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]){bool reserved=false;for(uint32_t k=i+1;k<f.lightCount;k++)if(snapshot.lights[k].id==shadowCache[j].id)reserved=true;if(!reserved){slot=int(j);break;}}
            }
            if(slot>=0) {
                uint32_t s=uint32_t(slot);assigned[s]=true;f.shadowIDs[s]=light.id;f.shadowLights[s]=light;
                uint64_t key=1469598103934665603ull;
                key=HashMemory(&light.position,sizeof(light.position),key);
                key=HashMemory(&light.radius,sizeof(light.radius),key);
                key=HashMemory(&light.revision,sizeof(light.revision),key);
                // Room and other non-tile solids are tested individually.
                for(const auto& object:sceneCache.StaticInstances())
                    if(shadowCasterNear(light,object))
                        key=HashMemory(&object,sizeof(object),key);
                for(uint32_t j=0;j<f.snapshotCount;++j)
                    if(shadowCasterNear(light,f.objects[j]))
                        key=HashMemory(&f.objects[j],sizeof(GpuInstance),key);
                // A cell has a stable 64-bit owner and a content signature.
                // Geometry outside this light's radius cannot dirty its map.
                // No camera-frustum test is involved in this lookup.
                if(tiles)for(const auto& [tileKey,tile]:tiles->ResidentTiles())
                    for(uint32_t cell=0;cell<GpuResidentTile::Cells;++cell){
                        if(!tile.shadowCasterCounts[cell])continue;
                        const uint64_t identity=tile.cellObjects[cell].front();
                        const auto obj=tile.byIdentity.find(identity);
                        if(obj==tile.byIdentity.end())
                            Fail("Resident shadow cell identity lookup failed");
                        const auto& sample=tile.objects[obj->second];
                        const double radius=double(light.radius)+
                            double(sample.cellRadius);
                        const double dx=double(sample.cellX)-double(light.position.x);
                        const double dz=double(sample.cellZ)-double(light.position.z);
                        if(dx*dx+dz*dz>radius*radius)continue;
                        key=HashMemory(&tileKey.x,sizeof(tileKey.x),key);
                        key=HashMemory(&tileKey.z,sizeof(tileKey.z),key);
                        key=HashMemory(&cell,sizeof(cell),key);
                        key=HashMemory(&tile.shadowCellSignatures[cell],
                                       sizeof(tile.shadowCellSignatures[cell]),key);
                    }
                f.shadowKeys[s]=key;
                if(!shadowCache[s].valid||shadowCache[s].id!=light.id||shadowCache[s].key!=key)f.shadowDirty|=1u<<s;
                auto at=XMLoadFloat3(&light.position);const XMVECTOR directions[]={XMVectorSet(1,0,0,0),XMVectorSet(-1,0,0,0),XMVectorSet(0,1,0,0),XMVectorSet(0,-1,0,0),XMVectorSet(0,0,1,0),XMVectorSet(0,0,-1,0)};
                for(uint32_t face=0;face<6;face++){auto up=face==2?XMVectorSet(0,0,-1,0):face==3?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0);XMStoreFloat4x4(&shadowMatrices[s*6+face],XMMatrixLookToLH(at,directions[face],up)*XMMatrixPerspectiveFovLH(XM_PIDIV2,1,std::max(.1f,light.radius),.02f));}
            }
            f.hasShadowedLights|=(light.flags&1)!=0;f.hasShadowAtlasLights|=slot>=0;
            f.lightData[i]={{light.position.x,light.position.y,light.position.z,std::max(.1f,light.radius)},{light.color.x,light.color.y,light.color.z,light.power},{light.direction.x,light.direction.y,light.direction.z,light.outerCos},{light.innerCos,float(slot),float(light.flags),0}};
        }
        f.lightsOffset=Reserve(f,std::max<uint64_t>(1,UINT64(f.lightCount))*sizeof(Light));
        if(f.lightCount)memcpy(f.uploadGeneration->mapped+f.lightsOffset,
            f.lightData.data(),UINT64(f.lightCount)*sizeof(Light));
    }

    const RenderScene& Graphics::Impl::Visuals(const FrameInput& input){
        auto& r=visualFrame;r.objects.clear();r.count=0;r.lights=baseLights;r.lightCount=uint32_t(r.lights.size());double displayTime=input.targetDisplayQpc?QpcSeconds(input.targetDisplayQpc):input.time;if(!sceneTimeOriginValid&&displayTime>0){sceneTimeOrigin=displayTime;sceneTimeOriginValid=true;}double sceneTime=sceneTimeOriginValid?displayTime-sceneTimeOrigin:0;r.time=displayTime;r.inputFrameId=input.frameId;r.simulationTick=0;
        if(contributions.appendContext)contributions.appendContext(contributions.context,r,input,sceneTime);
        if(r.count!=r.objects.size())Fail("Render contribution count mismatch");
        return r;
    }

    bool Graphics::Impl::Prepare(const FrameInput& input) {
        if(!sceneLoaded)return false;
        const bool samplePrepare=SessionLogCompact()&&(performanceProfiler.active||input.frameId%120u==0);
        const double prepareBegin=samplePrepare?QpcNowSeconds():0.;
        ApplyMultisampling();
        const auto& snapshot=Visuals(input);
        const double snapshotDone=samplePrepare?QpcNowSeconds():0.;
        active=nullptr;std::array<UINT64,GraphQueueCount> completed={copy.fence->GetCompletedValue(),compute.fence->GetCompletedValue(),direct.fence->GetCompletedValue()};for(auto value:completed)if(value==UINT64_MAX){Hr(device->GetDeviceRemovedReason(),"D3D12 device removed");Fail("Invalid GPU completion fence");}
        for(uint32_t i=0;i<3;i++){auto& candidate=frames[(frameNumber+i)%3];bool ready=true;for(uint32_t q=0;q<GraphQueueCount;q++)ready&=completed[q]>=candidate.queueFences[q];if(ready){active=&candidate;break;}}
        const GpuSceneCache* tileCatalog=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        uint64_t residentCount=0;
        if(tileCatalog)for(const auto& [key,tile]:tileCatalog->ResidentTiles())
            residentCount+=tile.objects.size();
        uint64_t totalCpu=uint64_t(sceneCache.StaticCount())+snapshot.count+residentCount;if(!active){LogEvent(snapshot.inputFrameId,"render.skip","reason=%s","frame_context_busy");return false;}if(totalCpu>=Invalid)Fail("CPU scene instance address space exhausted");
        auto& f=*active;
        UINT64 uploadRequired=UINT64(snapshot.count)*sizeof(GpuInstance)+animation.Motions().size()*sizeof(GpuMotion)+animation.Bones().size()*sizeof(GpuBone)+animation.Weights().size()*sizeof(float)+sizeof(Constants)+UINT64(snapshot.lightCount)*sizeof(Light)+8192+surfaceData.size()*sizeof(Surface);
        auto upload=std::atomic_load_explicit(&uploadGeneration,std::memory_order_acquire);if(!upload)Fail("upload generation unavailable");
        if(uploadRequired>upload->chunk){
            UINT64 capacity=upload->chunk;while(capacity<uploadRequired)capacity*=2;auto next=NewUploadGeneration(capacity);std::atomic_store_explicit(&uploadGeneration,next,std::memory_order_release);LogEvent(snapshot.inputFrameId,"gpu.capacity_generation","domain=graphics.upload generation=%llu old_bytes=%llu new_bytes=%llu",static_cast<unsigned long long>(next->generation),static_cast<unsigned long long>(upload->chunk*3),static_cast<unsigned long long>(next->chunk*3));upload=std::move(next);
        }
        f.uploadGeneration=upload;f.base=UINT64(&f-frames.data())*upload->chunk;auto* mapped=upload->mapped;
        f.sourceInput=snapshot.inputFrameId;f.sourceTick=snapshot.simulationTick;f.bump=0;f.simulationTime=sceneTimeOriginValid?snapshot.time-sceneTimeOrigin:0;animation.Evaluate(f.simulationTime);auto uploadAnimation=[&](const auto& values){using Value=typename std::decay_t<decltype(values)>::value_type;auto offset=Reserve(f,std::max(size_t(1),values.size())*sizeof(Value));if(!values.empty())memcpy(mapped+offset,values.data(),values.size()*sizeof(Value));else memset(mapped+offset,0,sizeof(Value));return offset;};f.motionOffset=uploadAnimation(animation.Motions());f.boneOffset=uploadAnimation(animation.Bones());f.weightOffset=uploadAnimation(animation.Weights());f.constantsOffset=Reserve(f,sizeof(Constants));f.constants=reinterpret_cast<Constants*>(mapped+f.constantsOffset);f.snapshotCount=snapshot.count;f.residentCount=uint32_t(residentCount);
        f.dynamicCount=f.snapshotCount; // Only transient snapshot objects need CPU conversion.
        f.cpuCount=uint32_t(totalCpu);f.instanceCount=f.cpuCount;f.additionalCount=0;
        f.objectsOffset=Reserve(f,std::max(1u,f.snapshotCount)*sizeof(GpuInstance));
        f.cpuObjects.resize(f.dynamicCount);f.objects=f.cpuObjects.data();
        struct Work {Impl* self;Frame* frame;const RenderScene* snapshot;float alpha;};double tickPhase=f.simulationTime/PhysicsDt;Work work{this,&f,&snapshot,std::clamp(float(tickPhase-std::floor(tickPhase)),0.f,1.f)};
        // Cap the number of tiny dispatches to around 12 jobs while keeping
        // work distribution across the five workers and the render thread.
        // Each instance is written once, at the same index as before.
        const uint32_t conversionGrain=std::max(256u,(f.snapshotCount+11u)/12u);
        jobs->ParallelFor(f.snapshotCount,conversionGrain,[](void* context,uint32_t begin,uint32_t end){auto& work=*static_cast<Work*>(context);auto& self=*work.self;auto& f=*work.frame;for(uint32_t i=begin;i<end;i++)f.objects[i]=self.ConvertObject(work.snapshot->objects[i],work.alpha);},&work);
        // Resident instances and identities remain in persistent GPU tile
        // buffers. Only the temporary snapshot is materialized here.
        std::vector<uint64_t> rayObjectIdentities(f.snapshotCount,0);
        const double conversionDone=samplePrepare?QpcNowSeconds():0.;
        rayScene.UpdateDynamic(sceneCache.StaticCount(),
            std::span<const GpuInstance>(f.objects,f.dynamicCount),
            std::span<const uint64_t>(rayObjectIdentities.data(),rayObjectIdentities.size()));
        f.deformationWork.BeginFrame();f.blasWork.BeginFrame();
        auto deformationRevision=[&](uint32_t dynamicIndex){uint32_t mesh=dynamicMeshes[dynamicIndex];uint64_t revision=1469598103934665603ull;const auto& motion=animation.Motions()[mesh];if(motion.binding.x!=Invalid){revision=HashMemory(&motion,sizeof(motion),revision);if(motion.binding.z){uint32_t first=motion.binding.y,count=motion.binding.z;if(uint64_t(first)+count>animation.Bones().size())Fail("animation bone revision range");if(count)revision=HashMemory(animation.Bones().data()+first,size_t(count)*sizeof(GpuBone),revision);}if(motion.morph.x){uint32_t first=motion.morph.y,count=motion.morph.x;if(uint64_t(first)+count>animation.Weights().size())Fail("animation weight revision range");if(count)revision=HashMemory(animation.Weights().data()+first,size_t(count)*sizeof(float),revision);}}else{const auto& d=meshes.deformations[mesh];revision=HashMemory(&d,sizeof(d),revision);if(d.axisAmplitude.w!=0&&d.frequencyPhase.x!=0){float stretch=1+d.axisAmplitude.w*std::sin(float(f.simulationTime)*d.frequencyPhase.x+d.frequencyPhase.y);revision=HashMemory(&stretch,sizeof(stretch),revision);}}return revision?revision:1ull;};
        auto requestDeformation=[&](uint32_t dynamicIndex,bool rayVisible){if(dynamicIndex==Invalid||dynamicIndex>=dynamicMeshes.size())Fail("DeformedObject references mesh without dynamic deformation capability");uint64_t revision=deformationRevision(dynamicIndex);f.pendingDynamicRevision[dynamicIndex]=revision;if(f.dynamicGeometryRevision[dynamicIndex]!=revision&&!f.deformationWork.Activate(dynamicIndex))Fail("deformation work index");if(rayVisible&&f.dynamicBlasRevision[dynamicIndex]!=revision&&!f.blasWork.Activate(dynamicIndex))Fail("dynamic BLAS work index");};
        for(uint32_t dynamicIndex:staticDeformedMeshes){bool rayVisible=std::find(staticDeformedRtMeshes.begin(),staticDeformedRtMeshes.end(),dynamicIndex)!=staticDeformedRtMeshes.end();requestDeformation(dynamicIndex,rayVisible);}
        for(uint32_t i=0;i<f.dynamicCount;i++)if(f.objects[i].flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[f.objects[i].mesh];requestDeformation(dynamicIndex,(f.objects[i].flags&NoRayTracing)==0);}
        if(tileCatalog)for(const auto& [key,tile]:tileCatalog->ResidentTiles())
            for(const auto& [mesh,usage]:tile.deformableMeshes){
                if(!usage.total)continue;
                if(mesh>=meshDynamicIndex.size())
                    Fail("Resident deformable mesh index out of range");
                requestDeformation(meshDynamicIndex[mesh],usage.ray!=0);
            }
        PrepareLights(f,snapshot);
        // CPU-only scene analysis reads ordinary cacheable memory; publish the
        // completed instances to write-combined UPLOAD memory only once.
        const double uploadCopyBegin=samplePrepare?QpcNowSeconds():0.;
        if(f.snapshotCount)memcpy(mapped+f.objectsOffset,f.objects,size_t(f.snapshotCount)*sizeof(GpuInstance));
        if(samplePrepare){
            const double finished=QpcNowSeconds();
            LogEvent(input.frameId,"render.prepare.breakdown",
                "instances=%u conversion_grain=%u snapshot_ms=%.3f conversion_ms=%.3f rest_ms=%.3f upload_copy_ms=%.3f total_ms=%.3f",
                f.dynamicCount,conversionGrain,
                (snapshotDone-prepareBegin)*1000.,
                (conversionDone-snapshotDone)*1000.,
                (finished-conversionDone)*1000.,
                (finished-uploadCopyBegin)*1000.,
                (finished-prepareBegin)*1000.);
        }
        return true;
    }

    bool Graphics::Impl::PrepareResidency(const XRFrame& xr,Frame& f,bool coreSceneWork) {
        residency.Begin();vramCache.RequireActive();if(visibleMaterialBits.size()!=staticMaterialBits.size())Fail("static material residency shape");
        std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);
        if(coreSceneWork){
            std::copy(staticMaterialBits.begin(),staticMaterialBits.end(),visibleMaterialBits.begin());
            auto markMaterial=[&](uint32_t material){if(material>=surfaceData.size())Fail("material residency index");visibleMaterialBits[material>>6]|=uint64_t(1)<<(material&63);};
            for(uint32_t i=0;i<f.dynamicCount;i++) {
                const auto& object=f.objects[i];const auto& range=meshes.ranges[object.mesh];
                XMVECTOR local=XMVectorMultiply(XMLoadFloat4(&range.bounds),XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.scale)));
                XMVECTOR center=XMVectorAdd(XMVector3Rotate(local,XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.rotation))),XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.positionRadius)));
                center=XMVectorSetW(center,1);bool visible=(object.flags&DeformedObject)!=0;
                for(uint32_t eye=0;eye<xr.viewCount&&!visible;eye++) {
                    auto matrix=XMLoadFloat4x4(&xr.vp[eye]);auto columns=XMMatrixTranspose(matrix);bool outside=false;
                    XMVECTOR planes[]={columns.r[3]+columns.r[0],columns.r[3]-columns.r[0],columns.r[3]+columns.r[1],columns.r[3]-columns.r[1],columns.r[2],columns.r[3]-columns.r[2]};
                    for(auto plane:planes)if(XMVectorGetX(XMVector4Dot(center,plane)) < -object.positionRadius.w*XMVectorGetX(XMVector3Length(plane))){outside=true;break;}
                    visible=!outside;
                }
                if(visible||surfaceData[object.material].extra.y)markMaterial(object.material);
            }
            // Keep every material used by resident tiles available, even for
            // reflections and ray hits outside the current view.
            const auto* tileCatalog=contributions.residentTiles?
                contributions.residentTiles(contributions.context):nullptr;
            if(tileCatalog)for(const auto& [key,tile]:tileCatalog->ResidentTiles())
                for(const auto& [material,count]:tile.materialCounts)
                    if(count)markMaterial(material);
            for(uint32_t material:contributions.streamMaterials)markMaterial(material);
        }
        if(++residencyPageGeneration==0){std::fill(residencyPageSeen.begin(),residencyPageSeen.end(),0);residencyPageGeneration=1;}requiredResidencyPages.clear();
        for(uint32_t word=0;word<visibleMaterialBits.size();word++){uint64_t bits=visibleMaterialBits[word];while(bits){uint32_t bit=uint32_t(std::countr_zero(bits));uint32_t material=word*64+bit;bits&=bits-1;if(material>=surfaceData.size())continue;for(uint32_t i=materialResidencyOffsets[material];i<materialResidencyOffsets[material+1];i++){uint32_t page=materialResidencyPages[i];if(page>=residencyPageSeen.size())Fail("material residency page index");if(residencyPageSeen[page]!=residencyPageGeneration){residencyPageSeen[page]=residencyPageGeneration;requiredResidencyPages.push_back(page);}}}}
        for(uint32_t page:requiredResidencyPages)residency.Require(page);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom)){LogEvent(xr.input.frameId,"render.skip","reason=residency_budget");active=nullptr;historyReady=false;return false;}
        
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());return true;
    }

    void Graphics::Impl::ClearCount(ID3D12GraphicsCommandList7* l,ID3D12Resource* r,DescriptorHandle descriptor){UINT zero[4]{};uint32_t index=Physical(descriptor);BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);l->ClearUnorderedAccessViewUint(Gpu(index),Cpu(index),r,zero,0,nullptr);BBarrier(l,r,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}

    void Graphics::Impl::ArgumentsReady(ID3D12GraphicsCommandList7* l,ID3D12Resource* args,ID3D12Resource* count){for(auto r:{args,count})BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}

    void Graphics::Impl::Viewport(ID3D12GraphicsCommandList7* l,UINT w,UINT h,UINT x,UINT y){D3D12_VIEWPORT v{float(x),float(y),float(w),float(h),0,1};D3D12_RECT r{LONG(x),LONG(y),LONG(x+w),LONG(y+h)};l->RSSetViewports(1,&v);l->RSSetScissorRects(1,&r);}
}
