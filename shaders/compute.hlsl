#include "common.hlsli"
#include "camera_visibility.hlsli"
bool ShadowFaceIntersects(float4 sphere,uint face){
    row_major float4x4 m=shadowVP[face];
    float4 c=mul(float4(sphere.xyz,1),m);
    float r=sphere.w;
    float rx=r*length(float3(m[0][0],m[1][0],m[2][0]));
    float ry=r*length(float3(m[0][1],m[1][1],m[2][1]));
    float rz=r*length(float3(m[0][2],m[1][2],m[2][2]));
    float rw=r*length(float3(m[0][3],m[1][3],m[2][3]));
    return !(c.x+rx<-(c.w+rw)||c.x-rx>c.w+rw||
             c.y+ry<-(c.w+rw)||c.y-ry>c.w+rw||
             c.z+rz<0||c.z-rz>c.w+rw);
}
[numthreads(8,8,1)]
void MergeReflection(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height||id.z>=ViewCount)return;
    RWTexture2DArray<float4> color=ResourceDescriptorHeap[param0];Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
    float4 c=color[id];c.rgb+=reflection.Load(int4(id,0)).rgb;color[id]=c;
}
[numthreads(64,1,1)]
void CullMain(uint3 id:SV_DispatchThreadID){
    uint local=id.x+id.y*4194240u,index=objectIndex+local;
    if(passIndex==7){
        if(local>=param0)return;
        GpuInstance instance=LoadInstanceStable(index);
        if(instance.flags&(1|2|4))return;
        RWByteAddressBuffer args=ResourceDescriptorHeap[CpuShadowArguments];
        RWByteAddressBuffer count=ResourceDescriptorHeap[CpuShadowCounter];
        float4 bounds=LoadMeshRange(instance.mesh).bounds;
        float scale=max(abs(instance.scale.x),max(abs(instance.scale.y),abs(instance.scale.z)));
        float4 sphere=float4(World(bounds.xyz,instance),bounds.w*scale+0.0001);
        [unroll]for(uint face=0;face<6;face++){
            if(!ShadowFaceIntersects(sphere,param1*6+face))continue;
            uint outIndex;count.InterlockedAdd(face*4,1,outIndex);
            args.Store4((face*param0+outIndex)*16,
                MeshletDispatch(index,LoadMeshRange(instance.mesh).meshletCount,1u));
        }
        return;
    }
    if(param0!=0xffffffffu&&local>=param0)return;
    if(index>=InstanceCount+AdditionalCount)return;

    const bool seedPass=passIndex==8u, remainingPass=passIndex==9u;
    bool cpuOwned=(passIndex==3||passIndex==5||seedPass||remainingPass);
    if(cpuOwned){
        RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
        marks[index]=0u;
    }
    GpuInstance instance=LoadCameraInstance(index);
    if(instance.flags&(16|64|128))return;
    StructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBounds];
    float4 sphere=bounds[index];uint mesh=instance.mesh;bool visible=false;
    const bool structural=StructuralCameraBox(instance);
    // CPU color consumes only the visibility mark; its material batches
    // generate their own indirect arguments. Preserve the stereo union
    // and protect real near-plane/near-structure cases, stop on first acceptance.
    if(passIndex==5u){
        [loop]for(uint view=0;view<ViewCount;++view){
            float2 lo,hi;float nearDepth;
            if(!CameraInstanceFrustum(instance,sphere,structural,view,lo,hi,nearDepth))continue;
            if(CameraNearBuilding(instance,sphere,structural,view)||
               !InstanceOccluded(lo,hi,nearDepth,view)){
                RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
                marks[index]=1u;return;
            }
        }
        return;
    }
    bool inFrustum=false, seed=false;
    // Seed is a workload classification, never a visibility cutoff. Use only
    // actual opaque geometry, with a large projected footprint and cheap range.
    uint meshletCount=LoadMeshRange(mesh).meshletCount;
    bool seedEligible=false,opaqueForHybrid=false;
    if(seedPass||remainingPass){
        StructuredBuffer<Surface> surfaces=ResourceDescriptorHeap[Materials];
        opaqueForHybrid=(instance.flags&(1u|2u|ShuttleSpeedDustFlag))==0u &&
            surfaces[instance.material].extra.y==0u;
        seedEligible=opaqueForHybrid&&meshletCount<=32u;
    }
    for(uint view=0;view<ViewCount;view++){
        float2 lo,hi;float nearDepth;
        if(!CameraInstanceFrustum(instance,sphere,structural,view,lo,hi,nearDepth))continue;
        inFrustum=true;
        const float2 pixels=(min(hi,1.f)-max(lo,0.f))*float2(Width,Height);
        seed=seed||(seedEligible&&min(pixels.x,pixels.y)>=32.f&&
            pixels.x*pixels.y>=16384.f);
        if(passIndex==3||passIndex==4||seedPass||
           CameraNearBuilding(instance,sphere,structural,view)||!InstanceOccluded(lo,hi,nearDepth,view))
            visible=true;
    }
    if(seedPass)visible=inFrustum&&seed;
    if(remainingPass&&seed)visible=true;
    const bool colorOnly=remainingPass&&param1==1u&&visible&&!seed&&opaqueForHybrid;
    const bool submitted=visible&&!colorOnly;
    // Baseline CPU-scene depth: count the real selected mesh ranges before
    // indirect dispatch. No new visibility test or per-meshlet atomics.
    uint depthStats=0xffffffffu;
    if(passIndex==3u||remainingPass)
        depthStats=DomainResource(ResourceDomainVisibility,VisibilityDepthStatsUAV);
    if(depthStats!=0xffffffffu){
        const uint candidates=WaveActiveCountBits(true);
        const uint submittedCount=WaveActiveCountBits(submitted);
        const uint occluders=WaveActiveCountBits(remainingPass&&seed&&inFrustum);
        const uint occluded=WaveActiveCountBits(remainingPass&&inFrustum&&!visible);
        const uint colorOnlyCount=WaveActiveCountBits(colorOnly);
        const uint nativeCount=WaveActiveSum(submitted&&!IsForestPage(mesh)?meshletCount:0u);
        const uint mg2Count=WaveActiveSum(submitted&&IsForestPage(mesh)?meshletCount:0u);
        const uint rejected=WaveActiveSum(visible?0u:meshletCount);
        if(WaveIsFirstLane()){
            RWByteAddressBuffer stats=ResourceDescriptorHeap[depthStats];
            const uint stripe=((index>>6u)&63u)*4u;
            stats.InterlockedAdd(stripe,candidates);
            stats.InterlockedAdd(64u*4u+stripe,submittedCount);
            if(occluders)stats.InterlockedAdd(320u*4u+stripe,occluders);
            if(occluded)stats.InterlockedAdd(384u*4u+stripe,occluded);
            if(colorOnlyCount)stats.InterlockedAdd(448u*4u+stripe,colorOnlyCount);
            if(nativeCount)stats.InterlockedAdd(128u*4u+stripe,nativeCount);
            if(mg2Count)stats.InterlockedAdd(192u*4u+stripe,mg2Count);
            if(rejected)stats.InterlockedAdd(256u*4u+stripe,rejected);
        }
    }
    if(!visible)return;
    if(cpuOwned){
        RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
        marks[index]=1u;
    }
    if(remainingPass&&(seed||colorOnly))return; // Seed already drawn, or depth written with opaque color.
    RWByteAddressBuffer args=ResourceDescriptorHeap[
        (passIndex==3||seedPass||remainingPass)?CpuDepthArguments:(passIndex==4?PublishedDepthArguments:
        PublishedCullArguments)];
    RWByteAddressBuffer count=ResourceDescriptorHeap[
        (passIndex==3||seedPass||remainingPass)?CpuDepthCounter:(passIndex==4?PublishedDepthCounter:
        PublishedCullCounter)];
    // One reservation per active wave instead of one global atomic per instance.
    uint waveBase=0u;
    const uint waveCount=WaveActiveCountBits(true);
    const uint laneOffset=WavePrefixCountBits(true);
    if(WaveIsFirstLane())count.InterlockedAdd(0,waveCount,waveBase);
    waveBase=WaveReadLaneFirst(waveBase);
    args.Store4((waveBase+laneOffset)*16,MeshletDispatch(index,meshletCount,1u));
}
[numthreads(64,1,1)]
void BatchOpaqueMain(uint3 id:SV_DispatchThreadID){


    const uint BatchWidth=16u;
    uint localFirst=(id.x+id.y*4194240u)*BatchWidth;
    if(localFirst>=param0)return;
    StructuredBuffer<uint> visible=ResourceDescriptorHeap[CpuBatchVisibilitySRV];
    RWByteAddressBuffer args=ResourceDescriptorHeap[CpuBatchArgumentsUAV];
    RWByteAddressBuffer count=ResourceDescriptorHeap[CpuBatchCountUAV];
    uint head=0,mesh=0,material=0,flags=0,length=0;
    [loop]for(uint j=0;j<BatchWidth&&localFirst+j<param0;++j){
        uint ordinal=objectIndex+localFirst+j;
        uint index=OpaqueRenderIndex(ordinal);
        bool present=visible[index]!=0u;
        uint nextMesh=0,nextMaterial=0,nextFlags=0;
        if(present){
            GpuInstance instance=LoadCameraInstance(index);
            nextMesh=instance.mesh;
            nextMaterial=instance.material;
            nextFlags=instance.flags;
        }
        bool compatible=present&&length>0u&&nextMesh==mesh&&
                        nextMaterial==material&&nextFlags==flags;
        if(length>0u&&!compatible){
            uint dest;count.InterlockedAdd(param1*4u,1,dest);
            args.Store4((objectIndex+dest)*16u,MeshletDispatch(head,LoadMeshRange(mesh).meshletCount,length));
            length=0u;
        }
        if(present){
            if(length==0u){
                head=ordinal;mesh=nextMesh;material=nextMaterial;flags=nextFlags;
            }
            ++length;
        }
    }
    if(length>0u){
        uint dest;count.InterlockedAdd(param1*4u,1,dest);
        args.Store4((objectIndex+dest)*16u,MeshletDispatch(head,LoadMeshRange(mesh).meshletCount,length));
    }
}


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
     uint index=OpaqueRenderIndex(global);
     if(visible[index]==0u){marks[global]=0u;return;}
     StructuredBuffer<uint4> runs=ResourceDescriptorHeap[NonUniformResourceIndex(passIndex)];
     uint runIndex=TileBatchRun(passIndex,local);
     uint runFirst=runs[runIndex+1u].x;

     bool begins=local==runFirst||((local-runFirst)%16u)==0u;
     if(!begins){
         uint prior=OpaqueRenderIndex(global-1u);
         GpuInstance current=LoadInstance(index);
         GpuInstance previous=LoadInstance(prior);
         StructuredBuffer<uint> meshes=ResourceDescriptorHeap[HotMeshes];
         begins=visible[prior]==0u||meshes[index]!=meshes[prior]||
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
     if(block>=(param0-1u)/256u+1u)return;
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
         if(visible[OpaqueRenderIndex(global+next)]==0u||marks[global+next]!=0u)break;
         ++length;
     }
     StructuredBuffer<uint> selectedMeshes=ResourceDescriptorHeap[HotMeshes];
     uint mesh=selectedMeshes[OpaqueRenderIndex(global)];
     RWByteAddressBuffer args=ResourceDescriptorHeap[CpuBatchArgumentsUAV];
     RWByteAddressBuffer counters=ResourceDescriptorHeap[CpuBatchCountUAV];
     uint ignored;
     counters.InterlockedAdd((param2+runId)*4u,1u,ignored);
     const uint clusters=LoadMeshRange(mesh).meshletCount;
     args.Store4(dest*16u,MeshletDispatch(global,clusters,length));
     const uint forestCounter=asuint(tileViewer.w);
     if(forestCounter!=0u&&IsForestPage(mesh))
         counters.InterlockedAdd((forestCounter-1u)*4u,clusters*length,ignored);
 }


