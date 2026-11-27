// RTXMG-style spatial group traversal, exact DAG cut and GPU cut sharing.
// All source-order masks are immutable before the hash/compact pass.
struct Page {
    float3 center; float radius;
    float error; uint isRoot,clusterCount,triangles;
    uint opaque,firstId,firstCluster,pad1;
};
struct Cluster { uint page,refined,triangles,localIndex; };
struct LodNode {
    float3 center; float radius;
    float error; uint force,page,escape;
};
struct Eye { float4 positionFocal,motionNear; };
struct Instance {
    uint first,count,structuralFirst,structuralCount;
    uint outputOffset,eyeCount,visible,previousCluster;
    uint nodeRoot,nodeEnd,pad0,pad1; // pad0 stores the GPU-only demand scale (float bits).
    Eye eyes[4];
};
StructuredBuffer<Page> Pages : register(t0);
StructuredBuffer<Cluster> Clusters : register(t1);
StructuredBuffer<Instance> Instances : register(t2);
StructuredBuffer<uint> Structural : register(t3);
StructuredBuffer<LodNode> Nodes : register(t4);
struct RegistryEntry {
    uint4 key;
    uint first,count,structuralFirst,structuralCount;
    uint nodeRoot,nodeEnd,pad0,pad1;
    float4 rootRadius,rotation,scale;
};
struct CameraEye { float4 planes[6]; float4 positionFocal,motionNear; };
StructuredBuffer<RegistryEntry> Registry : register(t5);
StructuredBuffer<uint4> PriorRoutes : register(t6);
StructuredBuffer<uint> PriorFirst : register(t7);
StructuredBuffer<CameraEye> Cameras : register(t8);
RWStructuredBuffer<Instance> Prepared : register(u3);
RWStructuredBuffer<uint> Scratch : register(u0);
RWStructuredBuffer<uint> HashTable : register(u1);
RWStructuredBuffer<uint> Output : register(u2);
cbuffer Settings : register(b0) {
    float ErrorPixels; uint InstanceCount,DispatchWidth,HashMask,HeaderWords;
    uint ScratchWords,PriorVariants,EyeCount;
};
static const uint Invalid=0xffffffffu;
groupshared uint TriangleSums[64],ClusterSums[64];
groupshared uint Cursor,ActivePage;
uint WordCount(uint count){return count/32u+uint(count%32u!=0);}
uint GroupIndex(uint3 group){return group.x+group.y*DispatchWidth;}
// Soft culling: secondary rays retain a complete cut, but its error may
// grow outside the stereo frustum and the anticipatory guard band.
static const float IndirectErrorScale=2.0;
static const float AnticipationGuard=0.25; // normalized side-plane separation
static const float IndirectTransition=0.5;
float DemandError(Instance inst){return ErrorPixels*asfloat(inst.pad0);}
// A21: every LOD consumer uses the same swept-eye distance over the
// existing publication prediction interval. A stationary eye reduces to
// the original point metric. The closest point may be inside the segment,
// not just at the predicted endpoint (e.g. when passing a tree).
float EyePathDistance(float3 center,Eye v){
    float3 delta=center-v.positionFocal.xyz;
    float3 motion=v.motionNear.xyz;
    float motion2=dot(motion,motion);
    float t=motion2>0.0?saturate(dot(delta,motion)/motion2):0.0;
    return length(delta-motion*t);
}
bool Refine(Page p,Instance inst){
    if(p.isRoot!=0)return true;
    for(uint eye=0;eye<inst.eyeCount;++eye){
        Eye v=inst.eyes[eye];
        float depth=max(v.motionNear.w,EyePathDistance(p.center,v)-p.radius);
        if(p.error>DemandError(inst)*depth/v.positionFocal.w)return true;
    }
    return false;
}
bool VisitNode(LodNode node,Instance inst){
    if(node.force!=0)return true;
    for(uint eye=0;eye<inst.eyeCount;++eye){
        Eye v=inst.eyes[eye];
        // The same swept metric as Refine is required for conservative
        // subtree pruning. Include the displacement magnitude in the
        // roundoff margin: delta-motion*t can cancel for a passing eye.
        float distance=EyePathDistance(node.center,v);
        float3 travel=abs(v.motionNear.xyz);
        float margin=1.e-5*(distance+node.radius+1.0+travel.x+travel.y+travel.z);
        float depth=max(v.motionNear.w,distance-node.radius-margin);
        if(node.error>=0.99999*DemandError(inst)*depth/v.positionFocal.w)return true;
    }
    return false;
}
float StructuralError(Page p,Instance inst){
    float error=0;
    for(uint eye=0;eye<inst.eyeCount;++eye){
        Eye v=inst.eyes[eye];
        float depth=max(v.motionNear.w,EyePathDistance(p.center,v)-p.radius);
        error=max(error,p.error*v.positionFocal.w/depth);
    }
    return error;
}
[numthreads(64,1,1)]
void Mg2SelectClear(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint index=GroupIndex(group)*64u+lane;
    if(index<=HashMask)HashTable[index]=0;
    if(index==0){
        Output[0]=HeaderWords;Output[1]=0;Output[2]=0;Output[3]=0;
        Scratch[ScratchWords]=0;
    }
}
uint PreviousCluster(uint4 key){
    if(PriorVariants==0)return Invalid;
    uint4 header=PriorRoutes[0];
    uint h=key.x*0x9e3779b1u^key.y*0x85ebca6bu^key.z*0xc2b2ae35u;
    h^=h>>16u;h*=0x7feb352du;h^=h>>15u;
    uint index=h&header.x;
    for(uint probe=0;probe<header.y;++probe){
        uint4 route=PriorRoutes[index+1u];
        if(route.z==Invalid)return Invalid;
        if(all(route.xyz==key.xyz)){
            uint variant=route.w&0x7fffffffu;
            return variant<PriorVariants?PriorFirst[variant]:Invalid;
        }
        index=(index+1u)&header.x;
    }
    return Invalid;
}
float3 RotateToLocal(float3 v,float4 rotation){
    float3 q=-rotation.xyz;
    return (rotation.w*rotation.w-dot(q,q))*v+2.0*q*dot(q,v)+2.0*rotation.w*cross(q,v);
}
[numthreads(64,1,1)]
void Mg2PrepareMain(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint index=GroupIndex(group)*64u+lane;
    if(index>=InstanceCount)return;
    RegistryEntry r=Registry[index];
    Instance inst=(Instance)0;
    inst.first=r.first;inst.count=r.count;
    inst.structuralFirst=r.structuralFirst;inst.structuralCount=r.structuralCount;
    inst.nodeRoot=r.nodeRoot;inst.nodeEnd=r.nodeEnd;inst.eyeCount=EyeCount;
    uint words=3u+WordCount(r.count);
    uint waveWords=WaveActiveSum(words);
    uint prefix=WavePrefixSum(words);
    uint offset=0;
    if(WaveIsFirstLane())InterlockedAdd(Scratch[ScratchWords],waveWords,offset);
    inst.outputOffset=WaveReadLaneFirst(offset)+prefix;
    float3 scale=r.scale.xyz;
    float3 absScale=abs(scale);
    float lo=min(absScale.x,min(absScale.y,absScale.z));
    float hi=max(absScale.x,max(absScale.y,absScale.z));
    float demandScale=IndirectErrorScale;
    for(uint eye=0;eye<EyeCount;++eye){
        CameraEye c=Cameras[eye];
        bool visible=true;
        float outside=0.0;
        for(uint j=0;j<6;++j){
            float3 n=c.planes[j].xyz;
            float nLength=length(n);
            float signedPlane=dot(c.planes[j],float4(r.rootRadius.xyz,1.0));
            float margin=signedPlane+r.rootRadius.w*nLength;
            if(margin<0.0)visible=false;
            // Only lateral planes relax the error; near/far clipping must
            // never reduce ray-visible geometry. Sweep the frustum over the
            // existing predicted translation, conservatively per plane.
            if(j<4u){
                float sweptMargin=margin+max(0.0,-dot(n,c.motionNear.xyz));
                outside=max(outside,max(0.0,-sweptMargin)/max(nLength,1.e-20));
            }
        }
        inst.visible|=uint(visible);
        // Angular guard protects near-edge head turns without introducing
        // a world-space distance cap. The sphere radius is already included.
        float range=max(length(r.rootRadius.xyz-c.positionFocal.xyz),c.motionNear.w);
        float t=saturate((outside/max(range,1.e-20)-AnticipationGuard)/IndirectTransition);
        float eyeScale=visible?1.0:lerp(1.0,IndirectErrorScale,t);
        demandScale=min(demandScale,eyeScale); // union of both eyes
        inst.eyes[eye].positionFocal=float4(RotateToLocal(c.positionFocal.xyz-r.rootRadius.xyz,r.rotation)/scale,c.positionFocal.w*hi/lo);
        inst.eyes[eye].motionNear=float4(RotateToLocal(c.motionNear.xyz,r.rotation)/scale,c.motionNear.w/lo);
    }
    inst.pad0=asuint(demandScale);
    // Keep the existing structural foliage policy; off-screen rays use
    // the ordinary DAG rather than newly opting into optical aggregation.
    inst.previousCluster=inst.visible!=0&&inst.structuralCount!=0?PreviousCluster(r.key):Invalid;
    Prepared[index]=inst;
}
[WaveSize(32)]
[numthreads(64,1,1)]
void Mg2SelectMain(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint index=GroupIndex(group);
    if(index>=InstanceCount)return;
    Instance inst=Instances[index];
    uint words=WordCount(inst.count);
    for(uint word=lane;word<words;word+=64u)Scratch[inst.outputOffset+3u+word]=0;
    if(lane==0)Cursor=inst.nodeRoot;
    DeviceMemoryBarrierWithGroupSync();
    GroupMemoryBarrierWithGroupSync();
    uint triangles=0,selected=0,visited=0,pruned=0;
    for(;;){
        if(lane==0){
            ActivePage=Invalid;
            while(Cursor<inst.nodeEnd){
                LodNode node=Nodes[Cursor];++visited;
                if(node.page!=Invalid){
                    Cursor=node.escape;
                    if(Refine(Pages[node.page],inst)){ActivePage=node.page;break;}
                }else if(VisitNode(node,inst)){
                    ++Cursor;
                }else{
                    Cursor=node.escape;++pruned;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        uint pageIndex=ActivePage;
        if(pageIndex==Invalid)break;
        Page page=Pages[pageIndex];
        // Groups are visited in spatial order. Membership is always emitted
        // in the original source order; a group is visited at most once.
        for(uint base=0;base<page.clusterCount;base+=64u){
            uint localIndex=base+lane;
            bool hit=false;
            if(localIndex<page.clusterCount){
                Cluster c=Clusters[page.firstCluster+localIndex];
                hit=true;
                if(c.refined!=Invalid)hit=!Refine(Pages[c.refined],inst);
                if(hit){triangles+=c.triangles;++selected;}
            }
            uint mask=WaveActiveBallot(hit).x;
            if(WaveIsFirstLane()&&mask!=0){
                uint sourceIndex=page.firstCluster-inst.first+localIndex;
                uint word=sourceIndex/32u,shift=sourceIndex%32u;
                uint ignored;
                InterlockedOr(Scratch[inst.outputOffset+3u+word],mask<<shift,ignored);
                if(shift!=0&&(mask>>(32u-shift))!=0)
                    InterlockedOr(Scratch[inst.outputOffset+4u+word],mask>>(32u-shift),ignored);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    TriangleSums[lane]=triangles;ClusterSums[lane]=selected;
    GroupMemoryBarrierWithGroupSync();
    for(uint stride=32u;stride>0;stride>>=1u){
        if(lane<stride){
            TriangleSums[lane]+=TriangleSums[lane+stride];
            ClusterSums[lane]+=ClusterSums[lane+stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0){
        uint best=0;
        triangles=TriangleSums[0];selected=ClusterSums[0];
        if(inst.visible!=0){
            for(uint i=0;i<inst.structuralCount;++i){
                uint pIndex=Structural[inst.structuralFirst+i];
                Page p=Pages[pIndex];
                if(p.triangles>=triangles||p.clusterCount>selected)continue;
                bool retained=p.firstId==inst.previousCluster;
                float budget=ErrorPixels*(p.opaque!=0?1.0:0.5)*(retained?1.0:0.8);
                if(StructuralError(p,inst)<=budget){
                    best=pIndex+1u;triangles=p.triangles;selected=p.clusterCount;
                }
            }
        }
        Scratch[inst.outputOffset]=best;
        Scratch[inst.outputOffset+1u]=visited;Scratch[inst.outputOffset+2u]=pruned;
    }
}
uint Mix(uint h,uint value){
    h=(h^value)*0x01000193u;
    return h^(h>>13u);
}
bool SameCut(Instance a,Instance b,uint structural,uint words){
    if(a.first!=b.first||a.count!=b.count||Scratch[b.outputOffset]!=structural)return false;
    if(structural!=0)return true;
    for(uint w=0;w<words;++w)
        if(Scratch[a.outputOffset+3u+w]!=Scratch[b.outputOffset+3u+w])return false;
    return true;
}
[WaveSize(32)]
[numthreads(64,1,1)]
void Mg2SelectCompact(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint index=GroupIndex(group)*64u+lane;
    bool valid=index<InstanceCount;
    uint visited=0,pruned=0;
    if(valid){
        uint off=Instances[index].outputOffset;
        visited=Scratch[off+1u];pruned=Scratch[off+2u];
    }
    visited=WaveActiveSum(visited);pruned=WaveActiveSum(pruned);
    if(WaveIsFirstLane()){
        InterlockedAdd(Output[2],visited);InterlockedAdd(Output[3],pruned);
    }
    if(!valid)return;
    Instance inst=Instances[index];
    uint structural=Scratch[inst.outputOffset],words=WordCount(inst.count);
    uint h=Mix(Mix(Mix(0x811c9dc5u,inst.first),inst.count),structural);
    uint selected=0;
    if(structural==0){
        for(uint w=0;w<words;++w){
            uint bits=Scratch[inst.outputOffset+3u+w];
            h=Mix(h,bits);selected+=countbits(bits);
        }
    }
    uint slot=h&HashMask;
    // No producer lock or spin on an unfinished payload: every candidate
    // mask is complete from the previous dispatch. Equality is exact, not
    // inferred from a hash; collisions probe other slots.
    for(;;){
        uint old;
        InterlockedCompareExchange(HashTable[slot],0u,index+1u,old);
        if(old==0){
            Output[4u+2u*index]=index;
            uint payload=structural!=0?0u:min(words,selected);
            uint offset;
            InterlockedAdd(Output[0],2u+payload,offset);
            InterlockedAdd(Output[1],1u);
            Output[5u+2u*index]=offset;Output[offset]=structural;
            if(structural!=0){Output[offset+1u]=0;return;}
            if(selected<=words){
                Output[offset+1u]=selected;uint cursor=offset+2u;
                for(uint w=0;w<words;++w){
                    uint bits=Scratch[inst.outputOffset+3u+w];
                    while(bits!=0){
                        Output[cursor++]=w*32u+uint(firstbitlow(bits));bits&=bits-1u;
                    }
                }
            }else{
                Output[offset+1u]=0x80000000u|words;
                for(uint w=0;w<words;++w)Output[offset+2u+w]=Scratch[inst.outputOffset+3u+w];
            }
            return;
        }
        uint rep=old-1u;
        if(SameCut(inst,Instances[rep],structural,words)){
            Output[4u+2u*index]=rep;return;
        }
        slot=(slot+1u)&HashMask;
    }
}
