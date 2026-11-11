#include "common.hlsli"
bool Frustum(float4 sphere,uint view,out float2 lo,out float2 hi,out float nearest){float3 p=sphere.xyz;float radius=sphere.w;float4 c=mul(float4(p,1),vp[view]);float rx=radius*length(float3(vp[view][0][0],vp[view][1][0],vp[view][2][0]));float ry=radius*length(float3(vp[view][0][1],vp[view][1][1],vp[view][2][1]));float rz=radius*length(float3(vp[view][0][2],vp[view][1][2],vp[view][2][2]));float rw=radius;if(c.x+rx<-(c.w+rw)||c.x-rx>c.w+rw||c.y+ry<-(c.w+rw)||c.y-ry>c.w+rw||c.z+rz<0||c.z-rz>c.w+rw){lo=0;hi=1;nearest=1;return false;}
    lo=1;hi=0;nearest=0;bool clipped=false;[unroll]for(uint k=0;k<8;k++){float3 offset=float3((k&1)?1:-1,(k&2)?1:-1,(k&4)?1:-1)*radius;float4 h=mul(float4(p+offset,1),vp[view]);if(h.w<=timing.x){clipped=true;break;}float2 uv=h.xy/h.w*float2(.5,-.5)+.5;lo=min(lo,uv);hi=max(hi,uv);nearest=max(nearest,h.z/h.w);}if(clipped){lo=0;hi=1;nearest=1;}return true;}
