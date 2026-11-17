// Persistent GPU cutFace soup.  One impact rebuilds the current geometry;
// no fragment-time sphere list, spatial pages, or CPU physics/readback.
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
    uint originalTriangles,poolRead,poolWrite,stateRead;
    uint stateWrite,blocksRead0,blocksRead1,blocksWrite0;
    uint blocksWrite1,nodesRead,impactsRead,impactControl;
    uint candidateList,blockResults,dispatchArgs,controlBuffer;
    uint allocator0,allocator1,freeRanges0,freeRanges1;
    uint pendingBuffer,pendingControl,instanceFlags;
    uint commandActive,commandPrepared,commandCount;
    uint originalCount,poolCapacity,blockCount,instanceCount;
    uint freeRangeCapacity,pendingCapacity,impactCapacity,recordHits;
};
static const uint MaxPolygon=32;
static const uint PlaneCount=26;
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
// GPU cut publication ABI. The active metadata, allocator, and draw commands
// are read-only until the end-of-frame Direct queue publication.
struct CutSpatialNode {
    float3 lower;uint leftOrBlock;
    float3 upper;uint rightChild;
};
struct CutSpatialBlock {
    float3 lower;uint firstTriangle;
    float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
};
// expected, removed, written, reserved. Indexed by persistent block ID.
static const uint SourceBlock=0u,ModifiedBlock=1u,EmptyBlock=2u;
static const uint InvalidCut=0xffffffffu;

uint ActiveBlockRead(uint slot){return slot==0u?blocksRead0:blocksRead1;}
uint StageBlockWrite(uint slot){return slot==0u?blocksWrite0:blocksWrite1;}
uint ActiveAllocator(uint slot){return slot==0u?allocator0:allocator1;}
uint ActiveFreeRanges(uint slot){return slot==0u?freeRanges0:freeRanges1;}

bool CutBoxOverlap(float3 a,float3 b,float3 c,float3 d){
    return all(a<=d)&&all(b>=c);
}
bool ValidCutFace(PolyVertex a,PolyVertex b,PolyVertex c){
    float3 n=cross(b.position-a.position,c.position-a.position);
    return dot(n,n)>1e-16f;
}
void AppendFace(PolyVertex a,PolyVertex b,PolyVertex c,
                uint material,uint instanceId,uint flags,
                bool write,uint first,inout uint count){
    if(!ValidCutFace(a,b,c))return;
    if(write){
        RWStructuredBuffer<CutTriangle> pool=ResourceDescriptorHeap[poolWrite];
        CutTriangle face;
        face.a=ToVertex(a);face.b=ToVertex(b);face.c=ToVertex(c);
        face.material=material;face.instanceId=instanceId;face.pad0=flags;face.pad1=0u;
        pool[first+count]=face;
    }
    ++count;
}
void AppendPoly(PolyVertex poly[MaxPolygon],uint n,
                uint material,uint instanceId,uint flags,
                bool write,uint first,inout uint count){
    if(n<3u)return;
    for(uint i=1u;i+1u<n;++i)
        AppendFace(poly[0],poly[i],poly[i+1u],material,instanceId,flags,
                   write,first,count);
}
// Identical supporting planes, clipping order, winding and interpolation to
// the previous whole-mesh implementation. Count and emit share this function.
uint ClipCutFace(CutTriangle face,CutImpact hit,bool write,uint first,out uint removed){
    removed=0u;
    uint count=0u;
    float radius=max(hit.radius,1e-6f);
    float3 lo=min(face.a.position,min(face.b.position,face.c.position));
    float3 hi=max(face.a.position,max(face.b.position,face.c.position));
    if(any(lo>hit.position+radius)||any(hi<hit.position-radius)){
        AppendFace(ToPoly(face.a),ToPoly(face.b),ToPoly(face.c),
                   face.material,face.instanceId,face.pad0,write,first,count);
        return count;
    }
    PolyVertex inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    inside[0]=ToPoly(face.a);inside[1]=ToPoly(face.b);inside[2]=ToPoly(face.c);
    uint insideCount=3u;
    for(uint p=0u;p<PlaneCount&&insideCount>=3u;++p){
        float3 normal=PlaneNormal(p);
        uint outsideCount=ClipPolygon(inside,insideCount,normal,hit.position,radius,false,outside);
        AppendPoly(outside,outsideCount,face.material,face.instanceId,face.pad0,
                   write,first,count);
        uint nextCount=ClipPolygon(inside,insideCount,normal,hit.position,radius,true,next);
        for(uint k=0u;k<nextCount;++k)inside[k]=next[k];
        insideCount=nextCount;
    }
    if(insideCount>=3u&&ValidCutFace(inside[0],inside[1],inside[2]))removed=1u;
    return count;
}
// Stable, coalescing first-fit allocator. Only the single-thread reserve and
// commit kernels modify the staging allocator. Old live ranges are not reusable
// until AFTER every new triangle has been written and validated.
bool ReserveRange(uint descriptor,uint requested,inout uint bump,
                  inout uint freeCount,out uint first){
    RWStructuredBuffer<uint2> free=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    for(uint i=0u;i<freeCount;++i){
        uint2 range=free[i];
        if(range.y<requested)continue;
        first=range.x;
        if(range.y==requested){
            for(uint j=i;j+1u<freeCount;++j)free[j]=free[j+1u];
            --freeCount;
        }else free[i]=uint2(range.x+requested,range.y-requested);
        return true;
    }
    if(bump>poolCapacity||requested>poolCapacity-bump)return false;
    first=bump;bump+=requested;return true;
}
bool RecycleRange(uint descriptor,uint first,uint count,inout uint freeCount){
    if(count==0u)return true;
    if(freeCount>=freeRangeCapacity)return false;
    RWStructuredBuffer<uint2> free=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    uint at=0u;
    while(at<freeCount&&free[at].x<first)++at;
    for(uint i=freeCount;i>at;--i)free[i]=free[i-1u];
    free[at]=uint2(first,count);++freeCount;
    // Merge all adjacent or overlapping intervals to keep the number of
    // free ranges bounded by the number of still-live allocations + 1.
    for(uint i=0u;i+1u<freeCount;){
        uint2 a=free[i],b=free[i+1u];
        if(a.x+a.y>=b.x){
            free[i]=uint2(a.x,max(a.x+a.y,b.x+b.y)-a.x);
            for(uint j=i+1u;j+1u<freeCount;++j)free[j]=free[j+1u];
            --freeCount;
        }else ++i;
    }
    return true;
}

