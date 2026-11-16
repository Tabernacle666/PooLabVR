// First visible impact. GPU hit -> persistent sphere aperture on one raster instance.
// This is a raster opening, not yet a replacement of the DXR BLAS geometry.
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint _pad;
};
struct CutHole {
    float3 center;float radius;
    uint renderInstance;uint active;uint padding0;uint padding1;
};
cbuffer HoleParameters:register(b0){
    uint impactDescriptor,controlDescriptor,rayMapDescriptor,holeDescriptor;
    uint rayInstanceCount,instanceCount,_pad0,_pad1;
};
[numthreads(1,1,1)]
void StoreFirstCutHole(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlDescriptor];
    if(control.Load(0)==0||control.Load(4)!=0||control.Load(8)!=0)return;
    StructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactDescriptor];
    RWStructuredBuffer<CutHole> holes=ResourceDescriptorHeap[holeDescriptor];
    CutHole existing=holes[0];
    if(existing.active!=0)return;
    ProjectileImpact impact=impacts[0];
    if(impact.instanceId>=rayInstanceCount||impact.radius<=0)return;
    StructuredBuffer<uint> rayMap=ResourceDescriptorHeap[rayMapDescriptor];
    uint instanceIndex=rayMap[impact.instanceId];
    if(instanceIndex>=instanceCount)return;
    CutHole hole=(CutHole)0;
    hole.center=impact.position;
    hole.radius=impact.radius;
    hole.renderInstance=instanceIndex;
    hole.active=1;
    holes[0]=hole;
}
