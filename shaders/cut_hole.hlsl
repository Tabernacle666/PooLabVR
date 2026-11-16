// Pending raster apertures. Successfully baked cuts are removed from this
// queue; they live permanently in the published triangle geometry.
// The first entry's padding0 contains the pending count and mixed-target bit.
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
    uint rayInstanceCount,instanceCount,impactCapacity,holeCapacity;
};
[numthreads(1,1,1)]
void StoreFirstCutHole(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlDescriptor];
    if(control.Load(4)!=0||control.Load(8)!=0)return;
    uint impactsCount=min(control.Load(0),impactCapacity);
    if(impactsCount==0)return;
    StructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactDescriptor];
    RWStructuredBuffer<CutHole> holes=ResourceDescriptorHeap[holeDescriptor];
    uint header=holes[0].padding0;
    bool mixedTargets=(header&0x80000000u)!=0;
    uint count=min(header&0x7fffffffu,holeCapacity);
    for(uint i=0;i<impactsCount&&count<holeCapacity;++i){
        ProjectileImpact impact=impacts[i];
        if(impact.instanceId>=instanceCount||impact.radius<=0)continue;
        CutHole hole=(CutHole)0;
        hole.center=impact.position;
        hole.radius=impact.radius;
        hole.renderInstance=impact.instanceId;
        hole.active=1;
        hole.padding1=impact.projectileId;
        if(count!=0&&holes[0].renderInstance!=impact.instanceId)mixedTargets=true;
        holes[count]=hole;
        ++count;
    }
    CutHole first=holes[0];
    first.padding0=count|(mixedTargets?0x80000000u:0);
    holes[0]=first;
}
