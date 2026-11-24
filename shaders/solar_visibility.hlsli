// O1b: world-space static visibility, dynamic shadows evaluated independently.
// Two agreeing DXR observations, same receiver and static revision, less than
// one current pixel of projected ray displacement. Never reuse across edits.
// Ping-pong buffers are read-only for the entire consuming frame.
static const uint SolarVisibilityMask=(1u<<19u)-1u;
static const uint SolarVisibilityStride=64u;
static const uint SolarMaxAge=8u;
static const uint OpticalNoSample=0xffffffffu;
uint OpticalHash(uint3 v){
    uint h=v.x*0x9e3779b9u^v.y*0x85ebca6bu^v.z*0xc2b2ae35u;
    h^=h>>16u;h*=0x7feb352du;h^=h>>15u;h*=0x846ca68bu;
    return h^(h>>16u);
}
uint OpticalSampleIndex(float2 position,uint view){
    if(reflectionSettings.z<=0.f)return OpticalNoSample;
    uint h=OpticalHash(uint3((uint2)position,view));
    return (h&255u)==(uint(reflectionSettings.z)-1u)?((h>>8u)&63u):OpticalNoSample;
}
void OpticalCount(uint sampleIndex,uint counter){
    if(sampleIndex==OpticalNoSample)return;
    RWByteAddressBuffer counters=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingOpticalStatsUAV)];
    uint ignored;
    counters.InterlockedAdd((counter*64u+sampleIndex)*4u,1u,ignored);
}

struct SolarVisibilityEntry {
    uint4 header;          // write frame, spatial key, revision lo, revision hi
    float4 originValue;    // canonical origin, static visibility
    float4 directionRange;// unit ray direction, TMax
    uint4 identity;        // receiver, material, packed normal, confirmations
};
uint SolarNormalKey(float3 n){
    const uint3 q=(uint3)round(saturate(n*.5f+.5f)*1023.f);
    return q.x|(q.y<<10u)|(q.z<<20u);
}
bool SolarCacheEligible(RayDesc ray,uint receiver,float3 normal,float footprint){
    if(receiver==0xffffffffu||solarCacheInfo.z==0xffffffffu||
       !isfinite(footprint)||footprint<=0.f||dot(normal,normal)<.9f||
       !all(isfinite(ray.Origin))||!all(isfinite(ray.Direction))||!isfinite(ray.TMax))return false;
    if(max(max(abs(ray.Origin.x),abs(ray.Origin.y)),abs(ray.Origin.z))*9.53674316e-7f>=footprint)return false;
    // The canonical solar ray is clipped to the active body's atmosphere.
    // External/orbital receivers continue along the exact path.
    if(solarSun.w>0.f&&
       length(ray.Origin-atmosphereSphere.xyz)>=atmosphereSphere.w-footprint)return false;
    return true;
}
float TraceSolarMask(RayDesc ray,uint mask){
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,mask,ray);
    float transmission=1.f;
    while(query.Proceed()){
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const float2 opacity=RayOpacity(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
            query.CandidateGeometryIndex(),query.CandidateTriangleBarycentrics());
        if(opacity.y==2)transmission*=1.f-opacity.x;
        else if(opacity.x>0.f)query.CommitNonOpaqueTriangleHit();
    }
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?0.f:transmission;
}
bool SolarCompatible(SolarVisibilityEntry entry,uint key,uint receiver,uint material,
                     uint normalKey,RayDesc ray,float footprint){
    if(entry.header.x==0xffffffffu||solarCacheInfo.z-entry.header.x>SolarMaxAge||
       entry.header.y!=key||any(entry.header.zw!=solarCacheInfo.xy)||
       any(entry.identity.xyz!=uint3(receiver,material,normalKey)))return false;
    if(entry.originValue.w!=0.f&&entry.originValue.w!=1.f)return false;
    const float3 delta=ray.Origin-entry.originValue.xyz;
    const float3 endpointDelta=delta+ray.Direction*ray.TMax-
        entry.directionRange.xyz*entry.directionRange.w;
    const float tolerance=.75f*footprint;
    return dot(delta,delta)<=tolerance*tolerance&&
           dot(endpointDelta,endpointDelta)<=tolerance*tolerance;
}
void SolarPublishVisibility(uint slot,uint key,uint receiver,uint material,uint normalKey,
                            RayDesc ray,float value,uint confirmations){
    if(value!=0.f&&value!=1.f)return;
    if(IsHelperLane())return;
    // One publisher per occupied slot in the active wave.
    const uint4 peers=WaveMatch(slot);
    const uint first=peers.x?firstbitlow(peers.x):
        peers.y?32u+firstbitlow(peers.y):peers.z?64u+firstbitlow(peers.z):96u+firstbitlow(peers.w);
    if(WaveGetLaneIndex()!=first)return;
    RWByteAddressBuffer cache=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint offset=slot*SolarVisibilityStride;
    const uint stamp=solarCacheInfo.z;
    const uint previous=cache.Load(offset);
    if(previous==stamp)return;
    uint observed;
    cache.InterlockedCompareExchange(offset,previous,stamp,observed);
    if(observed!=previous)return;
    // Single writer; no consumer reads this buffer before the frame barrier.
    cache.Store3(offset+4u,uint3(key,solarCacheInfo.xy));
    cache.Store4(offset+16u,asuint(float4(ray.Origin,value)));
    cache.Store4(offset+32u,asuint(float4(ray.Direction,ray.TMax)));
    cache.Store4(offset+48u,uint4(receiver,material,normalKey,confirmations));
}
float SolarStaticVisibility(RayDesc ray,uint receiver,uint material,float3 normal,
                            float footprint,uint sampleIndex){
    // Cell size follows the finer projected pixel dimension, not distance.
    const float cell=2.f*exp2(floor(log2(footprint)));
    const uint normalKey=SolarNormalKey(normal);
    const uint key=OpticalHash(asuint(floor(ray.Origin/cell)))^
        OpticalHash(uint3(receiver,material,asuint(cell)))^normalKey;
    const uint slot=key&SolarVisibilityMask;
    StructuredBuffer<SolarVisibilityEntry> cache=ResourceDescriptorHeap[
        DomainResource(ResourceDomainLighting,LightingSolarVisibility)];
    const SolarVisibilityEntry prior=cache[slot];
    const bool compatible=SolarCompatible(prior,key,receiver,material,normalKey,ray,footprint);
    if(compatible&&prior.identity.w>=1u){
        OpticalCount(sampleIndex,1u);
        return prior.originValue.w;
    }
    OpticalCount(sampleIndex,9u);
    if(prior.header.x!=0xffffffffu&&!compatible)OpticalCount(sampleIndex,3u);
    OpticalCount(sampleIndex,6u);
    const float value=TraceSolarMask(ray,1u);
    // Disagreement forces another exact observation, not interpolated shadowing.
    const uint confirmations=compatible&&prior.originValue.w==value?
        min(2u,prior.identity.w+1u):0u;
    SolarPublishVisibility(slot,key,receiver,material,normalKey,ray,value,confirmations);
    return value;
}
