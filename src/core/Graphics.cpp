#include "GraphicsInternal.h"
namespace lab {
namespace graphics_internal {
thread_local uint64_t traceFrame{};
thread_local TextureDependencies textureDependencies;
}











































































































    void Graphics::Impl::Bind(ID3D12GraphicsCommandList7* l,Frame& f,bool graphics){if(!f.uploadGeneration)Fail("frame upload generation missing");ID3D12DescriptorHeap* heaps[]={descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);auto address=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;if(graphics){l->SetGraphicsRootSignature(root.Get());l->SetGraphicsRootConstantBufferView(0,address);}else{l->SetComputeRootSignature(root.Get());l->SetComputeRootConstantBufferView(0,address);}}











    uint64_t Graphics::Impl::HashMemory(const void* data,size_t size,uint64_t hash){auto* bytes=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;i++){hash^=bytes[i];hash*=1099511628211ull;}return hash;}

    GpuInstance Graphics::Impl::ConvertObject(const RenderObject& o,float alpha)const{
        if(o.mesh>=meshes.ranges.size()||o.material>=surfaceData.size())Fail("render asset handle");
        // Identical consecutive poses are extremely common in streamed scenery.
        // Avoid quaternion slerp and position lerp when the exact input is
        // stationary; all moving PhysX bodies retain their original Blend path.
        const bool stationary=
            o.previous.p.x==o.current.p.x&&o.previous.p.y==o.current.p.y&&
            o.previous.p.z==o.current.p.z&&
            o.previous.q.x==o.current.q.x&&o.previous.q.y==o.current.q.y&&
            o.previous.q.z==o.current.q.z&&o.previous.q.w==o.current.q.w;
        const Pose p=stationary?o.current:Blend(o.previous,o.current,alpha);
        GpuInstance out{};auto bounds=meshes.ranges[o.mesh].bounds;float expansion=(o.flags&DeformedObject)?1+std::abs(meshes.deformations[o.mesh].axisAmplitude.w):1;
        out.positionRadius={p.p.x,p.p.y,p.p.z,bounds.w*std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)})*expansion};out.rotation={p.q.x,p.q.y,p.q.z,p.q.w};out.scale={o.scale.x,o.scale.y,o.scale.z,0};out.mesh=o.mesh;out.material=o.material;out.id=o.id;out.flags=o.flags;if(surfaceData[o.material].extra.y==2)out.flags|=DynamicObject|AlphaBlendObject;return out;
    }

    const GpuInstance& Graphics::Impl::CpuInstance(const Frame& f,uint32_t index)const{uint32_t staticCount=sceneCache.StaticCount();if(index<staticCount)return sceneCache.StaticInstances()[index];index-=staticCount;if(index>=f.dynamicCount)Fail("CPU instance index");return f.objects[index];}





    D3D12_RAYTRACING_GEOMETRY_DESC Graphics::Impl::DynamicGeometry(Frame& f,uint32_t mesh) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;g.Triangles.VertexBuffer={f.dynamicVertices->GetGPUVirtualAddress(),sizeof(Vertex)};g.Triangles.VertexCount=uint32_t(meshes.vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;g.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(meshes.ranges[mesh].firstIndex)*4;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;g.Triangles.IndexCount=meshes.ranges[mesh].indexCount;return g;
    }





    void Graphics::Impl::SetDomain(Frame& f,ResourceDomain domain,std::initializer_list<DescriptorHandle> descriptors){uint32_t d=uint32_t(domain);if(descriptors.size()>ResourceDomainWords)Fail("resource domain overflow");auto* table=f.resourceTableData[d];if(!table)Fail("resource domain not initialized");std::fill_n(table,ResourceDomainWords,DomainBindingEntry{});uint32_t i=0;for(DescriptorHandle handle:descriptors)table[i++]=handle.Valid()?DomainBindingEntry{Physical(handle),Generation(handle)}:DomainBindingEntry{};}

    void Graphics::Impl::BuildDynamic(ID3D12GraphicsCommandList7* list,Frame& f) {
        
        uint32_t ordinal=0;
        for(uint32_t dynamicIndex:f.blasWork.Active()){
            if(dynamicIndex>=dynamicMeshes.size()||dynamicIndex>=f.dynamicBLAS.size()||dynamicIndex>=f.dynamicBlasBuilt.size())Fail("dynamic work index");
            if(ordinal++)BBarrier(list,f.dynamicScratch.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            uint32_t mesh=dynamicMeshes[dynamicIndex];auto geometry=DynamicGeometry(f,mesh);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.pGeometryDescs=&geometry;build.DestAccelerationStructureData=f.dynamicBLAS[dynamicIndex]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.dynamicScratch->GetGPUVirtualAddress();bool built=f.dynamicBlasBuilt[dynamicIndex]!=0;if(built){build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;build.SourceAccelerationStructureData=build.DestAccelerationStructureData;}BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),built?D3D12_BARRIER_SYNC_ALL_SHADING:D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,built?D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ:D3D12_BARRIER_ACCESS_NO_ACCESS,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);list->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.dynamicBlasBuilt[dynamicIndex]=1;
        }
    }



















    void Graphics::Impl::PrepareLights(Frame& f,const RenderScene& snapshot) {
        f.lightCount=std::min(snapshot.lightCount,MaxLights);f.shadowDirty=0;f.hasShadowedLights=false;f.hasShadowAtlasLights=false;f.shadowIDs.fill(Invalid);f.shadowKeys.fill(0);
        uint64_t staticKey=staticShadowKey;
        for(uint32_t i=0;i<f.dynamicCount;i++){const auto& o=f.objects[i];if(o.flags&(DynamicObject|DeformedObject|NoShadow))continue;staticKey=HashMemory(&o,sizeof(o),staticKey);}
        std::array<bool,ShadowSlots> assigned{};
        for(uint32_t i=0;i<f.lightCount;i++) {
            auto light=snapshot.lights[i];if(light.animationNode!=Invalid){auto world=animation.NodeMatrix(light.animationNode)*XMMatrixScaling(1,1,-1);XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));}if(contributions.lightContext)contributions.lightContext(contributions.context,light,f.simulationTime);int slot=-1;
            if((light.flags&1)&&!(light.flags&(2|4))) {
                for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]&&shadowCache[j].id==light.id){slot=int(j);break;}
                if(slot<0)for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]){bool reserved=false;for(uint32_t k=i+1;k<f.lightCount;k++)if(snapshot.lights[k].id==shadowCache[j].id)reserved=true;if(!reserved){slot=int(j);break;}}
            }
            if(slot>=0) {
                uint32_t s=uint32_t(slot);assigned[s]=true;f.shadowIDs[s]=light.id;f.shadowLights[s]=light;
                uint64_t key=HashMemory(&light.position,sizeof(light.position),staticKey);key=HashMemory(&light.radius,sizeof(light.radius),key);key=HashMemory(&light.revision,sizeof(light.revision),key);f.shadowKeys[s]=key;
                if(!shadowCache[s].valid||shadowCache[s].id!=light.id||shadowCache[s].key!=key)f.shadowDirty|=1u<<s;
                auto at=XMLoadFloat3(&light.position);const XMVECTOR directions[]={XMVectorSet(1,0,0,0),XMVectorSet(-1,0,0,0),XMVectorSet(0,1,0,0),XMVectorSet(0,-1,0,0),XMVectorSet(0,0,1,0),XMVectorSet(0,0,-1,0)};
                for(uint32_t face=0;face<6;face++){auto up=face==2?XMVectorSet(0,0,-1,0):face==3?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0);XMStoreFloat4x4(&shadowMatrices[s*6+face],XMMatrixLookToLH(at,directions[face],up)*XMMatrixPerspectiveFovLH(XM_PIDIV2,1,std::max(.1f,light.radius),.02f));}
            }
            f.hasShadowedLights|=(light.flags&1)!=0;f.hasShadowAtlasLights|=slot>=0;
            f.lightData[i]={{light.position.x,light.position.y,light.position.z,std::max(.1f,light.radius)},{light.color.x,light.color.y,light.color.z,light.power},{light.direction.x,light.direction.y,light.direction.z,light.outerCos},{light.innerCos,float(slot),float(light.flags),0}};
        }
        f.lightsOffset=Reserve(f,sizeof(Light)*MaxLights);if(f.lightCount)memcpy(f.uploadGeneration->mapped+f.lightsOffset,f.lightData.data(),f.lightCount*sizeof(Light));
    }

    const RenderScene& Graphics::Impl::Visuals(const FrameInput& input){
        auto& r=visualFrame;r.objects.clear();r.count=0;r.lightCount=baseLightCount;for(uint32_t i=0;i<baseLightCount;i++)r.lights[i]=baseLights[i];double displayTime=input.targetDisplayQpc?QpcSeconds(input.targetDisplayQpc):input.time;if(!sceneTimeOriginValid&&displayTime>0){sceneTimeOrigin=displayTime;sceneTimeOriginValid=true;}double sceneTime=sceneTimeOriginValid?displayTime-sceneTimeOrigin:0;r.time=displayTime;r.inputFrameId=input.frameId;r.simulationTick=0;
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
        uint64_t totalCpu=uint64_t(sceneCache.StaticCount())+snapshot.count;if(!active){LogEvent(snapshot.inputFrameId,"render.skip","reason=%s","frame_context_busy");return false;}if(totalCpu>=Invalid)Fail("CPU scene instance address space exhausted");
        auto& f=*active;
        UINT64 uploadRequired=UINT64(snapshot.count)*sizeof(GpuInstance)+animation.Motions().size()*sizeof(GpuMotion)+animation.Bones().size()*sizeof(GpuBone)+animation.Weights().size()*sizeof(float)+sizeof(Constants)+MaxLights*sizeof(Light)+8192+surfaceData.size()*sizeof(Surface);
        auto upload=std::atomic_load_explicit(&uploadGeneration,std::memory_order_acquire);if(!upload)Fail("upload generation unavailable");
        if(uploadRequired>upload->chunk){
            UINT64 capacity=upload->chunk;while(capacity<uploadRequired)capacity*=2;auto next=NewUploadGeneration(capacity);std::atomic_store_explicit(&uploadGeneration,next,std::memory_order_release);LogEvent(snapshot.inputFrameId,"gpu.capacity_generation","domain=graphics.upload generation=%llu old_bytes=%llu new_bytes=%llu",static_cast<unsigned long long>(next->generation),static_cast<unsigned long long>(upload->chunk*3),static_cast<unsigned long long>(next->chunk*3));upload=std::move(next);
        }
        f.uploadGeneration=upload;f.base=UINT64(&f-frames.data())*upload->chunk;auto* mapped=upload->mapped;
        f.sourceInput=snapshot.inputFrameId;f.sourceTick=snapshot.simulationTick;f.bump=0;f.simulationTime=sceneTimeOriginValid?snapshot.time-sceneTimeOrigin:0;animation.Evaluate(f.simulationTime);auto uploadAnimation=[&](const auto& values){using Value=typename std::decay_t<decltype(values)>::value_type;auto offset=Reserve(f,std::max(size_t(1),values.size())*sizeof(Value));if(!values.empty())memcpy(mapped+offset,values.data(),values.size()*sizeof(Value));else memset(mapped+offset,0,sizeof(Value));return offset;};f.motionOffset=uploadAnimation(animation.Motions());f.boneOffset=uploadAnimation(animation.Bones());f.weightOffset=uploadAnimation(animation.Weights());f.constantsOffset=Reserve(f,sizeof(Constants));f.constants=reinterpret_cast<Constants*>(mapped+f.constantsOffset);f.dynamicCount=snapshot.count;f.cpuCount=uint32_t(totalCpu);f.instanceCount=f.cpuCount;f.additionalCount=0;f.objectsOffset=Reserve(f,std::max(1u,f.dynamicCount)*sizeof(GpuInstance));f.cpuObjects.resize(f.dynamicCount);f.objects=f.cpuObjects.data();
        struct Work {Impl* self;Frame* frame;const RenderScene* snapshot;float alpha;};double tickPhase=f.simulationTime/PhysicsDt;Work work{this,&f,&snapshot,std::clamp(float(tickPhase-std::floor(tickPhase)),0.f,1.f)};
        // Cap the number of tiny dispatches to around 12 jobs while keeping
        // work distribution across the five workers and the render thread.
        // Each instance is written once, at the same index as before.
        const uint32_t conversionGrain=std::max(256u,(f.dynamicCount+11u)/12u);
        jobs->ParallelFor(f.dynamicCount,conversionGrain,[](void* context,uint32_t begin,uint32_t end){auto& work=*static_cast<Work*>(context);auto& self=*work.self;auto& f=*work.frame;for(uint32_t i=begin;i<end;i++)f.objects[i]=self.ConvertObject(work.snapshot->objects[i],work.alpha);},&work);
        const double conversionDone=samplePrepare?QpcNowSeconds():0.;
        rayScene.UpdateDynamic(sceneCache.StaticCount(),std::span<const GpuInstance>(f.objects,f.dynamicCount));
        f.deformationWork.BeginFrame();f.blasWork.BeginFrame();
        auto deformationRevision=[&](uint32_t dynamicIndex){uint32_t mesh=dynamicMeshes[dynamicIndex];uint64_t revision=1469598103934665603ull;const auto& motion=animation.Motions()[mesh];if(motion.binding.x!=Invalid){revision=HashMemory(&motion,sizeof(motion),revision);if(motion.binding.z){uint32_t first=motion.binding.y,count=motion.binding.z;if(uint64_t(first)+count>animation.Bones().size())Fail("animation bone revision range");if(count)revision=HashMemory(animation.Bones().data()+first,size_t(count)*sizeof(GpuBone),revision);}if(motion.morph.x){uint32_t first=motion.morph.y,count=motion.morph.x;if(uint64_t(first)+count>animation.Weights().size())Fail("animation weight revision range");if(count)revision=HashMemory(animation.Weights().data()+first,size_t(count)*sizeof(float),revision);}}else{const auto& d=meshes.deformations[mesh];revision=HashMemory(&d,sizeof(d),revision);if(d.axisAmplitude.w!=0&&d.frequencyPhase.x!=0){float stretch=1+d.axisAmplitude.w*std::sin(float(f.simulationTime)*d.frequencyPhase.x+d.frequencyPhase.y);revision=HashMemory(&stretch,sizeof(stretch),revision);}}return revision?revision:1ull;};
        auto requestDeformation=[&](uint32_t dynamicIndex,bool rayVisible){if(dynamicIndex==Invalid||dynamicIndex>=dynamicMeshes.size())Fail("DeformedObject references mesh without dynamic deformation capability");uint64_t revision=deformationRevision(dynamicIndex);f.pendingDynamicRevision[dynamicIndex]=revision;if(f.dynamicGeometryRevision[dynamicIndex]!=revision&&!f.deformationWork.Activate(dynamicIndex))Fail("deformation work index");if(rayVisible&&f.dynamicBlasRevision[dynamicIndex]!=revision&&!f.blasWork.Activate(dynamicIndex))Fail("dynamic BLAS work index");};
        for(uint32_t dynamicIndex:staticDeformedMeshes){bool rayVisible=std::find(staticDeformedRtMeshes.begin(),staticDeformedRtMeshes.end(),dynamicIndex)!=staticDeformedRtMeshes.end();requestDeformation(dynamicIndex,rayVisible);}
        for(uint32_t i=0;i<f.dynamicCount;i++)if(f.objects[i].flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[f.objects[i].mesh];requestDeformation(dynamicIndex,(f.objects[i].flags&NoRayTracing)==0);}
        PrepareLights(f,snapshot);
        // CPU-only scene analysis reads ordinary cacheable memory; publish the
        // completed instances to write-combined UPLOAD memory only once.
        const double uploadCopyBegin=samplePrepare?QpcNowSeconds():0.;
        if(f.dynamicCount)memcpy(mapped+f.objectsOffset,f.objects,size_t(f.dynamicCount)*sizeof(GpuInstance));
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



uint32_t Graphics::SetFogSettings(uint64_t owner,const LabFogSettingsV1& settings){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetSettings(owner,settings);
}
uint32_t Graphics::SetFogWind(uint64_t owner,const LabFogWindV1& wind){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetWind(owner,wind):0;}
uint32_t Graphics::SetFogInput(uint64_t owner,const LabFogInputV1& input){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetInput(owner,input):0;}
void Graphics::ClearAtmosphere(uint64_t owner){if(impl_->atmosphereDomain)impl_->atmosphereDomain->Clear(owner);if(impl_->opticalDomain)impl_->opticalDomain->Clear(owner);}
uint32_t Graphics::SetFogTransport(uint64_t owner,const LabFogTransportV1& s){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetTransport(owner,s);
}
uint32_t Graphics::SetAntialiasing(uint64_t owner,const LabAntialiasingV1& s){
    if(!owner||s.structSize<sizeof(s)||s.mode>2)return 0;for(auto& c:impl_->aaClients)if(c.owner==owner){c.mode=s.mode;impl_->aaMode=s.mode;return 1;}impl_->aaClients.push_back({owner,s.mode});impl_->aaMode=s.mode;return 1;
}
uint32_t Graphics::SetMultisampling(uint64_t owner,const LabMultisamplingV1& s){
    if(!owner||s.structSize<sizeof(s)||(s.samples!=1&&s.samples!=4))return 0;
    if(s.samples==4)for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        if(FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))||!q.NumQualityLevels)return 0;
    }
    auto& clients=impl_->msaaClients;auto it=std::find_if(clients.begin(),clients.end(),[&](const auto& c){return c.owner==owner;});
    if(it==clients.end())clients.push_back({owner,s.samples});else it->samples=s.samples;
    impl_->requestedSamples=clients.back().samples;return 1;
}
uint32_t Graphics::SetSurfaceSettings(uint64_t owner,const LabSurfaceSettingsV1& s){
    if(!owner||s.structSize<sizeof(s)||s.material>65535||(!impl_->surfaceData.empty()&&s.material>=impl_->surfaceData.size()))return 0;
    const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(!std::isfinite(v[i])||v[i]<lo[i]||v[i]>hi[i])return 0;
    for(auto& e:impl_->surfaceEdits)if(e.owner==owner&&e.settings.material==s.material){if(memcmp(&e.settings,&s,sizeof(s))){e.settings=s;impl_->ApplySurfaceEdits();}return 1;}impl_->surfaceEdits.push_back({owner,s});impl_->ApplySurfaceEdits();return 1;
}
void Graphics::ClearRendering(uint64_t owner){
    std::erase_if(impl_->msaaClients,[&](const auto& c){return c.owner==owner;});impl_->requestedSamples=impl_->msaaClients.empty()?LAB_MSAA_SAMPLES:impl_->msaaClients.back().samples;
    std::erase_if(impl_->aaClients,[&](const auto& c){return c.owner==owner;});impl_->aaMode=impl_->aaClients.empty()?0:impl_->aaClients.back().mode;
    if(std::erase_if(impl_->surfaceEdits,[&](const auto& c){return c.owner==owner;}))impl_->ApplySurfaceEdits();
}
uint32_t Graphics::SetPopulationOpticalSettings(uint64_t owner,const LabPopulationOpticalSettingsV1& s){return impl_->opticalDomain?impl_->opticalDomain->SetSettings(owner,s):0;}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV1& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV2& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::HitTestTerminal(uint64_t owner,const LabTerminalHitQueryV2& q,LabTerminalHitV2& hit)const{return impl_->terminalDomain?impl_->terminalDomain->HitTest(owner,q,hit):0;}




