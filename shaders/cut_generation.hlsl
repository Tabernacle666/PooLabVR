// GPU-produced publication descriptor. Ranges and materials are copied from
// the actual hit instance; none of these fields are guessed on the CPU.
#include "impact_shared.hlsli"
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint padding;
};
struct MeshRange {
    uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;
    float4 bounds;
};
struct CutHole {
    float3 center;float radius;
    uint renderInstance;uint active;uint padding0;uint padding1;
};
struct CutGeneration {
    uint mesh,solid,triangles,ready;
    uint capTriangles,projectileId,flags,renderInstance;
    uint originalMaterial,sourceFirstIndex,sourceIndexCount,additionalImpacts;
    uint requiredTriangles,requiredSegments,requiredPolygons,requiredCapVertices;
};
cbuffer GenerationParameters:register(b0){
    uint resolvedDescriptor,surfaceControl,segmentControl,capControl;
    uint generationDescriptor,triangleCapacity,impactsDescriptor,impactControlDescriptor;
    uint instanceRoutesDescriptor,publishedInstancesDescriptor,worldInstanceCount,rangeCount;
    uint rangesDescriptor,totalInstanceCount,holesDescriptor,holeCapacity;
};
[numthreads(1,1,1)]
void FinalizeCutGeneration(uint3 tid:SV_DispatchThreadID){
    StructuredBuffer<ResolvedImpact> resolved=ResourceDescriptorHeap[resolvedDescriptor];
    RWByteAddressBuffer surface=ResourceDescriptorHeap[surfaceControl];
    RWByteAddressBuffer segments=ResourceDescriptorHeap[segmentControl];
    RWByteAddressBuffer cap=ResourceDescriptorHeap[capControl];
    RWByteAddressBuffer impactControl=ResourceDescriptorHeap[impactControlDescriptor];
    RWStructuredBuffer<CutGeneration> output=ResourceDescriptorHeap[generationDescriptor];
    CutGeneration previous=output[0];
    ResolvedImpact hit=resolved[0];
    if(previous.ready!=0&&previous.renderInstance!=hit.instanceId)return;
    uint impactCount=impactControl.Load(0);
    uint triangles=surface.Load(0);
    uint affected=surface.Load(12);
    uint capTriangles=cap.Load(8);
    uint flags=surface.Load(4)|surface.Load(8);
    if(affected==0)flags|=16u;
    // A later multi-impact queue will process jobs independently. A generation
    // covering only the first of several impacts is not complete.
    bool good=impactCount==1&&hit.status==0&&flags==0&&
        triangles<=triangleCapacity&&hit.mesh<rangeCount&&
        hit.instanceId<totalInstanceCount;
    CutGeneration gen=(CutGeneration)0;
    gen.mesh=hit.mesh;
    gen.solid=hit.solid;
    gen.flags=previous.ready!=0?(previous.flags^1u)&1u:0u;
    gen.additionalImpacts=impactCount>0?impactCount-1:0;
    gen.requiredTriangles=surface.Load(16);
    gen.requiredSegments=segments.Load(12);
    gen.requiredPolygons=cap.Load(20);
    gen.requiredCapVertices=cap.Load(24);
    if(good){
        StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[rangesDescriptor];
        MeshRange source=ranges[hit.mesh];
        CutInstance object;
        good=LoadCutInstance(instanceRoutesDescriptor,hit.instanceId,object)&&
             source.indexCount!=0&&object.mesh==hit.mesh;
        if(good){
            StructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactsDescriptor];
            gen.projectileId=impacts[0].projectileId;
            gen.renderInstance=hit.instanceId;
            gen.originalMaterial=object.material;
            gen.sourceFirstIndex=source.firstIndex;
            gen.sourceIndexCount=source.indexCount;
            gen.triangles=triangles;
            gen.capTriangles=(cap.Load(12)||cap.Load(16))?0:capTriangles;
        }
    }
    gen.ready=good?1:0;
    if(!good)return;
    output[0]=gen;

    // This projectile's hole has become real triangles. Remove its raster
    // fallback exactly once, without discarding previously failed cuts.
    // Only this one GPU thread changes the compact hole queue.
    RWStructuredBuffer<CutHole> holes=ResourceDescriptorHeap[holesDescriptor];
    uint count=min(holes[0].padding0&0x7fffffffu,holeCapacity);
    uint kept=0;
    for(uint i=0;i<count;++i){
        CutHole hole=holes[i];
        if(hole.active!=0&&hole.renderInstance==gen.renderInstance&&
           hole.padding1==gen.projectileId)continue;
        holes[kept++]=hole;
    }
    if(kept==0){
        holes[0]=(CutHole)0;
    }else{
        bool mixed=false;
        CutHole first=holes[0];
        for(uint j=1;j<kept;++j)
            if(holes[j].renderInstance!=first.renderInstance){mixed=true;break;}
        first.padding0=kept|(mixed?0x80000000u:0u);
        holes[0]=first;
    }
}