[numthreads(64,1,1)]
void RayMapFillMain(uint3 id:SV_DispatchThreadID){
    const uint local=id.x+id.y*4194240u;
    if(local>=param0)return;
    RWStructuredBuffer<uint> map=ResourceDescriptorHeap[param1];
    map[objectIndex+local]=passIndex==0xffffffffu?passIndex:passIndex+local;
}
[numthreads(64,1,1)]
void RayBuildMain(uint3 id:SV_DispatchThreadID){
    const uint NoRayTracingFlag=32;
    uint local=id.x+id.y*4194240u,index=objectIndex+local;
    if(param0!=0xffffffffu&&local>=param0)return;
    if(index>=RayInstanceCount)return;
    uint renderIndex=RenderInstanceForRay(index);
    StructuredBuffer<uint4> addresses=ResourceDescriptorHeap[BlasAddresses];
    RWByteAddressBuffer rays=ResourceDescriptorHeap[RayDescriptors];
    uint offset=index*64u;
    GpuInstance i=(GpuInstance)0;
    if(renderIndex!=0xffffffffu)i=LoadInstanceStable(renderIndex);
    if(renderIndex==0xffffffffu||(i.flags&NoRayTracingFlag)!=0u){
        // A reserved slot stays a valid descriptor during TLAS updates.
        rays.Store4(offset,asuint(float4(1,0,0,0)));
        rays.Store4(offset+16,asuint(float4(0,1,0,0)));
        rays.Store4(offset+32,asuint(float4(0,0,1,0)));
        rays.Store4(offset+48,uint4(index,0u,addresses[0].xy));
        return;
    }
    uint2 blas;
    if(IsForestPage(i.mesh)){
        StructuredBuffer<uint2> forest=ResourceDescriptorHeap[DomainResource(ResourceDomainMeshes,Mg2Addresses)];
        blas=forest[i.mesh&~Mg2MeshBit];
    }else{
        const uint4 address=addresses[i.mesh];
        blas=(i.flags&2)?address.zw:address.xy;
    }
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface material=materials[i.material];
    float3 x=Rotate(float3(i.scale.x,0,0),i.rotation),y=Rotate(float3(0,i.scale.y,0),i.rotation),z=Rotate(float3(0,0,i.scale.z),i.rotation);
    uint mask=(i.flags&4)?4:(i.flags&3)?2:1;
    const bool cutSource=renderIndex<cutGeometry.w&&i.id==0x80000000u+renderIndex&&(i.flags&19u)==0u;
    if(cutSource)mask=mask==4u?16u:8u;


    const uint rayCullDisabled=1u,rayForceOpaque=4u,rayForceNonOpaque=8u;
    // Forest cutout geometry has four-state OMM linkage in its native BLAS
    // and MG2 CLAS. FORCE_NON_OPAQUE would re-test known opaque microtriangles.
    const bool omm=(i.flags&OASIS_INSTANCE_FOREST)!=0u&&material.extra.y==1u;
    uint flags=rayCullDisabled|(material.extra.y==0u?rayForceOpaque:(omm?0u:rayForceNonOpaque));
    rays.Store4(offset,asuint(float4(x.x,y.x,z.x,i.positionRadius.x)));rays.Store4(offset+16,asuint(float4(x.y,y.y,z.y,i.positionRadius.y)));rays.Store4(offset+32,asuint(float4(x.z,y.z,z.z,i.positionRadius.z)));rays.Store4(offset+48,uint4(index|(mask<<24),flags<<24,blas));
}
// NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_OP_ARG_WRITE_INSTANCE.
// Pack only the dirty stable slots; no readback of instance descriptors.
[numthreads(64,1,1)]
void PtlasPackMain(uint3 id:SV_DispatchThreadID){
    uint local=id.x+id.y*4194240u;
    const bool addressOnly=(param0&0x80000000u)!=0u;
    if(local>=(param0&0x7fffffffu))return;
    uint slot=objectIndex+local, dst=passIndex+local*(addressOnly?16u:104u);
    RWByteAddressBuffer output=ResourceDescriptorHeap[param1];
    RWByteAddressBuffer rays=ResourceDescriptorHeap[RayDescriptors];
    if(addressOnly){
        const uint4 tail=rays.Load4(slot*64u+48u);
        output.Store4(dst,uint4(slot,tail.y&0xffffffu,tail.z,tail.w));
        return;
    }
    uint4 a=0,b=0,c=0,tail=0;
    if(slot<RayInstanceCount){
        uint src=slot*64u;
        a=rays.Load4(src);b=rays.Load4(src+16u);c=rays.Load4(src+32u);
        tail=rays.Load4(src+48u);
    }
    uint mask=tail.x>>24u;
    output.Store4(dst,a);output.Store4(dst+16u,b);output.Store4(dst+32u,c);
    output.Store4(dst+48u,uint4(0,0,0,0));output.Store2(dst+64u,uint2(0,0));
    output.Store4(dst+72u,uint4(slot,mask,tail.y&0xffffffu,tail.y>>24u));
    output.Store4(dst+88u,uint4(slot,slot>>8u,mask?tail.z:0u,mask?tail.w:0u));
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
    if(index>=objectIndex)return;
    RWStructuredBuffer<uint2> pairs=ResourceDescriptorHeap[LightBvhPairsUAV];
    if(index>=LightCount){pairs[index]=uint2(0xffffffffu,0xffffffffu);return;}
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    StructuredBuffer<LightBvhNode> bounds=ResourceDescriptorHeap[LightBvhBounds];
    LightBvhNode global=bounds[param0];
    float3 p=lights[index].positionRadius.xyz;
    uint code=all(isfinite(p))&&all(global.low<=global.high)?
        LightBvhMortonCode(p,global.low,global.high):0u;
    pairs[index]=uint2(code,index);
}


