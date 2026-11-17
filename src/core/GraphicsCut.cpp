#include "GraphicsInternal.h"
#include <utility>

namespace OASIS {
    void Graphics::Impl::EnsureCutPipelines(){
        if(cutPipelines[0])return;
        constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
            L"StepProjectiles",L"ResolveImpact",L"BuildImpactVolume",L"CutSurface",
            L"FindCutSegments",L"PartitionCap",L"EmitCap",L"MakeCutDispatch",
            L"PackCutGeometry",L"FinalizeCutRange",L"StampProjectiles",L"CompositeProjectiles",
            L"FinalizeCutGeneration",L"ScheduleCutDispatches",L"StampAimLasers"
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
        if(!cutOverlayMask.resource){
            cutOverlayMask.resource=Temporary(Texture(renderWidth,renderHeight,DXGI_FORMAT_R32_UINT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
            cutOverlaySRV=TextureSrv(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,{},true);
            cutOverlayUAV=TextureUav(cutOverlayMask.resource.Get(),DXGI_FORMAT_R32_UINT,0,{},true);
        }
        const uint32_t frameSlot=uint32_t(&f-frames.data());
        auto& cf=cutFrames[frameSlot];
        if(PublishCutStages&&triangleSolidsSRV.Valid()){
            EnsureCutScratch(cutMesh,f.instanceCount);
            EnsureCutScratch(cutMeshAlternate,f.instanceCount);
        }
        if(PublishCutStages&&cutMesh.maxTriangles&&!cutDrawArguments){
            cutDrawArguments=Temporary(
                Buffer(uint64_t(cutMesh.pageCount)*16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COMMON);
            cutDrawArgumentsUAV=UavBuffer(cutDrawArguments.Get(),cutMesh.pageCount*4u,0);
        }
        const uint32_t source=cutStepSerial&1u,destination=source^1u;
        struct StepParameters {
            uint32_t source,destination,impacts,control;
            uint32_t scene,count,capacity,mask;
            float deltaTime,maxSegmentLength,pad0,pad1;
            uint32_t spawnMask,left,right,reset;
            CutProjectile spawns[2];
            uint32_t rayMap,instanceRoutes,ranges,vertices;
            uint32_t indices,rayCount,rangeCount,instanceCount;
            uint32_t generation,packedVertices,packedIndices,packedVerticesAlternate;
            uint32_t packedIndicesAlternate,pageCount,pageTriangles,pad7;
        } step{};
        static_assert(sizeof(StepParameters)==256);
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
        step.rangeCount=uint32_t(meshes.ranges.size());step.instanceCount=f.instanceCount;
        if(PublishCutStages&&cutMesh.maxTriangles){
            step.generation=Physical(cutMesh.Scratch(CutScratchId::Generation).srv);
            step.packedVertices=Physical(cutMesh.Scratch(CutScratchId::PackedVertices).srv);
            step.packedIndices=Physical(cutMesh.Scratch(CutScratchId::PackedIndices).srv);
            step.packedVerticesAlternate=Physical(cutMeshAlternate.Scratch(CutScratchId::PackedVertices).srv);
            step.packedIndicesAlternate=Physical(cutMeshAlternate.Scratch(CutScratchId::PackedIndices).srv);
            step.pageCount=cutMesh.pageCount;step.pageTriangles=cutMesh.pageTriangles;
        }else{
            step.generation=step.packedVertices=step.packedIndices=Invalid;
            step.packedVerticesAlternate=step.packedIndicesAlternate=Invalid;
            step.pageCount=step.pageTriangles=0;
        }
        for(uint32_t hand=0;hand<2;hand++){
            const auto& shot=xr.shotHands[hand];
            if(!shot.fired)continue;
            const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
            CutProjectile p{};
            p.position=Add(shot.aim.pose.p,Scale(forward,.12f));
            p.velocity=Scale(forward,hand==0?95.f:28.f);
            p.radius=hand==0?.24f:.32f;
            p.acceleration=hand==0?XMFLOAT3{}:XMFLOAT3{0,-9.81f,0};
            p.lifetime=hand==0?3.f:5.f;
            p.id=cutNextId++;if(cutNextId==0)cutNextId=1;
            p.kind=hand;p.alive=1;
            step.spawns[hand]=p;step.spawnMask|=1u<<hand;
            if(hand==0)step.left=(p.id-1u)%CutProjectileCapacity;
            else step.right=(p.id-1u)%CutProjectileCapacity;
        }
        // All cutting kernels select the last validated source on the GPU.
        // A cut therefore never reads the buffer it is currently replacing.
        struct SourceDescriptors {
            XMUINT4 original,first,second,control;
        };
        const auto src=[&](CutFrame& frame)->XMUINT4{
            if(!frame.maxTriangles)return {Invalid,Invalid,Invalid,Invalid};
            return {
                Physical(frame.Scratch(CutScratchId::PackedVertices).srv),
                Physical(frame.Scratch(CutScratchId::PackedIndices).srv),
                Physical(frame.Scratch(CutScratchId::PackedRanges).srv),
                Physical(frame.Scratch(CutScratchId::PackedSolids).srv)
            };
        };
        SourceDescriptors sourceDescriptors{
            {Physical(verticesSRV),Physical(indicesSRV),
             Physical(rangesSRV),Physical(triangleSolidsSRV)},
            src(cutMesh),src(cutMeshAlternate),
            {cutMesh.maxTriangles?
                Physical(cutMesh.Scratch(CutScratchId::Generation).srv):Invalid,
             cutMesh.pageCount,cutMesh.pageTriangles,cutMesh.pageMeshlets}
        };
        struct ResolveParameters {
            uint32_t impacts,rayMap,instances,ranges;
            uint32_t solids,resolved,impactCount,rayCount;
            uint32_t rangeCount,solidCount,worldCount,control;
            uint32_t published,publishedCount,sourceVertices,sourceIndices;
            SourceDescriptors source;
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
        resolve.source=sourceDescriptors;
        struct DrawParameters {
            uint32_t projectiles,maskUav,maskSrv,colorUav;
            uint32_t capacity,width,height,views;
            XMFLOAT4X4 vp[MaxViews];
            XMFLOAT4 laserStart[2],laserEnd[2];
        } draw{};
        draw.projectiles=Physical(cutProjectileSRVs[destination]);
        draw.maskUav=Physical(cutOverlayUAV);draw.maskSrv=Physical(cutOverlaySRV);
        draw.colorUav=Physical(hdrResolvedUAV);
        draw.capacity=CutProjectileCapacity;draw.width=renderWidth;draw.height=renderHeight;draw.views=xr.viewCount;
        for(uint32_t i=0;i<xr.viewCount;i++)draw.vp[i]=xr.vp[i];
        for(uint32_t hand=0;hand<2;hand++){
            const auto& shot=xr.shotHands[hand];
            if(!shot.aim.tracked)continue;
            const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
            const auto begin=Add(shot.aim.pose.p,Scale(forward,.12f));
            const auto end=Add(begin,Scale(forward,24.f));
            draw.laserStart[hand]={begin.x,begin.y,begin.z,1.f};
            draw.laserEnd[hand]={end.x,end.y,end.z,1.f};
        }
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
            SourceDescriptors source;
        } surface{
            Physical(verticesSRV),Physical(indicesSRV),resource(CutScratchId::Planes,false),
            resource(CutScratchId::SurfaceVertices,true),
            resource(CutScratchId::SurfaceIndices,true),resource(CutScratchId::SurfaceControl,true),
            Physical(rangesSRV),cutMesh.triangleCapacity,
            80,cutMesh.triangleCapacity,Physical(triangleSolidsSRV),
            resource(CutScratchId::SurfaceSolids,true),
            Physical(cf.resolvedSRV),0,0,0,sourceDescriptors
        };
        dispatch.surface=write(&surface,sizeof(surface));
        struct SegmentParams {
            uint32_t vertices,indices,solids,faces;
            uint32_t segments,control,ranges,maximum;
            uint32_t facesCount,capacity,impact,index;
            SourceDescriptors source;
        } segments{
            Physical(verticesSRV),Physical(indicesSRV),Physical(triangleSolidsSRV),
            resource(CutScratchId::Faces,false),
            resource(CutScratchId::Segments,true),
            resource(CutScratchId::SegmentControl,true),
            Physical(rangesSRV),cutMesh.triangleCapacity,
            80,cutMesh.segmentCapacity,Physical(cf.resolvedSRV),0,
            sourceDescriptors
        };
        dispatch.segments=write(&segments,sizeof(segments));
        struct CapParams {
            uint32_t faces,segments,segmentControl,polygons;
            uint32_t vertices,heads,control,sourceVertices;
            uint32_t sourceIndices,sourceSolids,outputVertices,outputIndices;
            uint32_t faceCount,polygonCapacity,vertexCapacity,triangleCapacity;
            uint32_t sourceTriangles,outputSolids,impact,index;
            uint32_t surfaceControl,sourceRanges,pad1,pad2;
            SourceDescriptors source;
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
            Physical(rangesSRV),0,0,sourceDescriptors
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
            XMUINT4 destinationA0,destinationA1,destinationB0,destinationB1;
            XMUINT4 generationAndPadding;
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
        const auto packGeometry=[&](CutFrame& cf)->std::pair<XMUINT4,XMUINT4>{
            auto uav=[&](CutScratchId id){return Physical(cf.Scratch(id).uav);};
            return {
                XMUINT4{uav(CutScratchId::PackedVertices),uav(CutScratchId::PackedIndices),
                        uav(CutScratchId::PackedMeshlets),uav(CutScratchId::PackedRemap)},
                XMUINT4{uav(CutScratchId::PackedTriangles),uav(CutScratchId::PackedRanges),
                        uav(CutScratchId::PackedRegions),uav(CutScratchId::PackedSolids)}
            };
        };
        const auto packedA=packGeometry(cutMesh),packedB=packGeometry(cutMeshAlternate);
        pack.destinationA0=packedA.first;pack.destinationA1=packedA.second;
        pack.destinationB0=packedB.first;pack.destinationB1=packedB.second;
        pack.generationAndPadding={sourceDescriptors.control.x,
            cutMesh.pageCount,cutMesh.pageTriangles,cutMesh.pageMeshlets};
        dispatch.pack=write(&pack,sizeof(pack));
        struct GenerationParams {
            uint32_t impact,surfaceControl,segmentControl,capControl;
            uint32_t generation,capacity,projectileImpact,impactControl;
            uint32_t worldInstances,publishedInstances,worldCount,rangeCount;
            uint32_t ranges,totalCount,drawArguments,pageCount;
            uint32_t pageTriangles,pageMeshlets,padDraw0,padDraw1;
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
            Physical(rangesSRV),f.instanceCount,
            Physical(cutDrawArgumentsUAV),cutMesh.pageCount,
            cutMesh.pageTriangles,cutMesh.pageMeshlets,0,0
        };
        dispatch.generation=write(&generation,sizeof(generation));
        struct ScheduleParams {
            uint32_t resolved,impactControl,ranges,args;
            uint32_t maxTriangles,faceCount,pad0,pad1;
            SourceDescriptors source;
        } schedule{
            Physical(cf.resolvedSRV),Physical(cf.controlUAV),
            Physical(rangesSRV),resource(CutScratchId::IndirectArgs,true),
            cutMesh.triangleCapacity,80,resource(CutScratchId::Generation,false),0,
            sourceDescriptors
        };
        dispatch.schedule=write(&schedule,sizeof(schedule));
        return dispatch;
    }

    void Graphics::Impl::EnsureCutScratch(CutFrame& cf,uint32_t instanceCount){
        if(cf.maxTriangles)return;
        uint32_t maximum=0;
        for(const auto& range:meshes.ranges)maximum=std::max(maximum,range.indexCount/3u);
        if(!maximum)return;
        cf.maxTriangles=maximum;
        // Separate persistent pages per raster instance. The previous
        // one-entry generation silently rejected cuts on every other region.
        cf.pageCount=std::min(std::max(1u,instanceCount),MaxCutPages);
        cf.pageTriangles=std::max(8192u,GrowCapacity(0,uint64_t(maximum)*4u));
        cf.pageMeshlets=(cf.pageTriangles+20u)/21u;
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
        const uint64_t pages=cf.pageCount,triangles=uint64_t(cf.pageTriangles)*pages;
        const uint64_t meshlets=uint64_t(cf.pageMeshlets)*pages;
        allocate(CutScratchId::PackedVertices,triangles*3,sizeof(Vertex),true,true);
        allocate(CutScratchId::PackedIndices,triangles*3,4,true,true);
        allocate(CutScratchId::PackedMeshlets,meshlets,sizeof(Meshlet),true,true);
        allocate(CutScratchId::PackedRemap,triangles*3,4,true,true);
        allocate(CutScratchId::PackedTriangles,triangles,12,true,true);
        allocate(CutScratchId::PackedRanges,pages,sizeof(MeshRange),true,true);
        allocate(CutScratchId::PackedRegions,meshlets,sizeof(MeshletRegion),false,true);
        allocate(CutScratchId::PackedSolids,triangles,4,true,true);
        allocate(CutScratchId::Generation,pages,64,true,true);
        if(&cf==&cutMesh)
            cutGenerationClearUAV=UavBuffer(cf.Scratch(CutScratchId::Generation).resource.Get(),cf.pageCount*16,0);
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
            cf.pageCount=cf.pageTriangles=cf.pageMeshlets=0;
        }
        for(auto* cf:{&cutMesh,&cutMeshAlternate}){
            for(auto& scratch:cf->scratch){
                release(scratch.srv);release(scratch.uav);forget(scratch.resource);
            }
            cf->maxTriangles=cf->triangleCapacity=cf->segmentCapacity=
                cf->polygonCapacity=cf->capVertexCapacity=0;
            cf->pageCount=cf->pageTriangles=cf->pageMeshlets=0;
        }
        release(cutGenerationClearUAV);
        release(cutDrawArgumentsUAV);forget(cutDrawArguments);
        release(cutOverlaySRV);release(cutOverlayUAV);
        forget(cutOverlayMask.resource);cutOverlayMask.layout=D3D12_BARRIER_LAYOUT_COMMON;
        cutActive=false;cutFlightRemaining=0;cutStepSerial=0;cutNextId=1;
        cutGenerationInitialized=false;
    }

}
