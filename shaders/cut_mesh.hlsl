// Step 4a: keep step-3 rendering, collisions and full-mesh publication.
// Only BVH-selected blocks run the expensive 26-plane clipping routine;
// untouched triangles still flow through the same double-buffered soup.
#include "debris_layout.hlsli"

#include "cut_surface.hlsli"
struct CutImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId,primitiveIndex,kind,recess;
};
cbuffer CutParameters:register(b0) {
    uint originalTriangles,triangleA,triangleB,triangleAWrite;
    uint triangleBWrite,stateRead,stateWrite,impactBuffer;
    uint impactControl,outputControl,dispatchArguments,drawArguments;
    uint originalCount,capacity,planeCount,blockCount;
    uint bvhNodes,bvhBlocks,candidateSelectionRead,candidateSelectionWrite;
    // 4b-only cache. No raster, collision or legacy CommitCut consumer.
    uint cachePoolRead,cachePoolWrite,cacheStateRead,cacheStateWrite;
    uint cacheBlocksRead0,cacheBlocksRead1,cacheBlocksWrite0,cacheBlocksWrite1;
    uint cacheAlloc0,cacheAlloc1,cacheFree0,cacheFree1;
    uint cacheResults,cacheWork,cacheDispatch,cacheFreeCapacity;
    uint cacheDrawArguments,shadowPredicates;
    float4 shadowPositions[4];
    uint4 laserWork;
    uint4 laserWalls;
    uint4 brushLog;
    float4 laserCenter,laserLower,laserUpper,gridOrigin,gridCellSize;
    float4 laserPlanes[28],laserAxes[28],laserLimits[28];
    uint4 cutDebris; // event UAV, simulation time and diagnostic enable
};
static const uint MaxPolygon=32;
static const uint PlaneCount=26;
static const uint FullCutSelection=0xffffffffu;
static const uint LaserSectionCapacity=768u;
static const uint3 WallGrid=uint3(4u,2u,4u);
float LaserSectionEpsilon(){
    // Keep a floor for floating-point precision in the shared scene space.
    return max(.000002f,.000025f*laserUpper.w);
}
struct CutBrush {
    float4 centerRadius;
    float3 lower;uint count;
    float3 upper;uint padding;
    float4 planes[28];
};
bool CutBoxOverlap(float3 lo,float3 hi,float3 queryLo,float3 queryHi){
    return all(lo<=queryHi)&&all(hi>=queryLo);
}
bool CutRequested(){
    if(laserWork.x!=0u)return true;
    RWByteAddressBuffer hits=ResourceDescriptorHeap[impactControl];
    return hits.Load(0u)!=0u&&hits.Load(4u)==0u&&hits.Load(8u)==0u;
}
bool LaserBoxOverlap(float3 lo,float3 hi){
    for(uint p=0u;p<laserWork.x;++p){
        const float4 plane=laserPlanes[p];
        const float3 nearest=min((lo-laserCenter.xyz)*plane.xyz,(hi-laserCenter.xyz)*plane.xyz);
        if(dot(nearest,1.f.xxx)>plane.w+.0002f)return false;
    }
    return true;
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
// Scratch belongs to one cut transaction. Publish only after cache and mesh agree.
void SampleRemoved(CutTriangle face,float3 center){
    if(cutDebris.x==0xffffffffu)return;
    const uint solid=(face.pad0>>16u)&3u;
    const bool classified=solid==1u||solid==2u;
    if(!classified&&laserWork.x!=0u)return;
    const uint slot=classified?solid:3u;
    RWStructuredBuffer<uint4> debris=ResourceDescriptorHeap[cutDebris.x];
    if(debris[slot].w!=0u)return;
    uint claimed;
    InterlockedCompareExchange(debris[slot].w,0u,face.material+1u,claimed);
    if(claimed==0u)debris[slot].xyz=asuint(center);
}

void PublishDebris(){
    if(cutDebris.x==0xffffffffu)return;
    RWStructuredBuffer<uint4> debris=ResourceDescriptorHeap[cutDebris.x];
    uint4 header=debris[0];
    const bool laser=laserWork.x!=0u;
    const uint before=header.x;
    [unroll]for(uint slot=1u;slot<DEBRIS_RING_BASE;++slot){
        if(slot==3u&&(laser||header.x!=before))continue;
        const uint4 sample=debris[slot];
        if(sample.w==0u||sample.w==0xffffffffu)continue;
        const uint address=DEBRIS_RING_BASE+(header.x&(DEBRIS_RING_SIZE-1u))*DEBRIS_RING_STRIDE;
        debris[address]=uint4(sample.xyz,cutDebris.y);
        debris[address+1u]=uint4(laser?slot:0u,sample.w-1u,0u,0u);
        ++header.x;
        if(laser)header[min(slot,2u)]=asuint(asfloat(cutDebris.y)+.036f);
    }
    debris[0]=header;
}

[numthreads(1,1,1)]
void BeginCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer dispatch=ResourceDescriptorHeap[dispatchArguments];
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint4 version=state[0];
    control.Store4(0,uint4(0,0,0,0));
    if(cutDebris.z!=0u)control.Store(16u,0u);
    if(cutDebris.x!=0xffffffffu){
        RWStructuredBuffer<uint4> debris=ResourceDescriptorHeap[cutDebris.x];
        const uint4 header=debris[0];
        const float now=asfloat(cutDebris.y);
        const bool laser=laserWork.x!=0u;
        debris[1]=uint4(0u,0u,0u,laser&&now<asfloat(header.y)?0xffffffffu:0u);
        debris[2]=uint4(0u,0u,0u,laser&&now<asfloat(header.z)?0xffffffffu:0u);
        if(!laser)debris[3]=0u;
    }
    if(laserWalls.w!=0u){
        RWByteAddressBuffer predicates=ResourceDescriptorHeap[shadowPredicates];
        [unroll]for(uint i=0u;i<14u;++i)predicates.Store4(i*16u,uint4(0u,0u,0u,0u));
    }
    if(laserWork.x!=0u){
        RWByteAddressBuffer counts=ResourceDescriptorHeap[laserWork.z];
        for(uint p=0u;p<laserWork.x;++p)counts.Store(p*4u,0u);
        RWByteAddressBuffer candidates=ResourceDescriptorHeap[brushLog.z];
        candidates.Store(0u,0u);
    }
    dispatch.Store3(0,uint3(0u,1u,1u));
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    uint4 cacheVersion=cache[0];
    bool ready=cacheVersion.z==1u&&cacheVersion.x<2u&&
               cacheVersion.y==version.z;
    if(!CutRequested())return;
    const bool historyFull=brushLog.x!=0xffffffffu&&version.z>=brushLog.y;
    if(cutDebris.z!=0u)control.Store(16u,1u);
    if(blockCount==0u||!ready||historyFull){
        if(cutDebris.z!=0u)control.Store(4u,historyFull?32u:64u);
        return;
    }

    control.Store(12u,FullCutSelection);
    dispatch.Store3(0,uint3(min(blockCount,65535u),
                            (blockCount+65534u)/65535u,1u));
    // A candidate mark must identify an ATTEMPT, not merely a successful
    // cut generation: misses can otherwise reuse old selection stamps.
    uint stamp=version.w+1u;
    if(stamp==0u)stamp=1u;
    RWStructuredBuffer<uint4> writableState=ResourceDescriptorHeap[stateWrite];
    writableState[0]=uint4(version.x,version.y,version.z,stamp);

    // Six axial clipping planes bound the entire 26-plane cut:
    // center +/- radius is conservative, a sphere test would miss corners.
    if(bvhNodes==0xffffffffu||bvhBlocks==0xffffffffu||
       candidateSelectionWrite==0xffffffffu)return;
    float3 queryLo=laserLower.xyz,queryHi=laserUpper.xyz;
    if(laserWork.x==0u){
        StructuredBuffer<CutImpact> impacts=ResourceDescriptorHeap[impactBuffer];
        const CutImpact impact=impacts[0];
        const float r=max(impact.radius,1e-6f)*(impact.recess!=0u?1.75f:1.f);
        queryLo=impact.position-r;queryHi=impact.position+r;
    }
    StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[bvhNodes];
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[bvhBlocks];
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateSelectionWrite];
    uint nodeCount,nodeStride,blockArrayCount,blockStride,entryCount,entryStride;
    nodes.GetDimensions(nodeCount,nodeStride);
    blocks.GetDimensions(blockArrayCount,blockStride);
    candidates.GetDimensions(entryCount,entryStride);
    if(nodeCount==0u||blockArrayCount!=blockCount||entryCount!=blockCount*2u)return;
    uint selected=0u;
    uint stack[64],stackSize=1u;
    stack[0]=0u;
    while(stackSize!=0u){
        uint nodeId=stack[--stackSize];
        if(nodeId>=nodeCount)return;
        CutSpatialNode node=nodes[nodeId];
        if(!CutBoxOverlap(node.lower,node.upper,queryLo,queryHi))continue;
        if(laserWork.x!=0u&&!LaserBoxOverlap(node.lower,node.upper))continue;
        if(node.rightChild!=0xffffffffu){
            if(stackSize>62u)return;
            stack[stackSize++]=node.rightChild;
            stack[stackSize++]=node.leftOrBlock;
        }else{
            uint blockId=node.leftOrBlock;
            if(blockId>=blockCount)return;
            CutSpatialBlock block=blocks[blockId];
            if(!CutBoxOverlap(block.lower,block.upper,queryLo,queryHi))continue;
            if(selected>=blockCount)return;
            candidates[blockId]=stamp;
            candidates[blockCount+selected]=blockId;
            ++selected;
        }
    }
    control.Store(12u,selected);
    dispatch.Store3(0,uint3(min(selected,65535u),
                            max(1u,(selected+65534u)/65535u),1u));
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
    if(blockCount!=0u&&candidateSelectionRead!=0xffffffffu){
        RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
        if(control.Load(12u)!=FullCutSelection&&tri.pad1<blockCount){
            StructuredBuffer<uint> marks=ResourceDescriptorHeap[candidateSelectionRead];
            candidate=marks[tri.pad1]==version.w;
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
    if(!CutRequested())return;
    uint count=counter.Load(0u);
    if(counter.Load(4u)!=0||counter.Load(8u)==0||count>capacity)return;
    StructuredBuffer<uint4> current=ResourceDescriptorHeap[stateRead];
    RWStructuredBuffer<uint4> state=ResourceDescriptorHeap[stateWrite];
    uint4 previous=current[0];
    // A valid cut must be backed by an already committed 4e cache range.
    // Cache allocation/validation failure rejects this shot atomically:
    // last visible geometry, collisions and generation remain unchanged.
    RWByteAddressBuffer cacheWorkBuffer=ResourceDescriptorHeap[cacheWork];
    if(cacheWorkBuffer.Load(0u)!=1u||cacheWorkBuffer.Load(4u)!=0u){
        if(cutDebris.z!=0u&&cacheWorkBuffer.Load(4u)==0u)counter.Store(4u,128u);
        return;
    }
    StructuredBuffer<uint4> cacheStatus=ResourceDescriptorHeap[cacheStateRead];
    uint4 cacheGeneration=cacheStatus[0];
    if(cacheGeneration.z!=1u||cacheGeneration.y!=previous.z+1u){
        if(cutDebris.z!=0u)counter.Store(4u,128u);
        return;
    }
    if(cutDebris.z!=0u)counter.Store(16u,2u);
    uint slot=previous.y==1u?2u:1u;
    float3 center;
    float radius;
    if(laserWork.x!=0u){
        const float3 lower=max(laserLower.xyz,gridOrigin.xyz);
        const float3 upper=min(laserUpper.xyz,gridOrigin.xyz+gridCellSize.xyz*float3(WallGrid));
        center=(lower+upper)*.5f;
        radius=max(.001f,max(upper.x-lower.x,max(upper.y-lower.y,upper.z-lower.z))*.5f);
    }else{
        StructuredBuffer<CutImpact> events=ResourceDescriptorHeap[impactBuffer];
        const CutImpact impact=events[0];
        center=impact.position;radius=impact.radius;
    }
    if(brushLog.x!=0xffffffffu){
        // Index and lifetime are coupled to the successful cache revision.
        // Failed attempts cannot subtract material from a later wall.
        RWStructuredBuffer<CutBrush> brushes=ResourceDescriptorHeap[brushLog.x];
        const uint brush=previous.z;
        const bool swept=laserWork.x!=0u;
        brushes[brush].centerRadius=swept?laserCenter:float4(center,max(radius,1.e-6f));
        const float3 lo=swept?laserLower.xyz:center-radius;
        const float3 hi=swept?laserUpper.xyz:center+radius;
        brushes[brush].lower=max(lo,gridOrigin.xyz)-.0002f;
        brushes[brush].upper=min(hi,gridOrigin.xyz+gridCellSize.xyz*float3(WallGrid))+.0002f;
        brushes[brush].count=laserWork.x;
        for(uint p=0u;p<laserWork.x;++p)brushes[brush].planes[p]=laserPlanes[p];
    }
    state[0]=uint4(count,slot,previous.z+1u,previous.w);
    PublishDebris();
    RWByteAddressBuffer args=ResourceDescriptorHeap[drawArguments];
    args.Store4(0,uint4(0u,(count+20u)/21u,1u,1u));
    RWByteAddressBuffer predicates=ResourceDescriptorHeap[shadowPredicates];
    [unroll]for(uint light=0u;light<4u;++light){
        if(shadowPositions[light].w==0.0)continue;
        float3 delta=center-shadowPositions[light].xyz;
        float extent=max(radius,1e-6)+1e-4*(1.0+length(delta));
        uint anyFace=0u;
        [unroll]for(uint face=0u;face<6u;++face){
            uint axis=face/2u;
            float axisDistance=delta[axis]*((face&1u)!=0u?-1.0:1.0);
            float side=max(abs(delta[(axis+1u)%3u]),abs(delta[(axis+2u)%3u]));
            uint overlaps=uint(axisDistance+extent>=0.0&&side<=axisDistance+2.0*extent);
            const uint offset=(light*7u+face)*8u;
            if(laserWalls.w==0u)overlaps|=predicates.Load(offset);
            predicates.Store2(offset,uint2(overlaps,0u));
            anyFace|=overlaps;
        }
        predicates.Store2((light*7u+6u)*8u,uint2(anyFace,0u));
    }
}


// 4e: persistent geometry is authoritative after the first successful cut.
// Source blocks use immutable original ranges, Modified blocks use disjoint
// pool allocations, and Empty blocks have no triangles. Failed staging never
// changes the active generation; the complete-soup path is no longer run.
uint CacheBlockRead(uint slot){return slot==0u?cacheBlocksRead0:cacheBlocksRead1;}
uint CacheBlockWrite(uint slot){return slot==0u?cacheBlocksWrite0:cacheBlocksWrite1;}
uint CacheAllocator(uint slot){return slot==0u?cacheAlloc0:cacheAlloc1;}
uint CacheFreeList(uint slot){return slot==0u?cacheFree0:cacheFree1;}

bool CacheSelected(uint blockId,uint stamp,bool entireSoup){
    if(entireSoup)return true;
    StructuredBuffer<uint> marks=ResourceDescriptorHeap[candidateSelectionRead];
    return marks[blockId]==stamp;
}
void RejectCacheShot(uint error){
    // Preserve the active generation and all live ranges. Failed staging
    // never invalidates the geometry that the previous frame published.
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    work.Store(4u,error);
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
void EmitWallToCells(float3 a,float3 b,float3 c,float3 normal,uint solid,uint outputDescriptor);

// Human-scale impacts on a solid front face leave a recessed entry. New
// surfaces use volume cells so later impacts can find them in the BVH.
void EmitProjectileRecess(PolyVertex aperture[MaxPolygon],uint count,CutTriangle face,
                          CutImpact hit,uint outputDescriptor){
    const uint solid=(face.pad0>>16u)&3u;
    if((solid!=1u&&solid!=2u)||laserWalls.x==0xffffffffu)return;
    const float3 facing=face.a.normal+face.b.normal+face.c.normal;
    if(dot(facing,hit.direction)>=-.25f)return;

    const float3 recess=hit.direction*(hit.radius*.75f);
    const float3 bottomNormal=normalize(facing);
    const float3 first=aperture[0].position+recess;
    for(uint i=1u;i+1u<count;++i)
        EmitWallToCells(first,aperture[i].position+recess,aperture[i+1u].position+recess,
                        bottomNormal,solid,outputDescriptor);

    // Only brush edges form walls. The internal seams of the source mesh
    // join the adjacent triangles at the same depth.
    const float tolerance=max(2.e-6f,hit.radius*.001f);
    for(uint i=0u;i<count;++i){
        const float3 a=aperture[i].position,b=aperture[(i+1u)%count].position;
        bool perimeter=false;
        for(uint k=0u;k<PlaneCount&&!perimeter;++k){
            const float3 plane=PlaneNormal(k);
            perimeter=abs(dot(plane,a-hit.position)-hit.radius)<=tolerance&&
                      abs(dot(plane,b-hit.position)-hit.radius)<=tolerance;
        }
        if(!perimeter)continue;
        const float3 normal=cross(recess,b-a);
        const float area=dot(normal,normal);
        if(area<=1.e-16f)continue;
        const float3 wallNormal=normal*rsqrt(area);
        EmitWallToCells(a,a+recess,b,wallNormal,solid,outputDescriptor);
        EmitWallToCells(b,a+recess,b+recess,wallNormal,solid,outputDescriptor);
    }
}

// Only candidate blocks contribute triangles to the new cache generation.
void EmitCutBlockTriangle(CutTriangle tri,CutImpact hit,uint outputDescriptor){
    float radius=max(hit.radius,1e-6f);
    float3 lower=min(tri.a.position,min(tri.b.position,tri.c.position));
    float3 upper=max(tri.a.position,max(tri.b.position,tri.c.position));
    if(any(lower>hit.position+radius)||any(upper<hit.position-radius)){
        EmitTriangle(ToPoly(tri.a),ToPoly(tri.b),ToPoly(tri.c),
                     tri.material,tri.instanceId,tri.pad0,tri.pad1,outputDescriptor);
        return;
    }
    PolyVertex inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    inside[0]=ToPoly(tri.a);inside[1]=ToPoly(tri.b);inside[2]=ToPoly(tri.c);
    uint intersection=3u;
    for(uint p=0u;p<PlaneCount&&intersection>=3u;++p){
        intersection=ClipPolygon(inside,intersection,PlaneNormal(p),hit.position,radius,true,next);
        for(uint k=0u;k<intersection;++k)inside[k]=next[k];
    }
    bool removesArea=false;
    for(uint k=1u;k+1u<intersection&&!removesArea;++k){
        const float3 area=cross(inside[k].position-inside[0].position,
                               inside[k+1u].position-inside[0].position);
        removesArea=dot(area,area)>1.e-16f;
        if(removesArea)SampleRemoved(tri,(inside[0].position+inside[k].position+inside[k+1u].position)/3.f);
    }
    if(!removesArea){
        EmitTriangle(ToPoly(tri.a),ToPoly(tri.b),ToPoly(tri.c),
                     tri.material,tri.instanceId,tri.pad0,tri.pad1,outputDescriptor);
        return;
    }
    inside[0]=ToPoly(tri.a);inside[1]=ToPoly(tri.b);inside[2]=ToPoly(tri.c);
    uint countInside=3u;
    for(uint p=0u;p<PlaneCount&&countInside>=3u;++p){
        float3 normal=PlaneNormal(p);
        uint outsideCount=ClipPolygon(inside,countInside,normal,hit.position,radius,false,outside);
        EmitPolygon(outside,outsideCount,tri.material,tri.instanceId,tri.pad0,
                    tri.pad1,outputDescriptor);
        uint nextCount=ClipPolygon(inside,countInside,normal,hit.position,radius,true,next);
        for(uint k=0u;k<nextCount;++k)inside[k]=next[k];
        countInside=nextCount;
    }
    if(hit.recess!=0u&&countInside>=3u)
        EmitProjectileRecess(inside,countInside,tri,hit,outputDescriptor);
    // The pre-clip checked every fan triangle. Plane clipping can
    // leave coincident leading vertices on a previously cut face.
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    uint old;control.InterlockedAdd(8u,1u,old);
}
void EmitLaserTriangle(CutTriangle tri,uint outputDescriptor){
    const PolyVertex a=ToPoly(tri.a),b=ToPoly(tri.b),c=ToPoly(tri.c);
    if(!CutBoxOverlap(min(a.position,min(b.position,c.position)),
                      max(a.position,max(b.position,c.position)),laserLower.xyz,laserUpper.xyz)){
        EmitTriangle(a,b,c,tri.material,tri.instanceId,tri.pad0,tri.pad1,outputDescriptor);
        return;
    }
    PolyVertex inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    inside[0]=a;inside[1]=b;inside[2]=c;
    uint count=3u;
    // Check that an area is actually removed before splitting an untouched
    // triangle. A coplanar wall already at the edge of the kerf survives.
    for(uint p=0u;p<laserWork.x&&count>=3u;++p){
        const float4 plane=laserPlanes[p];
        count=ClipPolygon(inside,count,plane.xyz,laserCenter.xyz,plane.w-LaserSectionEpsilon(),true,next);
        for(uint k=0u;k<count;++k)inside[k]=next[k];
    }
    bool removesArea=false;
    for(uint k=1u;k+1u<count&&!removesArea;++k){
        const float3 area=cross(inside[k].position-inside[0].position,
                               inside[k+1u].position-inside[0].position);
        removesArea=dot(area,area)>1.e-16f;
        if(removesArea)SampleRemoved(tri,(inside[0].position+inside[k].position+inside[k+1u].position)/3.f);
    }
    if(!removesArea){
        EmitTriangle(a,b,c,tri.material,tri.instanceId,tri.pad0,tri.pad1,outputDescriptor);
        return;
    }
    inside[0]=a;inside[1]=b;inside[2]=c;
    count=3u;
    for(uint p=0u;p<laserWork.x&&count>=3u;++p){
        const float4 plane=laserPlanes[p];
        const uint outsideCount=ClipPolygon(inside,count,plane.xyz,laserCenter.xyz,plane.w,false,outside);
        EmitPolygon(outside,outsideCount,tri.material,tri.instanceId,tri.pad0,tri.pad1,outputDescriptor);
        count=ClipPolygon(inside,count,plane.xyz,laserCenter.xyz,plane.w,true,next);
        for(uint k=0u;k<count;++k)inside[k]=next[k];
    }
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    uint old;control.InterlockedAdd(8u,1u,old);
}
uint CutSelectionCount(uint selection){
    return selection==FullCutSelection?blockCount:selection;
}
uint CutSelectionBlock(uint selection,uint index){
    if(selection==FullCutSelection)return index;
    StructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateSelectionRead];
    return candidates[blockCount+index];
}
[numthreads(64,1,1)]
void ClipCacheBlocks(uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    uint workIndex=group.x+group.y*65535u;
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    uint selection=control.Load(12u);
    if(workIndex>=CutSelectionCount(selection))return;
    uint blockId=CutSelectionBlock(selection,workIndex);
    StructuredBuffer<uint4> generations=ResourceDescriptorHeap[stateRead];
    uint4 cutVersion=generations[0];
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    uint4 cacheVersion=cache[0];
    if(cacheVersion.z!=1u||cacheVersion.x>1u||
       cacheVersion.y!=cutVersion.z){
        if(lane==0u){uint ignored;control.InterlockedOr(4u,2u,ignored);}
        return;
    }
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockRead(cacheVersion.x))];
    uint blockEntries,stride;
    blocks.GetDimensions(blockEntries,stride);
    if(blockId>=blockEntries){
        if(lane==0u){uint ignored;control.InterlockedOr(4u,2u,ignored);}
        return;
    }
    CutSpatialBlock block=blocks[blockId];
    if(block.geometryState==2u)return; // Empty
    bool modified=block.geometryState==1u;
    uint first=modified?block.activeFirst:block.firstTriangle;
    uint length=modified?block.activeCount:block.triangleCount;
    StructuredBuffer<CutTriangle> source=ResourceDescriptorHeap[
        NonUniformResourceIndex(modified?cachePoolRead:originalTriangles)];
    uint available,faceStride;
    source.GetDimensions(available,faceStride);
    if(block.geometryState>2u||first>available||length>available-first){
        if(lane==0u){uint ignored;control.InterlockedOr(4u,2u,ignored);}
        return;
    }
    uint destDescriptor=cutVersion.y==1u?triangleBWrite:triangleAWrite;
    [branch]if(laserWork.x!=0u){
        for(uint i=lane;i<length;i+=64u)EmitLaserTriangle(source[first+i],destDescriptor);
        return;
    }
    StructuredBuffer<CutImpact> impacts=ResourceDescriptorHeap[impactBuffer];
    CutImpact impact=impacts[0];
    for(uint i=lane;i<length;i+=64u){
        CutTriangle face=source[first+i];
        EmitCutBlockTriangle(face,impact,destDescriptor);
    }
}

