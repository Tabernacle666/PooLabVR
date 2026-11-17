// Step 4a: keep step-3 rendering, collisions and full-mesh publication.
// Only BVH-selected blocks run the expensive 26-plane clipping routine;
// untouched triangles still flow through the same double-buffered soup.
struct CutVertex {
    float3 position,normal;
    float2 uv,uv1;
};
struct CutTriangle {
    CutVertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
struct CutImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId,primitiveIndex,kind,padding;
};
cbuffer CutParameters:register(b0) {
    uint originalTriangles,triangleA,triangleB,triangleAWrite;
    uint triangleBWrite,stateRead,stateWrite,impactBuffer;
    uint impactControl,outputControl,dispatchArguments,drawArguments;
    uint originalCount,capacity,planeCount,blockCount;
    uint bvhNodes,bvhBlocks,candidateMarksRead,candidateMarksWrite;
    // 4b-only cache. No raster, collision or legacy CommitCut consumer.
    uint cachePoolRead,cachePoolWrite,cacheStateRead,cacheStateWrite;
    uint cacheBlocksRead0,cacheBlocksRead1,cacheBlocksWrite0,cacheBlocksWrite1;
    uint cacheAlloc0,cacheAlloc1,cacheFree0,cacheFree1;
    uint cacheResults,cacheWork,cacheDispatch,cacheFreeCapacity;
    uint cacheDrawArguments;
};
static const uint MaxPolygon=32;
static const uint PlaneCount=26;
struct CutSpatialNode {float3 lower;uint leftOrBlock;float3 upper;uint rightChild;};
struct CutSpatialBlock {
    float3 lower;uint firstTriangle;float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
};
bool CutBoxOverlap(float3 lo,float3 hi,float3 queryLo,float3 queryHi){
    return all(lo<=queryHi)&&all(hi>=queryLo);
}
struct PolyVertex {
    float3 position,normal;
    float2 uv,uv1;
};
PolyVertex Blend(PolyVertex a,PolyVertex b,float t){
    PolyVertex v;
    v.position=lerp(a.position,b.position,t);
    v.normal=normalize(lerp(a.normal,b.normal,t));
    v.uv=lerp(a.uv,b.uv,t);
    v.uv1=lerp(a.uv1,b.uv1,t);
    return v;
}
float3 PlaneNormal(uint k){
    if(k<6){
        float3 n=0;
        uint axis=k/2;
        n[axis]=(k&1u)!=0?-1.f:1.f;
        return n;
    }
    if(k<18){
        k-=6u;
        uint pair=k/4u,signs=k%4u;
        float3 n=0;
        if(pair==0){n.x=(signs&1u)?-1:1;n.y=(signs&2u)?-1:1;}
        else if(pair==1){n.x=(signs&1u)?-1:1;n.z=(signs&2u)?-1:1;}
        else{n.y=(signs&1u)?-1:1;n.z=(signs&2u)?-1:1;}
        return normalize(n);
    }
    k-=18u;
    return normalize(float3((k&1u)?-1:1,(k&2u)?-1:1,(k&4u)?-1:1));
}
// Both polygons preserve source winding. The kept (inside) polygon is
// subsequently clipped by the next supporting plane; the outside is final.
uint ClipPolygon(PolyVertex src[MaxPolygon],uint count,
                 float3 normal,float3 center,float radius,bool inside,
                 out PolyVertex dst[MaxPolygon]){
    uint written=0;
    if(count==0)return 0;
    for(uint i=0;i<count;i++){
        PolyVertex a=src[i],b=src[(i+1u)%count];
        float da=dot(normal,a.position-center)-radius;
        float db=dot(normal,b.position-center)-radius;
        bool keepA=inside?da<=0.f:da>0.f;
        bool keepB=inside?db<=0.f:db>0.f;
        if(keepA&&written<MaxPolygon)dst[written++]=a;
        if(keepA!=keepB&&written<MaxPolygon){
            float denom=da-db;
            float t=abs(denom)>1e-12?saturate(da/denom):.5f;
            dst[written++]=Blend(a,b,t);
        }
    }
    return written;
}
PolyVertex ToPoly(CutVertex src){
    PolyVertex v;
    v.position=src.position;v.normal=src.normal;v.uv=src.uv;v.uv1=src.uv1;
    return v;
}
CutVertex ToVertex(PolyVertex src){
    CutVertex v;
    v.position=src.position;v.normal=src.normal;v.uv=src.uv;v.uv1=src.uv1;
    return v;
}
void EmitTriangle(PolyVertex a,PolyVertex b,PolyVertex c,
                  uint material,uint instanceId,uint flags,uint blockId,
                  uint outputDescriptor){
    float3 crossValue=cross(b.position-a.position,c.position-a.position);
    if(dot(crossValue,crossValue)<=1e-16)return;
    RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
    uint index;
    counter.InterlockedAdd(0u,1u,index);
    if(index>=capacity){
        uint ignored;counter.InterlockedOr(4u,1u,ignored);
        return;
    }
    RWStructuredBuffer<CutTriangle> target=ResourceDescriptorHeap[NonUniformResourceIndex(outputDescriptor)];
    CutTriangle cutFace;
    cutFace.a=ToVertex(a);cutFace.b=ToVertex(b);cutFace.c=ToVertex(c);
    cutFace.material=material;cutFace.instanceId=instanceId;
    cutFace.pad0=flags;cutFace.pad1=blockId;
    target[index]=cutFace;
}
void EmitPolygon(PolyVertex poly[MaxPolygon],uint count,
                 uint material,uint instanceId,uint flags,uint blockId,
                 uint outputDescriptor){
    if(count<3)return;
    for(uint j=1;j+1<count;j++)
        EmitTriangle(poly[0],poly[j],poly[j+1],
                     material,instanceId,flags,blockId,outputDescriptor);
}
[numthreads(1,1,1)]
void BeginCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer dispatch=ResourceDescriptorHeap[dispatchArguments];
    RWByteAddressBuffer hits=ResourceDescriptorHeap[impactControl];
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint4 version=state[0];
    control.Store4(0,uint4(0,0,0,0));
    uint hitCount=hits.Load(0);
    uint count=version.y==0?originalCount:version.x;
    uint groups=(hitCount>0&&hits.Load(4)==0&&hits.Load(8)==0&&
                 count>0&&count<=capacity)?(count+63u)/64u:0u;
    dispatch.Store3(0,uint3(groups,1,1));
    if(groups==0u)return;

    // Six axial clipping planes bound the entire 26-plane cut:
    // center +/- radius is conservative, a sphere test would miss corners.
    if(blockCount==0u||bvhNodes==0xffffffffu||
       bvhBlocks==0xffffffffu||candidateMarksWrite==0xffffffffu){
        control.Store(12u,1u);return;
    }
    StructuredBuffer<CutImpact> impacts=ResourceDescriptorHeap[impactBuffer];
    CutImpact impact=impacts[0];
    float r=max(impact.radius,1e-6f);
    float3 queryLo=impact.position-r,queryHi=impact.position+r;
    StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[bvhNodes];
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[bvhBlocks];
    RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[candidateMarksWrite];
    uint nodeCount,nodeStride,blockArrayCount,blockStride,markCount,markStride;
    nodes.GetDimensions(nodeCount,nodeStride);
    blocks.GetDimensions(blockArrayCount,blockStride);
    marks.GetDimensions(markCount,markStride);
    if(nodeCount==0u||blockArrayCount!=blockCount||markCount!=blockCount){
        control.Store(12u,1u);return;
    }
    uint stack[64],stackSize=1u;
    stack[0]=0u;
    const uint stamp=version.z+1u;
    while(stackSize!=0u){
        uint nodeId=stack[--stackSize];
        if(nodeId>=nodeCount){control.Store(12u,1u);return;}
        CutSpatialNode node=nodes[nodeId];
        if(!CutBoxOverlap(node.lower,node.upper,queryLo,queryHi))continue;
        if(node.rightChild!=0xffffffffu){
            if(stackSize>62u){control.Store(12u,1u);return;}
            stack[stackSize++]=node.rightChild;
            stack[stackSize++]=node.leftOrBlock;
        }else{
            uint blockId=node.leftOrBlock;
            if(blockId>=blockCount){control.Store(12u,1u);return;}
            CutSpatialBlock block=blocks[blockId];
            if(CutBoxOverlap(block.lower,block.upper,queryLo,queryHi))
                marks[blockId]=stamp;
        }
    }
}
[numthreads(64,1,1)]
void RebuildCutTriangles(uint3 id:SV_DispatchThreadID){
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint4 version=state[0];
    uint count=version.y==0?originalCount:version.x;
    if(id.x>=count)return;
    StructuredBuffer<CutImpact> hits=ResourceDescriptorHeap[impactBuffer];
    CutImpact hit=hits[0];
    uint sourceDescriptor=version.y==0?originalTriangles:
                          (version.y==1?triangleA:triangleB);
    uint destDescriptor=version.y==1?triangleBWrite:triangleAWrite;
    StructuredBuffer<CutTriangle> source=ResourceDescriptorHeap[NonUniformResourceIndex(sourceDescriptor)];
    CutTriangle tri=source[id.x];
    // Propagate source block ID through every cut and soup compaction.
    // Invalid metadata conservatively takes the original clipping path.
    bool candidate=true;
    if(blockCount!=0u&&candidateMarksRead!=0xffffffffu){
        RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
        if(control.Load(12u)==0u&&tri.pad1<blockCount){
            StructuredBuffer<uint> marks=ResourceDescriptorHeap[candidateMarksRead];
            candidate=marks[tri.pad1]==version.z+1u;
        }
    }
    if(!candidate){
        EmitTriangle(ToPoly(tri.a),ToPoly(tri.b),ToPoly(tri.c),
                     tri.material,tri.instanceId,tri.pad0,tri.pad1,destDescriptor);
        return;
    }
    float radius=max(hit.radius,1e-6);
    float3 lower=min(tri.a.position,min(tri.b.position,tri.c.position));
    float3 upper=max(tri.a.position,max(tri.b.position,tri.c.position));
    // An axis-plane of the 26-plane cut bounds every possible removed point.
    // Far triangles copy directly, with no polygon clipping work.
    if(any(lower>hit.position+radius)||any(upper<hit.position-radius)){
        EmitTriangle(ToPoly(tri.a),ToPoly(tri.b),ToPoly(tri.c),
                     tri.material,tri.instanceId,tri.pad0,tri.pad1,destDescriptor);
        return;
    }
    PolyVertex inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    inside[0]=ToPoly(tri.a);inside[1]=ToPoly(tri.b);inside[2]=ToPoly(tri.c);
    uint countInside=3;
    for(uint p=0;p<PlaneCount&&countInside>=3;p++){
        float3 normal=PlaneNormal(p);
        uint outsideCount=ClipPolygon(inside,countInside,normal,hit.position,radius,false,outside);
        EmitPolygon(outside,outsideCount,tri.material,tri.instanceId,tri.pad0,
                    tri.pad1,destDescriptor);
        uint nextCount=ClipPolygon(inside,countInside,normal,hit.position,radius,true,next);
        for(uint k=0;k<nextCount;k++)inside[k]=next[k];
        countInside=nextCount;
    }
    // A non-degenerate portion inside the 26-plane impact has been removed.
    if(countInside>=3){
        float3 normal=cross(inside[1].position-inside[0].position,
                            inside[2].position-inside[0].position);
        if(dot(normal,normal)>1e-16){
            RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
            uint old;counter.InterlockedAdd(8u,1u,old);
        }
    }
}
[numthreads(1,1,1)]
void CommitCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer impacts=ResourceDescriptorHeap[impactControl];
    if(impacts.Load(0)==0||impacts.Load(4)!=0||impacts.Load(8)!=0)return;
    uint count=counter.Load(0u);
    if(counter.Load(4u)!=0||counter.Load(8u)==0||count>capacity)return;
    StructuredBuffer<uint4> current=ResourceDescriptorHeap[stateRead];
    RWStructuredBuffer<uint4> state=ResourceDescriptorHeap[stateWrite];
    uint4 previous=current[0];
    uint slot=previous.y==1u?2u:1u;
    state[0]=uint4(count,slot,previous.z+1u,0u);
    RWByteAddressBuffer args=ResourceDescriptorHeap[drawArguments];
    args.Store4(0,uint4(0u,(count+20u)/21u,1u,1u));
}