[numthreads(256,1,1)]
void LightBvhBitonic(uint3 id:SV_DispatchThreadID){
    uint index=LightBvhThread(id);
    uint partner=index^param0;
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


            node.low=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
            node.high=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);
        }else if(all(isfinite(p))&&isfinite(radius)){
            radius=max(0.0f,radius);
            float3 inflate=max(float3(0.002f,0.002f,0.002f),
                (abs(p)+radius)*0.00001f);
            if(IsRoomLight(light)&&all(isfinite(light.directionOuter))){
                // Direct shading, probe bounce and reflected room lighting
                // all reject points outside this same volume.
                const float3 below=light.directionOuter.xyz;
                const float3 above=float3(below.x,light.directionOuter.w,below.z);
                node.low=p-below-(inflate+RoomLightMargin);
                node.high=p+above+(inflate+RoomLightMargin);
            }else{
                node.low=p-radius-inflate;
                node.high=p+radius+inflate;
            }
        }else{

            node.low=float3(-LightBvhHuge,-LightBvhHuge,-LightBvhHuge);
            node.high=float3(LightBvhHuge,LightBvhHuge,LightBvhHuge);
        }
        node.lightIndex=original;
    }
    nodes[objectIndex+slot]=node;
}


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


// Prefix offsets saturate at the pool size. Each overflowing cluster uses the
// complete BVH query in PixelMain, independently of its neighbours.
groupshared uint clusterScan[64];
groupshared uint clusterCarry;

