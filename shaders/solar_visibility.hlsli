// Solar visibility queries and validated spatial/temporal witnesses.
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


// Current-frame trace; optionally return a blocker distance for temporal revalidation.
float TraceSolarHit(RayDesc ray,uint sampleIndex,out float hitDistance){
    hitDistance=0.f;
    OpticalCount(sampleIndex,2u);
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
#if defined(OASIS_SOLAR_COMPUTE) && __SHADER_TARGET_MINOR >= 9
    // Only this adaptive solar query forces the two-state interpretation.
    // Non-OMM triangles (including transmissive glass) retain exact candidates.
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH|RAY_FLAG_FORCE_OMM_2_STATE,
        RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS> query;
#else
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
#endif
    query.TraceRayInline(scene,RAY_FLAG_NONE,HardwareRayMask(3u,false),ray);
    float transmission=1.f;
    while(query.Proceed()){
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        OpticalCount(sampleIndex,23u);
        const float2 opacity=RayOpacity(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
            query.CandidateGeometryIndex(),query.CandidateTriangleBarycentrics());
        if(opacity.y==2)transmission*=1.f-opacity.x;
        else if(opacity.x>0.f)query.CommitNonOpaqueTriangleHit();
    }
    if(query.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)return transmission;
    hitDistance=query.CommittedRayT();
    return 0.f;
}
float TraceAdaptiveSolar(RayDesc ray,uint sampleIndex){
    float hitDistance;return TraceSolarHit(ray,sampleIndex,hitDistance);
}
// Octahedral encoding retains the reconstructed geometric normal in two floats.
float2 SolarEncodeNormal(float3 n){
    n/=max(1.e-8f,abs(n.x)+abs(n.y)+abs(n.z));
    float2 signXY=float2(n.x>=0.f?1.f:-1.f,n.y>=0.f?1.f:-1.f);
    return n.z>=0.f?n.xy:(1.f-abs(n.yx))*signXY;
}
float3 SolarDecodeNormal(float2 e){
    float3 n=float3(e,1.f-abs(e.x)-abs(e.y));
    float t=saturate(-n.z);
    n.xy+=float2(n.x>=0.f?-t:t,n.y>=0.f?-t:t);
    return normalize(n);
}
// 15-bit octahedral components; zero is an unpublished receiver.
uint SolarPackReceiverNormal(float3 n){
    const uint2 q=uint2(round(saturate(SolarEncodeNormal(n)*.5f+.5f)*32767.f));
    return 0x80000000u|q.x|(q.y<<15u);
}
float3 SolarUnpackReceiverNormal(uint packed){
    return SolarDecodeNormal(float2(packed&32767u,(packed>>15u)&32767u)*(2.f/32767.f)-1.f);
}
uint2 SolarAnchorPixel(uint2 cell,uint anchor){
    return min(cell*2u+anchor,uint2(Width,Height)-1u);
}
float3 SolarWorldAt(uint2 pixel,float depth,uint view){
    return Unproject((float2(pixel)+.5f)/float2(Width,Height),depth,view);
}
// Preserve the same finite ray interval as the fragment visibility path.
float3 SolarDirection(float3 p){
    return normalize(((uint(globalSun.parameters.z)&2u)!=0u)?-globalSun.directionOuter.xyz:globalSun.positionRadius.xyz-p);
}
float SolarTraceLimit(float3 origin,float3 direction){
    const float distance=((uint(globalSun.parameters.z)&2u)!=0u)?timing.y:length(globalSun.positionRadius.xyz-origin);
    float limit=max(.003f,distance-.01f);
    if(solarSun.w>0.f&&sphereScene.z==0u){
        const float3 local=origin-atmosphereSphere.xyz;
        const float along=dot(local,direction);
        const float disc=along*along+atmosphereSphere.w*atmosphereSphere.w-dot(local,local);
        limit=clamp(-along+sqrt(max(0.f,disc))+.003f,.003f,limit);
    }
    return limit;
}
#ifndef OASIS_SOLAR_COMPUTE
bool SolarResolvedVisibility(float2 screen,uint view,float depth,float3 normal,uint receiver,uint sampleIndex,out float visibility){
    visibility=0.f;
    const uint descriptor=DomainResource(ResourceDomainLighting,LightingSolarVisibility);
    if(descriptor==0xffffffffu)return false;
    const uint receiverDescriptor=DomainResource(ResourceDomainLighting,LightingSolarReceivers);
    if(receiverDescriptor==0xffffffffu)return false;
    Texture2DArray<uint2> receivers=ResourceDescriptorHeap[receiverDescriptor];
    const uint2 owner=receivers.Load(int4(uint2(screen),view,0));
    if((owner.x&0x80000000u)==0u||owner.y!=receiver)return false;
    Texture2DArray<float4> resolved=ResourceDescriptorHeap[descriptor];
    const float4 v=resolved.Load(int4(uint2(screen),view,0));
    if(v.x<=0.f||abs(v.x-depth)>max(2.e-7f,abs(depth)*2.e-5f)||
       dot(SolarDecodeNormal(v.yz),normal)<.98f)return false;
    visibility=max(0.f,v.w);
    OpticalCount(sampleIndex,29u);
    return true;
}
uint SolarFirstLane(uint4 mask){
    return mask.x?uint(firstbitlow(mask.x)):mask.y?32u+uint(firstbitlow(mask.y)):
        mask.z?64u+uint(firstbitlow(mask.z)):96u+uint(firstbitlow(mask.w));
}
uint SolarLastLane(uint4 mask){
    return mask.w?96u+uint(firstbithigh(mask.w)):mask.z?64u+uint(firstbithigh(mask.z)):
        mask.y?32u+uint(firstbithigh(mask.y)):uint(firstbithigh(mask.x));
}
float AdaptiveSolarVisibility(RayDesc ray,uint receiver,uint material,float3 normal,
                              float footprint,float2 screen,uint view,uint sampleIndex){
    const uint2 cell=uint2(screen)>>1u;
    const uint tile=cell.x|(cell.y<<14u)|(view<<28u);
    if(IsHelperLane())return 1.f;
    if(receiver==0xffffffffu||!isfinite(footprint)||footprint<=0.f)
        return TraceAdaptiveSolar(ray,sampleIndex);
    const uint4 peers=WaveMatch(tile);
    if(countbits(peers.x)+countbits(peers.y)+countbits(peers.z)+countbits(peers.w)<=2u)
        return TraceAdaptiveSolar(ray,sampleIndex);
    const uint first=SolarFirstLane(peers),last=SolarLastLane(peers);
    const uint lane=WaveGetLaneIndex();
    const bool anchor=lane==first||lane==last;
    const uint firstReceiver=WaveReadLaneAt(receiver,first);
    const uint lastReceiver=WaveReadLaneAt(receiver,last);
    const uint firstMaterial=WaveReadLaneAt(material,first);
    const uint lastMaterial=WaveReadLaneAt(material,last);
    const float3 firstOrigin=WaveReadLaneAt(ray.Origin,first);
    const float3 lastOrigin=WaveReadLaneAt(ray.Origin,last);
    const float3 firstNormal=WaveReadLaneAt(normal,first);
    const float3 lastNormal=WaveReadLaneAt(normal,last);
    const float firstWidth=WaveReadLaneAt(footprint,first);
    const float lastWidth=WaveReadLaneAt(footprint,last);
    const bool compatible=receiver==firstReceiver&&receiver==lastReceiver&&
        material==firstMaterial&&material==lastMaterial&&
        dot(normal,firstNormal)>.999f&&dot(normal,lastNormal)>.999f&&
        length(ray.Origin-firstOrigin)<=3.f*max(footprint,firstWidth)&&
        length(ray.Origin-lastOrigin)<=3.f*max(footprint,lastWidth)&&
        max(footprint,max(firstWidth,lastWidth))<=2.f*min(footprint,min(firstWidth,lastWidth));
    float traceValue=-1.f;
    [branch]if(anchor)traceValue=TraceAdaptiveSolar(ray,sampleIndex);
    const float firstValue=WaveReadLaneAt(traceValue,first);
    const float lastValue=WaveReadLaneAt(traceValue,last);
    if(anchor)return traceValue;
    OpticalCount(sampleIndex,17u);
    // No averaging across disagreements, transparent shadows or surfaces.
    if(compatible&&firstValue==lastValue&&(firstValue==0.f||firstValue==1.f)){
        OpticalCount(sampleIndex,18u);
        return firstValue;
    }
    OpticalCount(sampleIndex,19u);
    return TraceAdaptiveSolar(ray,sampleIndex);
}

#endif // OASIS_SOLAR_COMPUTE
