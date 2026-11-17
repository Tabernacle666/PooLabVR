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
    uint rangesDescriptor,totalInstanceCount,drawArgumentsDescriptor,pageCount;
    uint pageTriangles,pageMeshlets,_paddingDraw0,_paddingDraw1;
};
[numthreads(1,1,1)]
void FinalizeCutGeneration(uint3 tid:SV_DispatchThreadID){
    StructuredBuffer<ResolvedImpact> resolved=ResourceDescriptorHeap[resolvedDescriptor];
    RWByteAddressBuffer surface=ResourceDescriptorHeap[surfaceControl];
    RWByteAddressBuffer segments=ResourceDescriptorHeap[segmentControl];
    RWByteAddressBuffer cap=ResourceDescriptorHeap[capControl];
    RWByteAddressBuffer impactControl=ResourceDescriptorHeap[impactControlDescriptor];
    RWStructuredBuffer<CutGeneration> output=ResourceDescriptorHeap[generationDescriptor];
    ResolvedImpact hit=resolved[0];
    uint impactCount=impactControl.Load(0);
    if(impactCount!=1||hit.status!=0||hit.instanceId>=pageCount)return;
    CutGeneration previous=output[hit.instanceId];
    uint triangles=surface.Load(0);
    uint affected=surface.Load(12);
    uint capTriangles=cap.Load(8);
    uint flags=surface.Load(4)|surface.Load(8)|
        segments.Load(4)|segments.Load(8)|cap.Load(12)|cap.Load(16);
    if(affected==0)flags|=16u;
    // A later multi-impact queue will process jobs independently. A generation
    // covering only the first of several impacts is not complete.
    bool good=impactCount==1&&hit.status==0&&flags==0&&
        triangles<=triangleCapacity&&triangles<=pageTriangles&&
        hit.mesh<rangeCount&&
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
    output[hit.instanceId]=gen;
    RWByteAddressBuffer drawArguments=ResourceDescriptorHeap[drawArgumentsDescriptor];
    // 16-byte engine indirect signature: object root constant, mesh dispatch.
    drawArguments.Store4(hit.instanceId*16u,
        uint4(hit.instanceId,(gen.triangles+20u)/21u,1u,1u));

}
