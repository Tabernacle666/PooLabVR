#include "GraphicsInternal.h"

namespace lab {
    void Graphics::Impl::Render(XRFrame& xr,const std::function<void()>& waitForStreams){
        traceFrame=xr.input.frameId;if(!active)return;if(xr.viewCount!=viewCount||!xr.viewCount||xr.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(xr.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    if(bound.deferred){if(!waitForStreams)Fail("Deferred GPU producer has no submission wait");waitForStreams();}
                    // Even an unused publication must finish production before
                    // retirement; enqueue this dependency without a CPU wait.
                    if(publication.producerFence&&publication.producerValue)Hr(copy.queue->Wait(publication.producerFence.Get(),publication.producerValue),"GPU publication early producer wait");
                    Hr(copy.queue->Signal(publication.consumerFence.Get(),publication.consumerValue),"GPU publication early consumer fence");
                }
                bound.stream->Release(publication);bound.stream=nullptr;bound.publication={};
            }
        };
        for(auto* stream:streams){
            auto* domain=FindDomain(stream->Domain());if(!domain)Fail("GPU stream domain is not registered");
            auto reserved=std::find_if(reservedStreams.begin(),reservedStreams.end(),[&](const auto& r){return r.stream==stream;});
            GpuPublication publication;
            const bool deferred=reserved!=reservedStreams.end();
            if(deferred){if(!waitForStreams)Fail("Deferred GPU producer has no submission wait");publication=std::move(reserved->publication);reserved->stream=nullptr;}
            else {if(waitForStreams)waitForStreams();publication=stream->Acquire();}
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
        f.instanceCount=meshDomain->TotalInstances();EnsureInstances(f,f.instanceCount);EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);bool copyStatic=!pendingStatic.empty();
        rayPublicationScratch.clear();if(rayPublicationScratch.capacity()<meshDomain->Bindings().size())rayPublicationScratch.reserve(meshDomain->Bindings().size());for(const auto& bound:meshDomain->Bindings()){const auto* instances=bound.publication.Find(GpuMeshPublicationAbi::Instances);uint64_t generation=bound.publication.generation;generation=HashMemory(&instances->generation,sizeof(instances->generation),generation?generation:1469598103934665603ull);rayPublicationScratch.push_back({generation,bound.base,bound.capacity});}rayScene.StagePublications(frameSlot,rayPublicationScratch);if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);auto rayCpuDirty=rayScene.PendingCpu(frameSlot);auto rayPublicationDirty=rayScene.PendingPublications(frameSlot);bool rayDirty=rayScene.NeedsTlas(frameSlot);
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
            for(uint32_t i=0;i<f.dynamicCount;i++){const auto& object=f.objects[i];if(object.flags&AlphaBlendObject)f.transparent[f.transparentCount++]=staticCount+i;if(!hasReflections&&object.material<surfaceData.size())hasReflections=SurfaceMayReflect(surfaceData[object.material],quality);}
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
            passPlan.rayWork=rayDirty&&passPlan.rayQueries;
        }
        auto& c=*f.constants;c={};bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        for(uint32_t view=0;view<xr.viewCount;view++){c.vp[view]=xr.vp[view];c.inverseVP[view]=xr.inverseVP[view];c.previousVP[view]=reflectionHistoryReady?lastVP[view]:xr.vp[view];c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};lastVP[view]=xr.vp[view];}
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV});
        SetDomain(f,ResourceDomain::Frame,{f.instanceSRV,f.argumentsUAV,f.countUAV,f.instanceUAV,f.cullArgumentsUAV,f.cullCountUAV,f.shadowArgumentsUAV,f.shadowCountUAV});
        SetDomain(f,ResourceDomain::Lighting,{f.lightsSRV,f.clusterSRV,f.clusterUAV,shadowSRV});
        SetDomain(f,ResourceDomain::History,{hierarchySRV,depthSRV,hdrResolvedSRV,reflectionsResolvedSRV,historySRV[previous],historyDepthSRV[previous],historyUAV[output],historyDepthUAV[output],historySRV[output],hierarchySRV});
        for(uint32_t mip=0;mip<hierarchyUAV.size();mip++)f.resourceTableData[uint32_t(ResourceDomain::History)][HistoryHiZMipBase+mip]={Physical(hierarchyUAV[mip]),Generation(hierarchyUAV[mip])};
        SetDomain(f,ResourceDomain::Animation,{f.dynamicSRV,f.dynamicUAV,deformationsSRV,f.motionSRV,f.boneSRV,f.weightSRV,skinSRV,morphSRV,f.deformedBoundsSRV,f.deformedBoundsUAV});
        SetDomain(f,ResourceDomain::Visibility,{f.hotBoundsSRV,f.hotBoundsUAV,f.hotMeshesSRV,f.hotMeshesUAV});
        SetDomain(f,ResourceDomain::RayTracing,{f.tlasSRV,f.rayUAV,f.blasAddressesSRV});
        c.resourceAbi={DescriptorIndex(f.resourceDirectorySRV),ResourceAbiVersion,0,0};c.dimensions={renderWidth,renderHeight,renderMips,0};c.outputDimensions={width,height,renderWidth,renderHeight};c.counts={coreSceneWork?f.instanceCount:0u,coreSceneWork?f.additionalCount:0u,coreSceneWork?f.lightCount:0u,coreSceneWork&&reflectionHistoryReady?1u:0u};c.topology={xr.viewCount,MsaaSamples,passPlan.reflections?1u:0u,0};c.timing={NearZ,FarZ,float(f.simulationTime),float(quality)};
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](uint32_t a,uint32_t b){auto distance=[&](uint32_t i){auto p=CpuInstance(f,i).positionRadius;auto delta=Sub({p.x,p.y,p.z},center);return Dot(delta,delta);};return distance(a)>distance(b);});
        }

        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useTexture=[&](uint32_t node,TextureResource& resource,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){frameGraph.Texture(node,&resource,resource.resource.Get(),resource.layout,mode,layout,sync,access);};

        if(surfacesDirty&&materials&&!surfaceData.empty()){
            UINT64 bytes=surfaceData.size()*sizeof(Surface),offset=Reserve(f,bytes);memcpy(f.uploadGeneration->mapped+offset,surfaceData.data(),size_t(bytes));
            auto node=frameGraph.AddPass("Materials.Update",GraphQueue::Direct,9,[&,offset,bytes](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyBufferRegion(materials.Get(),0,f.uploadGeneration->resource.Get(),offset,bytes);surfacesDirty=false;});
            useBuffer(node,materials.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        uint32_t coreColorNode=UINT32_MAX;
        if(coreSceneWork){
            auto uploadNode=frameGraph.AddPass("Upload",GraphQueue::Copy,10,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                if(!f.uploadGeneration)Fail("frame upload generation missing");auto uploadResource=f.uploadGeneration->resource.Get();auto copyAnimation=[&](ID3D12Resource* destination,UINT64 offset){l->CopyBufferRegion(destination,0,uploadResource,offset,destination->GetDesc().Width);};copyAnimation(f.motionBuffer.Get(),f.motionOffset);copyAnimation(f.boneBuffer.Get(),f.boneOffset);copyAnimation(f.weightBuffer.Get(),f.weightOffset);if(f.lightCount)l->CopyBufferRegion(f.lights.Get(),0,uploadResource,f.lightsOffset,UINT64(f.lightCount)*sizeof(Light));if(copyStatic){if(!residentStaticInstances)Fail("persistent static scene source missing");for(const auto& range:pendingStatic){UINT64 offset=UINT64(range.first)*sizeof(GpuInstance),bytes=UINT64(range.count)*sizeof(GpuInstance);l->CopyBufferRegion(f.instances.Get(),offset,residentStaticInstances.Get(),offset,bytes);}}if(f.dynamicCount)l->CopyBufferRegion(f.instances.Get(),UINT64(staticCount)*sizeof(GpuInstance),uploadResource,f.objectsOffset,UINT64(f.dynamicCount)*sizeof(GpuInstance));
            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})if(resource)useBuffer(uploadNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic||f.dynamicCount)useBuffer(uploadNode,f.instances.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic)useBuffer(uploadNode,residentStaticInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);if(f.lightCount)useBuffer(uploadNode,f.lights.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
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

        // Stream copies logically precede the renderer consumers of instances/control.
        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);
        domainBuilder.SetDomain(opticalDomain->Identity());opticalDomain->BuildFields(domainBuilder,domainContext,frameResources);domainBuilder.SetDomain(GpuDomainId::Invalid);
        c.resourceAbi.z=opticalDomain->FieldDescriptor();c.resourceAbi.w=opticalDomain->FieldCount();



        if(coreSceneWork){
            auto computeNode=frameGraph.AddPass("DeformCull",GraphQueue::Compute,30,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);auto dispatchPublished=[&](ID3D12GraphicsCommandList7* command,uint32_t pass,bool fullCapacity){command->SetComputeRoot32BitConstant(1,pass,1);command->SetComputeRoot32BitConstant(1,0,0);command->SetComputeRoot32BitConstant(1,f.cpuCount,2);if(f.cpuCount)command->Dispatch((f.cpuCount+63)/64,1,1);for(const auto& bound:meshDomain->Bindings()){command->SetComputeRoot32BitConstant(1,bound.base,0);command->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;uint64_t indirectOffset=controlBase+(fullCapacity?GpuMeshPublicationAbi::CapacityDispatchOffset:GpuMeshPublicationAbi::ActiveDispatchOffset);command->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),indirectOffset,nullptr,0);}command->SetComputeRoot32BitConstants(1,4,params,0);};if(passPlan.rayWork){l->SetPipelineState(rayBuildPSO.Get());for(const auto& range:rayCpuDirty){l->SetComputeRoot32BitConstant(1,range.first,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch((range.count+63)/64,1,1);}for(uint32_t bindingIndex:rayPublicationDirty){if(bindingIndex>=meshDomain->Bindings().size())Fail("ray publication dirty index");const auto& bound=meshDomain->Bindings()[bindingIndex];l->SetComputeRoot32BitConstant(1,bound.base,0);l->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::CapacityDispatchOffset,nullptr,0);}l->SetComputeRoot32BitConstants(1,4,params,0);}l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(hotDataPSO.Get());dispatchPublished(l,0,false);for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);ClearCount(l,f.count.Get(),f.countUAV);l->SetPipelineState(cullPSO.Get());dispatchPublished(l,0,false);ArgumentsReady(l,f.arguments.Get(),f.count.Get());if(f.shadowDirty){ClearCount(l,f.shadowCount.Get(),f.shadowCountUAV);uint32_t shadowParams[4]={0,2,f.cpuCount,0};l->SetComputeRoot32BitConstants(1,4,shadowParams,0);if(f.cpuCount)l->Dispatch((f.cpuCount+63)/64,1,1);ArgumentsReady(l,f.shadowArguments.Get(),f.shadowCount.Get());}});
            if(f.streamControl)useBuffer(computeNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);useBuffer(computeNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(computeNode,f.rayDescriptors.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);useBuffer(computeNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.arguments.Get(),f.count.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty)for(auto* resource:{f.shadowArguments.Get(),f.shadowCount.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);

            auto depthNode=frameGraph.AddPass("Depth",GraphQueue::Direct,40,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);if(passPlan.rayQueries&&!f.blasWork.Empty())BuildDynamic(l,f);if(passPlan.rayWork){AliasMemory(l);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=f.instanceCount+f.additionalCount;build.Inputs.InstanceDescs=f.rayDescriptors->GetGPUVirtualAddress();build.DestAccelerationStructureData=f.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();if(f.tlasBuilt&&f.lastTlasCount==f.instanceCount+f.additionalCount){build.SourceAccelerationStructureData=build.DestAccelerationStructureData;build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;}l->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(l,f.tlas.Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.lastTlasCount=f.instanceCount+f.additionalCount;f.tlasBuilt=true;}if(f.shadowDirty){auto dsv=Dsv(shadowDSV);l->OMSetRenderTargets(0,nullptr,FALSE,&dsv);l->SetPipelineState(shadowPSO.Get());for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot)){uint32_t x=(slot%2)*1536,y=(slot/2)*1024;D3D12_RECT rectangle{LONG(x),LONG(y),LONG(x+1536),LONG(y+1024)};l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,1,&rectangle);for(uint32_t face=0;face<6;face++){Viewport(l,512,512,x+(face%3)*512,y+(face/3)*512);l->SetGraphicsRoot32BitConstant(1,slot*6+face,1);l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.shadowArguments.Get(),0,f.shadowCount.Get(),0);}}EmitBarriers(l,compiled.barriersDuring[0]);}Viewport(l,renderWidth,renderHeight);auto depthHandle=Dsv(depthDSV);l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);l->SetPipelineState(depthPSO.Get());l->SetGraphicsRoot32BitConstants(1,4,params,0);l->ExecuteIndirect(indirect.Get(),f.instanceCount,f.arguments.Get(),0,f.count.Get(),0);});
            useBuffer(depthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(depthNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(depthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(depthNode,f.rayDescriptors.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.arguments.Get(),f.count.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty)for(auto* resource:{f.shadowArguments.Get(),f.shadowCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(passPlan.rayWork)useBuffer(depthNode,f.tlas.Get(),GraphMode::ReadWrite,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);useTexture(depthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);if(f.shadowDirty)useTexture(depthNode,shadow,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);

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
                            ClearCount(l,f.cullCount.Get(),f.cullCountUAV);l->SetPipelineState(cullPSO.Get());auto dispatchPublished=[&](ID3D12GraphicsCommandList7* command,uint32_t passIndex){command->SetComputeRoot32BitConstant(1,passIndex,1);command->SetComputeRoot32BitConstant(1,0,0);command->SetComputeRoot32BitConstant(1,f.cpuCount,2);if(f.cpuCount)command->Dispatch((f.cpuCount+63)/64,1,1);for(const auto& bound:meshDomain->Bindings()){command->SetComputeRoot32BitConstant(1,bound.base,0);command->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;command->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);}command->SetComputeRoot32BitConstants(1,4,params,0);};dispatchPublished(l,1);ArgumentsReady(l,f.cullArguments.Get(),f.cullCount.Get());
                        }else EmitBarriers(l,compiled.barriersDuring[0]);
                    }
                    if(passPlan.clusteredLights){l->SetPipelineState(clusterPSO.Get());l->Dispatch((((renderWidth+63)/64)*((renderHeight+63)/64)*24*viewCount+63)/64,1,1);}
                
                });
                if(passPlan.depthHierarchy){useTexture(visibilityNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(visibilityNode,hierarchy,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.occlusion){useBuffer(visibilityNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.streamControl)useBuffer(visibilityNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);for(auto* resource:{f.cullArguments.Get(),f.cullCount.Get()})useBuffer(visibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}
                if(passPlan.clusteredLights){useBuffer(visibilityNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(visibilityNode,f.clusters.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.depthHierarchy){
                    if(passPlan.occlusion)for(uint32_t mip=0;mip<mips;++mip){
                        D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=mip;range.NumMipLevels=1;range.NumArraySlices=viewCount;range.NumPlanes=1;
                        frameGraph.TransitionTextureSubresource(visibilityNode,hierarchy,range,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,mip+1==mips);
                    }
                    else frameGraph.TransitionTexture(visibilityNode,hierarchy,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
                }
            }
            ID3D12Resource* opaqueArguments=passPlan.occlusion?f.cullArguments.Get():f.arguments.Get();ID3D12Resource* opaqueCount=passPlan.occlusion?f.cullCount.Get():f.count.Get();

            auto opaqueNode=frameGraph.AddPass("Opaque",GraphQueue::Direct,60,[&,opaqueArguments,opaqueCount](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);Viewport(l,renderWidth,renderHeight);auto depthHandle=Dsv(depthDSV);D3D12_CPU_DESCRIPTOR_HANDLE targets[]={Rtv(hdrRTV),Rtv(reflectionsRTV)};float clear[]={.008f,.013f,.021f,1},black[]={0,0,0,0};l->ClearRenderTargetView(targets[0],clear,0,nullptr);if(passPlan.reflections)l->ClearRenderTargetView(targets[1],black,0,nullptr);l->OMSetRenderTargets(passPlan.reflections?2:1,targets,FALSE,&depthHandle);D3D12_SHADING_RATE_COMBINER combiners[]={D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());l->ExecuteIndirect(indirect.Get(),f.instanceCount,opaqueArguments,0,opaqueCount,0);if(f.transparentCount){l->SetPipelineState((passPlan.reflections?transparentPSO:transparentDirectPSO).Get());for(uint32_t index=0;index<f.transparentCount;index++){uint32_t object=f.transparent[index];l->SetGraphicsRoot32BitConstant(1,object,0);l->DispatchMesh(meshes.ranges[CpuInstance(f,object).mesh].meshletCount,1,1);}}});
            if(const auto* fields=frameResources.Find(GpuFrameResourceId::PopulationOpticalFields))useBuffer(opaqueNode,fields->resource,GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:domainContext.lightOcclusionTextures)frameGraph.Texture(opaqueNode,nullptr,field->resource,D3D12_BARRIER_LAYOUT_COMMON,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueNode;
            useBuffer(opaqueNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{opaqueArguments,opaqueCount})useBuffer(opaqueNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)useBuffer(opaqueNode,f.clusters.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayQueries)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(MsaaSamples>1){frameGraph.TransitionTexture(opaqueNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueNode,reflections,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);}else{frameGraph.TransitionTexture(opaqueNode,hdr,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueNode,reflections,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}frameGraph.TransitionTexture(opaqueNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

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
        auto beforeSubmit=[&](const GraphPass& pass){
            // CUDA/D3D12 requires the CUDA signal to be issued before the D3D
            // wait is issued. Independent batches have already been submitted.
            for(const auto& wait:pass.externalWaits)for(const auto& bound:publications)
                if(bound.deferred&&wait.fence==bound.publication.producerFence.Get()){
                    waitForStreams();return;
                }
        };
        if(performanceProfiler.active)frameGraph.ExecuteProfiled(performanceProfiler,*jobs,xr.input.frameId,batchCommands,beforeSubmit);
        else frameGraph.Execute(*jobs,xr.input.frameId,batchCommands,beforeSubmit);
        if(captureRecord){
            auto* slot=static_cast<TextureReadback*>(captureRecord);auto fence=frameGraph.Signal(presentNode);
            if(!slot->recorded||!fence){slot->failed=true;LogEvent(traceFrame,"capture.error","stage=copy_not_submitted");}
            else {slot->fenceValue=fence;slot->recorded=false;LogEvent(traceFrame,"capture.gpu","frame=%llu eye=%u fence=%llu",static_cast<unsigned long long>(slot->frameId),slot->subresource,static_cast<unsigned long long>(fence));}
        }
        FinishTextureReadbacks();if(copyStatic)sceneCache.Commit(frameSlot);if(coreSceneWork&&(!rayDirty||passPlan.rayWork))rayScene.Commit(frameSlot);if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;xr.submitted=true;LogEvent(traceFrame,"gpu.ray_scene","tlas_update=%u cpu_dirty_ranges=%u renewed_streams=%u",unsigned(passPlan.rayWork),uint32_t(rayCpuDirty.size()),uint32_t(rayPublicationDirty.size()));
    }
}
