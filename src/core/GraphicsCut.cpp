#include "GraphicsInternal.h"
#include <cmath>
#include <cstddef>
#include <numeric>

namespace OASIS {
void Graphics::Impl::EnsureCutPipelines(){
    if(cutPipelines[0])return;
    constexpr std::array<const wchar_t*,size_t(CutKernel::Count)> names={
        L"StepProjectiles",L"StampProjectiles",L"CompositeProjectiles",
        L"StampAimLasers",L"ClearMarkedTiles",L"BuildTileDispatchArgs",
        L"BeginCut",L"CopyCutState",L"CountCutTriangles",
        L"ReserveCutBlocks",L"WriteCutTriangles",L"CommitCut"
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
    std::vector<CutSpatialBlock> blocks;
    blocks.reserve((scene.meshes.indices.size()/3+63u)/64u);
    constexpr uint32_t TrianglesPerSpatialBlock=64;
    const float infinity=std::numeric_limits<float>::infinity();
    auto openBlock=[&](uint32_t first,uint32_t instance){
        CutSpatialBlock block{};
        const float hi=std::numeric_limits<float>::max();
        const float lo=std::numeric_limits<float>::lowest();
        block.lower={hi,hi,hi};block.upper={lo,lo,lo};
        block.firstTriangle=first;block.instanceId=instance;
        block.geometryState=uint32_t(CutGeometryState::Source);
        // Source => immutable firstTriangle/triangleCount; active range is unused.
        // Modified => activeFirst/activeCount; Empty => no active surface.
        return block;
    };
    auto closeBlock=[&](CutSpatialBlock& block){
        if(!block.triangleCount)return;
        // Outward ULP expansion: even degenerate world-space triangles remain enclosed.
        block.lower={std::nextafter(block.lower.x,-infinity),
                     std::nextafter(block.lower.y,-infinity),
                     std::nextafter(block.lower.z,-infinity)};
        block.upper={std::nextafter(block.upper.x,infinity),
                     std::nextafter(block.upper.y,infinity),
                     std::nextafter(block.upper.z,infinity)};
        blocks.push_back(block);
    };
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
        CutSpatialBlock block{};
        for(uint32_t i=0;i<range.indexCount;i+=3){
            if(source.size()>=UINT32_MAX/16u)Fail("Cut mesh source too large");
            CutTriangle tri{};
            tri.a=transformed(scene.meshes.indices[range.firstIndex+i]);
            tri.b=transformed(scene.meshes.indices[range.firstIndex+i+1]);
            tri.c=transformed(scene.meshes.indices[range.firstIndex+i+2]);
            tri.material=object.material;tri.instanceId=instance;
            tri.padding0=object.flags;
            // Bounds are accumulated from the exact transformed positions stored
            // in cutOriginal; no local-space meshlet AABB or second transform.
            if(!block.triangleCount)block=openBlock(uint32_t(source.size()),instance);
            for(const XMFLOAT3 p:{tri.a.position,tri.b.position,tri.c.position}){
                if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
                    Fail("Nonfinite cut triangle position");
                block.lower={std::min(block.lower.x,p.x),std::min(block.lower.y,p.y),
                             std::min(block.lower.z,p.z)};
                block.upper={std::max(block.upper.x,p.x),std::max(block.upper.y,p.y),
                             std::max(block.upper.z,p.z)};
            }
            source.push_back(tri);
            if(++block.triangleCount==TrianglesPerSpatialBlock){
                closeBlock(block);
                block={};
            }
        }
        // Scene objects already represent prepared 512-triangle regions.
        // Never merge their tails: a block belongs to exactly one instance.
        closeBlock(block);
    }
    if(source.empty())return;
    // Split block identity/order is independent of the BVH's node ordering.
    // Centroid partitioning reorders only block IDs, never cutOriginal.
    std::vector<uint32_t> blockIds(blocks.size());
    std::iota(blockIds.begin(),blockIds.end(),0u);
    std::vector<XMFLOAT3> centroids;
    centroids.reserve(blocks.size());
    for(const auto& block:blocks)
        centroids.push_back({block.lower.x*.5f+block.upper.x*.5f,
                             block.lower.y*.5f+block.upper.y*.5f,
                             block.lower.z*.5f+block.upper.z*.5f});
    std::vector<CutSpatialNode> nodes;
    nodes.reserve(blocks.size()*2u-1u);
    auto build=[&](auto&& self,uint32_t first,uint32_t end)->uint32_t{
        const uint32_t index=uint32_t(nodes.size());
        nodes.emplace_back();
        if(end-first==1u){
            const uint32_t id=blockIds[first];
            nodes[index]={blocks[id].lower,id,blocks[id].upper,Invalid};
            return index;
        }
        XMFLOAT3 low{std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max(),
                     std::numeric_limits<float>::max()};
        XMFLOAT3 high{std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest()};
        for(uint32_t i=first;i<end;++i){
            const auto& c=centroids[blockIds[i]];
            low={std::min(low.x,c.x),std::min(low.y,c.y),std::min(low.z,c.z)};
            high={std::max(high.x,c.x),std::max(high.y,c.y),std::max(high.z,c.z)};
        }
        const float extent[3]{high.x-low.x,high.y-low.y,high.z-low.z};
        uint32_t axis=extent[1]>extent[0]?1u:0u;
        if(extent[2]>extent[axis])axis=2u;
        const uint32_t middle=first+(end-first)/2u;
        auto coordinate=[&](uint32_t id){
            const auto& c=centroids[id];
            return axis==0?c.x:axis==1?c.y:c.z;
        };
        // The ID tie-breaker makes identical centroids deterministic and
        // guarantees a non-empty, balanced median partition.
        std::nth_element(blockIds.begin()+first,blockIds.begin()+middle,
                         blockIds.begin()+end,[&](uint32_t a,uint32_t b){
            if(coordinate(a)<coordinate(b))return true;
            if(coordinate(b)<coordinate(a))return false;
            return a<b;
        });
        const uint32_t left=self(self,first,middle);
        const uint32_t right=self(self,middle,end);
        const auto& a=nodes[left];const auto& b=nodes[right];
        nodes[index]={
            {std::min(a.lower.x,b.lower.x),std::min(a.lower.y,b.lower.y),
             std::min(a.lower.z,b.lower.z)},
            left,
            {std::max(a.upper.x,b.upper.x),std::max(a.upper.y,b.upper.y),
             std::max(a.upper.z,b.upper.z)},
            right
        };
        return index;
    };
    const uint32_t root=build(build,0u,uint32_t(blockIds.size()));
    if(root!=0||nodes.size()!=blocks.size()*2u-1u)Fail("Cut spatial tree layout");
    if(source.size()>UINT32_MAX/16u)Fail("Cut mesh source too large");
    cutInitialCount=uint32_t(source.size());
    cutCapacity=std::max(65536u,cutInitialCount*16u);
    cutStaticCount=uint32_t(scene.objects.size());
    cutOriginal=UploadVector(source);
    cutOriginalSRV=Srv(cutOriginal.Get(),cutInitialCount,sizeof(CutTriangle));
    // Metadata is UAV-capable from day one for future state/active-range
    // updates. This commit leaves it untouched after the initial upload.
    const UINT64 metadataBytes=UINT64(blocks.size())*sizeof(CutSpatialBlock);
    // Two full metadata tables are initialized identically. The active table
    // is selected by cutState.y on GPU; only the opposite table is rewritten.
    for(uint32_t slot=0;slot<2u;++slot){
        cutSpatialBlocks[slot]=Temporary(Buffer(metadataBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
        auto upload=Host(metadataBytes,D3D12_HEAP_TYPE_UPLOAD);
        void* mapped=nullptr;D3D12_RANGE empty{0,0};
        Hr(upload->Map(0,&empty,&mapped),"cut spatial blocks upload map");
        std::memcpy(mapped,blocks.data(),size_t(metadataBytes));
        upload->Unmap(0,nullptr);
        startup.list->CopyBufferRegion(cutSpatialBlocks[slot].Get(),0,upload.Get(),0,metadataBytes);
        BBarrier(startup.list.Get(),cutSpatialBlocks[slot].Get(),
            D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,
            D3D12_BARRIER_ACCESS_COPY_DEST,
            D3D12_BARRIER_ACCESS_SHADER_RESOURCE|D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        uploads.push_back(upload);
        cutSpatialBlocksSRV[slot]=Srv(cutSpatialBlocks[slot].Get(),
                                      uint32_t(blocks.size()),sizeof(CutSpatialBlock));
        cutSpatialBlocksUAV[slot]=UavBuffer(cutSpatialBlocks[slot].Get(),
                                      uint32_t(blocks.size()),sizeof(CutSpatialBlock));
    }
    cutSpatialNodes=UploadVector(nodes);
    cutSpatialNodesSRV=Srv(cutSpatialNodes.Get(),uint32_t(nodes.size()),sizeof(CutSpatialNode));
    cutSpatialBlockCount=uint32_t(blocks.size());
    cutSpatialNodeCount=uint32_t(nodes.size());
    cutSpatialRoot=root;
    if(cutSpatialBlockCount>(UINT32_MAX-4u)/2u)Fail("Cut free-list capacity");
    cutFreeRangeCapacity=cutSpatialBlockCount*2u+4u;

    // One persistent copy-on-write triangle pool replaces the old two complete
    // cut-soup buffers. No affected block ever overwrites its source range.
    cutPool=Temporary(Buffer(UINT64(cutCapacity)*sizeof(CutTriangle),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
    cutPoolSRV=Srv(cutPool.Get(),cutCapacity,sizeof(CutTriangle));
    cutPoolUAV=UavBuffer(cutPool.Get(),cutCapacity,sizeof(CutTriangle));

    auto allocate=[&](ComPtr<ID3D12Resource>& target,UINT64 bytes){
        target=Temporary(Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                         D3D12_RESOURCE_STATE_COMMON);
    };
    allocate(cutState,16u);
    cutStateSRV=Srv(cutState.Get(),1,sizeof(XMUINT4));
    cutStateUAV=UavBuffer(cutState.Get(),1,sizeof(XMUINT4));
    allocate(cutControl,16u);
    cutControlUAV=UavBuffer(cutControl.Get(),4,0);
    allocate(cutDispatchArguments,sizeof(D3D12_DISPATCH_ARGUMENTS));
    cutDispatchArgumentsUAV=UavBuffer(cutDispatchArguments.Get(),3,0);

    for(uint32_t slot=0;slot<2u;++slot){
        allocate(cutPoolAllocators[slot],16u);
        cutPoolAllocatorsUAV[slot]=UavBuffer(cutPoolAllocators[slot].Get(),4,0);
        allocate(cutFreeRanges[slot],UINT64(cutFreeRangeCapacity)*sizeof(XMUINT2));
        cutFreeRangesUAV[slot]=UavBuffer(cutFreeRanges[slot].Get(),
                                         cutFreeRangeCapacity,sizeof(XMUINT2));
    }
    allocate(cutCandidates,UINT64(cutSpatialBlockCount)*sizeof(uint32_t));
    cutCandidatesUAV=UavBuffer(cutCandidates.Get(),cutSpatialBlockCount,sizeof(uint32_t));
    allocate(cutBlockResults,UINT64(cutSpatialBlockCount)*sizeof(XMUINT4));
    cutBlockResultsUAV=UavBuffer(cutBlockResults.Get(),cutSpatialBlockCount,sizeof(XMUINT4));

    // The pending impact FIFO retains impacts when the pool cannot reserve
    // all required new ranges. No CPU readback or per-impact allocation.
    allocate(cutPendingImpacts,UINT64(CutPendingCapacity)*sizeof(CutImpact));
    cutPendingImpactsUAV=UavBuffer(cutPendingImpacts.Get(),CutPendingCapacity,sizeof(CutImpact));
    allocate(cutPendingControl,16u);
    cutPendingControlUAV=UavBuffer(cutPendingControl.Get(),4,0);

    // One flag per static instance and metadata slot. The rendering shader
    // samples only the component selected by cutState.y.
    allocate(cutInstanceReplaced,UINT64(cutStaticCount)*sizeof(XMUINT2));
    cutInstanceReplacedSRV=Srv(cutInstanceReplaced.Get(),cutStaticCount,sizeof(XMUINT2));
    cutInstanceReplacedUAV=UavBuffer(cutInstanceReplaced.Get(),cutStaticCount,sizeof(XMUINT2));
    std::vector<XMUINT2> noReplacements(cutStaticCount);
    const UINT64 flagBytes=UINT64(noReplacements.size())*sizeof(XMUINT2);
    auto flagUpload=Host(flagBytes,D3D12_HEAP_TYPE_UPLOAD);
    void* flagMap=nullptr;D3D12_RANGE empty{0,0};
    Hr(flagUpload->Map(0,&empty,&flagMap),"cut instance flags upload");
    std::memcpy(flagMap,noReplacements.data(),size_t(flagBytes));
    flagUpload->Unmap(0,nullptr);
    startup.list->CopyBufferRegion(cutInstanceReplaced.Get(),0,flagUpload.Get(),0,flagBytes);
    BBarrier(startup.list.Get(),cutInstanceReplaced.Get(),
        D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,
        D3D12_BARRIER_ACCESS_COPY_DEST,
        D3D12_BARRIER_ACCESS_SHADER_RESOURCE|D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    uploads.push_back(flagUpload);

    // Existing 16-byte signature: root block ID + DispatchMesh(x,y,z).
    // Both tables start empty; modified instances publish their full block
    // command set at the end of an image, never during rasterization.
    const UINT64 commandBytes=UINT64(cutSpatialBlockCount)*sizeof(XMUINT4);
    allocate(cutDrawArguments,commandBytes);
    cutDrawArgumentsUAV=UavBuffer(cutDrawArguments.Get(),
                                   uint32_t(commandBytes/4u),0);
    allocate(cutPreparedDrawArguments,commandBytes);
    cutPreparedDrawArgumentsUAV=UavBuffer(cutPreparedDrawArguments.Get(),
                                   uint32_t(commandBytes/4u),0);
    std::vector<XMUINT4> commands(cutSpatialBlockCount);
    for(uint32_t i=0;i<cutSpatialBlockCount;++i)commands[i]={i,0u,1u,1u};
    auto commandUpload=Host(commandBytes,D3D12_HEAP_TYPE_UPLOAD);
    void* commandMap=nullptr;
    Hr(commandUpload->Map(0,&empty,&commandMap),"cut commands upload");
    std::memcpy(commandMap,commands.data(),size_t(commandBytes));
    commandUpload->Unmap(0,nullptr);
    startup.list->CopyBufferRegion(cutDrawArguments.Get(),0,commandUpload.Get(),0,commandBytes);
    BBarrier(startup.list.Get(),cutDrawArguments.Get(),
        D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
        D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    uploads.push_back(commandUpload);
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
    uploadInit(cutPoolAllocators[0].Get(),{0,0,0,0});
    uploadInit(cutPoolAllocators[1].Get(),{0,0,0,0});
    uploadInit(cutPendingControl.Get(),{0,0,0,0});
}
Graphics::Impl::CutDispatch Graphics::Impl::PrepareCutDispatch(Frame& f,const XRFrame& xr,bool enableOverlay){
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
    if(enableOverlay&&!cutOverlayMask.resource){
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
    if(enableOverlay){
        if(++cutOverlayGeneration==0){
            cutOverlayGeneration=1;
            cutOverlayNeedsInitialization=true;
        }
    }
    const uint32_t currentTiles=cutOverlayGeneration&1u,previousTiles=currentTiles^1u;
    const bool resetOverlay=enableOverlay&&cutOverlayNeedsInitialization;
    if(enableOverlay)cutOverlayNeedsInitialization=false;
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
        uint32_t sourceTriangles,cutTrianglesA,cutTrianglesB,state,cutCount,cutStaticCount,bvhNodes,bvhBlocks;
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
    // The projectile ABI retains 256 bytes. The former two global soup
    // descriptors now carry metadata table 1 and the persistent pool.
    step.cutTrianglesA=cutInitialCount?Physical(cutSpatialBlocksSRV[1]):Invalid;
    step.cutTrianglesB=cutInitialCount?Physical(cutPoolSRV):Invalid;
    step.state=cutInitialCount?Physical(cutStateSRV):Invalid;
    step.cutCount=cutInitialCount;step.cutStaticCount=cutStaticCount;
    // The shader reads both SRVs only while cutState selects intact geometry.
    // These are the two reserved dwords in the existing 256-byte ABI.
    const bool hasCutBvh=cutSpatialRoot==0u&&cutSpatialBlockCount!=0u&&
        cutSpatialNodeCount!=0u&&cutInitialCount!=0u&&
        cutSpatialNodesSRV.Valid()&&cutSpatialBlocksSRV[0].Valid()&&
        cutSpatialBlocksSRV[1].Valid();
    step.bvhNodes=hasCutBvh?Physical(cutSpatialNodesSRV):Invalid;
    step.bvhBlocks=hasCutBvh?Physical(cutSpatialBlocksSRV[0]):Invalid;
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
    auto write=[&](const void* value,UINT64 size)->D3D12_GPU_VIRTUAL_ADDRESS{
        const UINT64 offset=Reserve(f,size);
        std::memcpy(f.uploadGeneration->mapped+offset,value,size);
        return f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
    };
    CutDispatch result{};
    result.source=source;result.destination=destination;
    result.overlayCurrent=currentTiles;result.overlayPrevious=previousTiles;
    result.overlayReset=resetOverlay;
    result.step=write(&step,sizeof(step));
    if(enableOverlay){
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
            result.overlay=write(&draw,sizeof(draw));
    }
    if(cutInitialCount){
        struct CutParameters{
            uint32_t original,poolRead,poolWrite,stateRead;
            uint32_t stateWrite,blocksRead0,blocksRead1,blocksWrite0;
            uint32_t blocksWrite1,nodesRead,impactsRead,impactControl;
            uint32_t candidateList,blockResults,dispatchArgs,controlBuffer;
            uint32_t allocator0,allocator1,freeRanges0,freeRanges1;
            uint32_t pendingBuffer,pendingControl,instanceFlags;
            uint32_t commandActive,commandPrepared;
            uint32_t originalCount,poolCapacity,blockCount,instanceCount;
            uint32_t freeRangeCapacity,pendingCapacity,impactCapacity,recordHits;
        } config{
            Physical(cutOriginalSRV),Physical(cutPoolSRV),Physical(cutPoolUAV),
            Physical(cutStateSRV),Physical(cutStateUAV),
            Physical(cutSpatialBlocksSRV[0]),Physical(cutSpatialBlocksSRV[1]),
            Physical(cutSpatialBlocksUAV[0]),Physical(cutSpatialBlocksUAV[1]),
            Physical(cutSpatialNodesSRV),Physical(cf.impactSRV),
            Physical(cf.controlUAV),Physical(cutCandidatesUAV),
            Physical(cutBlockResultsUAV),Physical(cutDispatchArgumentsUAV),
            Physical(cutControlUAV),
            Physical(cutPoolAllocatorsUAV[0]),Physical(cutPoolAllocatorsUAV[1]),
            Physical(cutFreeRangesUAV[0]),Physical(cutFreeRangesUAV[1]),
            Physical(cutPendingImpactsUAV),Physical(cutPendingControlUAV),
            Physical(cutInstanceReplacedUAV),Physical(cutDrawArgumentsUAV),
            Physical(cutPreparedDrawArgumentsUAV),
            cutInitialCount,cutCapacity,cutSpatialBlockCount,cutStaticCount,
            cutFreeRangeCapacity,CutPendingCapacity,CutProjectileCapacity,
            cutActive?1u:0u
        };
        static_assert(sizeof(CutParameters)==33u*sizeof(uint32_t));
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
    for(auto& h:cutSpatialBlocksSRV)release(h);
    for(auto& h:cutSpatialBlocksUAV)release(h);
    for(auto& h:cutPoolAllocatorsUAV)release(h);
    for(auto& h:cutFreeRangesUAV)release(h);
    for(auto& r:cutSpatialBlocks)forget(r);
    for(auto& r:cutPoolAllocators)forget(r);
    for(auto& r:cutFreeRanges)forget(r);
    release(cutSpatialNodesSRV);
    release(cutInstanceReplacedSRV);release(cutInstanceReplacedUAV);
    release(cutCandidatesUAV);release(cutBlockResultsUAV);
    release(cutPendingImpactsUAV);release(cutPendingControlUAV);
    forget(cutSpatialNodes);forget(cutInstanceReplaced);
    forget(cutCandidates);forget(cutBlockResults);
    forget(cutPendingImpacts);forget(cutPendingControl);
    cutSpatialBlockCount=cutSpatialNodeCount=cutFreeRangeCapacity=0;
    cutSpatialRoot=Invalid;cutHaveShot=false;
    release(cutPoolSRV);release(cutPoolUAV);
    release(cutStateSRV);release(cutStateUAV);release(cutControlUAV);
    release(cutDispatchArgumentsUAV);release(cutDrawArgumentsUAV);
    release(cutPreparedDrawArgumentsUAV);
    forget(cutOriginal);forget(cutPool);
    forget(cutState);forget(cutControl);forget(cutDispatchArguments);
    forget(cutDrawArguments);forget(cutPreparedDrawArguments);
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