// Queue new impacts before selecting the head. A failed reservation retains
// its impact; later frames attempt it again, without CPU round-trips.
[numthreads(1,1,1)]
void BeginCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer state=ResourceDescriptorHeap[pendingControl];
    RWStructuredBuffer<CutImpact> pending=ResourceDescriptorHeap[pendingBuffer];
    RWByteAddressBuffer step=ResourceDescriptorHeap[impactControl];
    StructuredBuffer<CutImpact> hits=ResourceDescriptorHeap[impactsRead];
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    RWByteAddressBuffer args=ResourceDescriptorHeap[dispatchArgs];
    uint4 queue=state.Load4(0u); // head, tail, size, overflow
    if(recordHits!=0u&&step.Load(4u)==0u&&step.Load(8u)==0u){
        uint incoming=min(step.Load(0u),impactCapacity);
        for(uint i=0u;i<incoming;++i){
            if(queue.z>=pendingCapacity){queue.w|=0x80000000u;break;}
            pending[queue.y]=hits[i];
            queue.y=(queue.y+1u)%pendingCapacity;
            ++queue.z;
        }
    }
    // Failed reservations retain their impacts but do not rerun the
    // expensive clipping passes every single VR frame.
    uint cooldown=queue.w&0xffffu;
    if(cooldown!=0u)queue.w=(queue.w&0xffff0000u)|(cooldown-1u);
    state.Store4(0u,queue);
    work.Store4(0u,uint4(0u,queue.z!=0u&&cooldown==0u?1u:0u,0u,0u));
    args.Store3(0u,uint3(0u,1u,1u));
    if(queue.z==0u||cooldown!=0u)return;

    CutImpact hit=pending[queue.x];
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateList];
    StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[nodesRead];
    StructuredBuffer<uint4> current=ResourceDescriptorHeap[stateRead];
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[NonUniformResourceIndex(
        ActiveBlockRead(current[0].y))];
    uint nodeCount,nodeStride;
    nodes.GetDimensions(nodeCount,nodeStride);
    if(nodeCount==0u||blockCount==0u){work.Store(8u,1u);return;}
    float r=max(hit.radius,1e-6f);
    float3 lower=hit.position-r,upper=hit.position+r;
    uint stack[64],top=1u,count=0u;
    stack[0]=0u;
    while(top!=0u){
        uint nodeIndex=stack[--top];
        if(nodeIndex>=nodeCount){work.Store(8u,1u);return;}
        CutSpatialNode node=nodes[nodeIndex];
        if(!CutBoxOverlap(node.lower,node.upper,lower,upper))continue;
        if(node.rightChild!=InvalidCut){
            if(top>62u){work.Store(8u,1u);return;}
            stack[top++]=node.rightChild;
            stack[top++]=node.leftOrBlock;
        }else{
            if(node.leftOrBlock>=blockCount){work.Store(8u,1u);return;}
            CutSpatialBlock block=blocks[node.leftOrBlock];
            if(block.geometryState==EmptyBlock)continue;
            if(count>=blockCount){work.Store(8u,1u);return;}
            candidates[count++]=node.leftOrBlock;
        }
    }
    work.Store(0u,count);
    args.Store3(0u,uint3(count,1u,1u));
}