bool Occluded(float2 lo,float2 hi,float nearest,uint view){if(any(lo<0)||any(hi>1)||nearest>=1)return false;Texture2DArray<float> hiz=ResourceDescriptorHeap[Hierarchy];float2 extent=(hi-lo)*float2(Width,Height);uint mip=min(HiZMips-1,(uint)ceil(log2(max(1,max(extent.x,extent.y)))));uint w,h,layers,levels;w=max(1u,Width>>mip);h=max(1u,Height>>mip);uint2 a=min(uint2(lo*float2(w,h)),uint2(w-1,h-1));uint2 b=min(uint2(hi*float2(w,h)),uint2(w-1,h-1));float depth=min(min(hiz.Load(int4(a,view,mip)),hiz.Load(int4(b,view,mip))),min(hiz.Load(int4(a.x,b.y,view,mip)),hiz.Load(int4(b.x,a.y,view,mip))));return nearest<depth-.0003;}
[numthreads(8,8,1)]
void MergeReflection(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height||id.z>=ViewCount)return;
    RWTexture2DArray<float4> color=ResourceDescriptorHeap[param0];Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
    float4 c=color[id];c.rgb+=reflection.Load(int4(id,0)).rgb;color[id]=c;
}
[numthreads(64,1,1)]
void CullMain(uint3 id:SV_DispatchThreadID){
    uint local=id.x,index=objectIndex+local;
    StructuredBuffer<MeshRange> meshes=ResourceDescriptorHeap[Ranges];
    if(passIndex==7){
        if(local>=param0)return;
        GpuInstance instance=LoadInstanceStable(index);
        if(instance.flags&(1|2|4))return;
        RWByteAddressBuffer args=ResourceDescriptorHeap[CpuShadowArguments];
        RWByteAddressBuffer count=ResourceDescriptorHeap[CpuShadowCounter];
        uint outIndex;count.InterlockedAdd(0,1,outIndex);
        args.Store4(outIndex*16,uint4(index,meshes[instance.mesh].meshletCount,1,1));
        return;
    }
    if(param0!=0xffffffffu&&local>=param0)return;
    if(index>=InstanceCount+AdditionalCount)return;
    // Publish both accepted and rejected CPU-owned visibility flags.
    bool cpuOwned=(passIndex==3||passIndex==5);
    if(cpuOwned){
        RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
        marks[index]=0u;
    }
    GpuInstance instance=LoadInstance(index);
    if(instance.flags&(16|64|128))return;
    StructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBounds];
    StructuredBuffer<uint> meshIds=ResourceDescriptorHeap[HotMeshes];
    float4 sphere=bounds[index];uint mesh=meshIds[index];bool visible=false;
    for(uint eye=0;eye<ViewCount;eye++){
        float2 lo,hi;float nearDepth;
        if(Frustum(sphere,eye,lo,hi,nearDepth)&&
           (passIndex==3||passIndex==4||!Occluded(lo,hi,nearDepth,eye)))
            visible=true;
    }
    if(!visible)return;
    if(cpuOwned){
        RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
        marks[index]=1u;
    }
    RWByteAddressBuffer args=ResourceDescriptorHeap[
        passIndex==3?CpuDepthArguments:(passIndex==4?PublishedDepthArguments:
        (passIndex==5?CpuCullArguments:PublishedCullArguments))];
    RWByteAddressBuffer count=ResourceDescriptorHeap[
        passIndex==3?CpuDepthCounter:(passIndex==4?PublishedDepthCounter:
        (passIndex==5?CpuCullCounter:PublishedCullCounter))];
    uint outIndex;count.InterlockedAdd(0,1,outIndex);
    args.Store4(outIndex*16,uint4(index,meshes[mesh].meshletCount,1,1));
}
[numthreads(64,1,1)]
void BatchOpaqueMain(uint3 id:SV_DispatchThreadID){
    // Each dispatch handles one adjacent pipeline-variant span. The run's
    // argument/count ranges are disjoint; object IDs and transforms stay intact.
    const uint BatchWidth=16u;
    uint localFirst=id.x*BatchWidth;
    if(localFirst>=param0)return;
    StructuredBuffer<uint> visible=ResourceDescriptorHeap[CpuBatchVisibilitySRV];
    StructuredBuffer<uint> selectedMeshes=ResourceDescriptorHeap[HotMeshes];
    StructuredBuffer<MeshRange> meshes=ResourceDescriptorHeap[Ranges];
    RWByteAddressBuffer args=ResourceDescriptorHeap[CpuBatchArgumentsUAV];
    RWByteAddressBuffer count=ResourceDescriptorHeap[CpuBatchCountUAV];
    uint head=0,mesh=0,material=0,flags=0,length=0;
    [loop]for(uint j=0;j<BatchWidth&&localFirst+j<param0;++j){
        uint index=objectIndex+localFirst+j;
        bool present=visible[index]!=0u;
        uint nextMesh=0,nextMaterial=0,nextFlags=0;
        if(present){
            GpuInstance instance=LoadInstance(index);
            nextMesh=selectedMeshes[index];
            nextMaterial=instance.material;
            nextFlags=instance.flags;
        }
        bool compatible=present&&length>0u&&nextMesh==mesh&&
                        nextMaterial==material&&nextFlags==flags;
        if(length>0u&&!compatible){
            uint dest;count.InterlockedAdd(param1*4u,1,dest);
            args.Store4((objectIndex+dest)*16u,uint4(head,meshes[mesh].meshletCount,length,1));
            length=0u;
        }
        if(present){
            if(length==0u){
                head=index;mesh=nextMesh;material=nextMaterial;flags=nextFlags;
            }
            ++length;
        }
    }
    if(length>0u){
        uint dest;count.InterlockedAdd(param1*4u,1,dest);
        args.Store4((objectIndex+dest)*16u,uint4(head,meshes[mesh].meshletCount,length,1));
    }
}

 // Resident opaque groups: one dispatch per tile, never per material run.
 // Each tile's run table starts with {runCount,0,0,0}, followed by
 // {localFirst,count,materialMask,0} sorted by authored localFirst.
 uint TileBatchRun(uint descriptor,uint local){
     StructuredBuffer<uint4> runs=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
     uint count=runs[0].x;
     uint lo=0,hi=count;
     while(lo+1u<hi){
         uint mid=(lo+hi)>>1;
         if(runs[mid+1u].x<=local)lo=mid;
         else hi=mid;
     }
     return lo;
 }
 [numthreads(64,1,1)]
 void TileBatchMark(uint3 thread:SV_DispatchThreadID){
     uint local=thread.x+thread.y*4194240u;
     if(local>=param0)return;
     uint global=objectIndex+local;
     StructuredBuffer<uint> visible=ResourceDescriptorHeap[CpuBatchVisibilitySRV];
     RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[TileMarksUAV];
     if(visible[global]==0u){marks[global]=0u;return;}
     StructuredBuffer<uint4> runs=ResourceDescriptorHeap[NonUniformResourceIndex(passIndex)];
     uint runIndex=TileBatchRun(passIndex,local);
     uint runFirst=runs[runIndex+1u].x;
     // The original 16-adjacent-object upper bound remains exact.
     bool begins=local==runFirst||((local-runFirst)%16u)==0u;
     if(!begins)begins=visible[global-1u]==0u;
     if(!begins){
         GpuInstance current=LoadInstance(global);
         GpuInstance previous=LoadInstance(global-1u);
         StructuredBuffer<uint> meshes=ResourceDescriptorHeap[HotMeshes];
         begins=meshes[global]!=meshes[global-1u]||
                current.material!=previous.material||
                current.flags!=previous.flags;
     }
     marks[global]=begins?1u:0u;
 }
 groupshared uint tileScan[256];
 [numthreads(256,1,1)]
 void TileBatchScan(uint3 tid:SV_DispatchThreadID,uint3 group:SV_GroupID,
                    uint localTid:SV_GroupIndex){
     uint block=group.x+group.y*65535u;
     uint local=block*256u+localTid;
     RWStructuredBuffer<uint> prefix=ResourceDescriptorHeap[TilePrefixesUAV];
     StructuredBuffer<uint> starts=ResourceDescriptorHeap[TileMarksSRV];
     uint value=local<param0?starts[objectIndex+local]:0u;
     tileScan[localTid]=value;
     GroupMemoryBarrierWithGroupSync();
     [unroll]for(uint distance=1u;distance<256u;distance<<=1u){
         uint above=localTid>=distance?tileScan[localTid-distance]:0u;
         GroupMemoryBarrierWithGroupSync();
         tileScan[localTid]+=above;
         GroupMemoryBarrierWithGroupSync();
     }
     if(local<param0)prefix[objectIndex+local]=tileScan[localTid]-value;
     if(localTid==255u){
         RWStructuredBuffer<uint> totals=ResourceDescriptorHeap[TileTotalsUAV];
         totals[param1+block]=tileScan[255u];
     }
 }
 [numthreads(64,1,1)]
 // A two-level stable prefix avoids quadratic work as tiles grow.
 // OffsetScan: param0=block count, param1=block first, passIndex=super first.
 [numthreads(256,1,1)]
 void TileBatchOffsetScan(uint3 tid:SV_DispatchThreadID,
                         uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
     uint block=tid.x;
     RWStructuredBuffer<uint> totals=ResourceDescriptorHeap[TileTotalsUAV];
     RWStructuredBuffer<uint> offsets=ResourceDescriptorHeap[TileOffsetsUAV];
     uint value=block<param0?totals[param1+block]:0u;
     tileScan[lane]=value;
     GroupMemoryBarrierWithGroupSync();
     [unroll]for(uint step=1u;step<256u;step<<=1u){
         uint above=lane>=step?tileScan[lane-step]:0u;
         GroupMemoryBarrierWithGroupSync();
         tileScan[lane]+=above;
         GroupMemoryBarrierWithGroupSync();
     }
     if(block<param0)offsets[param1+block]=tileScan[lane]-value;
     if(lane==255u)totals[passIndex+group.x]=tileScan[255u];
 }
 // Finalize: each block adds the totals of preceding 256-block groups.
 [numthreads(64,1,1)]
 void TileBatchOffsetFinalize(uint3 thread:SV_DispatchThreadID){
     uint block=thread.x;
     if(block>=param0)return;
     StructuredBuffer<uint> totals=ResourceDescriptorHeap[TileTotalsSRV];
     RWStructuredBuffer<uint> offsets=ResourceDescriptorHeap[TileOffsetsUAV];
     uint superBlock=block/256u;
     uint prefix=0u;
     [loop]for(uint i=0u;i<superBlock;++i)
         prefix+=totals[passIndex+i];
     offsets[param1+block]+=prefix;
 }
 [numthreads(64,1,1)]
 void TileBatchEmit(uint3 thread:SV_DispatchThreadID){
     uint local=thread.x+thread.y*4194240u;
     if(local>=param0)return;
     uint global=objectIndex+local;
     StructuredBuffer<uint> marks=ResourceDescriptorHeap[TileMarksSRV];
     if(marks[global]==0u)return;
     StructuredBuffer<uint4> runs=ResourceDescriptorHeap[NonUniformResourceIndex(passIndex)];
     uint runId=TileBatchRun(passIndex,local);
     uint4 run=runs[runId+1u];
     StructuredBuffer<uint> prefixes=ResourceDescriptorHeap[TilePrefixesSRV];
     StructuredBuffer<uint> offsets=ResourceDescriptorHeap[TileOffsetsSRV];
     uint before=prefixes[global]+offsets[param1+local/256u];
     uint beforeRun=prefixes[objectIndex+run.x]+offsets[param1+run.x/256u];
     uint dest=objectIndex+run.x+before-beforeRun;
     uint remaining=min(16u,param0-local);
     uint length=1u;
     StructuredBuffer<uint> visible=ResourceDescriptorHeap[CpuBatchVisibilitySRV];
     [loop]for(uint next=1u;next<remaining;++next){
         if(visible[global+next]==0u||marks[global+next]!=0u)break;
         ++length;
     }
     StructuredBuffer<uint> selectedMeshes=ResourceDescriptorHeap[HotMeshes];
     StructuredBuffer<MeshRange> meshes=ResourceDescriptorHeap[Ranges];
     uint mesh=selectedMeshes[global];
     RWByteAddressBuffer args=ResourceDescriptorHeap[CpuBatchArgumentsUAV];
     RWByteAddressBuffer counters=ResourceDescriptorHeap[CpuBatchCountUAV];
     uint ignored;
     counters.InterlockedAdd((param2+runId)*4u,1u,ignored);
     args.Store4(dest*16u,uint4(global,meshes[mesh].meshletCount,length,1u));
 }