// Sections of the original closed shells. Previously published brush
// volumes are subtracted from the new wall, including open projectile cuts.
struct LaserSection {
    float4 endpoints;
    uint type,padding;
};
float SectionHeight(LaserSection edge,float u){
    const float4 e=edge.endpoints;
    return lerp(e.y,e.w,saturate((u-e.x)/(e.z-e.x)));
}

[numthreads(64,1,1)]
void GatherLaserSections(uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    const uint blockId=group.x,side=group.y;
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    if(control.Load(12u)==0u||blockId>=blockCount||side>=laserWork.x)return;
    if(side==0u){
        StructuredBuffer<uint4> generation=ResourceDescriptorHeap[stateRead];
        const uint historyCount=generation[0].z;
        RWStructuredBuffer<CutBrush> brushes=ResourceDescriptorHeap[brushLog.x];
        RWByteAddressBuffer candidates=ResourceDescriptorHeap[brushLog.z];
        for(uint i=blockId*64u+lane;i<historyCount;i+=blockCount*64u){
            const float3 lo=brushes[i].lower,hi=brushes[i].upper;
            if(!CutBoxOverlap(lo,hi,laserLower.xyz,laserUpper.xyz)||!LaserBoxOverlap(lo,hi))continue;
            uint slot;
            candidates.InterlockedAdd(0u,1u,slot);
            candidates.Store((slot+1u)*4u,i);
        }
    }
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[bvhBlocks];
    const CutSpatialBlock block=blocks[blockId];
    if(block.geometryState>=2u)return;
    const float4 plane=laserPlanes[side],axis=laserAxes[side];
    const float3 lo=block.lower-laserCenter.xyz,hi=block.upper-laserCenter.xyz;
    const float lowerU=axis.w,upperU=laserLimits[side].x;
    const float distance=plane.w;
    if(dot(min(lo*plane.xyz,hi*plane.xyz),1.f.xxx)>distance||
       dot(max(lo*plane.xyz,hi*plane.xyz),1.f.xxx)<distance||
       dot(min(lo*axis.xyz,hi*axis.xyz),1.f.xxx)>upperU||
       dot(max(lo*axis.xyz,hi*axis.xyz),1.f.xxx)<lowerU)return;
    const uint first=block.firstTriangle,length=block.triangleCount;
    StructuredBuffer<CutTriangle> source=ResourceDescriptorHeap[originalTriangles];
    RWStructuredBuffer<LaserSection> sections=ResourceDescriptorHeap[laserWork.y];
    RWByteAddressBuffer counts=ResourceDescriptorHeap[laserWork.z];
    const float3 vAxis=cross(plane.xyz,axis.xyz);
    for(uint i=lane;i<length;i+=64u){
        const CutTriangle face=source[first+i];
        const uint solid=(face.pad0>>16u)&3u;
        if(solid==0u||solid>2u)continue;
        const float3 vertices[3]={face.a.position-laserCenter.xyz,
            face.b.position-laserCenter.xyz,face.c.position-laserCenter.xyz};
        const float3 d=float3(dot(vertices[0],plane.xyz),dot(vertices[1],plane.xyz),
                               dot(vertices[2],plane.xyz))-distance;
        float2 edges[2];
        uint written=0u;
        for(uint j=0u;j<3u;++j){
            const uint k=(j+1u)%3u;
            if((d[j]<=0.f)==(d[k]<=0.f))continue;
            const float3 at=lerp(vertices[j],vertices[k],d[j]/(d[j]-d[k]));
            edges[written++]=float2(dot(at,axis.xyz),dot(at,vAxis));
        }
        if(written!=2u)continue;
        float2 a=edges[0],b=edges[1];
        if(a.x>b.x){const float2 old=a;a=b;b=old;}
        if(b.x-a.x<=LaserSectionEpsilon()||a.x>=upperU||b.x<=lowerU)continue;
        const float slope=(b.y-a.y)/(b.x-a.x);
        const float left=max(a.x,lowerU),right=min(b.x,upperU);
        if(right-left<=LaserSectionEpsilon())continue;
        const float3 normal=cross(vertices[1]-vertices[0],vertices[2]-vertices[0]);
        const float direction=dot(normal,face.a.normal+face.b.normal+face.c.normal);
        const bool entry=dot(normal,vAxis)*(direction<0.f?-1.f:1.f)<0.f;
        uint ticket;
        counts.InterlockedAdd(side*4u,1u,ticket);
        if(ticket>=LaserSectionCapacity)continue;
        LaserSection result;
        result.endpoints=float4(left,a.y+(left-a.x)*slope,right,a.y+(right-a.x)*slope);
        result.type=solid*2u+uint(entry);result.padding=0u;
        sections[side*LaserSectionCapacity+ticket]=result;
    }
}

