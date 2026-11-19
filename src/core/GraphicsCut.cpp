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
        L"BeginCut",L"ClipCacheBlocks",L"CommitCut",
        L"BeginCache",L"CountCache",L"ReserveCache",L"ScatterCache",
        L"CommitCache",L"BuildCacheDraw",
        L"GatherLaserSections",L"BuildLaserWallArgs",L"EmitLaserWalls"
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
    const bool laserWalls=!outputSurface.desktop&&scene.laserWallMaterials[0]!=Invalid&&
        scene.laserWallMaterials[1]!=Invalid;
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
    auto appendObject=[&](uint32_t instance){
        const auto& object=scene.objects[instance];
        if((object.flags&(DynamicObject|DeformedObject|AlphaBlendObject))!=0)return;
        if(object.mesh>=scene.meshes.ranges.size())Fail("Cut mesh range missing");
        const auto& range=scene.meshes.ranges[object.mesh];
        uint32_t solid=0u;
        if(laserWalls&&object.material<scene.materialNames.size()){
            const auto& name=scene.materialNames[object.material];
            // Decals, the river and foliage are open surfaces.
            if(name=="Brushed charcoal structural metal"||name=="Graphite tower shells")solid=1u;
            else if(name=="Island soil"||name=="Island stone"||name=="Terraced sandstone")solid=2u;
        }
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
            tri.padding0=object.flags|(solid<<16u);
            // Bounds are accumulated from the exact transformed positions stored
            // in cutOriginal; no local-space meshlet AABB or second transform.
            if(!block.triangleCount)block=openBlock(uint32_t(source.size()),instance);
            // Preserve the stable source block identity through future cuts.
            // Source blocks are closed and appended in original triangle order.
            tri.padding1=uint32_t(blocks.size());
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
    };
    auto appendWalls=[&](SphereCutRange& range){
        // The walls span the interior between original exterior blocks.
        // Their own spatial cells give them bounds for later cuts and hits.
        auto lower=blocks[range.first].lower,upper=blocks[range.first].upper;
        for(size_t i=range.first;i<blocks.size();++i){
            const auto& block=blocks[i];
            lower={std::min(lower.x,block.lower.x),std::min(lower.y,block.lower.y),
                   std::min(lower.z,block.lower.z)};
            upper={std::max(upper.x,block.upper.x),std::max(upper.y,block.upper.y),
                   std::max(upper.z,block.upper.z)};
        }
        constexpr XMFLOAT3 pad{.002f,.002f,.002f};
        range.wallOrigin=Sub(lower,pad);
        const auto size=Sub(Add(upper,pad),range.wallOrigin);
        range.wallCellSize={size.x/float(LaserWallGrid.x),size.y/float(LaserWallGrid.y),size.z/float(LaserWallGrid.z)};
        cutWallMaterials=scene.laserWallMaterials;
        range.firstWall=uint32_t(blocks.size());
        const uint32_t owner=blocks.back().instanceId;
        for(uint32_t z=0;z<LaserWallGrid.z;++z)for(uint32_t y=0;y<LaserWallGrid.y;++y)
            for(uint32_t x=0;x<LaserWallGrid.x;++x){
                const XMFLOAT3 grid{float(x)*range.wallCellSize.x,float(y)*range.wallCellSize.y,
                                    float(z)*range.wallCellSize.z};
                const auto lo=Add(range.wallOrigin,grid);
                auto block=openBlock(0u,owner);
                block.lower=Sub(lo,pad);
                block.upper=Add(Add(lo,range.wallCellSize),pad);
                block.geometryState=uint32_t(CutGeometryState::Empty);
                blocks.push_back(block);
            }
    };
    size_t largestSphere=0;
    for(uint32_t owner=0;owner<sphereCount;++owner){
        const auto first=sphereCount==1u?0u:sphereObjectOffsets[owner];
        const auto end=sphereCount==1u?uint32_t(scene.objects.size()):sphereObjectOffsets[owner+1u];
        auto& range=sphereCutRanges[owner];
        range.first=uint32_t(blocks.size());
        const auto previousCount=source.size();
        for(uint32_t instance=first;instance<end;++instance)appendObject(instance);
        largestSphere=std::max(largestSphere,source.size()-previousCount);
        if(laserWalls&&blocks.size()>range.first)appendWalls(range);
        range.count=uint32_t(blocks.size())-range.first;
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
    const uint64_t capacity=DenseSphereField()?cutInitialCount+15u*uint64_t(largestSphere):
        16u*uint64_t(cutInitialCount);
    cutCapacity=uint32_t(std::max<uint64_t>(65536u,capacity));
    cutStaticCount=uint32_t(scene.objects.size());
    cutOriginal=UploadVector(source);
    cutOriginalSRV=Srv(cutOriginal.Get(),cutInitialCount,sizeof(CutTriangle));
    // Metadata is UAV-capable from day one for future state/active-range
    // updates. This commit leaves it untouched after the initial upload.
    const UINT64 metadataBytes=UINT64(blocks.size())*sizeof(CutSpatialBlock);
    cutSpatialBlocks=Temporary(Buffer(metadataBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                               D3D12_RESOURCE_STATE_COMMON);
    auto metadataUpload=Host(metadataBytes,D3D12_HEAP_TYPE_UPLOAD);
    void* mapped=nullptr;D3D12_RANGE empty{0,0};
    Hr(metadataUpload->Map(0,&empty,&mapped),"cut spatial blocks upload map");
    std::memcpy(mapped,blocks.data(),size_t(metadataBytes));
    metadataUpload->Unmap(0,nullptr);
    startup.list->CopyBufferRegion(cutSpatialBlocks.Get(),0,metadataUpload.Get(),0,metadataBytes);
    BBarrier(startup.list.Get(),cutSpatialBlocks.Get(),
             D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,
             D3D12_BARRIER_ACCESS_COPY_DEST,
             D3D12_BARRIER_ACCESS_SHADER_RESOURCE|D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    uploads.push_back(metadataUpload);
    cutSpatialBlocksSRV=Srv(cutSpatialBlocks.Get(),uint32_t(blocks.size()),sizeof(CutSpatialBlock));
    cutSpatialBlocksUAV=UavBuffer(cutSpatialBlocks.Get(),uint32_t(blocks.size()),sizeof(CutSpatialBlock));
    // Nodes never change after build; the first node is always the root.
    cutSpatialNodes=UploadVector(nodes);
    cutSpatialNodesSRV=Srv(cutSpatialNodes.Get(),uint32_t(nodes.size()),sizeof(CutSpatialNode));
    cutSpatialBlockCount=uint32_t(blocks.size());
    cutSpatialNodeCount=uint32_t(nodes.size());
    cutSpatialRoot=root;
    if(cutSpatialBlockCount>UINT32_MAX/5u)
        Fail("Cut cache draw arguments exceed 32-bit allocation");
    const uint32_t selectionEntries=cutSpatialBlockCount*2u;
    const UINT64 markBytes=UINT64(cutSpatialBlockCount)*sizeof(uint32_t);
    cutCandidateSelection=Temporary(Buffer(markBytes*2u,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                D3D12_RESOURCE_STATE_COMMON);
    cutCandidateSelectionSRV=Srv(cutCandidateSelection.Get(),selectionEntries,sizeof(uint32_t));
    cutCandidateSelectionUAV=UavBuffer(cutCandidateSelection.Get(),selectionEntries,sizeof(uint32_t));
    auto markUpload=Host(markBytes,D3D12_HEAP_TYPE_UPLOAD);
    void* markMapped=nullptr;D3D12_RANGE emptyMarks{0,0};
    Hr(markUpload->Map(0,&emptyMarks,&markMapped),"cut candidate marks upload map");
    std::memset(markMapped,0,size_t(markBytes));
    markUpload->Unmap(0,nullptr);
    startup.list->CopyBufferRegion(cutCandidateSelection.Get(),0,markUpload.Get(),0,markBytes);
    BBarrier(startup.list.Get(),cutCandidateSelection.Get(),
             D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
             D3D12_BARRIER_ACCESS_COPY_DEST,
             D3D12_BARRIER_ACCESS_SHADER_RESOURCE|D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    uploads.push_back(markUpload);
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
    if(laserWalls){
        constexpr uint32_t entries=LaserSweep::MaxPlanes*LaserSectionCapacity;
        constexpr uint32_t segmentStride=24;
        allocate(cutWallSegments,entries*segmentStride);
        cutWallSegmentsUAV=UavBuffer(cutWallSegments.Get(),entries,segmentStride);
        allocate(cutWallCounts,LaserSweep::MaxPlanes*sizeof(uint32_t));
        cutWallCountsUAV=UavBuffer(cutWallCounts.Get(),LaserSweep::MaxPlanes,0u);
        allocate(cutWallDispatch,sizeof(D3D12_DISPATCH_ARGUMENTS));
        cutWallDispatchUAV=UavBuffer(cutWallDispatch.Get(),3u,0u);
        cutBrushCapacity=InitialCutBrushCapacity;
        allocate(cutBrushes,cutBrushCapacity*CutBrushStride);
        cutBrushesUAV=UavBuffer(cutBrushes.Get(),cutBrushCapacity,CutBrushStride);
        allocate(cutBrushCandidates,(cutBrushCapacity+1u)*sizeof(uint32_t));
        cutBrushCandidatesUAV=UavBuffer(cutBrushCandidates.Get(),cutBrushCapacity+1u,0u);
    }
    allocate(cutState,16);
    cutStateSRV=Srv(cutState.Get(),1,sizeof(XMUINT4));
    cutStateUAV=UavBuffer(cutState.Get(),1,sizeof(XMUINT4));
    allocate(cutControl,20);
    cutControlUAV=UavBuffer(cutControl.Get(),5,0);
    allocate(cutDispatchArguments,12);
    cutDispatchArgumentsUAV=UavBuffer(cutDispatchArguments.Get(),3,0);
    allocate(cutDrawArguments,16);
    cutDrawArgumentsUAV=UavBuffer(cutDrawArguments.Get(),4,0);
    constexpr uint32_t shadowPredicateWords=ShadowSlots*CutShadowPredicatesPerSlot*2u;
    allocate(cutShadowPredicates,shadowPredicateWords*sizeof(uint32_t));
    cutShadowPredicatesUAV=UavBuffer(cutShadowPredicates.Get(),shadowPredicateWords,0);
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

    // 4b: the new persistent block cache is NOT bound to render/collision
    // yet. Full-mesh generations above remain the authoritative path.
    if(cutSpatialBlockCount>(UINT32_MAX-4u)/2u)
        Fail("Cut cache free list exceeds 32-bit allocation");
    cutCacheFreeCapacity=cutSpatialBlockCount*2u+4u;
    cutCachePoolCapacity=cutCapacity;
    cutCachePool=Temporary(
        Buffer(uint64_t(cutCachePoolCapacity)*sizeof(CutTriangle),
               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
        D3D12_RESOURCE_STATE_COMMON);
    cutCachePoolSRV=Srv(cutCachePool.Get(),cutCachePoolCapacity,sizeof(CutTriangle));
    cutCachePoolUAV=UavBuffer(cutCachePool.Get(),cutCachePoolCapacity,sizeof(CutTriangle));
    allocate(cutCacheState,16);
    cutCacheStateSRV=Srv(cutCacheState.Get(),1,sizeof(XMUINT4));
    cutCacheStateUAV=UavBuffer(cutCacheState.Get(),1,sizeof(XMUINT4));
    uploadInit(cutCacheState.Get(),{0,0,1,0}); // active=0, valid, generation=0
    for(uint32_t slot=0;slot<2u;++slot){
        cutCacheBlocks[slot]=Temporary(
            Buffer(metadataBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON);
        startup.list->CopyBufferRegion(cutCacheBlocks[slot].Get(),0,
                                       metadataUpload.Get(),0,metadataBytes);
        BBarrier(startup.list.Get(),cutCacheBlocks[slot].Get(),
            D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
            D3D12_BARRIER_ACCESS_COPY_DEST,
            D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        cutCacheBlockSRVs[slot]=Srv(cutCacheBlocks[slot].Get(),
                                    cutSpatialBlockCount,sizeof(CutSpatialBlock));
        cutCacheBlockUAVs[slot]=UavBuffer(cutCacheBlocks[slot].Get(),
                                    cutSpatialBlockCount,sizeof(CutSpatialBlock));
        allocate(cutCacheAllocators[slot],16);
        cutCacheAllocatorUAVs[slot]=UavBuffer(cutCacheAllocators[slot].Get(),4,0);
        uploadInit(cutCacheAllocators[slot].Get(),{0,0,0,0});
        const UINT64 freeBytes=uint64_t(cutCacheFreeCapacity)*sizeof(XMUINT2);
        cutCacheFreeLists[slot]=Temporary(
            Buffer(freeBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON);
        cutCacheFreeListUAVs[slot]=UavBuffer(cutCacheFreeLists[slot].Get(),
                                            cutCacheFreeCapacity,sizeof(XMUINT2));
    }
    allocate(cutCacheResults,cutSpatialBlockCount*sizeof(XMUINT2));
    cutCacheResultsUAV=UavBuffer(cutCacheResults.Get(),
                                cutSpatialBlockCount,sizeof(XMUINT2));
    allocate(cutCacheWork,32);
    cutCacheWorkUAV=UavBuffer(cutCacheWork.Get(),8,0);
    allocate(cutCacheDispatch,12);
    cutCacheDispatchUAV=UavBuffer(cutCacheDispatch.Get(),3,0);
    // Draw commands are GPU-generated only after a successful cache commit.
    // Initialize to zero dispatches, so an intact scene never consumes
    // undefined ExecuteIndirect arguments.
    const UINT64 commandsBytes=uint64_t(cutSpatialBlockCount)*5u*sizeof(uint32_t);
    cutCacheDrawArguments=Temporary(Buffer(commandsBytes,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
    cutCacheDrawArgumentsUAV=UavBuffer(cutCacheDrawArguments.Get(),
        cutSpatialBlockCount*5u,0);
    auto drawUpload=Host(commandsBytes,D3D12_HEAP_TYPE_UPLOAD);
    std::vector<uint32_t> zeroDraws(size_t(cutSpatialBlockCount)*5u);
    for(uint32_t i=0;i<cutSpatialBlockCount;++i){
        zeroDraws[size_t(i)*5u]=i;
        zeroDraws[size_t(i)*5u+3u]=1u;
        zeroDraws[size_t(i)*5u+4u]=1u;
    }
    void* drawPtr=nullptr;D3D12_RANGE noRead{0,0};
    Hr(drawUpload->Map(0,&noRead,&drawPtr),"cut cache draw upload map");
    std::memcpy(drawPtr,zeroDraws.data(),size_t(commandsBytes));
    drawUpload->Unmap(0,nullptr);
    startup.list->CopyBufferRegion(cutCacheDrawArguments.Get(),0,
                                   drawUpload.Get(),0,commandsBytes);
    BBarrier(startup.list.Get(),cutCacheDrawArguments.Get(),
        D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
        D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    uploads.push_back(drawUpload);
}
float Graphics::Impl::CutFlightDuration(uint32_t hand,const XRFrame& frame)const{
    if(outputSurface.desktop)return hand==1u?5.f:3.f;

    constexpr float baseDuration=3.f;
    const auto& atmosphere=SphereAtmosphere(renderSphere);
    const float units=frame.input.sceneUnitsPerMeter;
    if(hand!=1u||units>=1.f||atmosphere.radius<=0.f)return baseDuration;
    const auto& muzzle=frame.shotHands[hand].aim.pose;
    const auto direction=Unit(Sub(Transform({0.f,0.f,1.f},muzzle),muzzle.p));
    const auto offset=Sub(muzzle.p,atmosphere.center);
    const float along=Dot(offset,direction);
    const float radiusSquared=atmosphere.radius*atmosphere.radius;
    const float discriminant=along*along+radiusSquared-Dot(offset,offset);
    if(discriminant<=0.f)return baseDuration;

    // From the enlarged scene the shell can be over three seconds away.
    // Keep the original speed and use the forward exit distance per shot.
    const float exit=-along+std::sqrt(discriminant);
    return std::max(baseDuration,exit/(ScoriaSpeed*units)+.05f);
}

Graphics::Impl::CutDispatch Graphics::Impl::PrepareCutDispatch(Frame& f,const XRFrame& xr,bool runProjectiles,bool sphereProxy){
    const auto& atmosphere=SphereAtmosphere(renderSphere);
    const auto& membrane=RenderMembrane();
    const auto& debris=RenderDebris();
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
        cutOverlayTileCountX=(width+15u)/16u;
        cutOverlayTileCountY=(height+15u)/16u;
        const uint64_t capacity=uint64_t(cutOverlayTileCountX)*cutOverlayTileCountY*viewCount;
        if(!capacity||capacity>UINT32_MAX/4u)Fail("Projectile tile capacity");
        cutOverlayMask.resource=Temporary(Texture(width,height,DXGI_FORMAT_R32_UINT,
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
        uint32_t sourceTriangles,cutTrianglesA,cutTrianglesB,state,cutCount,cutStaticCount,bvhNodes,bvhBlocks;
        // 4c read-only cache descriptors; legacy full-mesh fallback stays.
        uint32_t cachePool,cacheState,cacheBlocks0,cacheBlocks1;
        uint32_t membraneEvents,membraneCapacity;
        float membraneTime;
        float membraneTravelTime;
        XMFLOAT4 membraneSphere;
    } step{};
    static_assert(sizeof(StepParameters)==304);
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
    // The shader reads both SRVs only while cutState selects intact geometry.
    // These are the two reserved dwords in the existing 256-byte ABI.
    const bool hasCutBvh=cutSpatialRoot==0u&&cutSpatialBlockCount!=0u&&
        cutSpatialNodeCount!=0u&&cutInitialCount!=0u&&
        cutSpatialNodesSRV.Valid()&&cutSpatialBlocksSRV.Valid();
    step.bvhNodes=hasCutBvh?Physical(cutSpatialNodesSRV):Invalid;
    step.bvhBlocks=hasCutBvh?Physical(cutSpatialBlocksSRV):Invalid;
    const bool hasCutCache=hasCutBvh&&cutCacheStateSRV.Valid()&&
        cutCachePoolSRV.Valid()&&cutCacheBlockSRVs[0].Valid()&&
        cutCacheBlockSRVs[1].Valid();
    step.cachePool=hasCutCache?Physical(cutCachePoolSRV):Invalid;
    step.cacheState=hasCutCache?Physical(cutCacheStateSRV):Invalid;
    step.cacheBlocks0=hasCutCache?Physical(cutCacheBlockSRVs[0]):Invalid;
    step.cacheBlocks1=hasCutCache?Physical(cutCacheBlockSRVs[1]):Invalid;
    step.membraneEvents=Invalid;
    if(atmosphere.membrane&&atmosphere.radius>0.f){
        step.membraneEvents=Physical(membrane.eventUAV);
        step.membraneCapacity=MembraneEventCapacity;
        step.membraneTime=f.constants->timing.z;
        step.membraneTravelTime=f.constants->membraneAnimation.y;
        step.membraneSphere=f.constants->atmosphereSphere;
    }
    const bool desktop=outputSurface.desktop;
    const float units=xr.input.sceneUnitsPerMeter;
    constexpr uint32_t scoriaProjectileKind=2u; // Matches projectile_draw.hlsl.
    for(uint32_t hand=desktop?0u:1u;hand<2;++hand){
        const auto& shot=xr.shotHands[hand];
        if(!shot.fired||!shot.aim.tracked)continue;
        const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
        CutProjectile p{};
        p.position=desktop?Add(shot.aim.pose.p,Scale(forward,.12f*units)):shot.aim.pose.p;
        const float speed=desktop?(hand==0?95.f:28.f):ScoriaSpeed;
        p.velocity=Scale(forward,speed*units);
        p.radius=(hand==0?.24f:.32f)*units;
        p.visualRadius=.008f*units;
        if(desktop&&hand==1)p.acceleration={0,-9.81f*units,0};
        p.lifetime=CutFlightDuration(hand,xr);
        p.id=cutNextId++;if(!cutNextId)cutNextId=1;
        p.kind=desktop?hand:scoriaProjectileKind;
        if(!desktop&&units<1.f)p.kind|=0x80000000u;
        p.alive=1;
        step.spawns[hand]=p;step.spawnMask|=1u<<hand;
        if(hand==0)step.left=(p.id-1u)%CutProjectileCapacity;
        else step.right=(p.id-1u)%CutProjectileCapacity;
    }
    struct DrawParameters {
        uint32_t projectiles,maskUav,maskSrv,colorUav;
        uint32_t capacity,width,height,views;
        XMFLOAT4X4 vp[MaxViews];
        XMFLOAT4 laserStart[2+LaserAfterimageCount],laserEnd[2+LaserAfterimageCount];
        uint32_t tileSeenUav,tileActiveUav,tilePreviousUav,tileCountUav;
        uint32_t tileArgsUav,tileGeneration,tileCountX,tileCountY;
        uint32_t sceneDepth,depthWidth,depthHeight,depthSamples;
        XMFLOAT4 depthJitter,viewScale;
    } draw{};
    static_assert(LaserAfterimageCount==6u&&sizeof(DrawParameters)==624u);
    draw.projectiles=Physical(cutProjectileSRVs[destination]);
    draw.maskUav=Physical(cutOverlayUAV);draw.maskSrv=Physical(cutOverlaySRV);
    draw.colorUav=Physical(hdrResolvedUAV);
    draw.capacity=CutProjectileCapacity;draw.width=width;
    draw.height=height;draw.views=xr.viewCount;
    draw.tileSeenUav=Physical(cutOverlayTileLastSeenUAV);
    draw.tileActiveUav=Physical(cutOverlayActiveTileUAVs[currentTiles]);
    draw.tilePreviousUav=Physical(cutOverlayActiveTileUAVs[previousTiles]);
    draw.tileCountUav=Physical(cutOverlayActiveCountUAVs[currentTiles]);
    draw.tileArgsUav=Physical(cutOverlayTileDispatchArgsUAV);
    draw.tileGeneration=cutOverlayGeneration;
    draw.tileCountX=cutOverlayTileCountX;draw.tileCountY=cutOverlayTileCountY;
    draw.sceneDepth=sphereProxy?Invalid:Physical(depthSRV);draw.depthWidth=renderWidth;draw.depthHeight=renderHeight;
    draw.depthSamples=MsaaSamples;draw.depthJitter=f.constants->dlssJitter;
    draw.viewScale={units,xr.input.SceneNearZ(),0.f,0.f};
    for(uint32_t view=0;view<xr.viewCount;++view)draw.vp[view]=xr.vp[view];
    for(uint32_t hand=desktop?0u:1u;hand<2;++hand){
        const auto& shot=xr.shotHands[hand];
        if(!shot.aim.tracked)continue;
        const XMFLOAT3 forward=Unit(Sub(Transform({0,0,1},shot.aim.pose),shot.aim.pose.p));
        const auto begin=desktop?Add(shot.aim.pose.p,Scale(forward,.12f*units)):shot.aim.pose.p;
        auto end=Add(begin,Scale(forward,LaserReach*units));
        // W encodes the existing overlay style. Zero means no sight ray.
        draw.laserStart[hand]={begin.x,begin.y,begin.z,desktop?1.f:2.f};
        draw.laserEnd[hand]={end.x,end.y,end.z,1.f};
    }
    auto beam=[&](uint32_t index,const LaserRay& ray,float style,float strength){
        draw.laserStart[index]={ray.origin.x,ray.origin.y,ray.origin.z,style};
        draw.laserEnd[index]={ray.end.x,ray.end.y,ray.end.z,strength};
    };
    if(xr.basaltRay)beam(1u,*xr.basaltRay,xr.laserHeld?4.f:3.f,1.f);
    for(uint32_t i=0;i<xr.laserAfterimageCount;++i)
        beam(2u+i,xr.laserAfterimages[i].ray,5.f,xr.laserAfterimages[i].strength);
    auto write=[&](const void* value,UINT64 size)->D3D12_GPU_VIRTUAL_ADDRESS{
        const UINT64 offset=Reserve(f,size);
        std::memcpy(f.uploadGeneration->mapped+offset,value,size);
        return f.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
    };
    CutDispatch result{};
    result.source=source;result.destination=destination;
    result.laserLines=2u+xr.laserAfterimageCount;
    result.overlayCurrent=currentTiles;result.overlayPrevious=previousTiles;
    result.overlayReset=resetOverlay;
    if(runProjectiles)result.step=write(&step,sizeof(step));
    result.overlay=write(&draw,sizeof(draw));
    const auto& cutRange=sphereCutRanges[renderSphere];
    const auto sweep=xr.laserCut&&xr.basaltRay&&cutRange.firstWall!=Invalid?
        MakeLaserSweep(xr.previousLaserRay.value_or(*xr.basaltRay),*xr.basaltRay,units):LaserSweep{};
    if(cutInitialCount&&(runProjectiles||sweep.count)){
        struct CutParameters{
            uint32_t original,a,b,aWrite,bWrite,stateRead,stateWrite,impacts;
            uint32_t impactControl,outputControl,dispatchArgs,drawArgs;
            uint32_t originalCount,capacity,planeCount,blockCount;
            uint32_t bvhNodes,bvhBlocks,candidateSelectionRead,candidateSelectionWrite;
            uint32_t cachePoolRead,cachePoolWrite,cacheStateRead,cacheStateWrite;
            uint32_t cacheBlocksRead0,cacheBlocksRead1,cacheBlocksWrite0,cacheBlocksWrite1;
            uint32_t cacheAlloc0,cacheAlloc1,cacheFree0,cacheFree1;
            uint32_t cacheResults,cacheWork,cacheDispatch,cacheFreeCapacity;
            uint32_t cacheDrawArguments,shadowPredicates,cachePoolCapacity,shadowPadding;
            XMFLOAT4 shadowPositions[ShadowSlots];
            XMUINT4 laserWork; // plane count, segment, count and dispatch UAVs
            XMUINT4 laserWalls; // first block, metal, soil, reset shadow predicates
            XMUINT4 brushLog; // committed brush UAV, capacity and candidate UAV
            XMFLOAT4 laserCenter,laserLower,laserUpper,gridOrigin,gridCellSize;
            std::array<XMFLOAT4,LaserSweep::MaxPlanes> laserPlanes,laserAxes,laserLimits;
            XMUINT4 cutDebris,cutOwner;
        } config{
            Physical(cutOriginalSRV),Physical(cutBufferSRVs[0]),
            Physical(cutBufferSRVs[1]),Physical(cutBufferUAVs[0]),
            Physical(cutBufferUAVs[1]),Physical(cutStateSRV),
            Physical(cutStateUAV),Physical(cf.impactSRV),
            Physical(cf.controlUAV),Physical(cutControlUAV),
            Physical(cutDispatchArgumentsUAV),Physical(cutDrawArgumentsUAV),
            cutInitialCount,cutCapacity,26u,cutSpatialBlockCount,
            cutSpatialRoot==0u?Physical(cutSpatialNodesSRV):Invalid,
            cutSpatialRoot==0u?Physical(cutSpatialBlocksSRV):Invalid,
            Physical(cutCandidateSelectionSRV),Physical(cutCandidateSelectionUAV),
            Physical(cutCachePoolSRV),Physical(cutCachePoolUAV),
            Physical(cutCacheStateSRV),Physical(cutCacheStateUAV),
            Physical(cutCacheBlockSRVs[0]),Physical(cutCacheBlockSRVs[1]),
            Physical(cutCacheBlockUAVs[0]),Physical(cutCacheBlockUAVs[1]),
            Physical(cutCacheAllocatorUAVs[0]),Physical(cutCacheAllocatorUAVs[1]),
            Physical(cutCacheFreeListUAVs[0]),Physical(cutCacheFreeListUAVs[1]),
            Physical(cutCacheResultsUAV),Physical(cutCacheWorkUAV),
            Physical(cutCacheDispatchUAV),cutCacheFreeCapacity,
            Physical(cutCacheDrawArgumentsUAV),Physical(cutShadowPredicatesUAV),
            cutCachePoolCapacity,0u,{}
        };
        static_assert(ShadowSlots==4u);
        static_assert(offsetof(CutParameters,shadowPositions)==40u*sizeof(uint32_t));
        for(uint32_t slot=0;slot<ShadowSlots;++slot){
            if(f.shadowIDs[slot]==Invalid)continue;
            const auto& position=f.shadowLights[slot].position;
            config.shadowPositions[slot]={position.x,position.y,position.z,1.f};
        }
        static_assert(offsetof(CutParameters,laserWork)==56u*sizeof(uint32_t));
        config.brushLog={cutBrushes?Physical(cutBrushesUAV):Invalid,cutBrushCapacity,
                         cutBrushCandidates?Physical(cutBrushCandidatesUAV):Invalid,0u};
        config.laserWalls={cutRange.firstWall,cutWallMaterials[0],cutWallMaterials[1],1u};
        config.cutDebris={atmosphere.radius>0.f&&debris.eventUAV.Valid()?Physical(debris.eventUAV):Invalid,
                          std::bit_cast<uint32_t>(float(f.simulationTime)),uint32_t(cutDiagnosticEnabled),0u};
        auto xyz=[](XMFLOAT3 p){return XMFLOAT4{p.x,p.y,p.z,0.f};};
        config.gridOrigin=xyz(cutRange.wallOrigin);
        config.gridCellSize=xyz(cutRange.wallCellSize);
        config.cutOwner={cutRange.first,cutRange.first+cutRange.count,0u,0u};
        if(runProjectiles)result.projectileCut=write(&config,sizeof(config));
        if(sweep.count){
            config.laserWork={sweep.count,Physical(cutWallSegmentsUAV),
                Physical(cutWallCountsUAV),Physical(cutWallDispatchUAV)};
            config.laserWalls.w=runProjectiles?0u:1u;
            config.laserCenter=xyz(sweep.center);
            config.laserLower=xyz(sweep.lower);
            config.laserUpper=xyz(sweep.upper);
            config.laserUpper.w=units;
            config.laserPlanes=sweep.planes;
            config.laserAxes=sweep.axes;
            config.laserLimits=sweep.limits;
            result.laserCut=write(&config,sizeof(config));
        }
    }
    return result;
}
void Graphics::Impl::ResetCutResources(){
    cutBrushCapacity=cutBrushBound=cutBrushSampleBound=0;
    cutBrushSampleFence=0;
    cutBrushSample.Reset();
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
    release(cutSpatialBlocksSRV);release(cutSpatialBlocksUAV);release(cutSpatialNodesSRV);
    release(cutCandidateSelectionSRV);release(cutCandidateSelectionUAV);
    forget(cutSpatialBlocks);forget(cutSpatialNodes);forget(cutCandidateSelection);
    release(cutCachePoolSRV);release(cutCachePoolUAV);
    release(cutCacheStateSRV);release(cutCacheStateUAV);
    for(auto& h:cutCacheBlockSRVs)release(h);
    for(auto& h:cutCacheBlockUAVs)release(h);
    for(auto& h:cutCacheAllocatorUAVs)release(h);
    for(auto& h:cutCacheFreeListUAVs)release(h);
    release(cutCacheResultsUAV);release(cutCacheWorkUAV);
    release(cutCacheDispatchUAV);release(cutCacheDrawArgumentsUAV);
    forget(cutCachePool);forget(cutCacheState);forget(cutCacheDrawArguments);
    for(auto& r:cutCacheBlocks)forget(r);
    for(auto& r:cutCacheAllocators)forget(r);
    for(auto& r:cutCacheFreeLists)forget(r);
    forget(cutCacheResults);forget(cutCacheWork);forget(cutCacheDispatch);
    cutCacheFreeCapacity=cutCachePoolCapacity=0;
    cutCacheNeedsGrowth=false;
    for(auto& frame:frames){
        frame.cutCacheReadbackCapacity=0;
        frame.cutDiagnosticPoolCapacity=0;
    }
    cutSpatialBlockCount=cutSpatialNodeCount=0;cutSpatialRoot=Invalid;
    for(auto& h:cutBufferSRVs)release(h);
    for(auto& h:cutBufferUAVs)release(h);
    release(cutStateSRV);release(cutStateUAV);release(cutControlUAV);
    release(cutDispatchArgumentsUAV);release(cutDrawArgumentsUAV);
    forget(cutOriginal);for(auto& r:cutBuffers)forget(r);
    forget(cutState);forget(cutControl);forget(cutDispatchArguments);
    forget(cutDrawArguments);
    release(cutShadowPredicatesUAV);
    forget(cutShadowPredicates);
    lastCutShadowSlots=0;
    cutShadowsPending=false;
    release(cutWallSegmentsUAV);release(cutWallCountsUAV);release(cutWallDispatchUAV);
    forget(cutWallSegments);forget(cutWallCounts);forget(cutWallDispatch);
    release(cutBrushesUAV);release(cutBrushCandidatesUAV);
    forget(cutBrushes);forget(cutBrushCandidates);
    sphereCutRanges={};cutWallMaterials={Invalid,Invalid};
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