// Materialize compact-ray-to-render identity on GPU only if its layout
// changed. Root constants: compact first, render first, count, map UAV.
[numthreads(64,1,1)]
void RayMapFillMain(uint3 id:SV_DispatchThreadID){
    const uint local=id.x+id.y*4194240u; // 65535 groups * 64 threads
    if(local>=param0)return;
    RWStructuredBuffer<uint> map=ResourceDescriptorHeap[param1];
    map[objectIndex+local]=passIndex+local;
}
[numthreads(64,1,1)]
void RayBuildMain(uint3 id:SV_DispatchThreadID){
    const uint NoRayTracingFlag=32;
    uint local=id.x+id.y*4194240u,index=objectIndex+local;
    if(param0!=0xffffffffu&&local>=param0)return;
    if(index>=RayInstanceCount)return;
    uint renderIndex=RenderInstanceForRay(index);
    GpuInstance i=LoadInstanceStable(renderIndex);StructuredBuffer<uint4> addresses=ResourceDescriptorHeap[BlasAddresses];uint4 address=addresses[i.mesh];bool noRay=(i.flags&NoRayTracingFlag)!=0;uint2 blas=noRay?address.xy:((i.flags&2)?address.zw:address.xy);StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface material=materials[i.material];
    float3 x=Rotate(float3(i.scale.x,0,0),i.rotation),y=Rotate(float3(0,i.scale.y,0),i.rotation),z=Rotate(float3(0,0,i.scale.z),i.rotation);RWByteAddressBuffer rays=ResourceDescriptorHeap[RayDescriptors];uint offset=index*64;
    uint mask=(i.flags&NoRayTracingFlag)?0:(i.flags&4)?4:(i.flags&3)?2:1;
    // D3D12 instance flags: disable triangle culling (1), force opaque (4),
    // or force non-opaque (8). Alpha-mask and alpha-blend surfaces must retain
    // candidate hits so RayOpacity can evaluate coverage and transmission.
    const uint rayCullDisabled=1u,rayForceOpaque=4u,rayForceNonOpaque=8u;
    uint flags=rayCullDisabled|(material.extra.y==0u?rayForceOpaque:rayForceNonOpaque);
    rays.Store4(offset,asuint(float4(x.x,y.x,z.x,i.positionRadius.x)));rays.Store4(offset+16,asuint(float4(x.y,y.y,z.y,i.positionRadius.y)));rays.Store4(offset+32,asuint(float4(x.z,y.z,z.z,i.positionRadius.z)));rays.Store4(offset+48,uint4(index|(mask<<24),flags<<24,blas));
}
[numthreads(8,8,1)]
void DepthMain(uint3 id:SV_DispatchThreadID){if(id.x>=Width||id.y>=Height||id.z>=ViewCount)return;RWTexture2DArray<float> output=ResourceDescriptorHeap[HiZUAV(0)];
float d;
if(topology.y>1){
Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];d=1;[unroll]for(uint i=0;i<4;i++)d=min(d,depth.Load(int3(id),i));
}else{
Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];d=depth.Load(int4(id,0));
}

    output[id]=d;
}
[numthreads(8,8,1)]
void HiZMain(uint3 id:SV_DispatchThreadID){uint2 size=max(uint2(1,1),uint2(Width,Height)>>passIndex);if(any(id.xy>=size)||id.z>=ViewCount)return;Texture2DArray<float> src=ResourceDescriptorHeap[Hierarchy];RWTexture2DArray<float> dst=ResourceDescriptorHeap[HiZUAV(passIndex)];uint2 previous=max(uint2(1,1),uint2(Width,Height)>>(passIndex-1));uint2 begin=id.xy*previous/size,end=((id.xy+1)*previous+size-1)/size;float d=1;for(uint y=begin.y;y<end.y;y++)for(uint x=begin.x;x<end.x;x++)d=min(d,src.Load(int4(min(uint2(x,y),previous-1),id.z,passIndex-1)));dst[id]=d;}
// Spatial light hierarchy. All stages read f.lights after PhysX's GPU copy;
// the Morton ordering may persist for up to 256 frames, but boxes never do.
static const float LightBvhHuge=3.402823466e38f;
groupshared float3 lightBvhSharedLow[256];
groupshared float3 lightBvhSharedHigh[256];