void Graphics::ClearTerminal(uint64_t owner){if(impl_->terminalDomain)impl_->terminalDomain->Clear(owner);}
Graphics::Graphics():impl_(std::make_unique<Impl>()){impl_->facade=this;}Graphics::~Graphics()=default;void Graphics::Initialize(const XrGraphicsRequirementsD3D12KHR& r,bool gpuValidation){impl_->Initialize(r,gpuValidation);}void Graphics::Configure(const XR& x,const SceneData& assets,JobSystem& jobs,RenderContributions extra){impl_->Configure(x,assets,jobs,extra);}void Graphics::ReplaceScene(const SceneData& assets,RenderContributions extra){impl_->ReplaceScene(assets,extra);}void Graphics::UnloadScene(){impl_->UnloadScene();}bool Graphics::SceneLoaded()const{return impl_->sceneLoaded;}void Graphics::RegisterDomain(std::unique_ptr<GpuDomain> domain){impl_->RegisterDomain(std::shared_ptr<GpuDomain>(std::move(domain)));}void Graphics::RegisterSharedDomain(std::shared_ptr<GpuDomain> domain){impl_->RegisterDomain(std::move(domain));}void Graphics::InitializeDomain(GpuDomain& domain){if(!impl_->domainServicesReady)Fail("GPU domain services not initialized");domain.Initialize(*impl_);}bool Graphics::UnregisterDomain(GpuWorldDomain id){return impl_->UnregisterDomain(id);}void Graphics::AttachStream(GpuStream& stream){if(stream.Domain()==GpuDomainId::Invalid||!impl_->FindDomain(stream.Domain()))Fail("GPU stream domain is not registered");impl_->streams.push_back(&stream);impl_->publications.reserve(impl_->streams.size());}void Graphics::DetachStream(GpuStream& stream){impl_->DetachStream(stream);}void Graphics::ReserveStreams(){impl_->ReserveStreams();}void Graphics::CancelReservedStreams(){impl_->CancelReservedStreams();}bool Graphics::Prepare(const FrameInput& s){return impl_->Prepare(s);}GpuMemoryStats Graphics::MemoryStats(){impl_->CollectManaged();return impl_->MemoryStats();}GpuValidationState Graphics::ValidationState()const{return impl_->validation.State(impl_->device.Get());}void Graphics::Render(XRFrame& f,const std::function<void()>& waitForStreams){impl_->Render(f,waitForStreams);}void Graphics::WaitIdle(){impl_->WaitStartup(impl_->direct);impl_->WaitStartup(impl_->compute);impl_->WaitStartup(impl_->copy);}ID3D12Device* Graphics::Device()const{return impl_->device.Get();}ID3D12RootSignature* Graphics::BindlessRootSignature()const{return impl_->root.Get();}ID3D12DescriptorHeap* Graphics::BindlessResourceHeap()const{return impl_->descriptors.Get();}ID3D12DescriptorHeap* Graphics::BindlessSamplerHeap()const{return impl_->samplers.Get();}GpuResourceLease Graphics::CreateManagedBuffer(const GpuBufferDesc& d){return impl_->CreateManagedBuffer(d);}GpuResourceLease Graphics::CreateManagedTexture2D(const GpuTextureDesc& d){return impl_->CreateManagedTexture2D(d);}GpuResourceLease Graphics::CreateManagedTexture3D(const GpuTextureDesc& d){return impl_->CreateManagedTexture3D(d);}GpuResourceLease Graphics::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){return impl_->UploadStaticBuffer(bytes,stride,name);}GpuResourceLease Graphics::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){return impl_->UploadStaticTexture2D(d,subresources);}uint32_t Graphics::DescriptorIndex(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorIndex(h):Invalid;}uint32_t Graphics::DescriptorGeneration(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorGeneration(h):0;}ID3D12CommandQueue* Graphics::Queue()const{return impl_->direct.queue.Get();}
}

namespace lab {










}