// Copy only small metadata, flags, allocation bookkeeping and indirect args;
// no triangle from an unaffected block is copied.
[numthreads(64,1,1)]
void CopyCutState(uint3 tid:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    if(work.Load(4u)==0u||work.Load(8u)!=0u)return;
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint active=state[0].y,staging=active^1u;
    StructuredBuffer<CutSpatialBlock> src=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveBlockRead(active))];
    RWStructuredBuffer<CutSpatialBlock> dst=ResourceDescriptorHeap[NonUniformResourceIndex(StageBlockWrite(staging))];
    RWStructuredBuffer<uint4> results=ResourceDescriptorHeap[blockResults];
    RWStructuredBuffer<uint2> flags=ResourceDescriptorHeap[instanceFlags];
    RWByteAddressBuffer oldAlloc=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveAllocator(active))];
    RWByteAddressBuffer newAlloc=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveAllocator(staging))];
    RWStructuredBuffer<uint2> oldFree=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveFreeRanges(active))];
    RWStructuredBuffer<uint2> newFree=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveFreeRanges(staging))];
    uint i=tid.x;
    if(i<blockCount){
        dst[i]=src[i];
        results[i]=uint4(0u,0u,0u,0u);
    }
    if(i<instanceCount){
        uint2 pair=flags[i];
        if(staging==0u)pair.x=pair.y;else pair.y=pair.x;
        flags[i]=pair;
    }
    if(i==0u)newAlloc.Store4(0u,oldAlloc.Load4(0u));
    uint activeFreeCount=oldAlloc.Load(4u);
    if(i<activeFreeCount&&i<freeRangeCapacity)newFree[i]=oldFree[i];
}

// Each dispatched group counts the complete result of one candidate block;
// Source and Modified use different ranges. Empty is always surface-free.
[numthreads(64,1,1)]
void CountCutTriangles(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    if(work.Load(8u)!=0u)return;
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateList];
    uint blockId=candidates[group.x];
    if(blockId>=blockCount){uint was;work.InterlockedOr(8u,1u,was);return;}
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[NonUniformResourceIndex(
        ActiveBlockRead(state[0].y))];
    CutSpatialBlock block=blocks[blockId];
    if(block.geometryState==EmptyBlock)return;
    uint first=block.geometryState==SourceBlock?block.firstTriangle:block.activeFirst;
    uint length=block.geometryState==SourceBlock?block.triangleCount:block.activeCount;
    if((block.geometryState==SourceBlock&&
        (first>originalCount||length>originalCount-first))||
       (block.geometryState==ModifiedBlock&&
        (first>poolCapacity||length>poolCapacity-first))){
        uint was;work.InterlockedOr(8u,1u,was);return;
    }
    StructuredBuffer<CutTriangle> src=ResourceDescriptorHeap[NonUniformResourceIndex(
        block.geometryState==SourceBlock?originalTriangles:poolRead)];
    RWByteAddressBuffer pendingCtl=ResourceDescriptorHeap[pendingControl];
    RWStructuredBuffer<CutImpact> pending=ResourceDescriptorHeap[pendingBuffer];
    CutImpact hit=pending[pendingCtl.Load(0u)];
    uint localExpected=0u,localRemoved=0u;
    for(uint i=lane;i<length;i+=64u){
        uint removed;
        localExpected+=ClipCutFace(src[first+i],hit,false,0u,removed);
        localRemoved+=removed;
    }
    RWStructuredBuffer<uint4> results=ResourceDescriptorHeap[blockResults];
    uint old;
    InterlockedAdd(results[blockId].x,localExpected,old);
    if(old>InvalidCut-localExpected){uint was;work.InterlockedOr(8u,1u,was);}
    InterlockedAdd(results[blockId].y,localRemoved,old);
}

