#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::EnsureCutPipelines(){
        if(cutPipelines[0])return;
        constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
            L"StepProjectiles",L"ResolveImpact",L"BuildImpactVolume",L"CutSurface",
            L"FindCutSegments",L"PartitionCap",L"EmitCap",L"MakeCutDispatch",
            L"PackCutGeometry",L"FinalizeCutRange",L"StampProjectiles",L"CompositeProjectiles",
            L"FinalizeCutGeneration",L"ScheduleCutDispatches",L"StoreFirstCutHole"
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
                cf.resolvedSRV=Srv(cf.resolved.Get(),CutProjectileCapacity,sizeof(CutResolved));
                cf.resolvedUAV=UavBuffer(cf.resolved.Get(),CutProjectileCapacity,sizeof(CutResolved));
            }
        }
        if(!cutHole){
            cutHole=Temporary(Buffer(sizeof(CutHole),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            cutHoleSRV=Srv(cutHole.Get(),1,sizeof(CutHole));
            cutHoleUAV=UavBuffer(cutHole.Get(),1,sizeof(CutHole));
            cutHoleClearUAV=UavBuffer(cutHole.Get(),8,0);
            cutHoleCleared=false;
        }
        if(!cutOverlayMask.resource){
            cutOverlayMask.resource=Temporary(Texture(renderWidth,renderHeight,DXGI_FORMAT_R32_UINT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
            cutOverlaySRV=TextureSrv(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,{},true);
            cutOverlayUAV=TextureUav(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,0,{},true);
        }
        const uint32_t frameSlot=uint32_t(&f-frames.data());
        auto& cf=cutFrames[frameSlot];
        if(RunUnpublishedCutStages&&triangleSolidsSRV.Valid())EnsureCutScratch(cutMesh);
        const uint32_t source=cutStepSerial&1u,destination=source^1u;
        struct StepParameters {
            uint32_t source,destination,impacts,control;
            uint32_t scene,count,capacity,mask;
            float deltaTime,maxSegmentLength,pad0,pad1;
            uint32_t spawnMask,left,right,reset;
            CutProjectile spawns[2];
            uint32_t rayMap,instanceRoutes,ranges,vertices;
            uint32_t indices,rayCount,rangeCount,pad2;
        } step{};
        static_assert(sizeof(StepParameters)==224);
        step.source=Physical(cutProjectileSRVs[source]);
        step.destination=Physical(cutProjectileUAVs[destination]);
        step.impacts=Physical(cf.impactUAV);
        step.control=Physical(cf.controlUAV);
        step.scene=Physical(f.tlasSRV);step.count=CutProjectileCapacity;step.capacity=CutProjectileCapacity;step.mask=0xffu;
        step.deltaTime=std::clamp(float(double(xr.displayPeriod)*1e-9),.00001f,.05f);
        step.maxSegmentLength=1.f;step.reset=cutStepSerial==0?1u:0u;
        step.rayMap=Physical(f.rayMapSRV);step.instanceRoutes=Physical(f.instanceRangesSRV);
        step.ranges=Physical(rangesSRV);step.vertices=Physical(verticesSRV);
        step.indices=Physical(indicesSRV);step.rayCount=f.rayInstanceCount;
        step.rangeCount=uint32_t(meshes.ranges.size());
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
            uint32_t published,publishedCount,sourceVertices,sourceIndices;
        } resolve{};
        resolve.impacts=Physical(cf.impactSRV);resolve.rayMap=Physical(f.rayMapSRV);
        resolve.instances=Physical(f.instanceRangesSRV);resolve.ranges=Physical(rangesSRV);
        resolve.solids=triangleSolidsSRV.Valid()?Physical(triangleSolidsSRV):Invalid;
        resolve.resolved=Physical(cf.resolvedUAV);resolve.impactCount=CutProjectileCapacity;
        resolve.rayCount=f.rayInstanceCount;resolve.rangeCount=uint32_t(meshes.ranges.size());
        resolve.solidCount=uint32_t(meshes.indices.size()/3);resolve.worldCount=f.cpuCount;
        resolve.control=Physical(cf.controlUAV);
        resolve.published=Physical(f.publishedInstanceSRV);
        resolve.publishedCount=f.instanceCount-f.cpuCount;
        resolve.sourceVertices=Physical(verticesSRV);
        resolve.sourceIndices=Physical(indicesSRV);
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
        CutDispatch dispatch{};
        dispatch.step=write(&step,sizeof(step));
        dispatch.resolve=write(&resolve,sizeof(resolve));
        dispatch.overlay=write(&draw,sizeof(draw));
        dispatch.source=source;dispatch.destination=destination;
        struct HoleParameters{
            uint32_t impacts,control,rayMap,hole;
            uint32_t rayCount,instanceCount,pad0,pad1;
        } hole{
            Physical(cf.impactSRV),Physical(cf.controlUAV),
            Physical(f.rayMapSRV),Physical(cutHoleUAV),
            f.rayInstanceCount,f.instanceCount,0,0
        };
        dispatch.hole=write(&hole,sizeof(hole));
        if(!cutMesh.maxTriangles)return dispatch;
        const auto resource=[&](CutScratchId id,bool writable)->uint32_t{
            const auto& scratch=cutMesh.Scratch(id);
            return Physical(writable?scratch.uav:scratch.srv);
        };
        struct VolumeParams{uint32_t impact,faces,planes,index;};
        VolumeParams volume{Physical(cf.resolvedSRV),
            resource(CutScratchId::Faces,true),resource(CutScratchId::Planes,true),0};
        dispatch.volume=write(&volume,sizeof(volume));
        struct SurfaceParams {
            uint32_t vertices,indices,planes,outputVertices;
            uint32_t outputIndices,control,ranges,maximum;
            uint32_t planesCount,capacity,solids,outputSolids;
            uint32_t impact,index,pad0,pad1;
        } surface{
            Physical(verticesSRV),Physical(indicesSRV),resource(CutScratchId::Planes,false),
            resource(CutScratchId::SurfaceVertices,true),
            resource(CutScratchId::SurfaceIndices,true),resource(CutScratchId::SurfaceControl,true),
            Physical(rangesSRV),cutMesh.maxTriangles,
            80,cutMesh.triangleCapacity,Physical(triangleSolidsSRV),
            resource(CutScratchId::SurfaceSolids,true),
            Physical(cf.resolvedSRV),0,0,0
        };
        dispatch.surface=write(&surface,sizeof(surface));
        struct SegmentParams {
            uint32_t vertices,indices,solids,faces;
            uint32_t segments,control,ranges,maximum;
            uint32_t facesCount,capacity,impact,index;
        } segments{
            Physical(verticesSRV),Physical(indicesSRV),Physical(triangleSolidsSRV),
            resource(CutScratchId::Faces,false),
            resource(CutScratchId::Segments,true),
            resource(CutScratchId::SegmentControl,true),
            Physical(rangesSRV),cutMesh.maxTriangles,
            80,cutMesh.segmentCapacity,Physical(cf.resolvedSRV),0
        };
        dispatch.segments=write(&segments,sizeof(segments));
        struct CapParams {
            uint32_t faces,segments,segmentControl,polygons;
            uint32_t vertices,heads,control,sourceVertices;
            uint32_t sourceIndices,sourceSolids,outputVertices,outputIndices;
            uint32_t faceCount,polygonCapacity,vertexCapacity,triangleCapacity;
            uint32_t sourceTriangles,outputSolids,impact,index;
            uint32_t surfaceControl,sourceRanges,pad1,pad2;
        } cap{
            resource(CutScratchId::Faces,false),resource(CutScratchId::Segments,false),
            resource(CutScratchId::SegmentControl,true),resource(CutScratchId::CapPolygons,true),
            resource(CutScratchId::CapVertices,true),resource(CutScratchId::CapHeads,true),
            resource(CutScratchId::CapControl,true),Physical(verticesSRV),
            Physical(indicesSRV),Physical(triangleSolidsSRV),
            resource(CutScratchId::SurfaceVertices,true),resource(CutScratchId::SurfaceIndices,true),
            80,cutMesh.polygonCapacity,cutMesh.capVertexCapacity,cutMesh.triangleCapacity,
            uint32_t(meshes.indices.size()/3),resource(CutScratchId::SurfaceSolids,true),
            Physical(cf.resolvedSRV),0,resource(CutScratchId::SurfaceControl,true),
            Physical(rangesSRV),0,0
        };
        dispatch.cap=write(&cap,sizeof(cap));
        struct IndirectParams {
            uint32_t control,args,countOffset,overflowOffset;
            uint32_t ambiguityOffset,threads,outputOffset,pad;
        };
        IndirectParams indirectCap{
            resource(CutScratchId::CapControl,true),resource(CutScratchId::IndirectArgs,true),
            0,12,16,64,36,resource(CutScratchId::SurfaceControl,true)
        };
        IndirectParams indirectPack{
            resource(CutScratchId::SurfaceControl,true),resource(CutScratchId::IndirectArgs,true),
            0,4,8,64,48,resource(CutScratchId::CapControl,true)
        };
        dispatch.capIndirect=write(&indirectCap,sizeof(indirectCap));
        dispatch.packIndirect=write(&indirectPack,sizeof(indirectPack));
        struct PackParams {
            uint32_t source,control,vertices,indices;
            uint32_t meshlets,remap,triangles,ranges;
            uint32_t regions,countOffset,overflowOffset,ambiguityOffset;
            uint32_t vertexBase,indexBase,meshletBase,remapBase;
            uint32_t triangleBase,rangeIndex,triangleCapacity,solids;
            uint32_t stagingSolids,sourceRanges,impact,index;
            XMFLOAT4 unusedBounds;
        } pack{
            resource(CutScratchId::SurfaceVertices,false),
            resource(CutScratchId::SurfaceControl,true),
            resource(CutScratchId::PackedVertices,true),
            resource(CutScratchId::PackedIndices,true),
            resource(CutScratchId::PackedMeshlets,true),
            resource(CutScratchId::PackedRemap,true),
            resource(CutScratchId::PackedTriangles,true),
            resource(CutScratchId::PackedRanges,true),
            resource(CutScratchId::PackedRegions,true),
            0,4,8,
            0,0,0,0,
            0,0,cutMesh.triangleCapacity,
            resource(CutScratchId::PackedSolids,true),
            resource(CutScratchId::SurfaceSolids,false),
            Physical(rangesSRV),Physical(cf.resolvedSRV),0,{}
        };
        dispatch.pack=write(&pack,sizeof(pack));
        struct GenerationParams {
            uint32_t impact,surfaceControl,segmentControl,capControl;
            uint32_t generation,capacity,projectileImpact,impactControl;
            uint32_t worldInstances,publishedInstances,worldCount,rangeCount;
            uint32_t ranges,totalCount,pad0,pad1;
        } generation{
            Physical(cf.resolvedSRV),
            resource(CutScratchId::SurfaceControl,true),
            resource(CutScratchId::SegmentControl,true),
            resource(CutScratchId::CapControl,true),
            resource(CutScratchId::Generation,true),
            cutMesh.triangleCapacity,
            Physical(cf.impactSRV),Physical(cf.controlUAV),
            Physical(f.instanceRangesSRV),Physical(f.publishedInstanceSRV),
            f.cpuCount,uint32_t(meshes.ranges.size()),
            Physical(rangesSRV),f.instanceCount,0,0
        };
        dispatch.generation=write(&generation,sizeof(generation));
        struct ScheduleParams {
            uint32_t resolved,impactControl,ranges,args;
            uint32_t maxTriangles,faceCount,pad0,pad1;
        } schedule{
            Physical(cf.resolvedSRV),Physical(cf.controlUAV),
            Physical(rangesSRV),resource(CutScratchId::IndirectArgs,true),
            cutMesh.maxTriangles,80,resource(CutScratchId::Generation,false),0
        };
        dispatch.schedule=write(&schedule,sizeof(schedule));
        return dispatch;
    }

    void Graphics::Impl::EnsureCutScratch(CutFrame& cf){
        if(cf.maxTriangles)return;
        uint32_t maximum=0;
        for(const auto& range:meshes.ranges)maximum=std::max(maximum,range.indexCount/3u);
        if(!maximum)return;
        cf.maxTriangles=maximum;
        // Each impact is processed in its own staging generation. A lack of
        // capacity marks that generation invalid; it never corrupts live meshes.
        cf.triangleCapacity=GrowCapacity(0,uint64_t(maximum)*32u+2048u);
        cf.segmentCapacity=GrowCapacity(0,uint64_t(maximum)*80u);
        cf.polygonCapacity=GrowCapacity(0,uint64_t(cf.segmentCapacity)*4u);
        cf.capVertexCapacity=GrowCapacity(0,uint64_t(cf.polygonCapacity)*8u);
        auto allocate=[&](CutScratchId id,uint64_t elements,uint32_t stride,bool read,bool write){
            if(!elements||elements>UINT32_MAX)Fail("Cut GPU scratch address space");
            auto& target=cf.Scratch(id);
            target.resource=Temporary(Buffer(elements*(stride?stride:4u),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            if(read){
                if(!stride)Fail("Raw scratch views must use the UAV descriptor");
                target.srv=Srv(target.resource.Get(),uint32_t(elements),stride);
            }
            if(write)target.uav=UavBuffer(target.resource.Get(),uint32_t(elements),stride);
        };
        allocate(CutScratchId::Faces,80,48,true,true);
        allocate(CutScratchId::Planes,80,16,true,true);
        allocate(CutScratchId::Segments,cf.segmentCapacity,32,true,true);
        allocate(CutScratchId::SegmentControl,4,0,false,true);
        allocate(CutScratchId::SurfaceVertices,uint64_t(cf.triangleCapacity)*3,sizeof(Vertex),true,true);
        allocate(CutScratchId::SurfaceIndices,uint64_t(cf.triangleCapacity)*3,4,false,true);
        allocate(CutScratchId::SurfaceSolids,cf.triangleCapacity,4,true,true);
        allocate(CutScratchId::SurfaceControl,5,0,false,true);
        allocate(CutScratchId::CapPolygons,cf.polygonCapacity,32,false,true);
        allocate(CutScratchId::CapVertices,cf.capVertexCapacity,12,false,true);
        allocate(CutScratchId::CapHeads,80,4,false,true);
        allocate(CutScratchId::CapControl,8,0,false,true);
        allocate(CutScratchId::IndirectArgs,15,0,false,true);
        allocate(CutScratchId::PackedVertices,uint64_t(cf.triangleCapacity)*3,sizeof(Vertex),true,true);
        allocate(CutScratchId::PackedIndices,uint64_t(cf.triangleCapacity)*3,4,false,true);
        allocate(CutScratchId::PackedMeshlets,(cf.triangleCapacity+20u)/21u,sizeof(Meshlet),true,true);
        allocate(CutScratchId::PackedRemap,uint64_t(cf.triangleCapacity)*3,4,true,true);
        allocate(CutScratchId::PackedTriangles,cf.triangleCapacity,12,true,true);
        allocate(CutScratchId::PackedRanges,1,sizeof(MeshRange),false,true);
        allocate(CutScratchId::PackedRegions,(cf.triangleCapacity+20u)/21u,sizeof(MeshletRegion),false,true);
        allocate(CutScratchId::PackedSolids,cf.triangleCapacity,4,false,true);
        allocate(CutScratchId::Generation,1,64,true,true);
        cutGenerationClearUAV=UavBuffer(cf.Scratch(CutScratchId::Generation).resource.Get(),16,0);
    }

    void Graphics::Impl::ResetCutResources(){
        auto release=[&](DescriptorHandle& h){if(h.Valid())ReleaseDescriptor(h);};
        auto forget=[&](ComPtr<ID3D12Resource>& resource){passGraph.Forget(resource.Get());resource.Reset();};
        for(auto& h:cutProjectileSRVs)release(h);
        for(auto& h:cutProjectileUAVs)release(h);
        for(auto& resource:cutProjectiles)forget(resource);
        for(auto& cf:cutFrames){
            release(cf.impactSRV);release(cf.impactUAV);release(cf.controlUAV);release(cf.resolvedSRV);release(cf.resolvedUAV);
            forget(cf.impacts);forget(cf.control);forget(cf.resolved);
            for(auto& scratch:cf.scratch){
                release(scratch.srv);release(scratch.uav);forget(scratch.resource);
            }
            cf.maxTriangles=cf.triangleCapacity=cf.segmentCapacity=
                cf.polygonCapacity=cf.capVertexCapacity=0;
        }
        for(auto& scratch:cutMesh.scratch){
            release(scratch.srv);release(scratch.uav);forget(scratch.resource);
        }
        release(cutGenerationClearUAV);
        cutMesh.maxTriangles=cutMesh.triangleCapacity=cutMesh.segmentCapacity=
            cutMesh.polygonCapacity=cutMesh.capVertexCapacity=0;
        release(cutHoleSRV);release(cutHoleUAV);release(cutHoleClearUAV);
        forget(cutHole);cutHoleCleared=false;
        release(cutOverlaySRV);release(cutOverlayUAV);
        forget(cutOverlayMask.resource);cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
        cutActive=false;cutStepSerial=0;cutNextId=1;
    }

}
