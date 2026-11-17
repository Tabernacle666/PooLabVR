#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::Render(XRFrame& xr,const GpuProducerCoordinator& coordinator){
if(!active)return;
        const int64_t cpuRenderStart=developerMode?QpcCounter():0;
        if(developerMode){
            if(!timerInitialized){
                timerInitialized=true;
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
                const bool copyTiming=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options)))&&options.CopyQueueTimestampQueriesSupported;
                std::array<QueueState*,GraphQueueCount> queues{&copy,&compute,&direct};
                for(uint32_t q=0;q<GraphQueueCount;++q){
                    if(q==QI(GraphQueue::Copy)&&!copyTiming)continue;
                    auto& t=timerQueues[q];
                    if(FAILED(queues[q]->queue->GetTimestampFrequency(&t.frequency))||!t.frequency)continue;
                    D3D12_QUERY_HEAP_DESC heap{};
                    heap.Type=q==QI(GraphQueue::Copy)?D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                    heap.Count=2u*GpuBatchTimer::Slots*GpuBatchTimer::MaxBatches;
                    if(FAILED(device->CreateQueryHeap(&heap,IID_PPV_ARGS(&t.queries))))continue;
                    D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC buffer{};
                    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
                    buffer.Width=UINT64(heap.Count)*sizeof(UINT64);
                    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
                    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,
                        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.readback))))t.queries.Reset();
                }
            }
            std::array<QueueState*,GraphQueueCount> queues{&copy,&compute,&direct};
            for(auto& sample:timerSamples){
                if(!sample.pending)continue;
                bool ready=true;
                for(uint32_t q=0;q<GraphQueueCount;++q)
                    if(sample.counts[q]&&queues[q]->fence->GetCompletedValue()<sample.fences[q])ready=false;
                if(!ready)continue;
                bool valid=true;double gpuMs=0.;
                for(uint32_t q=0;q<GraphQueueCount;++q){
                    if(!sample.counts[q])continue;
                    auto& t=timerQueues[q];
                    const SIZE_T begin=SIZE_T(sample.slot)*GpuBatchTimer::MaxBatches*2u*sizeof(UINT64);
                    const D3D12_RANGE range{begin,begin+SIZE_T(sample.counts[q])*2u*sizeof(UINT64)};
                    void* mapped=nullptr;
                    if(FAILED(t.readback->Map(0,&range,&mapped))||!mapped){valid=false;continue;}
                    const auto* ticks=static_cast<const UINT64*>(mapped)+sample.slot*GpuBatchTimer::MaxBatches*2u;
                    UINT64 elapsed=0;
                    for(uint32_t n=0;n<sample.counts[q];++n){
                        if(ticks[n*2u+1]<ticks[n*2u]){valid=false;break;}
                        elapsed+=ticks[n*2u+1]-ticks[n*2u];
                    }
                    D3D12_RANGE empty{0,0};t.readback->Unmap(0,&empty);
                    gpuMs=std::max(gpuMs,1000.0*double(elapsed)/double(t.frequency));
                }
                if(valid){
                    const float ms=std::max(sample.cpuMs,float(gpuMs));
                    if(ms>0&&std::isfinite(ms))potentialFrameMs=potentialFrameMs>0?
                        potentialFrameMs*.8f+ms*.2f:ms;
                }
                sample.pending=false;
            }
        }
        if(xr.viewCount!=viewCount||!xr.viewCount||xr.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        if(xr.color>=xrRTV.size()||xr.depth>=xrDSV.size())Fail("Output image index out of range");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(xr.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.groups.empty()||publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    if(bound.deferred){
                        if(!coordinator.finishFallback)Fail("Deferred GPU producer cancellation without worker join");
                        coordinator.finishFallback();
                    }

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


                if(coordinator.finishFallback)coordinator.finishFallback();
                publication=stream->Acquire();
            }


            if(!publication){continue;}
            if(publication.groups.empty())Fail("GPU publication missing resource groups");if(publication.domain!=stream->Domain())Fail("GPU publication changed stream domain");if(!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)Fail("GPU stream publication missing producer/consumer fence");if(!domain->ValidatePublication(publication))Fail("GPU publication rejected by domain contract");
            DescribePublication(publication);publications.push_back({stream,domain,std::move(publication),deferred});domain->BindPublication(publications.back().publication);
        }
        auto& f=*active;
        const bool coreSceneWork=f.cpuCount!=0||!meshDomain->Bindings().empty();
        f.instanceCount=meshDomain->TotalInstances();
        if(coreSceneWork) {
            EnsureInstances(f,sceneCache.StaticCount()+f.snapshotCount,f.cpuCount,f.instanceCount);
            EnsureLightBuffers(f);
            EnsureLightBvhBuffers(f);
        }
        const GpuSceneCache* tileCatalog=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        auto residentBatches=PrepareResidentBatches(f,tileCatalog,coreSceneWork);
        auto& tileBatchDispatch=residentBatches.tileBatchDispatch;
        auto& opaqueRuns=residentBatches.opaqueRuns;
        const uint32_t tileBatchBlocks=residentBatches.tileBatchBlocks;
        EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);bool copyStatic=!pendingStatic.empty();
        auto rayFrame=PrepareRayScene(f,tileCatalog,frameSlot);
        auto& rayTileAcks=rayFrame.acks;
        const auto rayCpuDirty=rayFrame.cpuDirty;
        const auto rayPublicationDirty=rayFrame.publicationDirty;
        const bool rayDirty=rayFrame.dirty;
        if(coreSceneWork||!publications.empty()){
            if(!PrepareResidency(xr,f,coreSceneWork)){releasePublications(true);publications.clear();endDomains();return;}
        }else{
            std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);requiredResidencyPages.clear();
        }
        if(xr.input.trackingRevision!=lastTrackingRevision||(lastDisplayTime&&xr.displayTime-lastDisplayTime>xr.displayPeriod*2))historyReady=false;lastTrackingRevision=xr.input.trackingRevision;lastDisplayTime=xr.displayTime;

        renderWidth=width;renderHeight=height;renderMips=std::bit_width(std::max(renderWidth,renderHeight));constexpr uint32_t quality=3u;uint32_t output=uint32_t(frameNumber&1),previous=1-output;
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


            if(!hasReflections&&!meshDomain->Bindings().empty()&&contributions.streamMaterials.empty())hasReflections=true;
        }else{
            hasReflections=false;historyReady=false;
        }
        // Only live projectiles simulate. Merely aiming must never run the
        // mesh reconstruction graph; published meshlets remain resident.
        const float cutDt=std::clamp(float(double(xr.displayPeriod)*1e-9),.00001f,.05f);
        bool aimVisible=false;
        if(coreSceneWork&&f.instanceCount){
            for(uint32_t hand=0;hand<2;++hand){
                const auto& shot=xr.shotHands[hand];
                aimVisible|=shot.aim.tracked;
                if(shot.fired)
                    cutFlightRemaining=std::max(cutFlightRemaining,hand==0?3.f:5.f);
            }
        }
        cutActive=cutFlightRemaining>0;
        RenderPassPlan passPlan{};
        if(coreSceneWork){
            passPlan.occlusion=f.instanceCount>SmallSceneOcclusionThreshold;
            passPlan.depthHierarchy=passPlan.occlusion||hasReflections;
            passPlan.clusteredLights=f.lightCount>DirectLightLimit;
            passPlan.reflections=hasReflections;
            passPlan.shadowAtlas=f.hasShadowAtlasLights;
            passPlan.rayQueries=passPlan.reflections||f.hasShadowedLights||cutActive;
            // The first user shot must build TLAS even when the ray snapshot
            // did not change while no ray tracing passes were active.
            passPlan.rayWork=(rayDirty||(cutActive&&!f.tlasBuilt))&&
                passPlan.rayQueries&&f.rayInstanceCount>0;
        }
        auto& c=*f.constants;c={};
        if(tileCatalog)c.tileViewer={
            float(tileCatalog->TileViewerX()),float(tileCatalog->TileViewerZ()),0,0};
        bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        for(uint32_t view=0;view<xr.viewCount;view++){c.vp[view]=xr.vp[view];c.inverseVP[view]=xr.inverseVP[view];c.previousVP[view]=reflectionHistoryReady?lastVP[view]:xr.vp[view];c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};lastVP[view]=xr.vp[view];}
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV,meshletRegionsSRV,geometryUAVs[0],geometryUAVs[1],geometryUAVs[2],geometryUAVs[3],geometryUAVs[4],geometryUAVs[5],triangleSolidsSRV});
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
        const auto hudMemory=managedMemory.Budget();
        constexpr double gibibyte=1024.0*1024.0*1024.0;
        const float estimatedFps=potentialFrameMs>0?1000.f/potentialFrameMs:-1.f;
        const float margin=estimatedFps>0&&hudFps>0?100.f*(estimatedFps/hudFps-1.f):-1.f;
        c.hud={hudFps,estimatedFps,margin,developerMode?1.f:0.f};
        c.atmosphereSphere={atmosphere.center.x,atmosphere.center.y,atmosphere.center.z,atmosphere.radius};
        c.atmosphereColor={atmosphere.color.x,atmosphere.color.y,atmosphere.color.z,atmosphere.density};
        c.atmosphereParams={-1.f,1.f,0,0};
        if(f.lightCount){
            c.atmosphereLight=f.lightData[0].positionRadius;c.atmosphereEmission=f.lightData[0].colorPower;
            c.atmosphereParams.x=f.lightData[0].parameters.y;
            if(atmosphere.emissionReferencePower>0)c.atmosphereParams.y=f.lightData[0].colorPower.w/atmosphere.emissionReferencePower;
        }
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](uint32_t a,uint32_t b){auto distance=[&](uint32_t i){auto p=CpuInstance(f,i).positionRadius;auto delta=Sub({p.x,p.y,p.z},center);return Dot(delta,delta);};return distance(a)>distance(b);});
        }

        const bool runCut=cutActive&&coreSceneWork&&f.instanceCount&&passPlan.rayQueries;
        const bool runOverlay=coreSceneWork&&f.instanceCount&&(runCut||aimVisible);
        CutDispatch cutDispatch{};
        if(runOverlay)cutDispatch=PrepareCutDispatch(f,xr);
        c.resourceAbi.w=Invalid;
        if(cutGenerationInitialized&&cutMesh.maxTriangles){
            c.resourceAbi.w=Physical(cutMesh.Scratch(CutScratchId::Generation).srv);
            c.cutGeometry={Physical(cutMesh.Scratch(CutScratchId::PackedMeshlets).srv),
                Physical(cutMesh.Scratch(CutScratchId::PackedVertices).srv),
                Physical(cutMesh.Scratch(CutScratchId::PackedRemap).srv),
                Physical(cutMesh.Scratch(CutScratchId::PackedTriangles).srv)};
            c.cutGeometryAlternate={
                Physical(cutMeshAlternate.Scratch(CutScratchId::PackedMeshlets).srv),
                Physical(cutMeshAlternate.Scratch(CutScratchId::PackedVertices).srv),
                Physical(cutMeshAlternate.Scratch(CutScratchId::PackedRemap).srv),
                Physical(cutMeshAlternate.Scratch(CutScratchId::PackedTriangles).srv)};
            c.cutGeometryPage={cutMesh.pageCount,cutMesh.pageTriangles,
                               cutMesh.pageMeshlets,0};
        }
        const bool drawCut=c.resourceAbi.w!=Invalid;
        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto dispatchScene64=[](ID3D12GraphicsCommandList7* list,uint32_t count){
            const uint32_t groups=uint32_t((uint64_t(count)+63u)/64u);
            if(groups)list->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,1);
        };
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useCutMesh=[&](uint32_t node){
            if(!drawCut)return;
            useBuffer(node,cutMesh.Scratch(CutScratchId::Generation).resource.Get(),
                GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* cf:{&cutMesh,&cutMeshAlternate})
                for(auto id:{CutScratchId::PackedVertices,CutScratchId::PackedMeshlets,
                             CutScratchId::PackedRemap,CutScratchId::PackedTriangles})
                    useBuffer(node,cf->Scratch(id).resource.Get(),GraphMode::Read,
                        D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        };
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


        if(coreSceneWork&&f.cpuCount){
            auto cpuVisibilityNode=frameGraph.AddPass("CpuScene.Visibility",GraphQueue::Compute,20,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);
                l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetComputeRoot32BitConstant(1,0,0);
                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                l->SetPipelineState(hotDataPSO.Get());
                dispatchScene64(l,f.cpuCount);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                ClearCount(l,f.cpuVisibility.depthCount.Get(),f.cpuVisibility.depthCountUAV);
                l->SetPipelineState(cullPSO.Get());
                l->SetComputeRoot32BitConstant(1,3,1);
                dispatchScene64(l,f.cpuCount);
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
                if(drawCut){
                    l->SetGraphicsRoot32BitConstant(1,1,4);
                    l->ExecuteIndirect(indirect.Get(),cutMesh.pageCount,
                                       cutDrawArguments.Get(),0,nullptr,0);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                }
            });
            useCutMesh(cpuDepthNode);
            if(drawCut)useBuffer(cpuDepthNode,cutDrawArguments.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useTileSources(cpuDepthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(cpuDepthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuDepthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useTexture(cpuDepthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        }


        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);

        c.resourceAbi.z=Invalid;


        const bool bvhSortNeeded=AddLightBvhPass(f,coreSceneWork);

        if(coreSceneWork){


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

            if(passPlan.rayWork){
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
                    (((range.count+63u)/64u)+65534u)/65535u,1);}l->SetComputeRoot32BitConstants(1,4,params,0);}});
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
            if(passPlan.rayWork&&f.cpuCount)
                useBuffer(computeNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayWork&&f.instanceCount>f.cpuCount)
                useBuffer(computeNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(computeNode,f.rayDescriptors.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }

            auto depthNode=frameGraph.AddPass("Depth",GraphQueue::Direct,40,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Bind(l,f,true);
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                if(passPlan.rayQueries&&!f.blasWork.Empty())BuildDynamic(l,f);
                if(passPlan.rayWork){
                    AliasMemory(l);
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                    build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                    build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
                    build.Inputs.NumDescs=f.rayInstanceCount;
                    build.Inputs.InstanceDescs=f.rayDescriptors->GetGPUVirtualAddress();
                    build.DestAccelerationStructureData=f.tlas->GetGPUVirtualAddress();
                    build.ScratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();
                    if(f.tlasBuilt&&f.lastTlasCount==f.rayInstanceCount){
                        build.SourceAccelerationStructureData=build.DestAccelerationStructureData;
                        build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                    }
                    l->BuildRaytracingAccelerationStructure(&build,0,nullptr);
                    BBarrier(l,f.tlas.Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
                    f.lastTlasCount=f.rayInstanceCount;f.tlasBuilt=true;
                }
                if(f.shadowDirty){
                    auto dsv=Dsv(shadowDSV);l->OMSetRenderTargets(0,nullptr,FALSE,&dsv);
                    if(f.cpuCount)Bind(l,f,false);
                    bool firstShadow=true;
                    for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot)){
                        uint32_t x=(slot%2)*1536,y=(slot/2)*1024;
                        D3D12_RECT rectangle{LONG(x),LONG(y),LONG(x+1536),LONG(y+1024)};
                        l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.f,0,1,&rectangle);
                        if(!f.cpuCount)continue;
                        if(!firstShadow)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})
                            BBarrier(l,resource,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        firstShadow=false;
                        ClearCount(l,f.cpuVisibility.shadowCount.Get(),f.cpuVisibility.shadowCountUAV);
                        const uint32_t shadowParams[4]={0,7,f.cpuCount,slot};
                        l->SetComputeRoot32BitConstants(1,4,shadowParams,0);
                        l->SetPipelineState(cullPSO.Get());
                        dispatchScene64(l,f.cpuCount);
                        ArgumentsReady(l,f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get());
                        l->SetPipelineState(shadowPSO.Get());
                        for(uint32_t face=0;face<6;face++){
                            Viewport(l,512,512,x+(face%3)*512,y+(face/3)*512);
                            l->SetGraphicsRoot32BitConstant(1,slot*6+face,1);
                            l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.shadowArguments.Get(),
                                uint64_t(face)*f.cpuCount*16u,f.cpuVisibility.shadowCount.Get(),uint64_t(face)*4u);
                        }
                    }
                    EmitBarriers(l,compiled.barriersDuring[0]);
                }
                Viewport(l,renderWidth,renderHeight);
                auto depthHandle=Dsv(depthDSV);
                if(!f.cpuCount)l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
                l->SetPipelineState(depthPSO.Get());
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                if(f.instanceCount>f.cpuCount)l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,f.publishedVisibility.depthArguments.Get(),0,f.publishedVisibility.depthCount.Get(),0);
            });
            useCutMesh(depthNode);
            useTileSources(depthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(depthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.shadowDirty&&f.cpuCount)useBuffer(depthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(depthNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(depthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(depthNode,f.rayDescriptors.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.instanceCount>f.cpuCount)for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty&&f.cpuCount)for(auto* resource:{f.cpuVisibility.shadowArguments.Get(),f.cpuVisibility.shadowCount.Get()})useBuffer(depthNode,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(passPlan.rayWork)useBuffer(depthNode,f.tlas.Get(),GraphMode::ReadWrite,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);useTexture(depthNode,depth,f.cpuCount?GraphMode::ReadWrite:GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);if(f.shadowDirty)useTexture(depthNode,shadow,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);

            if(f.shadowDirty)frameGraph.TransitionTexture(depthNode,shadow,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
            frameGraph.TransitionTexture(depthNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            uint32_t cutStageNode=UINT32_MAX;
            if(runCut){
                auto& cf=cutFrames[frameSlot];
                auto stepNode=frameGraph.AddPass("Projectile.Step",GraphQueue::Direct,44,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);
                    const UINT cleared[4]{};
                    if(!cutGenerationInitialized){
                        if(cutDrawArguments){
                            l->ClearUnorderedAccessViewUint(
                                Gpu(Physical(cutDrawArgumentsUAV)),
                                Cpu(Physical(cutDrawArgumentsUAV)),
                                cutDrawArguments.Get(),cleared,0,nullptr);
                            BBarrier(l,cutDrawArguments.Get(),
                                D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        }
                        if(cutMesh.maxTriangles){
                            auto* generation=cutMesh.Scratch(CutScratchId::Generation).resource.Get();
                            l->ClearUnorderedAccessViewUint(Gpu(Physical(cutGenerationClearUAV)),
                                Cpu(Physical(cutGenerationClearUAV)),generation,cleared,0,nullptr);
                            BBarrier(l,generation,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                        }
                    }
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cf.controlUAV)),Cpu(Physical(cf.controlUAV)),cf.control.Get(),cleared,0,nullptr);
                    BBarrier(l,cf.control.Get(),D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BindCutPipeline(l,CutKernel::Projectile,cutDispatch.step);
                    l->Dispatch(CutProjectileCapacity,1,1);
                    BBarrier(l,cf.impacts.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    BBarrier(l,cf.control.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                });
                frameGraph.Dependency(depthNode,stepNode);
                useTileSources(stepNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                for(auto* resource:{ranges.Get(),vertices.Get(),indices.Get(),
                                    f.worldInstances.Get(),f.publishedInstances.Get()})
                    useBuffer(stepNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(PublishCutStages&&cutMesh.maxTriangles){
                    for(auto* cf:{&cutMesh,&cutMeshAlternate})
                        for(auto id:{CutScratchId::PackedVertices,CutScratchId::PackedIndices})
                            useBuffer(stepNode,cf->Scratch(id).resource.Get(),GraphMode::Read,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    useBuffer(stepNode,cutMesh.Scratch(CutScratchId::Generation).resource.Get(),
                        cutGenerationInitialized?GraphMode::Read:GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                }
                if(!cutGenerationInitialized&&cutDrawArguments)
                    useBuffer(stepNode,cutDrawArguments.Get(),GraphMode::Write,
                        D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(f.rayInstanceCount){
                    useBuffer(stepNode,f.rayInstanceMap.Get(),GraphMode::Read,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    useBuffer(stepNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
                }
                useBuffer(stepNode,cutProjectiles[cutDispatch.source].Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(stepNode,cutProjectiles[cutDispatch.destination].Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(stepNode,cf.control.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(stepNode,cf.impacts.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                if(triangleSolidsSRV.Valid()){
                    auto resolveNode=frameGraph.AddPass("Projectile.Resolve",GraphQueue::Direct,45,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        BindCutPipeline(l,CutKernel::Resolve,cutDispatch.resolve);
                        l->Dispatch((CutProjectileCapacity+63u)/64u,1,1);
                    });
                    frameGraph.Dependency(stepNode,resolveNode);
                    useTileSources(resolveNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                    for(auto* resource:{cf.impacts.Get(),f.worldInstances.Get(),f.publishedInstances.Get(),ranges.Get(),triangleSolids.Get(),vertices.Get(),indices.Get()})
                        useBuffer(resolveNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    useBuffer(resolveNode,cf.control.Get(),GraphMode::Read,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    useBuffer(resolveNode,cf.resolved.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    // Stage a replacement meshlet generation from the latest baked
                    // source. Publish it only after all pack/cap passes succeed.
                    if(PublishCutStages&&cutMesh.maxTriangles){
                        auto staged=frameGraph.AddPass("Projectile.StageCut",GraphQueue::Direct,65,
                            [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                            auto& cut=cutMesh;
                            auto buffer=[&](CutScratchId id)->ID3D12Resource*{
                                return cut.Scratch(id).resource.Get();
                            };
                            auto clear=[&](CutScratchId id){
                                auto& scratch=cut.Scratch(id);
                                const UINT zeros[4]{};
                                l->ClearUnorderedAccessViewUint(
                                    Gpu(Physical(scratch.uav)),Cpu(Physical(scratch.uav)),
                                    scratch.resource.Get(),zeros,0,nullptr);
                                BBarrier(l,scratch.resource.Get(),
                                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            };
                            ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                            l->SetDescriptorHeaps(2,heaps);
                            clear(CutScratchId::SurfaceControl);
                            clear(CutScratchId::SegmentControl);
                            clear(CutScratchId::CapControl);
                            auto barrier=[&](CutScratchId id,D3D12_BARRIER_ACCESS next){
                                BBarrier(l,buffer(id),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    next==D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT?
                                        D3D12_BARRIER_SYNC_EXECUTE_INDIRECT:D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,next);
                            };
                            BindCutPipeline(l,CutKernel::Volume,cutDispatch.volume);
                            l->Dispatch(2,1,1);
                            barrier(CutScratchId::Faces,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            barrier(CutScratchId::Planes,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            barrier(CutScratchId::Generation,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            BindCutPipeline(l,CutKernel::Schedule,cutDispatch.schedule);
                            l->Dispatch(1,1,1);
                            BBarrier(l,buffer(CutScratchId::Generation),
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            barrier(CutScratchId::IndirectArgs,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                            BindCutPipeline(l,CutKernel::Surface,cutDispatch.surface);
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,buffer(CutScratchId::IndirectArgs),0,nullptr,0);
                            for(auto id:{CutScratchId::SurfaceVertices,CutScratchId::SurfaceIndices,
                                         CutScratchId::SurfaceSolids,CutScratchId::SurfaceControl})
                                barrier(id,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            // The source mesh is already segmented into independent
                            // render regions. Cap parity on a partial region invents
                            // incorrect interior walls; publish only real clipped
                            // source triangles until whole-solid caps are available.
                            BBarrier(l,buffer(CutScratchId::IndirectArgs),
                                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            BindCutPipeline(l,CutKernel::Indirect,cutDispatch.packIndirect);
                            l->Dispatch(1,1,1);
                            barrier(CutScratchId::SurfaceControl,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            barrier(CutScratchId::IndirectArgs,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                            barrier(CutScratchId::SurfaceVertices,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            barrier(CutScratchId::SurfaceSolids,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                            BindCutPipeline(l,CutKernel::Pack,cutDispatch.pack);
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,buffer(CutScratchId::IndirectArgs),48,nullptr,0);
                            // A generation can be marked ready only after ALL
                            // packed geometry writes have finished. This also
                            // separates the pack writes from the range update.
                            for(auto id:{CutScratchId::PackedVertices,CutScratchId::PackedIndices,
                                         CutScratchId::PackedMeshlets,CutScratchId::PackedRemap,
                                         CutScratchId::PackedTriangles,CutScratchId::PackedRegions,
                                         CutScratchId::PackedSolids,CutScratchId::PackedRanges}){
                                barrier(id,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                                BBarrier(l,cutMeshAlternate.Scratch(id).resource.Get(),
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            }
                            BindCutPipeline(l,CutKernel::Finalize,cutDispatch.pack);
                            l->Dispatch(1,1,1);
                            barrier(CutScratchId::PackedRanges,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            BBarrier(l,cutMeshAlternate.Scratch(CutScratchId::PackedRanges).resource.Get(),
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            barrier(CutScratchId::SurfaceControl,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            barrier(CutScratchId::CapControl,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            BindCutPipeline(l,CutKernel::Generation,cutDispatch.generation);
                            l->Dispatch(1,1,1);
                            for(auto id:{CutScratchId::Faces,CutScratchId::Planes,
                                         CutScratchId::SurfaceVertices,CutScratchId::SurfaceSolids})
                                BBarrier(l,buffer(id),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                            BBarrier(l,buffer(CutScratchId::IndirectArgs),
                                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                        });
                        cutStageNode=staged;
                        frameGraph.Dependency(resolveNode,staged);
                    }
                }
            }

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
                                dispatchScene64(l,f.cpuCount);
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


            for(auto it=f.tileDrawCache.begin();it!=f.tileDrawCache.end();){
                if(!tiles||!tiles->contains(it->first))
                    it=f.tileDrawCache.erase(it);
                else ++it;
            }
            std::vector<ID3D12CommandList*> residentOpaqueLists;
            residentOpaqueLists.reserve(tileBatchDispatch.size());
            const bool reflectedMode=passPlan.reflections;
            const uint32_t modeIndex=reflectedMode?1u:0u;

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
                    l->SetGraphicsRoot32BitConstant(1,0,4);
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
            useCutMesh(opaqueNode);
            useCutMesh(opaqueTileNode);
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
                    if(drawCut){
                        l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());
                        l->SetGraphicsRoot32BitConstant(1,1,4);
                        l->ExecuteIndirect(indirect.Get(),cutMesh.pageCount,
                                       cutDrawArguments.Get(),0,nullptr,0);
                        l->SetGraphicsRoot32BitConstant(1,0,4);
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
            useCutMesh(opaqueTailNode);
            if(drawCut)useBuffer(opaqueTailNode,cutDrawArguments.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            if(cutStageNode!=UINT32_MAX){
                frameGraph.Dependency(opaqueTailNode,cutStageNode);
                useTileSources(cutStageNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
                auto& cf=cutFrames[frameSlot];
                for(auto* read:{vertices.Get(),indices.Get(),ranges.Get(),triangleSolids.Get(),
                                cf.resolved.Get(),cf.impacts.Get(),f.worldInstances.Get(),f.publishedInstances.Get()})
                    useBuffer(cutStageNode,read,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(cutStageNode,cf.control.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(cutStageNode,cutDrawArguments.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                for(auto& scratch:cutMesh.scratch)
                    useBuffer(cutStageNode,scratch.resource.Get(),GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto id:{CutScratchId::PackedVertices,CutScratchId::PackedIndices,
                             CutScratchId::PackedMeshlets,CutScratchId::PackedRemap,
                             CutScratchId::PackedTriangles,CutScratchId::PackedRanges,
                             CutScratchId::PackedRegions,CutScratchId::PackedSolids})
                    useBuffer(cutStageNode,cutMeshAlternate.Scratch(id).resource.Get(),
                        GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                            D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
            if(f.rayInstanceCount)useBuffer(opaqueNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
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

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);


            if(f.rayInstanceCount)useBuffer(opaqueTileNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
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

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTileNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTileNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTileNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTileNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTileNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTileNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(f.rayInstanceCount)useBuffer(opaqueTailNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
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

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTailNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTailNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTailNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTailNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTailNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTailNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

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


        if(aaMode){
            if(!aaSourceLease){GpuTextureDesc d{};d.width=renderWidth;d.height=renderHeight;d.depthOrArraySize=viewCount;d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.uav=false;d.name=L"FXAA.SpatialSource";aaSourceLease=CreateManagedTexture2D(d);if(!aaSourceLease)Fail("FXAA source allocation");aaSource.resource=aaSourceLease->resource;aaSourceArraySrv=TextureSrv(aaSource.resource.Get(),d.format,{},true);aaSource.layout=D3D12_BARRIER_LAYOUT_COMMON;}
            auto copy=frameGraph.AddPass("FXAA.Snapshot",GraphQueue::Direct,92,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyResource(aaSource.resource.Get(),resolvedColor.resource.Get());});
            useTexture(copy,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);useTexture(copy,aaSource,GraphMode::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            auto filter=frameGraph.AddPass("FXAA.Spatial",GraphQueue::Direct,93,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t p[]{DescriptorIndex(aaSourceArraySrv),Physical(hdrResolvedUAV),aaMode,0};l->SetComputeRoot32BitConstants(1,4,p,0);l->SetPipelineState(fxaaPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
            useTexture(filter,aaSource,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(filter,resolvedColor,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
        for(auto& domain:externalDomains){domainBuilder.SetDomain(domain->Identity());domain->BuildPasses(domainBuilder,domainContext);}domainBuilder.SetDomain(GpuDomainId::Invalid);

        if(runOverlay){
            auto clearMarks=frameGraph.AddPass("Projectile.ClearMarks",GraphQueue::Direct,94,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);
                const UINT cleared[4]{};
                l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayUAV)),Cpu(Physical(cutOverlayUAV)),cutOverlayMask.resource.Get(),cleared,0,nullptr);
            });
            useTexture(clearMarks,cutOverlayMask,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            uint32_t marksProducer=clearMarks;
            if(runCut){
                auto stamp=frameGraph.AddPass("Projectile.Stamp",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    BindCutPipeline(l,CutKernel::Stamp,cutDispatch.overlay);
                    l->Dispatch(CutProjectileCapacity,xr.viewCount,1);
                });
                frameGraph.Dependency(clearMarks,stamp);
                useBuffer(stamp,cutProjectiles[cutDispatch.destination].Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(stamp,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                marksProducer=stamp;
            }
            auto laser=frameGraph.AddPass("Projectile.Laser",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                BindCutPipeline(l,CutKernel::Laser,cutDispatch.overlay);
                l->Dispatch((std::max(renderWidth,renderHeight)+63u)/64u,2,xr.viewCount);
            });
            frameGraph.Dependency(marksProducer,laser);
            useTexture(laser,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            auto composite=frameGraph.AddPass("Projectile.Composite",GraphQueue::Direct,96,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                BindCutPipeline(l,CutKernel::Composite,cutDispatch.overlay);
                l->Dispatch((renderWidth+7u)/8u,(renderHeight+7u)/8u,viewCount);
            });
            frameGraph.Dependency(coreColorNode,composite);
            frameGraph.Dependency(laser,composite);
            useTexture(composite,cutOverlayMask,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useTexture(composite,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            coreColorNode=composite;
        }
        auto presentNode=frameGraph.AddPass("Present",GraphQueue::Direct,100,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
            auto* output=xrColorTargets[xr.color].Get();
            D3D12_TEXTURE_BARRIER displayBarrier{};
            D3D12_BARRIER_GROUP displayGroup{};displayGroup.Type=D3D12_BARRIER_TYPE_TEXTURE;displayGroup.NumBarriers=1;displayGroup.pTextureBarriers=&displayBarrier;
            if(outputSurface.desktop){
                const bool simultaneous=(output->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0;
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_NONE;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_RENDER_TARGET;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_NO_ACCESS;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_RENDER_TARGET;
                displayBarrier.LayoutBefore=D3D12_BARRIER_LAYOUT_PRESENT;displayBarrier.LayoutAfter=simultaneous?D3D12_BARRIER_LAYOUT_COMMON:D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                displayBarrier.pResource=output;l->Barrier(1,&displayGroup);
            }
            Viewport(l,width,height);auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);l->OMSetRenderTargets(1,&target,FALSE,&ds);Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);l->SetGraphicsRoot32BitConstant(1,passPlan.reflections?1u:0u,3);l->SetPipelineState(presentPSO.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->DrawInstanced(3,1,0,0);RecordCapture(l,output);
            if(outputSurface.desktop){
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_ALL;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_NONE;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_RENDER_TARGET;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_NO_ACCESS;
                displayBarrier.LayoutBefore=displayBarrier.LayoutAfter;displayBarrier.LayoutAfter=D3D12_BARRIER_LAYOUT_PRESENT;
                l->Barrier(1,&displayGroup);
            }
        });useTexture(presentNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)useTexture(presentNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(presentNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        if(atmosphere.radius>0&&atmosphere.density>0&&f.hasShadowAtlasLights&&f.lightCount&&f.lightData[0].parameters.y>=0)useTexture(presentNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

        frameGraph.Dependency(coreColorNode,presentNode);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Budget().headroom)){releasePublications(true);publications.clear();managedMemory.EndFrame(SubmittedFences());endDomains();active=nullptr;historyReady=false;return;}
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());
        for(const auto& bound:publications)frameGraph.PublicationComplete(bound.publication,presentNode);
        frameGraph.CompileExecutionBatches();
        auto batchCommands=[&](GraphQueue queue,uint32_t index)->Commands&{
            auto& slots=f.batchCommands[QI(queue)];while(slots.size()<=index){auto commands=std::make_unique<Commands>();CommandsInit(*commands,queue==GraphQueue::Copy?D3D12_COMMAND_LIST_TYPE_COPY:queue==GraphQueue::Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT);slots.push_back(std::move(commands));}return *slots[index];
        };
        const auto progress=coordinator.progress?
            coordinator.progress:std::make_shared<GpuSubmissionProgress>();


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
        GpuBatchTimer timer{};
        bool recordTiming=false;
        if(developerMode){
            std::array<uint32_t,GraphQueueCount> counts{};
            for(const auto& batch:frameGraph.executionBatches)++counts[QI(batch.queue)];
            const uint32_t slot=uint32_t(frameNumber%GpuBatchTimer::Slots);
            bool supported=!timerSamples[slot].pending;
            for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                if(counts[q]>GpuBatchTimer::MaxBatches||!timerQueues[q].queries||!timerQueues[q].readback)supported=false;
            }
            if(supported){
                recordTiming=true;timer.slot=slot;timer.counts=counts;
                for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                    timer.queries[q]=timerQueues[q].queries.Get();
                    timer.readbacks[q]=timerQueues[q].readback.Get();
                }
            }
        }
        frameGraph.Execute(*jobs,batchCommands,canSubmit,progress,recordTiming?&timer:nullptr);
if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");


        if(coreSceneWork&&bvhSortNeeded){
            f.lightBvhSorted=true;
            f.lightBvhPendingSort=false;
            f.lightBvhSortedCount=f.lightCount;
            f.lightBvhSortSnapshot=f.lightData;
            f.lightBvhLastSortFrame=frameNumber;
        }

        if(copyStatic)sceneCache.Commit(frameSlot);

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
        if(recordTiming){
            auto& sample=timerSamples[timer.slot];
            sample.slot=timer.slot;sample.counts=timer.counts;sample.fences=f.queueFences;
            sample.cpuMs=cpuPrepareMs+float(QpcSeconds(QpcCounter()-cpuRenderStart)*1000.0);
            sample.pending=true;
        }
        if(runCut){
            cutGenerationInitialized=true;
            ++cutStepSerial;
            cutFlightRemaining=std::max(0.f,cutFlightRemaining-cutDt);
        }
        residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;xr.submitted=true;
        const int64_t hudNow=QpcCounter();
        if(!hudStart)hudStart=hudNow;
        else{
            ++hudFrames;
            if(hudNow-hudStart>=QpcFrequency()){
                hudFps=float(double(hudFrames)*double(QpcFrequency())/double(hudNow-hudStart));
                hudFrames=0;hudStart=hudNow;
            }
        }
    }


}
