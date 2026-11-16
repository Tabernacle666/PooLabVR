#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::EnsureCutPipelines(){
        if(cutPipelines[0])return;
        constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
            L"StepProjectiles",L"ResolveImpact",L"BuildImpactVolume",L"CutSurface",
            L"FindCutSegments",L"PartitionCap",L"EmitCap",L"MakeCutDispatch",
            L"PackCutGeometry",L"FinalizeCutRange",L"StampProjectiles",L"CompositeProjectiles"
        };
        std::array<ComPtr<ID3D12PipelineState>,size_t(CutKernel::Count)> prepared;
        for(size_t i=0;i<names.size();++i){
            auto bytes=Shader(names[i]);ComputePipeline stream{};
            stream.root.data=root.Get();stream.cs.data={bytes.data(),bytes.size()};
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(stream),&stream};
            prepared[i]=Pipeline(names[i],desc,Hash(bytes));
        }
        cutPipelines=std::move(prepared);
    }

    void Graphics::Impl::BindCutPipeline(ID3D12GraphicsCommandList7* list,CutKernel kernel,D3D12_GPU_VIRTUAL_ADDRESS parameters){
        if(!list||!parameters||(parameters&255))Fail("Invalid cut pipeline arguments");
        const auto index=size_t(kernel);if(index>=cutPipelines.size()||!cutPipelines[index])Fail("Cut pipeline is not initialized");
        ID3D12DescriptorHeap* heaps[]={descriptors.Get(),samplers.Get()};list->SetDescriptorHeaps(2,heaps);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootConstantBufferView(0,parameters);
        list->SetPipelineState(cutPipelines[index].Get());
    }
    Graphics::Impl::CutDispatch Graphics::Impl::PrepareCutDispatch(Frame& f,const XRFrame& xr){
        EnsureCutPipelines();
        if(!cutProjectiles[0]){
            for(uint32_t i=0;i<2;i++){
                cutProjectiles[i]=Temporary(Buffer(UINT64(CutProjectileCapacity)*sizeof(CutProjectile),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                cutProjectileSRVs[i]=Srv(cutProjectiles[i].Get(),CutProjectileCapacity,sizeof(CutProjectile));
                cutProjectileUAVs[i]=UavBuffer(cutProjectiles[i].Get(),CutProjectileCapacity,sizeof(CutProjectile));
            }
            for(auto& cf:cutFrames){
                cf.impacts=Temporary(Buffer(UINT64(CutProjectileCapacity)*sizeof(CutImpact),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                cf.control=Temporary(Buffer(16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                cf.resolved=Temporary(Buffer(UINT64(CutProjectileCapacity)*sizeof(CutResolved),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                cf.impactSRV=Srv(cf.impacts.Get(),CutProjectileCapacity,sizeof(CutImpact));
                cf.impactUAV=UavBuffer(cf.impacts.Get(),CutProjectileCapacity,sizeof(CutImpact));
                cf.controlUAV=UavBuffer(cf.control.Get(),4,0);
                cf.resolvedUAV=UavBuffer(cf.resolved.Get(),CutProjectileCapacity,sizeof(CutResolved));
            }
        }
        if(!cutOverlayMask.resource){
            cutOverlayMask.resource=Temporary(Texture(renderWidth,renderHeight,DXGI_FORMAT_R32_UINT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
            cutOverlaySRV=TextureSrv(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,{},true);
            cutOverlayUAV=TextureUav(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,0,{},true);
        }
        const uint32_t frameSlot=uint32_t(&f-frames.data());
        const auto& cf=cutFrames[frameSlot];
        const uint32_t source=cutStepSerial&1u,destination=source^1u;
        struct StepParameters {
            uint32_t source,destination,impacts,control;
            uint32_t scene,count,capacity,mask;
            float deltaTime,maxSegmentLength,pad0,pad1;
            uint32_t spawnMask,left,right,reset;
            CutProjectile spawns[2];
        } step{};
        step.source=Physical(cutProjectileSRVs[source]);
        step.destination=Physical(cutProjectileUAVs[destination]);
        step.impacts=Physical(cf.impactUAV);
        step.control=Physical(cf.controlUAV);
        step.scene=Physical(f.tlasSRV);step.count=CutProjectileCapacity;step.capacity=CutProjectileCapacity;step.mask=0xffu;
        step.deltaTime=std::clamp(float(double(xr.displayPeriod)*1e-9),.001f,.05f);
        step.maxSegmentLength=1.f;step.reset=cutStepSerial==0?1u:0u;
        for(uint32_t hand=0;hand<2;hand++){
            const auto& shot=xr.shotHands[hand];
            if(!shot.fired)continue;
            const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
            CutProjectile p{};
            p.position=Add(shot.aim.pose.p,Scale(forward,.12f));
            p.velocity=Scale(forward,hand==0?95.f:28.f);
            p.radius=hand==0?.6f:.85f;
            p.acceleration=hand==0?XMFLOAT3{}:XMFLOAT3{0,-9.81f,0};
            p.lifetime=hand==0?3.f:5.f;
            p.id=cutNextId++;if(cutNextId==0)cutNextId=1;
            p.kind=hand;p.alive=1;
            step.spawns[hand]=p;step.spawnMask|=1u<<hand;
            if(hand==0)step.left=(p.id-1u)%CutProjectileCapacity;
            else step.right=(p.id-1u)%CutProjectileCapacity;
        }
        struct ResolveParameters {
            uint32_t impacts,rayMap,instances,ranges;
            uint32_t solids,resolved,impactCount,rayCount;
            uint32_t rangeCount,solidCount,worldCount,control;
            uint32_t published,publishedCount,pad0,pad1;
        } resolve{};
        resolve.impacts=Physical(cf.impactSRV);resolve.rayMap=Physical(f.rayMapSRV);
        resolve.instances=Physical(f.worldInstanceSRV);resolve.ranges=Physical(rangesSRV);
        resolve.solids=triangleSolidsSRV.Valid()?Physical(triangleSolidsSRV):Invalid;
        resolve.resolved=Physical(cf.resolvedUAV);resolve.impactCount=CutProjectileCapacity;
        resolve.rayCount=f.rayInstanceCount;resolve.rangeCount=uint32_t(meshes.ranges.size());
        resolve.solidCount=uint32_t(meshes.indices.size()/3);resolve.worldCount=f.cpuCount;
        resolve.control=Physical(cf.controlUAV);
        resolve.published=Physical(f.publishedInstanceSRV);
        resolve.publishedCount=f.instanceCount-f.cpuCount;
        struct DrawParameters {
            uint32_t projectiles,maskUav,maskSrv,colorUav;
            uint32_t capacity,width,height,views;
            XMFLOAT4X4 vp[MaxViews];
        } draw{};
        draw.projectiles=Physical(cutProjectileSRVs[destination]);
        draw.maskUav=Physical(cutOverlayUAV);draw.maskSrv=Physical(cutOverlaySRV);
        draw.colorUav=Physical(hdrResolvedUAV);
        draw.capacity=CutProjectileCapacity;draw.width=renderWidth;draw.height=renderHeight;draw.views=xr.viewCount;
        for(uint32_t i=0;i<xr.viewCount;i++)draw.vp[i]=xr.vp[i];
        auto write=[&](const void* value,UINT64 size)->D3D12_GPU_VIRTUAL_ADDRESS{
            const UINT64 offset=Reserve(f,size);
            std::memcpy(f.uploadGeneration->mapped+offset,value,size);
            return f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
        };
        return {write(&step,sizeof(step)),write(&resolve,sizeof(resolve)),write(&draw,sizeof(draw)),source,destination};
    }

    void Graphics::Impl::ResetCutResources(){
        auto release=[&](DescriptorHandle& h){if(h.Valid())ReleaseDescriptor(h);};
        auto forget=[&](ComPtr<ID3D12Resource>& resource){passGraph.Forget(resource.Get());resource.Reset();};
        for(auto& h:cutProjectileSRVs)release(h);
        for(auto& h:cutProjectileUAVs)release(h);
        for(auto& resource:cutProjectiles)forget(resource);
        for(auto& cf:cutFrames){
            release(cf.impactSRV);release(cf.impactUAV);release(cf.controlUAV);release(cf.resolvedUAV);
            forget(cf.impacts);forget(cf.control);forget(cf.resolved);
        }
        release(cutOverlaySRV);release(cutOverlayUAV);
        forget(cutOverlayMask.resource);cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
        cutActive=false;cutStepSerial=0;cutNextId=1;
    }

}
