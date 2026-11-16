// Resolve a DXR hit to the mesh range and welded solid entirely on the GPU.
#include "impact_shared.hlsli"
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint _pad;
};
struct GpuInstance {float4 positionRadius;float4 rotation;float4 scale;uint mesh;uint material;uint id;uint flags;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
cbuffer ResolveParameters:register(b0){
    uint impactsDescriptor,rayMapDescriptor,instancesDescriptor,rangesDescriptor;
    uint solidsDescriptor,resolvedDescriptor,impactCount,rayInstanceCount;
    uint rangeCount,solidCount,worldInstanceCount,impactControlDescriptor;
    uint publishedInstancesDescriptor,publishedInstanceCount,_reserved2,_reserved3;
};
static const uint Invalid=0xffffffff;
float3 RotateByQuaternion(float3 v,float4 q){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}

[numthreads(64,1,1)]
void ResolveImpact(uint3 dispatch:SV_DispatchThreadID){
    uint index=dispatch.x+dispatch.y*4194240u;if(index>=impactCount)return;
    RWByteAddressBuffer control=ResourceDescriptorHeap[impactControlDescriptor];
    if(index>=control.Load(0)||control.Load(4)||control.Load(8)){
        RWStructuredBuffer<ResolvedImpact> stale=ResourceDescriptorHeap[resolvedDescriptor];
        ResolvedImpact invalid=(ResolvedImpact)0;invalid.status=1;
        stale[index]=invalid;
        return;
    }
    StructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactsDescriptor];
    StructuredBuffer<uint> rayMap=ResourceDescriptorHeap[rayMapDescriptor];
    StructuredBuffer<GpuInstance> instances=ResourceDescriptorHeap[instancesDescriptor];
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[rangesDescriptor];
    StructuredBuffer<uint> solids=ResourceDescriptorHeap[solidsDescriptor];
    RWStructuredBuffer<ResolvedImpact> resolved=ResourceDescriptorHeap[resolvedDescriptor];
    ProjectileImpact hit=impacts[index];ResolvedImpact outHit=(ResolvedImpact)0;
    outHit.worldPosition=hit.position;outHit.worldRadius=hit.radius;outHit.instanceId=hit.instanceId;
    outHit.solid=Invalid;outHit.mesh=Invalid;outHit.status=1;
    if(hit.instanceId>=rayInstanceCount){resolved[index]=outHit;return;}
    uint instanceIndex=rayMap[hit.instanceId];
    if(instanceIndex>=worldInstanceCount+publishedInstanceCount){resolved[index]=outHit;return;}
    GpuInstance instance;
    if(instanceIndex<worldInstanceCount)instance=instances[instanceIndex];
    else{
        StructuredBuffer<GpuInstance> published=ResourceDescriptorHeap[publishedInstancesDescriptor];
        instance=published[instanceIndex-worldInstanceCount];
    }
    if(instance.mesh>=rangeCount){resolved[index]=outHit;return;}
    MeshRange range=ranges[instance.mesh];
    if(range.firstIndex%3||hit.primitiveIndex>=range.indexCount/3){resolved[index]=outHit;return;}
    uint triangleIndex=range.firstIndex/3+hit.primitiveIndex;
    if(triangleIndex>=solidCount||solids[triangleIndex]==Invalid){resolved[index]=outHit;return;}
    // A sphere remains a sphere under uniform scale and rotation. Explicitly
    // reject other transforms until ellipsoidal cuts are implemented.
    float3 scale=instance.scale.xyz;
    float magnitude=abs(scale.x);
    if(magnitude<1e-8||abs(abs(scale.y)-magnitude)>magnitude*1e-5||
       abs(abs(scale.z)-magnitude)>magnitude*1e-5){outHit.status=2;resolved[index]=outHit;return;}
    float4 inverseRotation=float4(-instance.rotation.xyz,instance.rotation.w);
    outHit.localPosition=RotateByQuaternion(hit.position-instance.positionRadius.xyz,inverseRotation)/scale;
    outHit.localRadius=hit.radius/magnitude;outHit.solid=solids[triangleIndex];outHit.mesh=instance.mesh;outHit.status=0;
    resolved[index]=outHit;
}
