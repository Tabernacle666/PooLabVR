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
