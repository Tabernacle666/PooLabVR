struct ResolvedImpact {
    float3 worldPosition;float worldRadius;
    float3 localPosition;float localRadius;
    uint solid;uint mesh;uint instanceId;uint status;
};

struct CutInstance {
    float4 positionRadius,rotation,scale;
    uint mesh,material,id,flags;
};
struct CutInstanceRange {uint first,count,descriptor,kind;};
struct CutResidentInstance {
    CutInstance instance;
    float4 sector;
    uint identityLow,identityHigh,enabled,reserved;
};

// Use the same instance routing as rasterization and the TLAS builder.
// Resident tiles have their own 96-byte records rather than entries in the
// 64-byte world-instance buffer.
bool LoadCutInstance(uint rangesDescriptor,uint index,out CutInstance instance){
    instance.positionRadius=0;instance.rotation=0;instance.scale=0;
    instance.mesh=0;instance.material=0;instance.id=0;instance.flags=0;
    StructuredBuffer<CutInstanceRange> routes=ResourceDescriptorHeap[rangesDescriptor];
    uint count=routes[0].kind;
    if(count==0)return false;
    uint lo=0,hi=count;
    while(lo+1u<hi){
        uint mid=(lo+hi)>>1;
        if(routes[mid].first<=index)lo=mid;else hi=mid;
    }
    CutInstanceRange route=routes[lo];
    if(route.descriptor==0xffffffffu||index<route.first||index-route.first>=route.count)return false;
    uint local=index-route.first;
    // Resident page routing uses bit 31 plus the first element in the
    // shared page. Do not resolve a selected MG2 mesh or suppression here:
    // physical interactions always address the actual source instance.
    if(lo!=0u&&(route.kind&0x80000000u)!=0u){
        StructuredBuffer<CutResidentInstance> tiles=ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
        CutResidentInstance item=tiles[(route.kind&0x7fffffffu)+local];
        instance=item.instance;
        return item.enabled!=0;
    }
    StructuredBuffer<CutInstance> instances=ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
    instance=instances[local];
    return true;
}