[numthreads(1,1,1)]
void BuildLaserWallArgs(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer counts=ResourceDescriptorHeap[laserWork.z];
    RWByteAddressBuffer args=ResourceDescriptorHeap[laserWork.w];
    args.Store3(0u,uint3(0u,1u,1u));
    if(control.Load(4u)!=0u||control.Load(8u)==0u)return;
    uint maxCount=0u;
    for(uint p=0u;p<laserWork.x;++p)maxCount=max(maxCount,counts.Load(p*4u));
    if(maxCount>LaserSectionCapacity){control.Store(4u,4u);return;}
    args.Store3(0u,uint3(maxCount*2u,laserWork.x,1u));
}

uint OrderedFloat(float v){
    const uint bits=asuint(v);
    return (bits&0x80000000u)!=0u?~bits:bits^0x80000000u;
}
float FromOrderedFloat(uint bits){
    return asfloat((bits&0x80000000u)!=0u?bits^0x80000000u:~bits);
}
static const uint MaxWallCrossings=128u;
groupshared uint wallRight,wallDuplicate,wallCount;
groupshared uint wallIndices[MaxWallCrossings];
groupshared float wallHeights[MaxWallCrossings];

uint ClipWall(float3 source[MaxPolygon],uint count,float4 plane,float3 center,
              bool inside,out float3 result[MaxPolygon]){
    uint written=0u;
    for(uint i=0u;i<count;++i){
        const float3 a=source[i],b=source[(i+1u)%count];
        const float da=dot(plane.xyz,a-center)-plane.w;
        const float db=dot(plane.xyz,b-center)-plane.w;
        const bool keepA=inside?da<=0.f:da>0.f,keepB=inside?db<=0.f:db>0.f;
        if(keepA&&written<MaxPolygon)result[written++]=a;
        if(keepA!=keepB&&written<MaxPolygon)result[written++]=lerp(a,b,saturate(da/(da-db)));
    }
    return written;
}
bool WallHasArea(float3 a,float3 b,float3 c){
    const float3 n=cross(b-a,c-a);
    return dot(n,n)>1.e-16f;
}
PolyVertex WallVertex(float3 position,float3 normal){
    PolyVertex result;
    result.position=position;
    result.normal=normal;result.uv=position.xz;result.uv1=result.uv;
    return result;
}
void EmitWallToCells(float3 a,float3 b,float3 c,float3 normal,uint solid,uint outputDescriptor){
    const float3 lower=min(a,min(b,c)),upper=max(a,max(b,c));
    const float3 maxCell=float3(WallGrid)-1.f;
    const uint3 first=uint3(clamp(floor((lower-gridOrigin.xyz)/gridCellSize.xyz),0.f,maxCell));
    const uint3 last=uint3(clamp(floor((upper-gridOrigin.xyz)/gridCellSize.xyz),0.f,maxCell));
    const uint material=solid==1u?laserWalls.y:laserWalls.z;
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[bvhBlocks];
    float3 poly[MaxPolygon],next[MaxPolygon];
    for(uint z=first.z;z<=last.z;++z)for(uint y=first.y;y<=last.y;++y)
        for(uint x=first.x;x<=last.x;++x){
            const float3 cell=gridOrigin.xyz+gridCellSize.xyz*float3(x,y,z);
            const uint blockId=laserWalls.x+(z*WallGrid.y+y)*WallGrid.x+x;
            poly[0]=a;poly[1]=b;poly[2]=c;
            uint size=3u;
            for(uint p=0u;p<6u&&size>=3u;++p){
                const float extent=(p&1u)==0u?gridCellSize[p/2u]:0.f;
                size=ClipWall(poly,size,float4(PlaneNormal(p),extent),cell,true,next);
                for(uint i=0u;i<size;++i)poly[i]=next[i];
            }
            if(size<3u)continue;
            const uint owner=blocks[blockId].instanceId;
            const PolyVertex firstVertex=WallVertex(poly[0],normal);
            for(uint i=1u;i+1u<size;++i)
                EmitTriangle(firstVertex,WallVertex(poly[i],normal),WallVertex(poly[i+1u],normal),
                             material,owner,solid<<16u,blockId,outputDescriptor);
        }
}
float4 LoggedPlane(RWStructuredBuffer<CutBrush> brushes,uint index,uint side,uint count,float radius){
    float4 plane=count!=0u?brushes[index].planes[side]:float4(PlaneNormal(side),radius);
    // A coplanar wall that was already generated belongs to the old brush.
    plane.w+=.000008f;
    return plane;
}
struct WallFragment {float3 a,b,c;uint nextBrush;};
void EmitNewWall(float3 a,float3 b,float3 c,float3 normal,uint solid,uint outputDescriptor){
    if(!WallHasArea(a,b,c))return;
    RWByteAddressBuffer candidates=ResourceDescriptorHeap[brushLog.z];
    const uint count=candidates.Load(0u);
    if(count==0u){EmitWallToCells(a,b,c,normal,solid,outputDescriptor);return;}
    RWStructuredBuffer<CutBrush> brushes=ResourceDescriptorHeap[brushLog.x];
    static const uint MaxFragments=40u;
    WallFragment pending[MaxFragments];
    WallFragment initial;
    initial.a=a;initial.b=b;initial.c=c;initial.nextBrush=0u;
    pending[0]=initial;
    uint size=1u,iterations=0u;
    float3 inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    while(size!=0u){
        if(++iterations>384u)break;
        const WallFragment face=pending[--size];
        const float3 lo=min(face.a,min(face.b,face.c)),hi=max(face.a,max(face.b,face.c));
        bool changed=false;
        bool overflow=false;
        for(uint k=face.nextBrush;k<count;++k){
            const uint index=candidates.Load((k+1u)*4u);
            if(!CutBoxOverlap(lo,hi,brushes[index].lower,brushes[index].upper))continue;
            const float4 center=brushes[index].centerRadius;
            const uint planeCount=brushes[index].count,planes=planeCount==0u?PlaneCount:planeCount;
            inside[0]=face.a;inside[1]=face.b;inside[2]=face.c;
            uint length=3u;
            for(uint p=0u;p<planes&&length>=3u;++p){
                const float4 plane=LoggedPlane(brushes,index,p,planeCount,center.w);
                length=ClipWall(inside,length,plane,center.xyz,true,next);
                for(uint i=0u;i<length;++i)inside[i]=next[i];
            }
            bool area=false;
            for(uint i=1u;i+1u<length&&!area;++i)
                area=WallHasArea(inside[0],inside[i],inside[i+1u]);
            if(!area)continue;
            changed=true;
            inside[0]=face.a;inside[1]=face.b;inside[2]=face.c;
            length=3u;
            for(uint p=0u;p<planes&&length>=3u&&!overflow;++p){
                const float4 plane=LoggedPlane(brushes,index,p,planeCount,center.w);
                const uint kept=ClipWall(inside,length,plane,center.xyz,false,outside);
                for(uint i=1u;i+1u<kept;++i){
                    if(!WallHasArea(outside[0],outside[i],outside[i+1u]))continue;
                    if(size==MaxFragments){overflow=true;break;}
                    WallFragment fragment;
                    fragment.a=outside[0];fragment.b=outside[i];fragment.c=outside[i+1u];
                    fragment.nextBrush=k+1u;
                    pending[size++]=fragment;
                }
                length=ClipWall(inside,length,plane,center.xyz,true,next);
                for(uint i=0u;i<length;++i)inside[i]=next[i];
            }
            break;
        }
        if(overflow)break;
        if(!changed)EmitWallToCells(face.a,face.b,face.c,normal,solid,outputDescriptor);
        if(size==0u)return;
    }
    // Bounded work and scratch are part of the same atomic cut attempt.
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    uint ignored;control.InterlockedOr(4u,8u,ignored);
}
void WriteWallStrip(uint side,float left,float right,LaserSection a,LaserSection b,
                    uint outputDescriptor){
    const float4 plane=laserPlanes[side];
    const float3 u=laserAxes[side].xyz,v=cross(plane.xyz,u);
    const float3 anchor=laserCenter.xyz+plane.w*plane.xyz;
    float3 wall[MaxPolygon],next[MaxPolygon];
    wall[0]=anchor+left*u+SectionHeight(a,left)*v;
    wall[1]=anchor+left*u+SectionHeight(b,left)*v;
    wall[2]=anchor+right*u+SectionHeight(b,right)*v;
    wall[3]=anchor+right*u+SectionHeight(a,right)*v;
    uint count=4u;
    for(uint p=0u;p<laserWork.x&&count>=3u;++p){
        if(p==side)continue;
        count=ClipWall(wall,count,laserPlanes[p],laserCenter.xyz,true,next);
        for(uint k=0u;k<count;++k)wall[k]=next[k];
    }
    for(uint i=1u;i+1u<count;++i)
        EmitNewWall(wall[0],wall[i],wall[i+1u],-plane.xyz,a.type/2u,outputDescriptor);
}

