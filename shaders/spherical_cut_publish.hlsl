// Pack a completed triangle-soup staging buffer into the engine's meshlet ABI.
// This writes a new generation; the caller publishes it only after all passes
// succeed and previous GPU readers have finished using the old generation.
#include "impact_shared.hlsli"
#include "cut_source.hlsli"
struct Vertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct Meshlet {uint vertexOffset;uint triangleOffset;uint vertexCount;uint triangleCount;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
struct MeshletRegion {float3 lower;uint mesh;float3 upper;uint reserved;};
cbuffer PublishParameters : register(b0) {
    uint stagingVertices,stagingControl,verticesDescriptor,indicesDescriptor;
    uint meshletsDescriptor,remapDescriptor,trianglesDescriptor,rangesDescriptor;
    uint regionsDescriptor,countOffset,overflowOffset,ambiguityOffset;
    uint vertexBase,indexBase,meshletBase,remapBase;
    uint triangleBase,rangeIndex,triangleCapacity,solidsDescriptor;
    uint stagingSolids,sourceRanges,resolvedImpact,impactIndex;
    float4 _unusedBounds;
    uint4 destinationA0,destinationA1,destinationB0,destinationB1;
    uint4 generationAndPadding;
};
static const uint TrianglesPerMeshlet=21;
float4 SourceBounds(){
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    StructuredBuffer<MeshRange> source=ResourceDescriptorHeap[sourceRanges];
    ResolvedImpact hit=impacts[impactIndex];
    float4 original=source[hit.mesh].bounds;
    // Caps may extend outside one render region while remaining inside the
    // original solid. Include the entire cut volume in conservative bounds.
    original.w=max(original.w,length(hit.localPosition-original.xyz)+hit.localRadius);
    return original;
}
bool ValidCut(){
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    return impacts[impactIndex].status==0;
}

[numthreads(64,1,1)]
void PackCutGeometry(uint3 dispatch:SV_DispatchThreadID){
    uint index=dispatch.x+dispatch.y*4194240u;
    RWByteAddressBuffer control=ResourceDescriptorHeap[stagingControl];
    if(!ValidCut()||control.Load(overflowOffset)||control.Load(ambiguityOffset))return;
    uint count=control.Load(countOffset);
    if(count>triangleCapacity||index>=count)return;
    StructuredBuffer<Vertex> source=ResourceDescriptorHeap[stagingVertices];
    StructuredBuffer<uint> sourceSolids=ResourceDescriptorHeap[stagingSolids];
    uint4 a0=SelectCutDestination(generationAndPadding.x,destinationA0,destinationB0);
    uint4 a1=SelectCutDestination(generationAndPadding.x,destinationA1,destinationB1);
    RWStructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[NonUniformResourceIndex(a0.x)];
    RWStructuredBuffer<uint> indices=ResourceDescriptorHeap[NonUniformResourceIndex(a0.y)];
    RWStructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[NonUniformResourceIndex(a0.z)];
    RWStructuredBuffer<uint> remap=ResourceDescriptorHeap[NonUniformResourceIndex(a0.w)];
    RWStructuredBuffer<uint3> triangles=ResourceDescriptorHeap[NonUniformResourceIndex(a1.x)];
    RWStructuredBuffer<MeshletRegion> regions=ResourceDescriptorHeap[NonUniformResourceIndex(a1.z)];
    RWStructuredBuffer<uint> solids=ResourceDescriptorHeap[NonUniformResourceIndex(a1.w)];
    uint meshlet=index/TrianglesPerMeshlet,local=index%TrianglesPerMeshlet;
    uint first=index*3;
    for(uint i=0;i<3;i++){
        vertices[vertexBase+first+i]=source[first+i];
        indices[indexBase+first+i]=vertexBase+first+i;
        remap[remapBase+first+i]=vertexBase+first+i;
    }
    triangles[triangleBase+index]=uint3(local*3,local*3+1,local*3+2);
    solids[indexBase/3+index]=sourceSolids[index];
    if(local==0){
        uint meshletTriangles=min(TrianglesPerMeshlet,count-index);
        Meshlet m;m.vertexOffset=remapBase+first;m.triangleOffset=triangleBase+index;
        m.vertexCount=meshletTriangles*3;m.triangleCount=meshletTriangles;meshlets[meshletBase+meshlet]=m;
        // Conservative bounds remain valid for every subset of the source solid.
        float4 rangeBounds=SourceBounds();
        MeshletRegion region;region.lower=rangeBounds.xyz-rangeBounds.www;
        region.upper=rangeBounds.xyz+rangeBounds.www;region.mesh=rangeIndex;region.reserved=0;
        regions[meshletBase+meshlet]=region;
    }
}

[numthreads(1,1,1)]
void FinalizeCutRange(uint3 dispatch:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[stagingControl];
    if(!ValidCut()||control.Load(overflowOffset)||control.Load(ambiguityOffset))return;
    uint count=control.Load(countOffset);if(count==0||count>triangleCapacity)return;
    uint4 selected=SelectCutDestination(generationAndPadding.x,destinationA1,destinationB1);
    RWStructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[NonUniformResourceIndex(selected.y)];
    MeshRange range;range.firstMeshlet=meshletBase;range.meshletCount=(count+TrianglesPerMeshlet-1)/TrianglesPerMeshlet;
    range.firstVertex=vertexBase;range.vertexCount=count*3;range.firstIndex=indexBase;range.indexCount=count*3;
    range.bounds=SourceBounds();ranges[rangeIndex]=range;
}
