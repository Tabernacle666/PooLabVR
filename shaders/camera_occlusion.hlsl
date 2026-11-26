#include "common.hlsli"
#include "camera_visibility.hlsli"

// Four compacted boxes per mesh group. All eyes share output topology and
// indices. Back-face culling discards the opposite faces in hardware.
// Use a constant conservative reverse-Z depth and w=1 for every vertex:
// no depth extrapolation, including under conservative rasterization.
// Only the visibility UAV is written; depth and solar receivers are read-only.
struct RasterProxyVertex {
    float4 position:SV_Position;
    nointerpolation uint instanceIndex:TEXCOORD0;
};
static const uint3 ProxyTriangles[12]={
    uint3(0,4,2),uint3(2,4,6),uint3(1,3,5),uint3(3,7,5),
    uint3(0,1,4),uint3(1,5,4),uint3(2,6,3),uint3(3,6,7),
    uint3(0,2,1),uint3(1,2,3),uint3(4,5,6),uint3(5,7,6)
};
[outputtopology("triangle")]
[numthreads(32,1,1)]
void RasterOcclusionMS(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
    out vertices RasterProxyVertex verts[32],out indices uint3 tris[48]){
    const uint first=(group.x+group.y*65535u)*4u;
    const uint boxes=min(4u,objectIndex>first?objectIndex-first:0u);
    SetMeshOutputCounts(boxes*8u,boxes*12u);
    if(tid<boxes*8u){
        RWByteAddressBuffer candidates=ResourceDescriptorHeap[CpuCullArguments];
        const uint4 record=candidates.Load4(16u+(first+tid/8u)*16u);
        RasterProxyVertex v;
        v.instanceIndex=record.x;
        v.position=float4(2.f,2.f,0.f,1.f);
        if((record.y&(1u<<view))!=0u){
            const GpuInstance instance=LoadCameraInstance(record.x);
            StructuredBuffer<float4> bounds=ResourceDescriptorHeap[HotBounds];
            const float4 sphere=bounds[record.x];
            const float4 box=CameraBox(instance,sphere);
            const uint corner=tid&7u;
            const float3 signCorner=float3((corner&1u)?1.f:-1.f,(corner&2u)?1.f:-1.f,(corner&4u)?1.f:-1.f);
            const float3 p=CameraBoxCenter(instance,sphere,box)+Rotate(box.xyz*signCorner,instance.rotation);
            const float4 clip=mul(float4(p,1.f),vp[view]);
            const float nearest=asfloat(view==0u?record.z:record.w);
            v.position=float4(clip.xy/clip.w,nearest,1.f);
        }
        verts[tid]=v;
    }
    for(uint t=tid;t<boxes*12u;t+=32u)
        tris[t]=ProxyTriangles[t%12u]+(t/12u)*8u;
}

[earlydepthstencil]
void RasterOcclusionPS(RasterProxyVertex input){
    RWStructuredBuffer<uint> marks=ResourceDescriptorHeap[CpuBatchVisibilityUAV];
    InterlockedOr(marks[input.instanceIndex],1u);
}