// One group per distinct U endpoint. Inside each strip, neighboring
// entry/exit edges of the same shell type form a planar trapezoid.
[numthreads(64,1,1)]
void EmitLaserWalls(uint lane:SV_GroupIndex,uint3 group:SV_GroupID){
    const uint side=group.y,index=group.x;
    RWByteAddressBuffer counts=ResourceDescriptorHeap[laserWork.z];
    const uint count=counts.Load(side*4u);
    if(index>=count*2u)return;
    RWStructuredBuffer<LaserSection> sections=ResourceDescriptorHeap[laserWork.y];
    const uint base=side*LaserSectionCapacity;
    const float4 initial=sections[base+index/2u].endpoints;
    const float left=(index&1u)!=0u?initial.z:initial.x;
    if(lane==0u){wallRight=OrderedFloat(laserLimits[side].x);wallDuplicate=0u;wallCount=0u;}
    GroupMemoryBarrierWithGroupSync();
    for(uint i=lane;i<count;i+=64u){
        const float4 e=sections[base+i].endpoints;
        if(e.x>left+LaserSectionEpsilon())InterlockedMin(wallRight,OrderedFloat(e.x));
        else if(abs(e.x-left)<=LaserSectionEpsilon()&&i*2u<index)InterlockedOr(wallDuplicate,1u);
        if(e.z>left+LaserSectionEpsilon())InterlockedMin(wallRight,OrderedFloat(e.z));
        else if(abs(e.z-left)<=LaserSectionEpsilon()&&i*2u+1u<index)InterlockedOr(wallDuplicate,1u);
    }
    GroupMemoryBarrierWithGroupSync();
    const float right=FromOrderedFloat(wallRight);
    if(wallDuplicate!=0u||right-left<=LaserSectionEpsilon())return;
    const float mid=(left+right)*.5f;
    for(uint i=lane;i<count;i+=64u){
        const LaserSection edge=sections[base+i];
        if(mid<=edge.endpoints.x||mid>=edge.endpoints.z)continue;
        uint slot;
        InterlockedAdd(wallCount,1u,slot);
        if(slot>=MaxWallCrossings)continue;
        wallIndices[slot]=i;
        wallHeights[slot]=SectionHeight(edge,mid);
    }
    GroupMemoryBarrierWithGroupSync();
    if(wallCount>MaxWallCrossings){
        if(lane==0u){
            RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
            uint ignored;control.InterlockedOr(4u,16u,ignored);
        }
        return;
    }
    StructuredBuffer<uint4> generation=ResourceDescriptorHeap[stateRead];
    const uint destination=generation[0].y==1u?triangleBWrite:triangleAWrite;
    for(uint i=lane;i<wallCount;i+=64u){
        const LaserSection entry=sections[base+wallIndices[i]];
        if((entry.type&1u)==0u)continue;
        const float low=wallHeights[i];
        float closest=3.4e38f;
        uint next=0xffffffffu;
        for(uint j=0u;j<wallCount;++j){
            const float depth=wallHeights[j];
            if(depth<=low+LaserSectionEpsilon()||depth>=closest)continue;
            const uint type=sections[base+wallIndices[j]].type;
            if(type/2u!=entry.type/2u)continue;
            closest=depth;next=j;
        }
        if(next==0xffffffffu)continue;
        const LaserSection exit=sections[base+wallIndices[next]];
        if((exit.type&1u)!=0u)continue;
        WriteWallStrip(side,left,right,entry,exit,destination);
    }
}

