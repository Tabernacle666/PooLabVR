// A generation is made visible to the future publisher only after all
// writes and every capacity/ambiguity check have completed on the GPU.
// No draw or acceleration structure may use a generation with ready == 0.
#include "impact_shared.hlsli"
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint padding;
};
struct CutGeneration {
    uint mesh,solid,triangles,ready;
    uint capTriangles,projectileId,flags,reserved;
};
cbuffer GenerationParameters:register(b0){
    uint resolvedDescriptor,surfaceControl,segmentControl,capControl;
    uint generationDescriptor,triangleCapacity,impactsDescriptor,_reserved;
};
[numthreads(1,1,1)]
void FinalizeCutGeneration(uint3 id:SV_DispatchThreadID){
    StructuredBuffer<ResolvedImpact> resolved=ResourceDescriptorHeap[resolvedDescriptor];
    RWByteAddressBuffer surface=ResourceDescriptorHeap[surfaceControl];
    RWByteAddressBuffer segments=ResourceDescriptorHeap[segmentControl];
    RWByteAddressBuffer cap=ResourceDescriptorHeap[capControl];
    RWStructuredBuffer<CutGeneration> output=ResourceDescriptorHeap[generationDescriptor];
    ResolvedImpact hit=resolved[0];
    uint triangles=surface.Load(0),flags=surface.Load(4)|surface.Load(8);
    flags|=segments.Load(4)|segments.Load(8);
    flags|=cap.Load(12)|cap.Load(16);
    bool good=hit.status==0&&flags==0&&triangles>0&&triangles<=triangleCapacity;
    CutGeneration generation=(CutGeneration)0;
    generation.mesh=hit.mesh;generation.solid=hit.solid;
    generation.triangles=good?triangles:0;
    generation.capTriangles=good?cap.Load(8):0;
    generation.projectileId=0;
    if(good){
        StructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactsDescriptor];
        generation.projectileId=impacts[0].projectileId;
    }
    generation.ready=good?1:0;
    generation.flags=flags;
    output[0]=generation;
}