// Reservation is transactional: only staging allocator/list and staging blocks
// change. Freeing previous live ranges is forbidden until writing succeeds.
[numthreads(1,1,1)]
void ReserveCutBlocks(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    if(work.Load(4u)==0u||work.Load(8u)!=0u)return;
    uint candidatesCount=work.Load(0u);
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint active=state[0].y,staging=active^1u;
    RWStructuredBuffer<CutSpatialBlock> stage=ResourceDescriptorHeap[NonUniformResourceIndex(StageBlockWrite(staging))];
    RWStructuredBuffer<uint4> results=ResourceDescriptorHeap[blockResults];
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateList];
    RWStructuredBuffer<uint2> flags=ResourceDescriptorHeap[instanceFlags];
    RWByteAddressBuffer allocator=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveAllocator(staging))];
    uint freeDescriptor=ActiveFreeRanges(staging);
    uint bump=allocator.Load(0u),freeCount=allocator.Load(4u);
    if(freeCount>freeRangeCapacity||bump>poolCapacity){work.Store(8u,1u);return;}
    for(uint i=0u;i<candidatesCount;++i){
        uint blockId=candidates[i];uint4 result=results[blockId];
        if(result.y==0u)continue;
        CutSpatialBlock block=stage[blockId];
        if(block.instanceId>=instanceCount){work.Store(8u,1u);return;}
        if(result.x!=0u){
            uint first;
            if(!ReserveRange(freeDescriptor,result.x,bump,freeCount,first)){
                work.Store(8u,1u);return;
            }
            block.geometryState=ModifiedBlock;
            block.activeFirst=first;
            block.activeCount=result.x;
        }else{
            block.geometryState=EmptyBlock;
            block.activeFirst=0u;
            block.activeCount=0u;
        }
        stage[blockId]=block;
        uint2 pair=flags[block.instanceId];
        if(staging==0u)pair.x=1u;else pair.y=1u;
        flags[block.instanceId]=pair;
        work.Store(12u,1u); // at least one actual destructive change
    }
    allocator.Store(0u,bump);allocator.Store(4u,freeCount);
    if(work.Load(12u)==0u)return;
    // Rebuild only small per-block draw arguments. An instance becomes
    // replaced as a whole: untouched Source blocks remain drawable.
    RWByteAddressBuffer draw=ResourceDescriptorHeap[commandPrepared];
    for(uint i=0u;i<blockCount;++i){
        CutSpatialBlock block=stage[i];
        if(block.instanceId>=instanceCount){work.Store(8u,1u);return;}
        uint2 pair=flags[block.instanceId];
        bool replaced=(staging==0u?pair.x:pair.y)!=0u;
        uint count=replaced?(block.geometryState==SourceBlock?block.triangleCount:
                             block.geometryState==ModifiedBlock?block.activeCount:0u):0u;
        draw.Store4(i*16u,uint4(i,(count+20u)/21u,1u,1u));
    }
}

// Write only changed candidates. Every triangle reserves a disjoint slice
// inside its new block allocation; no source range is overwritten in place.
[numthreads(64,1,1)]
void WriteCutTriangles(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    if(work.Load(8u)!=0u||work.Load(12u)==0u)return;
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateList];
    uint blockId=candidates[group.x];
    RWStructuredBuffer<uint4> results=ResourceDescriptorHeap[blockResults];
    if(results[blockId].y==0u)return;
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint active=state[0].y,staging=active^1u;
    StructuredBuffer<CutSpatialBlock> originalBlocks=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveBlockRead(active))];
    StructuredBuffer<CutSpatialBlock> stagedBlocks=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveBlockRead(staging))];
    CutSpatialBlock srcBlock=originalBlocks[blockId],dstBlock=stagedBlocks[blockId];
    if(dstBlock.geometryState==EmptyBlock)return;
    uint first=srcBlock.geometryState==SourceBlock?srcBlock.firstTriangle:srcBlock.activeFirst;
    uint count=srcBlock.geometryState==SourceBlock?srcBlock.triangleCount:srcBlock.activeCount;
    StructuredBuffer<CutTriangle> src=ResourceDescriptorHeap[NonUniformResourceIndex(
        srcBlock.geometryState==SourceBlock?originalTriangles:poolRead)];
    RWStructuredBuffer<CutImpact> pending=ResourceDescriptorHeap[pendingBuffer];
    RWByteAddressBuffer queue=ResourceDescriptorHeap[pendingControl];
    CutImpact hit=pending[queue.Load(0u)];
    for(uint i=lane;i<count;i+=64u){
        CutTriangle face=src[first+i];
        uint removed;
        uint n=ClipCutFace(face,hit,false,0u,removed);
        if(n==0u)continue;
        uint outOffset;
        InterlockedAdd(results[blockId].z,n,outOffset);
        if(outOffset>dstBlock.activeCount||n>dstBlock.activeCount-outOffset){
            uint was;work.InterlockedOr(8u,1u,was);continue;
        }
        uint written=ClipCutFace(face,hit,true,dstBlock.activeFirst+outOffset,removed);
        if(written!=n){uint was;work.InterlockedOr(8u,1u,was);}
    }
}