// The local scratch count may be zero when a candidate block becomes Empty.
// Final global cache metadata, not the scratch, defines the visible world.

// Runs after the legacy cutter has finished its UAV writes, but BEFORE its
// existing CommitCut. Compute an indirect work size from the actual output.
[numthreads(1,1,1)]
void BeginCache(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[cacheWork];
    RWByteAddressBuffer args=ResourceDescriptorHeap[cacheDispatch];
    work.Store4(0u,uint4(0u,0u,0u,0u));
    if(cutDebris.z!=0u)work.Store4(16u,0u);
    args.Store3(0u,uint3(0u,1u,1u));
    RWByteAddressBuffer cut=ResourceDescriptorHeap[outputControl];
    uint triangles=cut.Load(0u);
    if(!CutRequested()||
       cut.Load(4u)!=0u||cut.Load(8u)==0u||triangles>capacity)return;
    RWStructuredBuffer<uint2> result=ResourceDescriptorHeap[cacheResults];
    uint selection=cut.Load(12u);
    for(uint i=0u;i<CutSelectionCount(selection);++i)
        result[CutSelectionBlock(selection,i)]=uint2(0u,0u);
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
    entire|=cutStatus.Load(12u)==FullCutSelection;
    if(!CacheSelected(blockId,version.w,entire))return;
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
    entire|=cutStatus.Load(12u)==FullCutSelection;
    StructuredBuffer<uint4> cut=ResourceDescriptorHeap[stateRead];
    uint stamp=cut[0].w;
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
                if(cutDebris.z!=0u){
                    uint total=capacity-bump,largest=total;
                    for(uint j=0u;j<freeCount;++j){
                        const uint length=newFree[j].y;
                        total+=length;
                        largest=max(largest,length);
                    }
                    work.Store4(16u,uint4(needed,capacity,total,largest));
                }
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
    entire|=cutStatus.Load(12u)==FullCutSelection;
    if(!CacheSelected(blockId,version.w,entire))return;
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
    if(work.Load(4u)!=0u)return;
    RWStructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateWrite];
    uint4 meta=cache[0];
    uint active=meta.x,staging=active^1u;
    bool recache=meta.z==0u;
    RWByteAddressBuffer cutStatus=ResourceDescriptorHeap[outputControl];
    uint selection=recache?FullCutSelection:cutStatus.Load(12u);
    uint selectedCount=CutSelectionCount(selection);
    RWStructuredBuffer<uint2> results=ResourceDescriptorHeap[cacheResults];
    for(uint i=0u;i<selectedCount;++i){
        uint blockId=CutSelectionBlock(selection,i);
        if(results[blockId].x!=results[blockId].y){
            RejectCacheShot(3u);return;
        }
    }
    StructuredBuffer<CutSpatialBlock> oldBlocks=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheBlockRead(active))];
    RWByteAddressBuffer newAlloc=ResourceDescriptorHeap[
        NonUniformResourceIndex(CacheAllocator(staging))];
    uint freeCount=newAlloc.Load(4u);
    uint freeDesc=CacheFreeList(staging);
    if(!recache){
        for(uint i=0u;i<selectedCount;++i){
            uint blockId=CutSelectionBlock(selection,i);
            CutSpatialBlock old=oldBlocks[blockId];
            if(old.geometryState!=1u||old.activeCount==0u)continue;
            if(old.activeFirst>capacity||old.activeCount>capacity-old.activeFirst||
               !CacheRecycleRange(freeDesc,old.activeFirst,old.activeCount,freeCount)){
                RejectCacheShot(4u);return;
            }
        }
    }
    uint bump=newAlloc.Load(0u);
    RWStructuredBuffer<uint2> free=ResourceDescriptorHeap[NonUniformResourceIndex(freeDesc)];
    while(freeCount!=0u){
        const uint2 last=free[freeCount-1u];
        if(last.x+last.y!=bump)break;
        bump=last.x;
        --freeCount;
    }
    newAlloc.Store2(0u,uint2(bump,freeCount));
    cache[0]=uint4(staging,meta.y+1u,1u,0u);
}