uint LightBvhThread(uint3 id){return id.x+id.y*16776960u;}
uint LightBvhSpread(uint v){
    v&=1023u;
    v=(v|(v<<16u))&0x030000ffu;
    v=(v|(v<<8u))&0x0300f00fu;
    v=(v|(v<<4u))&0x030c30c3u;
    v=(v|(v<<2u))&0x09249249u;
    return v;
}
uint LightBvhMortonCode(float3 p,float3 low,float3 high){
    float3 extent=max(high-low,float3(1e-4f,1e-4f,1e-4f));
    uint3 q=(uint3)round(saturate((p-low)/extent)*1023.0f);
    return (LightBvhSpread(q.x)<<2u)|
           (LightBvhSpread(q.y)<<1u)|LightBvhSpread(q.z);
}
LightBvhNode LightBvhEmptyNode(){
    LightBvhNode result;
    result.low=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);result.high=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
    result.lightIndex=0xffffffffu;result.reserved=0u;
    return result;
}
// First reduce centres of the CURRENT GPU light records in 256-light groups.
[numthreads(256,1,1)]
void LightBvhPartialBounds(uint3 id:SV_DispatchThreadID,
    uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint block=group.x+group.y*65535u;
    uint index=block*256u+lane;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    float3 p=0;
    bool valid=index<LightCount;
    if(valid)p=lights[index].positionRadius.xyz;
    valid=valid&&all(isfinite(p));
    lightBvhSharedLow[lane]=valid?p:float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);
    lightBvhSharedHigh[lane]=valid?p:float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint stride=128u;stride>0u;stride>>=1u){
        if(lane<stride){
            lightBvhSharedLow[lane]=min(lightBvhSharedLow[lane],
                                         lightBvhSharedLow[lane+stride]);
            lightBvhSharedHigh[lane]=max(lightBvhSharedHigh[lane],
                                          lightBvhSharedHigh[lane+stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0u){
        RWStructuredBuffer<LightBvhNode> bounds=
            ResourceDescriptorHeap[LightBvhBoundsUAV];
        LightBvhNode node=LightBvhEmptyNode();
        node.low=lightBvhSharedLow[0];
        node.high=lightBvhSharedHigh[0];
        bounds[block]=node;
    }
}
// Second reduction consumes every partial group, not only the first 256.
[numthreads(256,1,1)]
void LightBvhGlobalBounds(uint lane:SV_GroupIndex){
    StructuredBuffer<LightBvhNode> bounds=ResourceDescriptorHeap[LightBvhBounds];
    uint groupCount=param0;
    float3 low=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge),high=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
    [loop]for(uint group=lane;group<groupCount;group+=256u){
        LightBvhNode node=bounds[group];
        low=min(low,node.low);
        high=max(high,node.high);
    }
    lightBvhSharedLow[lane]=low;
    lightBvhSharedHigh[lane]=high;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint stride=128u;stride>0u;stride>>=1u){
        if(lane<stride){
            lightBvhSharedLow[lane]=min(lightBvhSharedLow[lane],
                                         lightBvhSharedLow[lane+stride]);
            lightBvhSharedHigh[lane]=max(lightBvhSharedHigh[lane],
                                          lightBvhSharedHigh[lane+stride]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0u){
        RWStructuredBuffer<LightBvhNode> target=
            ResourceDescriptorHeap[LightBvhBoundsUAV];
        LightBvhNode node=LightBvhEmptyNode();
        node.low=lightBvhSharedLow[0];
        node.high=lightBvhSharedHigh[0];
        target[groupCount]=node;
    }
}
[numthreads(256,1,1)]
void LightBvhMakeMorton(uint3 id:SV_DispatchThreadID){
    uint index=LightBvhThread(id);
    if(index>=objectIndex)return; // objectIndex=P
    RWStructuredBuffer<uint2> pairs=ResourceDescriptorHeap[LightBvhPairsUAV];
    if(index>=LightCount){pairs[index]=uint2(0xffffffffu,0xffffffffu);return;}
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    StructuredBuffer<LightBvhNode> bounds=ResourceDescriptorHeap[LightBvhBounds];
    LightBvhNode global=bounds[param0]; // param0=partial-group count
    float3 p=lights[index].positionRadius.xyz;
    uint code=all(isfinite(p))&&all(global.low<=global.high)?
        LightBvhMortonCode(p,global.low,global.high):0u;
    pairs[index]=uint2(code,index);
}
// Every unordered pair is owned by exactly one lane per bitonic stage.
// Two keys compare by Morton code, then original light index.
[numthreads(256,1,1)]
void LightBvhBitonic(uint3 id:SV_DispatchThreadID){
    uint index=LightBvhThread(id);
    uint partner=index^param0; // passIndex=sort size, param0=compare stride
    if(index>=objectIndex||partner>=objectIndex||index>=partner)return;
    RWStructuredBuffer<uint2> pairs=ResourceDescriptorHeap[LightBvhPairsUAV];
    uint2 a=pairs[index],b=pairs[partner];
    bool greater=a.x>b.x||(a.x==b.x&&a.y>b.y);
    bool lesser=a.x<b.x||(a.x==b.x&&a.y<b.y);
    bool ascending=(index&passIndex)==0u;
    if((ascending&&greater)||(!ascending&&lesser)){
        pairs[index]=b;
        pairs[partner]=a;
    }
}
// Refit every sorted leaf from the CURRENT GPU positions; padded leaves are
// marked empty. The conservative enlargement covers float-rounding errors.
[numthreads(256,1,1)]
void LightBvhRefitLeaves(uint3 id:SV_DispatchThreadID){
    uint slot=LightBvhThread(id);
    if(slot>=objectIndex)return;
    StructuredBuffer<uint2> pairs=ResourceDescriptorHeap[LightBvhPairs];
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    RWStructuredBuffer<LightBvhNode> nodes=
        ResourceDescriptorHeap[LightBvhNodesUAV];
    uint original=pairs[slot].y;
    LightBvhNode node=LightBvhEmptyNode();
    if(original<LightCount){
        Light light=lights[original];
        float3 p=light.positionRadius.xyz;
        float radius=light.positionRadius.w;
        if(((uint)light.parameters.z&2u)!=0u){
            // Directional lights have no positional attenuation in reflected
            // BRDF. A global conservative bound keeps them visible to every
            // BVH point query; each consumer retains its own exclusions.
            node.low=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
            node.high=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);
        }else if(all(isfinite(p))&&isfinite(radius)){
            radius=max(0.0f,radius);
            float3 inflate=max(float3(0.002f,0.002f,0.002f),
                (abs(p)+radius)*0.00001f);
            node.low=p-radius-inflate;
            node.high=p+radius+inflate;
        }else{
            // A malformed current record may not silently cull a light.
            node.low=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
            node.high=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);
        }
        node.lightIndex=original;
    }
    nodes[objectIndex+slot]=node;
}
// One independent node per invocation, one level per dispatch. CPU inserts
// a UAV barrier between descending levels, so parents never read stale boxes.
[numthreads(256,1,1)]
void LightBvhRefitParents(uint3 id:SV_DispatchThreadID){
    uint offset=LightBvhThread(id);
    if(offset>=param0)return;
    uint index=passIndex+offset;
    RWStructuredBuffer<LightBvhNode> nodes=
        ResourceDescriptorHeap[LightBvhNodesUAV];
    LightBvhNode a=nodes[index*2u],b=nodes[index*2u+1u];
    LightBvhNode parent=LightBvhEmptyNode();
    parent.low=min(a.low,b.low);
    parent.high=max(a.high,b.high);
    nodes[index]=parent;
}

