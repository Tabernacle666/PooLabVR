// Resolve a DXR hit to the mesh range and welded solid entirely on the GPU.
#include "impact_shared.hlsli"
#include "cut_source.hlsli"
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint _pad;
};
struct CutVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
cbuffer ResolveParameters:register(b0){
    uint impactsDescriptor,rayMapDescriptor,instanceRoutesDescriptor,rangesDescriptor;
    uint solidsDescriptor,resolvedDescriptor,impactCount,rayInstanceCount;
    uint rangeCount,solidCount,worldInstanceCount,impactControlDescriptor;
    uint publishedInstancesDescriptor,publishedInstanceCount,sourceVerticesDescriptor,sourceIndicesDescriptor;
    uint4 sourceOriginal,sourceFirst,sourceSecond,sourceControl;
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
    RWStructuredBuffer<ResolvedImpact> resolved=ResourceDescriptorHeap[resolvedDescriptor];
    ProjectileImpact hit=impacts[index];ResolvedImpact outHit=(ResolvedImpact)0;
    outHit.worldPosition=hit.position;outHit.worldRadius=hit.radius;outHit.instanceId=hit.instanceId;
    outHit.solid=Invalid;outHit.mesh=Invalid;outHit.status=1;
    // Projectile hits carry the global raster instance index, whether they
    // originated in the compact DXR scene or direct resident-triangle scan.
    uint instanceIndex=hit.instanceId;
    if(instanceIndex>=worldInstanceCount+publishedInstanceCount){resolved[index]=outHit;return;}
    CutInstance instance;
    if(!LoadCutInstance(instanceRoutesDescriptor,instanceIndex,instance)){resolved[index]=outHit;return;}
    // The published geometry identifies this raster instance, not a compact
    // TLAS InstanceID; both differ when physical publications are present.
    outHit.instanceId=instanceIndex;
    if(instance.mesh>=rangeCount){resolved[index]=outHit;return;}
    CutSourceChoice src=SelectCutSource(sourceControl.x,instanceIndex,instance.mesh,sourceControl.y,
                                        sourceOriginal,sourceFirst,sourceSecond);
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[NonUniformResourceIndex(src.ranges)];
    StructuredBuffer<uint> solids=ResourceDescriptorHeap[NonUniformResourceIndex(src.solids)];
    MeshRange range=ranges[src.mesh];
    if(range.firstIndex%3||hit.primitiveIndex>=range.indexCount/3){resolved[index]=outHit;return;}
    uint triangleIndex=range.firstIndex/3+hit.primitiveIndex;
    uint limit=src.published!=0?src.count:solidCount;
    if(triangleIndex>=limit){resolved[index]=outHit;return;}
    uint solid=solids[triangleIndex];
    if(solid==Invalid&&src.published==0){
        StructuredBuffer<CutVertex> vertices=ResourceDescriptorHeap[NonUniformResourceIndex(src.vertices)];
        StructuredBuffer<uint> indices=ResourceDescriptorHeap[NonUniformResourceIndex(src.indices)];
        uint first=range.firstIndex+hit.primitiveIndex*3;
        float3 a=vertices[indices[first]].position;
        float3 b=vertices[indices[first+1]].position;
        float3 c=vertices[indices[first+2]].position;
        float3 n=cross(b-a,c-a);
        if(dot(n,n)<1e-14){resolved[index]=outHit;return;}
        // Mark a vertical open sheet separately from an open ground surface.
        if(n.y*n.y<.0625*dot(n,n))solid=0xfffffffeu;
    }
    // A sphere remains a sphere under uniform scale and rotation. Explicitly
    // reject other transforms until ellipsoidal cuts are implemented.
    float3 scale=instance.scale.xyz;
    float magnitude=abs(scale.x);
    if(magnitude<1e-8||abs(abs(scale.y)-magnitude)>magnitude*1e-5||
       abs(abs(scale.z)-magnitude)>magnitude*1e-5){outHit.status=2;resolved[index]=outHit;return;}
    float4 inverseRotation=float4(-instance.rotation.xyz,instance.rotation.w);
    outHit.localPosition=RotateByQuaternion(hit.position-instance.positionRadius.xyz,inverseRotation)/scale;
    outHit.localRadius=hit.radius/magnitude;outHit.solid=solid;outHit.mesh=instance.mesh;outHit.status=0;
    resolved[index]=outHit;
}