// Step 4b: a persistent, independently validated block cache. The known-good
// global triangle soup remains authoritative for render, DXR and collision.
// Source blocks use immutable cutOriginal ranges; Modified blocks use a
// disjoint allocation in cachePool; Empty blocks own no triangles.
// Failure invalidates ONLY this cache, never the legacy cut publication.
// Next successful shot rebuilds the cache from the authoritative global soup.
uint CacheBlockRead(uint slot){return slot==0u?cacheBlocksRead0:cacheBlocksRead1;}
uint CacheBlockWrite(uint slot){return slot==0u?cacheBlocksWrite0:cacheBlocksWrite1;}
uint CacheAllocator(uint slot){return slot==0u?cacheAlloc0:cacheAlloc1;}
uint CacheFreeList(uint slot){return slot==0u?cacheFree0:cacheFree1;}

bool CacheSelected(uint blockId,uint stamp,bool entireSoup){
    if(entireSoup)return true;
    StructuredBuffer<uint> marks=ResourceDescriptorHeap[candidateMarksRead];
    return marks[blockId]==stamp;
}
void InvalidateCache(uint error){
    RWStructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateWrite];
    uint4 current=cache[0];
    cache[0]=uint4(current.x,current.y,0u,error);
}
bool CacheReserveRange(uint descriptor,uint requested,
                       inout uint bump,inout uint freeCount,out uint start){
    start=0u;
    RWStructuredBuffer<uint2> free=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    for(uint i=0u;i<freeCount;++i){
        uint2 span=free[i];
        if(span.x>capacity||span.y>capacity-span.x)return false;
        if(span.y<requested)continue;
        start=span.x;
        if(span.y==requested){
            for(uint j=i;j+1u<freeCount;++j)free[j]=free[j+1u];
            --freeCount;
        }else free[i]=uint2(span.x+requested,span.y-requested);
        return true;
    }
    if(bump>capacity||requested>capacity-bump)return false;
    start=bump;bump+=requested;return true;
}
bool CacheRecycleRange(uint descriptor,uint start,uint length,inout uint freeCount){
    if(length==0u)return true;
    if(freeCount>=cacheFreeCapacity)return false;
    RWStructuredBuffer<uint2> free=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    uint at=0u;
    while(at<freeCount&&free[at].x<start)++at;
    for(uint j=freeCount;j>at;--j)free[j]=free[j-1u];
    free[at]=uint2(start,length);
    ++freeCount;
    for(uint j=0u;j+1u<freeCount;){
        uint2 a=free[j],b=free[j+1u];
        if(a.x+a.y>=b.x){
            free[j]=uint2(a.x,max(a.x+a.y,b.x+b.y)-a.x);
            for(uint k=j+1u;k+1u<freeCount;++k)free[k]=free[k+1u];
            --freeCount;
        }else ++j;
    }
    return true;
}
// Runs after the legacy cutter has finished its UAV writes, but BEFORE its
// existing CommitCut. Compute an indirect work size from the actual output.
[numthreads(1,1,1)]
void BeginCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    RWByteAddressBuffer args=ResourceDescriptorHeap[cacheDispatch];
    work.Store4(0u,uint4(0u,0u,0u,0u));
    args.Store3(0u,uint3(0u,1u,1u));
    RWByteAddressBuffer hits=ResourceDescriptorHeap[impactControl];
    RWByteAddressBuffer cut=ResourceDescriptorHeap[outputControl];
    uint triangles=cut.Load(0u);
    if(hits.Load(0u)==0u||hits.Load(4u)!=0u||hits.Load(8u)!=0u||
       cut.Load(4u)!=0u||cut.Load(8u)==0u||triangles>capacity)return;
    RWStructuredBuffer<uint2> result=ResourceDescriptorHeap[cacheResults];
    for(uint i=0u;i<blockCount;++i)result[i]=uint2(0u,0u);
    work.Store4(0u,uint4(1u,0u,triangles,0u));
    args.Store3(0u,uint3((triangles+63u)/64u,1u,1u));
}
// One thread per resulting triangle; source block provenance comes from 4a.
// Changed blocks are selected by stamps, not by order in the compacted soup.
[numthreads(64,1,1)]
void CountCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    if(work.Load(0u)==0u)return;
    uint count=work.Load(8u),index=id.x;
    if(index>=count)return;
    StructuredBuffer<uint4> oldCut=ResourceDescriptorHeap[stateRead];
    uint4 version=oldCut[0];
    StructuredBuffer<CutTriangle> output=ResourceDescriptorHeap[
        NonUniformResourceIndex(version.y==1u?triangleB:triangleA)];
    uint blockId=output[index].pad1;
    if(blockId>=blockCount){
        uint ignored;work.InterlockedOr(4u,1u,ignored);return;
    }
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    bool entire=cache[0].z==0u;
    RWByteAddressBuffer cutStatus=ResourceDescriptorHeap[outputControl];
    entire|=cutStatus.Load(12u)!=0u;
    if(!CacheSelected(blockId,version.z+1u,entire))return;
    RWStructuredBuffer<uint2> results=ResourceDescriptorHeap[cacheResults];
    uint ignored;
    InterlockedAdd(results[blockId].x,1u,ignored);
}
// Prepare the opposite metadata table and opposite allocator only. The current
// table and all live ranges are left untouched until Scatter succeeds.
[numthreads(1,1,1)]
void ReserveCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    if(work.Load(0u)==0u||work.Load(4u)!=0u)return;
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    uint4 meta=cache[0];
    uint active=meta.x,staging=active^1u;
    if(active>1u){work.Store(4u,1u);return;}
    bool recache=meta.z==0u;
    bool entire=recache;
    RWByteAddressBuffer cutStatus=ResourceDescriptorHeap[outputControl];
    entire|=cutStatus.Load(12u)!=0u;
    StructuredBuffer<uint4> cut=ResourceDescriptorHeap[stateRead];
    uint stamp=cut[0].z+1u;
    StructuredBuffer<CutSpatialBlock> oldBlocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockRead(active))];
    StructuredBuffer<CutSpatialBlock> originals=ResourceDescriptorHeap[bvhBlocks];
    RWStructuredBuffer<CutSpatialBlock> newBlocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockWrite(staging))];
    RWStructuredBuffer<uint2> results=ResourceDescriptorHeap[cacheResults];
    RWByteAddressBuffer oldAlloc=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheAllocator(active))];
    RWByteAddressBuffer newAlloc=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheAllocator(staging))];
    RWStructuredBuffer<uint2> oldFree=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheFreeList(active))];
    RWStructuredBuffer<uint2> newFree=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheFreeList(staging))];
    // A selection fallback scans every block but MUST preserve the active
    // pool ranges. Only an already-invalid cache may rebuild from offset 0.
    uint bump=recache?0u:oldAlloc.Load(0u);
    uint freeCount=recache?0u:oldAlloc.Load(4u);
    if(bump>capacity||freeCount>cacheFreeCapacity){
        work.Store(4u,1u);return;
    }
    uint freeDesc=CacheFreeList(staging);
    for(uint i=0u;i<freeCount;++i)newFree[i]=oldFree[i];
    // Metadata O(blocks) is small; all triangle work is already parallel.
    for(uint i=0u;i<blockCount;++i){
        CutSpatialBlock old=oldBlocks[i];
        if(entire)old=originals[i];
        if(old.geometryState>2u||
           (old.geometryState==1u&&
            (old.activeFirst>capacity||
             old.activeCount>capacity-old.activeFirst))){
            work.Store(4u,3u);return;
        }
        newBlocks[i]=old;
        if(!CacheSelected(i,stamp,entire))continue;
        uint needed=results[i].x;
        if(needed!=0u){
            uint start;
            if(!CacheReserveRange(freeDesc,needed,bump,freeCount,start)){
                work.Store(4u,2u);return;
            }
            old.activeFirst=start;old.activeCount=needed;
            old.geometryState=1u; // Modified
        }else{
            old.geometryState=2u; // Empty
            old.activeFirst=0u;old.activeCount=0u;
        }
        newBlocks[i]=old;
    }
    newAlloc.Store4(0u,uint4(bump,freeCount,0u,0u));
}
// Scatter only touched block triangles into individually allocated ranges;
// never overwrite an active range. Zero-count Empty blocks do no writes.
[numthreads(64,1,1)]
void ScatterCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    if(work.Load(0u)==0u||work.Load(4u)!=0u)return;
    uint index=id.x,count=work.Load(8u);
    if(index>=count)return;
    StructuredBuffer<uint4> cut=ResourceDescriptorHeap[stateRead];
    uint4 version=cut[0];
    StructuredBuffer<CutTriangle> soup=ResourceDescriptorHeap[
        NonUniformResourceIndex(version.y==1u?triangleB:triangleA)];
    CutTriangle cutFace=soup[index];
    uint blockId=cutFace.pad1;
    if(blockId>=blockCount){
        uint ignored;work.InterlockedOr(4u,1u,ignored);return;
    }
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    bool entire=cache[0].z==0u;
    RWByteAddressBuffer cutStatus=ResourceDescriptorHeap[outputControl];
    entire|=cutStatus.Load(12u)!=0u;
    if(!CacheSelected(blockId,version.z+1u,entire))return;
    uint stage=cache[0].x^1u;
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockRead(stage))];
    CutSpatialBlock block=blocks[blockId];
    RWStructuredBuffer<uint2> results=ResourceDescriptorHeap[cacheResults];
    uint offset;
    InterlockedAdd(results[blockId].y,1u,offset);
    if(block.geometryState!=1u||block.activeFirst>capacity||
       offset>=block.activeCount||block.activeCount>capacity-block.activeFirst){
        uint ignored;work.InterlockedOr(4u,1u,ignored);return;
    }
    RWStructuredBuffer<CutTriangle> pool=ResourceDescriptorHeap[cachePoolWrite];
    pool[block.activeFirst+offset]=cutFace;
}
// Validate every changed block before flipping the cache-internal generation.
// Old allocations enter the STAGED free list only after all writes complete.
// This state is deliberately NOT read by render or collision in 4b.
[numthreads(1,1,1)]
void CommitCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    if(work.Load(0u)==0u)return;
    if(work.Load(4u)!=0u){InvalidateCache(work.Load(4u));return;}
    RWStructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateWrite];
    uint4 meta=cache[0];
    uint active=meta.x,staging=active^1u;
    bool recache=meta.z==0u;
    bool entire=recache;
    RWByteAddressBuffer cutStatus=ResourceDescriptorHeap[outputControl];
    entire|=cutStatus.Load(12u)!=0u;
    StructuredBuffer<uint4> cut=ResourceDescriptorHeap[stateRead];
    uint stamp=cut[0].z+1u;
    RWStructuredBuffer<uint2> results=ResourceDescriptorHeap[cacheResults];
    for(uint i=0u;i<blockCount;++i)
        if(CacheSelected(i,stamp,entire)&&results[i].x!=results[i].y){
            InvalidateCache(3u);return;
        }
    StructuredBuffer<CutSpatialBlock> oldBlocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockRead(active))];
    RWByteAddressBuffer newAlloc=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheAllocator(staging))];
    uint freeCount=newAlloc.Load(4u);
    uint freeDesc=CacheFreeList(staging);
    if(!recache){
        for(uint i=0u;i<blockCount;++i){
            if(!CacheSelected(i,stamp,entire))continue;
            CutSpatialBlock old=oldBlocks[i];
            if(old.geometryState!=1u||old.activeCount==0u)continue;
            if(old.activeFirst>capacity||old.activeCount>capacity-old.activeFirst||
               !CacheRecycleRange(freeDesc,old.activeFirst,old.activeCount,freeCount)){
                InvalidateCache(4u);return;
            }
        }
    }
    newAlloc.Store(4u,freeCount);
    cache[0]=uint4(staging,meta.y+1u,1u,0u);
}


// 4d: publish one mesh-dispatch command per immutable source block after
// CommitCache has validated the whole generation. This runs AFTER all raster
// readers of the prior generation, on the same Direct queue. It does not
// touch the legacy full-soup draw command (fallback).
[numthreads(64,1,1)]
void BuildCacheDraw(uint3 id:SV_DispatchThreadID){
    const uint blockId=id.x;
    if(blockId>=blockCount)return;
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    StructuredBuffer<uint4> legacy=ResourceDescriptorHeap[stateRead];
    uint4 version=cache[0];
    // CacheCommit precedes the existing CommitCut. The next cut revision
    // must be exactly the cache revision we are preparing for raster.
    if(version.z!=1u||version.x>1u||
       version.y!=legacy[0].z+1u)return;
    uint descriptor=CacheBlockRead(version.x);
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(descriptor)];
    CutSpatialBlock block=blocks[blockId];
    uint count=block.geometryState==0u?block.triangleCount:
               block.geometryState==1u?block.activeCount:0u;
    RWStructuredBuffer<uint4> commands=ResourceDescriptorHeap[cacheDrawArguments];
    commands[blockId]=uint4(blockId,(count+20u)/21u,1u,1u);
}