uint ClusterTotal(){
    return ((Width+63)/64)*((Height+63)/64)*24*ViewCount;
}
float4 ClusterSphere(uint index){
    uint x=(Width+63)/64,y=(Height+63)/64;
    uint cx=index%x,cy=(index/x)%y,cz=(index/(x*y))%24,view=index/(x*y*24);
    float z0=timing.x*pow(timing.y/timing.x,float(cz)/24);
    float z1=timing.x*pow(timing.y/timing.x,float(cz+1)/24);
    float2 uv0=float2(cx*64,cy*64)/float2(Width,Height);
    float2 uv1=min(float2((cx+1)*64,(cy+1)*64)/float2(Width,Height),1);
    float3 a=Unproject(uv0,DepthAt(z0),view);
    float3 b=Unproject(uv1,DepthAt(z1),view);
    float3 center=(a+b)*.5;
    float radius=0;
    [unroll]for(uint i=0;i<8;i++){
        float2 uv=float2((i&1)?uv0.x:uv1.x,(i&2)?uv0.y:uv1.y);
        radius=max(radius,length(Unproject(uv,DepthAt((i&4)?z0:z1),view)-center));
    }
    return float4(center,radius);
}
bool InCluster(Light light,float4 sphere){
    return distance(sphere.xyz,light.positionRadius.xyz)<
        sphere.w+light.positionRadius.w;
}
// One GPU thread owns one of the existing 64x64 / 24-depth / per-view
// clusters. The BVH contains CURRENT GPU lights, including PhysX mutations.
// Retain the exact ClusterSphere and InCluster acceptance test above.
// The fixed-size cache is never used once an additional (33rd) local
// candidate is found: 0xffffffff means per-point BVH traversal, not truncation.
[numthreads(64,1,1)]
void ClusterSpatialMain(uint3 id:SV_DispatchThreadID){
    uint cluster=id.x;
    if(cluster>=ClusterTotal())return;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    RWStructuredBuffer<uint2> clusters=ResourceDescriptorHeap[ClusterUAV];
    RWStructuredBuffer<uint> indices=ResourceDescriptorHeap[ClusterIndicesUAV];
    RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[ClusterBoundsUAV];
    float4 sphere=ClusterSphere(cluster);
    bounds[cluster]=sphere;
    uint first=cluster*ClusterLocalCapacity;
    uint count=0u,cursor=1u;
    [loop]while(true){
        uint index=LightBvhNextLight(cursor,LightBvhLeafCount,
                                    sphere.xyz,sphere.w);
        if(index==0xffffffffu)break;
        Light light=lights[index];
        if(((uint)light.parameters.z&2u)!=0u||!InCluster(light,sphere))
            continue;
        if(count==ClusterLocalCapacity){
            count=ClusterOverflow;
            break;
        }
        indices[first+count]=index;
        ++count;
    }
    clusters[cluster]=uint2(first,count);
    if(cluster==0u){
        // The existing directional list and its original-index order stay
        // separate from the BVH local cache (one global scan, not per-cell).
        RWStructuredBuffer<uint> directional=
            ResourceDescriptorHeap[DirectionalIndicesUAV];
        RWStructuredBuffer<uint> directionalCount=
            ResourceDescriptorHeap[DirectionalCountUAV];
        uint n=0u;
        [loop]for(uint index=0u;index<LightCount;++index)
            if(((uint)lights[index].parameters.z&2u)!=0u)
                directional[n++]=index;
        directionalCount[0]=n;
    }
}
[numthreads(8,8,1)]
void DenoiseMain(uint3 id:SV_DispatchThreadID){if(id.x>=Width||id.y>=Height||id.z>=ViewCount)return;Texture2DArray<float4> input=ResourceDescriptorHeap[ReflectionResolved];Texture2DArray<float> depth=ResourceDescriptorHeap[DepthResolved];Texture2DArray<float4> history=ResourceDescriptorHeap[HistoryPrevious];Texture2DArray<float> oldDepth=ResourceDescriptorHeap[HistoryDepthPrevious];RWTexture2DArray<float4> output=ResourceDescriptorHeap[HistoryOutput];RWTexture2DArray<float> outputDepth=ResourceDescriptorHeap[HistoryDepthOutput];SamplerState linearSampler=SamplerDescriptorHeap[2];float4 current=input.Load(int4(id,0));float z=depth.Load(int4(id,0));float2 uv=(float2(id.xy)+.5)/float2(Width,Height);float3 world=Unproject(uv,z,id.z);float4 previous=mul(float4(world,1),previousVP[id.z]);float2 puv=previous.xy/previous.w*float2(.5,-.5)+.5;if(HistoryValid&&current.a>0&&previous.w>0&&all(puv>0)&&all(puv<1)){float d=oldDepth.SampleLevel(linearSampler,float3(puv*float2(Width,Height)/float2(outputDimensions.xy),id.z),0);if(abs(LinearDepth(d)-LinearDepth(previous.z/previous.w))<.035){float3 lo=current.rgb,hi=current.rgb;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){float3 c=input.Load(int4(clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(Width-1,Height-1)),id.z,0)).rgb;lo=min(lo,c);hi=max(hi,c);}current.rgb=lerp(current.rgb,clamp(history.SampleLevel(linearSampler,float3(puv*float2(Width,Height)/float2(outputDimensions.xy),id.z),0).rgb,lo,hi),.65);}}output[id]=current;outputDepth[id]=z;}

