// GPU-owned dispatch dimensions. Empty frames generate zero work without
// transferring impact counters to the CPU.
#include "impact_shared.hlsli"
struct MeshRange {uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;float4 bounds;};
struct CutGeneration {
    uint mesh,solid,triangles,ready;
    uint capTriangles,projectileId,flags,renderInstance;
    uint originalMaterial,sourceFirstIndex,sourceIndexCount,additionalImpacts;
    uint requiredTriangles,requiredSegments,requiredPolygons,requiredCapVertices;
};
cbuffer ScheduleParameters:register(b0){
    uint resolvedDescriptor,impactControlDescriptor,rangesDescriptor,argumentsDescriptor;
    uint maxTriangleCount,faceCount,generationDescriptor,_padding1;
};
[numthreads(1,1,1)]
void ScheduleCutDispatches(uint3 tid:SV_DispatchThreadID){
    StructuredBuffer<ResolvedImpact> hits=ResourceDescriptorHeap[resolvedDescriptor];
    RWByteAddressBuffer control=ResourceDescriptorHeap[impactControlDescriptor];
    RWByteAddressBuffer args=ResourceDescriptorHeap[argumentsDescriptor];
    StructuredBuffer<CutGeneration> generation=ResourceDescriptorHeap[generationDescriptor];
    bool valid=generation[0].ready==0&&control.Load(0)!=0&&control.Load(4)==0&&control.Load(8)==0&&hits[0].status==0;
    uint groups=0;
    if(valid){
        StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[rangesDescriptor];
        uint count=min(maxTriangleCount,ranges[hits[0].mesh].indexCount/3u);
        groups=(count+63u)/64u;
    }
    uint x=min(groups,65535u);
    uint tiles=groups==0?1:(groups+65534u)/65535u;
    args.Store3(0,uint3(x,groups==0?1:tiles,1));
    args.Store3(12,uint3(x,valid?faceCount:1,groups==0?1:tiles));
    args.Store3(24,uint3(valid?2u:0u,1u,1u));
}
