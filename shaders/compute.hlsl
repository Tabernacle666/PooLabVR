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
void CullMain(uint3 id:SV_DispatchThreadID){uint local=id.x,index=objectIndex+local;StructuredBuffer<GpuInstance> instanceData=ResourceDescriptorHeap[Instances];StructuredBuffer<MeshRange> meshes=ResourceDescriptorHeap[Ranges];if(passIndex==2){if(local>=param0)return;GpuInstance instance=instanceData[index];if(instance.flags&(1|2|4))return;RWByteAddressBuffer args=ResourceDescriptorHeap[ShadowArguments];RWByteAddressBuffer count=ResourceDescriptorHeap[ShadowCounter];uint outIndex;count.InterlockedAdd(0,1,outIndex);args.Store4(outIndex*16,uint4(index,meshes[instance.mesh].meshletCount,1,1));return;}if(param0!=0xffffffffu&&local>=param0)return;if(index>=InstanceCount+AdditionalCount)return;if(instanceData[index].flags&(16|64|128))return;StructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBounds];StructuredBuffer<uint> meshIds=ResourceDescriptorHeap[HotMeshes];float4 sphere=bounds[index];uint mesh=meshIds[index];bool visible=false;for(uint eye=0;eye<ViewCount;eye++){float2 lo,hi;float nearDepth;if(Frustum(sphere,eye,lo,hi,nearDepth)&&(passIndex==0||!Occluded(lo,hi,nearDepth,eye)))visible=true;}if(!visible)return;RWByteAddressBuffer args=ResourceDescriptorHeap[passIndex==0?FrameArguments:CullArguments];RWByteAddressBuffer count=ResourceDescriptorHeap[passIndex==0?FrameCounter:CullCounter];uint outIndex;count.InterlockedAdd(0,1,outIndex);args.Store4(outIndex*16,uint4(index,meshes[mesh].meshletCount,1,1));}
[numthreads(64,1,1)]
void RayBuildMain(uint3 id:SV_DispatchThreadID){
    const uint NoRayTracingFlag=32;
    uint local=id.x,index=objectIndex+local;if(param0!=0xffffffffu&&local>=param0)return;if(index>=InstanceCount)return;StructuredBuffer<GpuInstance> instances=ResourceDescriptorHeap[Instances];GpuInstance i=instances[index];StructuredBuffer<uint4> addresses=ResourceDescriptorHeap[BlasAddresses];uint4 address=addresses[i.mesh];bool noRay=(i.flags&NoRayTracingFlag)!=0;uint2 blas=noRay?address.xy:((i.flags&2)?address.zw:address.xy);StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface material=materials[i.material];
    float3 x=Rotate(float3(i.scale.x,0,0),i.rotation),y=Rotate(float3(0,i.scale.y,0),i.rotation),z=Rotate(float3(0,0,i.scale.z),i.rotation);RWByteAddressBuffer rays=ResourceDescriptorHeap[RayDescriptors];uint offset=index*64;
    uint mask=(i.flags&NoRayTracingFlag)?0:(i.flags&4)?4:(i.flags&3)?2:1;uint flags=1|(material.extra.y?8:0);
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
[numthreads(64,1,1)]
void ClusterMain(uint3 id:SV_DispatchThreadID) {
    if(LightCount<=4)return; // Small light sets use the exact direct loop in scene.hlsl.
    uint x=(Width+63)/64,y=(Height+63)/64,total=x*y*24*ViewCount;if(id.x>=total)return;
    uint cx=id.x%x,cy=(id.x/x)%y,cz=(id.x/(x*y))%24,view=id.x/(x*y*24);
    float z0=timing.x*pow(timing.y/timing.x,float(cz)/24),z1=timing.x*pow(timing.y/timing.x,float(cz+1)/24);
    float2 uv0=float2(cx*64,cy*64)/float2(Width,Height),uv1=min(float2((cx+1)*64,(cy+1)*64)/float2(Width,Height),1);
    float3 a=Unproject(uv0,DepthAt(z0),view),b=Unproject(uv1,DepthAt(z1),view),center=(a+b)*.5;float radius=0;
    [unroll]for(uint i=0;i<8;i++){float2 uv=float2((i&1)?uv0.x:uv1.x,(i&2)?uv0.y:uv1.y);radius=max(radius,length(Unproject(uv,DepthAt((i&4)?z0:z1),view)-center));}
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];uint4 mask=0;
    for(uint l=0;l<LightCount;l++)if(((uint)lights[l].parameters.z&2)||distance(center,lights[l].positionRadius.xyz)<radius+lights[l].positionRadius.w)mask[l/32]|=1u<<(l%32);
    RWStructuredBuffer<uint4> clusters=ResourceDescriptorHeap[ClusterUAV];clusters[id.x]=mask;
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
void HotDataMain(uint3 id:SV_DispatchThreadID){uint localIndex=id.x,index=objectIndex+localIndex;if(param0!=0xffffffffu&&localIndex>=param0)return;if(index>=InstanceCount+AdditionalCount)return;StructuredBuffer<GpuInstance> instances=ResourceDescriptorHeap[Instances];RWStructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBoundsUAV];RWStructuredBuffer<uint> meshes=ResourceDescriptorHeap[HotMeshesUAV];GpuInstance i=instances[index];StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];float4 local=ranges[i.mesh].bounds;if(i.flags&2){StructuredBuffer<float4> deformed=ResourceDescriptorHeap[DeformedBounds];local=deformed[i.mesh];}float3 center=World(local.xyz,i);bounds[index]=float4(center,local.w*max(abs(i.scale.x),max(abs(i.scale.y),abs(i.scale.z))));meshes[index]=i.mesh;}