[numthreads(64,1,1)]
void DeformMain(uint3 id:SV_DispatchThreadID) {
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];MeshRange range=ranges[param0];if(id.x>=range.vertexCount)return;
    StructuredBuffer<Vertex> source=ResourceDescriptorHeap[Vertices];StructuredBuffer<Deformation> deformations=ResourceDescriptorHeap[Deformations];RWStructuredBuffer<Vertex> output=ResourceDescriptorHeap[DynamicVerticesUAV];
    StructuredBuffer<Motion> motions=ResourceDescriptorHeap[Motions];Motion motion=motions[param0];uint index=range.firstVertex+id.x;Vertex v=source[index];
    if(motion.binding.x!=0xffffffff){
        StructuredBuffer<MorphVertex> morphs=ResourceDescriptorHeap[Morphs];StructuredBuffer<float> weights=ResourceDescriptorHeap[Weights];
        for(uint target=0;target<motion.morph.x;target++){MorphVertex delta=morphs[motion.binding.w+target*range.vertexCount+id.x];float weight=weights[motion.morph.y+target];v.position+=delta.position*weight;v.normal+=delta.normal*weight;}
        if(motion.binding.z){
            StructuredBuffer<SkinVertex> influences=ResourceDescriptorHeap[Skin];StructuredBuffer<Bone> bones=ResourceDescriptorHeap[Bones];SkinVertex skin=influences[index];float3 position=0,normal=0;
            [unroll]for(uint i=0;i<4;i++){if(skin.weights[i]>0){Bone bone=bones[motion.binding.y+skin.joints[i]];position+=mul(float4(v.position,1),bone.transform).xyz*skin.weights[i];normal+=mul(float4(v.normal,0),bone.normalTransform).xyz*skin.weights[i];}if(skin.weights1[i]>0){Bone bone=bones[motion.binding.y+skin.joints1[i]];position+=mul(float4(v.position,1),bone.transform).xyz*skin.weights1[i];normal+=mul(float4(v.normal,0),bone.normalTransform).xyz*skin.weights1[i];}}
            v.position=position;v.normal=normalize(normal);
        }else{v.position=mul(float4(v.position,1),motion.transform).xyz;v.normal=normalize(mul(float4(v.normal,0),motion.normalTransform).xyz);}
    }else{
        Deformation d=deformations[param0];float3 axis=normalize(d.axisAmplitude.xyz);float stretch=1+d.axisAmplitude.w*sin(timing.z*d.frequencyPhase.x+d.frequencyPhase.y);
        float3 relative=v.position-range.bounds.xyz;v.position+=axis*dot(relative,axis)*(stretch-1);v.normal=normalize(v.normal+axis*dot(v.normal,axis)*(1/max(.01,stretch)-1));
    }
    output[index]=v;
}
groupshared float3 BoundsMin[64];
groupshared float3 BoundsMax[64];
[numthreads(64,1,1)]
void DeformBoundsMain(uint tid:SV_GroupIndex){
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[DynamicVertices];MeshRange range=ranges[param0];float3 lo=3.402823e38,hi=-3.402823e38;
    for(uint i=tid;i<range.vertexCount;i+=64){float3 p=vertices[range.firstVertex+i].position;lo=min(lo,p);hi=max(hi,p);}BoundsMin[tid]=lo;BoundsMax[tid]=hi;GroupMemoryBarrierWithGroupSync();
    for(uint stride=32;stride;stride>>=1){if(tid<stride){BoundsMin[tid]=min(BoundsMin[tid],BoundsMin[tid+stride]);BoundsMax[tid]=max(BoundsMax[tid],BoundsMax[tid+stride]);}GroupMemoryBarrierWithGroupSync();}
    if(tid==0){RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[DeformedBoundsUAV];bounds[param0]=float4((BoundsMin[0]+BoundsMax[0])*.5,length(BoundsMax[0]-BoundsMin[0])*.5);}
}
[numthreads(64,1,1)]
void HotDataMain(uint3 id:SV_DispatchThreadID){uint localIndex=id.x,index=objectIndex+localIndex;if(param0!=0xffffffffu&&localIndex>=param0)return;if(index>=InstanceCount+AdditionalCount)return;RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBoundsUAV];RWStructuredBuffer<uint> meshes=ResourceDescriptorHeap[HotMeshesUAV];GpuInstance i=LoadInstance(index);StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];float4 local=ranges[i.mesh].bounds;if(i.flags&2){StructuredBuffer<float4> deformed=ResourceDescriptorHeap[DeformedBounds];local=deformed[i.mesh];}float3 center=World(local.xyz,i);bounds[index]=float4(center,local.w*max(abs(i.scale.x),max(abs(i.scale.y),abs(i.scale.z))));meshes[index]=i.mesh;}