uint ClusterAdd(uint a,uint b) { return a+min(b,param0-a); }

uint ClusterInclusiveScan(uint lane,uint count) {
    clusterScan[lane]=min(count,param0);
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=1u;step<64u;step*=2u){
        const uint add=lane>=step?clusterScan[lane-step]:0u;
        GroupMemoryBarrierWithGroupSync();
        clusterScan[lane]=ClusterAdd(clusterScan[lane],add);
        GroupMemoryBarrierWithGroupSync();
    }
    return clusterScan[lane];
}

[numthreads(64,1,1)]
void ClusterCountMain(uint lane:SV_GroupIndex,uint3 group:SV_GroupID) {
    const uint cluster=group.x*64u+lane;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    uint count=0u;
    if(cluster<ClusterTotal()){
        const float4 sphere=ClusterSphere(cluster);
        RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[ClusterBoundsUAV];
        bounds[cluster]=sphere;
        uint cursor=1u;
        for(;;){
            const uint index=LightBvhNextLight(cursor,LightBvhLeafCount,sphere.xyz,sphere.w);
            if(index==0xffffffffu)break;
            const Light light=lights[index];
            if(((uint)light.parameters.z&2u)==0u&&InCluster(light,sphere))++count;
        }
    }
    const uint inclusive=ClusterInclusiveScan(lane,count);
    if(cluster<ClusterTotal()){
        RWStructuredBuffer<uint2> clusters=ResourceDescriptorHeap[ClusterUAV];
        const uint exclusive=lane==0u?0u:clusterScan[lane-1u];
        clusters[cluster]=uint2(exclusive,count);
    }
    if(lane==63u){
        RWStructuredBuffer<uint> totals=ResourceDescriptorHeap[ClusterGroupCountsUAV];
        totals[group.x]=inclusive;
    }
    if(cluster==0u){
        RWStructuredBuffer<uint> directional=ResourceDescriptorHeap[DirectionalIndicesUAV];
        RWStructuredBuffer<uint> length=ResourceDescriptorHeap[DirectionalCountUAV];
        uint n=0u;
        for(uint i=0u;i<LightCount;++i)
            if(((uint)lights[i].parameters.z&2u)!=0u)directional[n++]=i;
        length[0]=n;
    }
}

