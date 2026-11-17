#include "GraphicsInternal.h"

namespace OASIS {
void Graphics::Impl::EnsureCutPipelines(){
    if(cutPipelines[0])return;
    constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
        L"StepProjectiles",L"StampProjectiles",L"CompositeProjectiles",
        L"StampAimLasers",L"ClearMarkedTiles",L"BuildTileDispatchArgs",
        L"BeginCut",L"RebuildCutTriangles",L"CommitCut"
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
void Graphics::Impl::BindCutPipeline(ID3D12GraphicsCommandList7* list,CutKernel kernel,
                                     D3D12_GPU_VIRTUAL_ADDRESS parameters){
    if(!list||!parameters||(parameters&255))Fail("Invalid cut pipeline arguments");
    const auto index=size_t(kernel);
    if(index>=cutPipelines.size()||!cutPipelines[index])Fail("Uninitialized cut pipeline");
    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
    list->SetDescriptorHeaps(2,heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootConstantBufferView(0,parameters);
    list->SetPipelineState(cutPipelines[index].Get());
}
// Build the full immutable source once. Objects are flattened to world-space
// triangles, retaining their original per-primitive material and instance ID.
// Subsequent cuts remain on the GPU; the CPU never reads the modified mesh.
void Graphics::Impl::InitializeCutSource(const SceneData& scene){
    cutInitialCount=cutCapacity=cutStaticCount=0;
    if(scene.objects.empty())return;
    std::vector<CutTriangle> source;
    source.reserve(scene.meshes.indices.size()/3);
    for(uint32_t instance=0;instance<scene.objects.size();++instance){
        const auto& object=scene.objects[instance];
        if((object.flags&(DynamicObject|DeformedObject|AlphaBlendObject))!=0)continue;
        if(object.mesh>=scene.meshes.ranges.size())Fail("Cut mesh range missing");
        const auto& range=scene.meshes.ranges[object.mesh];
        if(uint64_t(range.firstIndex)+range.indexCount>scene.meshes.indices.size()||
           range.indexCount%3!=0)Fail("Cut source index range");
        auto transformed=[&](uint32_t index)->Vertex{
            if(index>=scene.meshes.vertices.size())Fail("Cut source vertex index");
            auto v=scene.meshes.vertices[index];
            const auto scale=object.scale;
            v.position=Transform({v.position.x*scale.x,v.position.y*scale.y,
                                  v.position.z*scale.z},object.current);
            const XMFLOAT3 localNormal{
                v.normal.x/(abs(scale.x)>1e-8f?scale.x:1.f),
                v.normal.y/(abs(scale.y)>1e-8f?scale.y:1.f),
                v.normal.z/(abs(scale.z)>1e-8f?scale.z:1.f)};
            v.normal=Unit(Sub(Transform(localNormal,object.current),object.current.p));
            return v;
        };
        for(uint32_t i=0;i<range.indexCount;i+=3){
            CutTriangle tri{};
            tri.a=transformed(scene.meshes.indices[range.firstIndex+i]);
            tri.b=transformed(scene.meshes.indices[range.firstIndex+i+1]);
            tri.c=transformed(scene.meshes.indices[range.firstIndex+i+2]);
            tri.material=object.material;tri.instanceId=instance;
            tri.padding0=object.flags;
            source.push_back(tri);
        }
    }
    if(source.empty())return;
    if(source.size()>UINT32_MAX/16u)Fail("Cut mesh source too large");
    cutInitialCount=uint32_t(source.size());
    cutCapacity=std::max(65536u,cutInitialCount*16u);
    cutStaticCount=uint32_t(scene.objects.size());
    cutOriginal=UploadVector(source);
    cutOriginalSRV=Srv(cutOriginal.Get(),cutInitialCount,sizeof(CutTriangle));
    for(int i=0;i<2;++i){
        cutBuffers[i]=Temporary(Buffer(uint64_t(cutCapacity)*sizeof(CutTriangle),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
        cutBufferSRVs[i]=Srv(cutBuffers[i].Get(),cutCapacity,sizeof(CutTriangle));
        cutBufferUAVs[i]=UavBuffer(cutBuffers[i].Get(),cutCapacity,sizeof(CutTriangle));
    }
    auto allocate=[&](ComPtr<ID3D12Resource>& target,uint32_t bytes){
        target=Temporary(Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                         D3D12_RESOURCE_STATE_COMMON);
    };
    allocate(cutState,16);
    cutStateSRV=Srv(cutState.Get(),1,sizeof(XMUINT4));
    cutStateUAV=UavBuffer(cutState.Get(),1,sizeof(XMUINT4));
    allocate(cutControl,16);
    cutControlUAV=UavBuffer(cutControl.Get(),4,0);
    allocate(cutDispatchArguments,12);
    cutDispatchArgumentsUAV=UavBuffer(cutDispatchArguments.Get(),3,0);
    allocate(cutDrawArguments,16);
    cutDrawArgumentsUAV=UavBuffer(cutDrawArguments.Get(),4,0);
    auto uploadInit=[&](ID3D12Resource* target,const XMUINT4& value){
        auto upload=Host(16,D3D12_HEAP_TYPE_UPLOAD);
        void* ptr=nullptr;D3D12_RANGE empty{0,0};
        Hr(upload->Map(0,&empty,&ptr),"cut state upload");
        std::memcpy(ptr,&value,sizeof(value));upload->Unmap(0,nullptr);
        startup.list->CopyBufferRegion(target,0,upload.Get(),0,16);
        BBarrier(startup.list.Get(),target,D3D12_BARRIER_SYNC_COPY,
                 D3D12_BARRIER_SYNC_ALL_SHADING,
                 D3D12_BARRIER_ACCESS_COPY_DEST,
                 D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                     D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        uploads.push_back(upload);
    };
    uploadInit(cutState.Get(),{cutInitialCount,0,0,0});
    uploadInit(cutDrawArguments.Get(),{0,0,1,1});
}
Graphics::Impl::CutDispatch Graphics::Impl::PrepareCutDispatch(Frame& f,const XRFrame& xr){
    EnsureCutPipelines();
    if(!cutProjectiles[0]){
        for(uint32_t i=0;i<2;i++){
            cutProjectiles[i]=Temporary(
                Buffer(uint64_t(CutProjectileCapacity)*sizeof(CutProjectile),
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COMMON);
            cutProjectileSRVs[i]=Srv(cutProjectiles[i].Get(),CutProjectileCapacity,sizeof(CutProjectile));
            cutProjectileUAVs[i]=UavBuffer(cutProjectiles[i].Get(),CutProjectileCapacity,sizeof(CutProjectile));
        }
        for(auto& cf:cutFrames){
            cf.impacts=Temporary(Buffer(uint64_t(CutProjectileCapacity)*sizeof(CutImpact),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                           D3D12_RESOURCE_STATE_COMMON);
            cf.control=Temporary(Buffer(16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                           D3D12_RESOURCE_STATE_COMMON);
            cf.impactSRV=Srv(cf.impacts.Get(),CutProjectileCapacity,sizeof(CutImpact));
            cf.impactUAV=UavBuffer(cf.impacts.Get(),CutProjectileCapacity,sizeof(CutImpact));
            cf.controlUAV=UavBuffer(cf.control.Get(),4,0);
        }
    }
    if(!cutOverlayMask.resource){
        cutOverlayTileCountX=(renderWidth+15u)/16u;
        cutOverlayTileCountY=(renderHeight+15u)/16u;
        const uint64_t capacity=uint64_t(cutOverlayTileCountX)*cutOverlayTileCountY*viewCount;
        if(!capacity||capacity>UINT32_MAX/4u)Fail("Projectile tile capacity");
        cutOverlayMask.resource=Temporary(Texture(renderWidth,renderHeight,DXGI_FORMAT_R32_UINT,
            1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON);
        cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
        cutOverlaySRV=TextureSrv(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,{},true);
        cutOverlayUAV=TextureUav(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,0,{},true);
        auto allocate=[&](ComPtr<ID3D12Resource>& r,uint32_t bytes){
            r=Temporary(Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                        D3D12_RESOURCE_STATE_COMMON);
        };
        allocate(cutOverlayTileLastSeen,uint32_t(capacity)*4u);
        cutOverlayTileLastSeenUAV=UavBuffer(cutOverlayTileLastSeen.Get(),uint32_t(capacity),0);
        for(uint32_t i=0;i<2;++i){
            allocate(cutOverlayActiveTiles[i],uint32_t(capacity)*4u);
            cutOverlayActiveTileUAVs[i]=UavBuffer(cutOverlayActiveTiles[i].Get(),uint32_t(capacity),0);
            allocate(cutOverlayActiveCounts[i],4u);
            cutOverlayActiveCountUAVs[i]=UavBuffer(cutOverlayActiveCounts[i].Get(),1,0);
        }
        allocate(cutOverlayTileDispatchArgs,2u*sizeof(D3D12_DISPATCH_ARGUMENTS));
        cutOverlayTileDispatchArgsUAV=UavBuffer(cutOverlayTileDispatchArgs.Get(),6,0);
        cutOverlayNeedsInitialization=true;
        cutOverlayGeneration=0;
    }
    if(++cutOverlayGeneration==0){
        cutOverlayGeneration=1;
        cutOverlayNeedsInitialization=true;
    }
    const uint32_t currentTiles=cutOverlayGeneration&1u,previousTiles=currentTiles^1u;
    const bool resetOverlay=cutOverlayNeedsInitialization;
    cutOverlayNeedsInitialization=false;
    auto& cf=cutFrames[uint32_t(&f-frames.data())];
    const uint32_t source=cutStepSerial&1u,destination=source^1u;
    struct StepParameters {
        uint32_t source,destination,impacts,control;
        uint32_t scene,count,capacity,mask;
        float deltaTime,maxSegmentLength,pad0,pad1;
        uint32_t spawnMask,left,right,reset;
        CutProjectile spawns[2];
        uint32_t rayMap,instanceRoutes,ranges,vertices;
        uint32_t indices,rayCount,rangeCount,instanceCount;
        uint32_t sourceTriangles,cutTrianglesA,cutTrianglesB,state,cutCount,cutStaticCount,pad5,pad6;
    } step{};
    static_assert(sizeof(StepParameters)==256);
    step.source=Physical(cutProjectileSRVs[source]);
    step.destination=Physical(cutProjectileUAVs[destination]);
    step.impacts=Physical(cf.impactUAV);step.control=Physical(cf.controlUAV);
    step.scene=Physical(f.tlasSRV);step.count=CutProjectileCapacity;
    step.capacity=CutProjectileCapacity;step.mask=0xffu;
    step.deltaTime=std::clamp(float(double(xr.displayPeriod)*1e-9),.00001f,.05f);
    step.maxSegmentLength=1.f;step.reset=cutStepSerial==0?1u:0u;
    step.rayMap=Physical(f.rayMapSRV);step.instanceRoutes=Physical(f.instanceRangesSRV);
    step.ranges=Physical(rangesSRV);step.vertices=Physical(verticesSRV);
    step.indices=Physical(indicesSRV);step.rayCount=f.rayInstanceCount;
    step.rangeCount=uint32_t(meshes.ranges.size());step.instanceCount=f.instanceCount;
    step.sourceTriangles=cutInitialCount?Physical(cutOriginalSRV):Invalid;
    step.cutTrianglesA=cutInitialCount?Physical(cutBufferSRVs[0]):Invalid;
    step.cutTrianglesB=cutInitialCount?Physical(cutBufferSRVs[1]):Invalid;
    step.state=cutInitialCount?Physical(cutStateSRV):Invalid;
    step.cutCount=cutInitialCount;step.cutStaticCount=cutStaticCount;
    for(uint32_t hand=0;hand<2;++hand){
        const auto& shot=xr.shotHands[hand];
        if(!shot.fired)continue;
        const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
        CutProjectile p{};
        p.position=Add(shot.aim.pose.p,Scale(forward,.12f));
        p.velocity=Scale(forward,hand==0?95.f:28.f);
        p.radius=hand==0?.24f:.32f;
        p.acceleration=hand==0?XMFLOAT3{}:XMFLOAT3{0,-9.81f,0};
        p.lifetime=hand==0?3.f:5.f;
        p.id=cutNextId++;if(!cutNextId)cutNextId=1;
        p.kind=hand;p.alive=1;
        step.spawns[hand]=p;step.spawnMask|=1u<<hand;
        if(hand==0)step.left=(p.id-1u)%CutProjectileCapacity;
        else step.right=(p.id-1u)%CutProjectileCapacity;
    }
    struct DrawParameters {
        uint32_t projectiles,maskUav,maskSrv,colorUav;
        uint32_t capacity,width,height,views;
        XMFLOAT4X4 vp[MaxViews];
        XMFLOAT4 laserStart[2],laserEnd[2];
        uint32_t tileSeenUav,tileActiveUav,tilePreviousUav,tileCountUav;
        uint32_t tileArgsUav,tileGeneration,tileCountX,tileCountY;
    } draw{};
    draw.projectiles=Physical(cutProjectileSRVs[destination]);
    draw.maskUav=Physical(cutOverlayUAV);draw.maskSrv=Physical(cutOverlaySRV);
    draw.colorUav=Physical(hdrResolvedUAV);
    draw.capacity=CutProjectileCapacity;draw.width=renderWidth;
    draw.height=renderHeight;draw.views=xr.viewCount;
    draw.tileSeenUav=Physical(cutOverlayTileLastSeenUAV);
    draw.tileActiveUav=Physical(cutOverlayActiveTileUAVs[currentTiles]);
    draw.tilePreviousUav=Physical(cutOverlayActiveTileUAVs[previousTiles]);
    draw.tileCountUav=Physical(cutOverlayActiveCountUAVs[currentTiles]);
    draw.tileArgsUav=Physical(cutOverlayTileDispatchArgsUAV);
    draw.tileGeneration=cutOverlayGeneration;
    draw.tileCountX=cutOverlayTileCountX;draw.tileCountY=cutOverlayTileCountY;
    for(uint32_t view=0;view<xr.viewCount;++view)draw.vp[view]=xr.vp[view];
    for(uint32_t hand=0;hand<2;++hand){
        const auto& shot=xr.shotHands[hand];
        if(!shot.aim.tracked)continue;
        const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
        auto begin=Add(shot.aim.pose.p,Scale(forward,.12f));
        auto end=Add(begin,Scale(forward,24.f));
        draw.laserStart[hand]={begin.x,begin.y,begin.z,1.f};
        draw.laserEnd[hand]={end.x,end.y,end.z,1.f};
    }
    auto write=[&](const void* value,UINT64 size)->D3D12_GPU_VIRTUAL_ADDRESS{
        const UINT64 offset=Reserve(f,size);
        std::memcpy(f.uploadGeneration->mapped+offset,value,size);
        return f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
    };
    CutDispatch result{};
    result.source=source;result.destination=destination;
    result.overlayCurrent=currentTiles;result.overlayPrevious=previousTiles;
    result.overlayReset=resetOverlay;
    result.step=write(&step,sizeof(step));result.overlay=write(&draw,sizeof(draw));
    if(cutInitialCount){
        struct CutParameters{
            uint32_t original,a,b,aWrite,bWrite,stateRead,stateWrite,impacts;
            uint32_t impactControl,outputControl,dispatchArgs,drawArgs;
            uint32_t originalCount,capacity,planeCount,pad;
        } config{
            Physical(cutOriginalSRV),Physical(cutBufferSRVs[0]),
            Physical(cutBufferSRVs[1]),Physical(cutBufferUAVs[0]),
            Physical(cutBufferUAVs[1]),Physical(cutStateSRV),
            Physical(cutStateUAV),Physical(cf.impactSRV),
            Physical(cf.controlUAV),Physical(cutControlUAV),
            Physical(cutDispatchArgumentsUAV),Physical(cutDrawArgumentsUAV),
            cutInitialCount,cutCapacity,26u,0u
        };
        result.begin=result.clip=result.commit=write(&config,sizeof(config));
    }
    return result;
}
void Graphics::Impl::ResetCutResources(){
    auto release=[&](DescriptorHandle& h){if(h.Valid())ReleaseDescriptor(h);h={};};
    auto forget=[&](ComPtr<ID3D12Resource>& r){passGraph.Forget(r.Get());r.Reset();};
    for(auto& h:cutProjectileSRVs)release(h);
    for(auto& h:cutProjectileUAVs)release(h);
    for(auto& r:cutProjectiles)forget(r);
    for(auto& cf:cutFrames){
        release(cf.impactSRV);release(cf.impactUAV);release(cf.controlUAV);
        forget(cf.impacts);forget(cf.control);
    }
    release(cutOriginalSRV);
    for(auto& h:cutBufferSRVs)release(h);
    for(auto& h:cutBufferUAVs)release(h);
    release(cutStateSRV);release(cutStateUAV);release(cutControlUAV);
    release(cutDispatchArgumentsUAV);release(cutDrawArgumentsUAV);
    forget(cutOriginal);for(auto& r:cutBuffers)forget(r);
    forget(cutState);forget(cutControl);forget(cutDispatchArguments);
    forget(cutDrawArguments);
    release(cutOverlaySRV);release(cutOverlayUAV);
    release(cutOverlayTileLastSeenUAV);release(cutOverlayTileDispatchArgsUAV);
    for(auto& h:cutOverlayActiveTileUAVs)release(h);
    for(auto& h:cutOverlayActiveCountUAVs)release(h);
    forget(cutOverlayMask.resource);cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
    forget(cutOverlayTileLastSeen);forget(cutOverlayTileDispatchArgs);
    for(auto& r:cutOverlayActiveTiles)forget(r);
    for(auto& r:cutOverlayActiveCounts)forget(r);
    cutOverlayTileCountX=cutOverlayTileCountY=cutOverlayGeneration=0;
    cutOverlayNeedsInitialization=true;
    cutInitialCount=cutCapacity=cutStaticCount=0;
    cutStepSerial=0;cutNextId=1;cutFlightRemaining=0.f;cutActive=false;
}
}