// The final publication runs after this frame's depth, opaque, shadow and
// collision readers on Direct. Old ranges are recycled into the NEXT allocator
// only after every write and count has been verified. No readback or CPU wait.
void DelayCutRetry(){
    RWByteAddressBuffer queue=ResourceDescriptorHeap[pendingControl];
    uint4 q=queue.Load4(0u);
    q.w=(q.w&0xffff0000u)|30u;
    queue.Store4(0u,q);
}
[numthreads(1,1,1)]
void CommitCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer work=ResourceDescriptorHeap[controlBuffer];
    if(work.Load(4u)==0u)return;
    if(work.Load(8u)!=0u){DelayCutRetry();return;}
    uint candidatesCount=work.Load(0u);
    StructuredBuffer<uint4> current=ResourceDescriptorHeap[stateRead];
    uint4 state=current[0];
    uint active=state.y,staging=active^1u;
    RWStructuredBuffer<CutSpatialBlock> oldBlocks=ResourceDescriptorHeap[NonUniformResourceIndex(StageBlockWrite(active))];
    RWStructuredBuffer<CutSpatialBlock> nextBlocks=ResourceDescriptorHeap[NonUniformResourceIndex(StageBlockWrite(staging))];
    RWStructuredBuffer<uint4> results=ResourceDescriptorHeap[blockResults];
    RWStructuredBuffer<uint> candidates=ResourceDescriptorHeap[candidateList];
    uint published=work.Load(12u);
    for(uint i=0u;i<candidatesCount;++i){
        uint idBlock=candidates[i];
        uint4 result=results[idBlock];
        if(result.y!=0u&&result.x!=result.z){
            work.Store(8u,1u);DelayCutRetry();return;
        }
    }
    if(published!=0u){
        RWByteAddressBuffer allocator=ResourceDescriptorHeap[NonUniformResourceIndex(ActiveAllocator(staging))];
        uint freeCount=allocator.Load(4u),freeDescriptor=ActiveFreeRanges(staging);
        for(uint i=0u;i<candidatesCount;++i){
            uint idBlock=candidates[i];
            if(results[idBlock].y==0u)continue;
            CutSpatialBlock old=oldBlocks[idBlock];
            if(old.geometryState==ModifiedBlock&&old.activeCount!=0u){
                if(!RecycleRange(freeDescriptor,old.activeFirst,old.activeCount,freeCount)){
                    work.Store(8u,1u);DelayCutRetry();return;
                }
            }
        }
        allocator.Store(4u,freeCount);
        // One packed entry per DRAWABLE block, not per source block.
        // The count buffer and the metadata generation publish together.
        RWByteAddressBuffer activeDraw=ResourceDescriptorHeap[commandActive];
        RWByteAddressBuffer preparedDraw=ResourceDescriptorHeap[commandPrepared];
        RWByteAddressBuffer countBuffer=ResourceDescriptorHeap[commandCount];
        uint compact=0u;
        for(uint i=0u;i<blockCount;++i){
            uint4 cmd=preparedDraw.Load4(i*16u);
            if(cmd.y!=0u)activeDraw.Store4((compact++)*16u,cmd);
        }
        countBuffer.Store(0u,compact);
        RWStructuredBuffer<uint4> stateWriteBuffer=ResourceDescriptorHeap[stateWrite];
        stateWriteBuffer[0]=uint4(state.x,staging,state.z+1u,0u);
    }
    RWByteAddressBuffer queue=ResourceDescriptorHeap[pendingControl];
    uint4 q=queue.Load4(0u);
    if(q.z>0u){
        q.x=(q.x+1u)%pendingCapacity;--q.z;
        q.w&=0xffff0000u; // publication clears retry cooldown
        queue.Store4(0u,q);
    }
}