[numthreads(64,1,1)]
void ClusterOffsetsMain(uint lane:SV_GroupIndex) {
    StructuredBuffer<uint> totals=ResourceDescriptorHeap[ClusterGroupCounts];
    RWStructuredBuffer<uint> offsets=ResourceDescriptorHeap[ClusterGroupOffsetsUAV];
    if(lane==0u)clusterCarry=0u;
    GroupMemoryBarrierWithGroupSync();
    for(uint base=0u;base<param1;base+=64u){
        const uint index=base+lane;
        ClusterInclusiveScan(lane,index<param1?totals[index]:0u);
        const uint carry=clusterCarry;
        const uint exclusive=lane==0u?0u:clusterScan[lane-1u];
        if(index<param1)offsets[index]=ClusterAdd(carry,exclusive);
        GroupMemoryBarrierWithGroupSync();
        if(lane==63u)clusterCarry=ClusterAdd(carry,clusterScan[63]);
        GroupMemoryBarrierWithGroupSync();
    }
}

[numthreads(64,1,1)]
void ClusterFillMain(uint3 thread:SV_DispatchThreadID,uint3 group:SV_GroupID) {
    const uint cluster=thread.x;
    if(cluster>=ClusterTotal())return;
    RWStructuredBuffer<uint2> clusters=ResourceDescriptorHeap[ClusterUAV];
    StructuredBuffer<uint> offsets=ResourceDescriptorHeap[ClusterGroupOffsets];
    const uint2 local=clusters[cluster];
    const uint first=ClusterAdd(offsets[group.x],local.x);
    if(local.y>param0-first){
        clusters[cluster]=uint2(0u,ClusterOverflow);
        return;
    }
    clusters[cluster]=uint2(first,local.y);
    if(local.y==0u)return;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    RWStructuredBuffer<uint> indices=ResourceDescriptorHeap[ClusterIndicesUAV];
    StructuredBuffer<float4> bounds=ResourceDescriptorHeap[ClusterBounds];
    const float4 sphere=bounds[cluster];
    uint cursor=1u,count=0u;
    for(;;){
        const uint index=LightBvhNextLight(cursor,LightBvhLeafCount,sphere.xyz,sphere.w);
        if(index==0xffffffffu)break;
        const Light light=lights[index];
        if(((uint)light.parameters.z&2u)==0u&&InCluster(light,sphere))
            indices[first+count++]=index;
    }
}
[numthreads(8,8,1)]
void DenoiseMain(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height||id.z>=ViewCount)return;
    Texture2DArray<float4> input=ResourceDescriptorHeap[ReflectionResolved];
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthResolved];
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[HistoryOutput];
    RWTexture2DArray<float> outputDepth=ResourceDescriptorHeap[HistoryDepthOutput];
    float4 current=input.Load(int4(id,0));
    const float z=depth.Load(int4(id,0));
    [branch]if(HistoryValid&&current.a>0.f){
        const float2 uv=(float2(id.xy)+.5f)/float2(Width,Height);
        const float3 world=Unproject(uv,z,id.z);
        const float4 previous=mul(float4(world,1),previousVP[id.z]);
        const float2 puv=previous.xy/previous.w*float2(.5f,-.5f)+.5f;
        if(previous.w>0.f&&all(puv>0.f)&&all(puv<1.f)){
            Texture2DArray<float> oldDepth=ResourceDescriptorHeap[HistoryDepthPrevious];
            SamplerState linearSampler=SamplerDescriptorHeap[2];
            const float3 sampleUv=float3(puv*float2(Width,Height)/float2(outputDimensions.xy),id.z);
            const float d=oldDepth.SampleLevel(linearSampler,sampleUv,0);
            if(abs(LinearDepth(d)-LinearDepth(previous.z/previous.w))<.035f){
                Texture2DArray<float4> history=ResourceDescriptorHeap[HistoryPrevious];
                float3 lo=current.rgb,hi=current.rgb;
                [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){
                    const float3 value=input.Load(int4(clamp(int2(id.xy)+int2(x,y),
                        int2(0,0),int2(Width-1,Height-1)),id.z,0)).rgb;
                    lo=min(lo,value);hi=max(hi,value);
                }
                current.rgb=lerp(current.rgb,clamp(history.SampleLevel(linearSampler,sampleUv,0).rgb,lo,hi),.65f);
            }
        }
    }
    output[id]=current;
    outputDepth[id]=z;
}

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
void HotDataMain(uint3 id:SV_DispatchThreadID){uint localIndex=id.x+id.y*4194240u,index=objectIndex+localIndex;if(param0!=0xffffffffu&&localIndex>=param0)return;if(index>=InstanceCount+AdditionalCount)return;RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBoundsUAV];RWStructuredBuffer<uint> meshes=ResourceDescriptorHeap[HotMeshesUAV];GpuInstance i=LoadInstance(index);float4 local=LoadMeshRange(i.mesh).bounds;if(i.flags&2){StructuredBuffer<float4> deformed=ResourceDescriptorHeap[DeformedBounds];local=deformed[i.mesh];}float3 center=World(local.xyz,i);bounds[index]=float4(center,local.w*max(abs(i.scale.x),max(abs(i.scale.y),abs(i.scale.z))));meshes[index]=i.mesh;}