// 4d: two root constants (block ID and block triangle count) plus
// DispatchMesh. The count travels with the command; mesh output counts never
// need a GPU metadata lookup ahead of SetMeshOutputCounts.
// Execute only after both cache and original cut commits on the Direct queue.
[numthreads(64,1,1)]
void BuildCacheDraw(uint3 id:SV_DispatchThreadID){
    uint blockId=id.x;
    if(blockId>=blockCount)return;
    StructuredBuffer<uint4> cache=ResourceDescriptorHeap[cacheStateRead];
    StructuredBuffer<uint4> currentCut=ResourceDescriptorHeap[stateRead];
    uint4 cacheVersion=cache[0],cutVersion=currentCut[0];
    bool valid=cacheVersion.x<2u&&cacheVersion.z==1u&&
        cutVersion.y!=0u&&cacheVersion.y==cutVersion.z;

    uint count=0u;
    if(valid){
        uint descriptor=CacheBlockRead(cacheVersion.x);
        StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
            NonUniformResourceIndex(descriptor)];
        CutSpatialBlock block=blocks[blockId];
        count=block.geometryState==0u?block.triangleCount:
              block.geometryState==1u?block.activeCount:0u;
    }
    RWByteAddressBuffer commands=ResourceDescriptorHeap[cacheDrawArguments];
    uint offset=blockId*20u;
    commands.Store2(offset,uint2(blockId,count));
    commands.Store3(offset+8u,uint3((count+20u)/21u,1u,1u));

    // Both draw paths are GPU-gated with the same generation. Never emit
    // the global soup and block commands for the same cut revision.
    if(blockId==0u&&valid){
        RWByteAddressBuffer globalDraw=ResourceDescriptorHeap[drawArguments];
        globalDraw.Store(4u,0u);
    }
}
