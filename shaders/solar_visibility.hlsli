// Adaptive solar visibility: exact current-frame anchors, no temporal lookup.
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


// Current-frame exact anchors: no temporal witness lookup.
float TraceAdaptiveSolar(RayDesc ray,uint sampleIndex){
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
    return 0.f;
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
uint2 SolarAnchorPixel(uint2 cell,uint anchor){
    return min(cell*2u+anchor,uint2(Width,Height)-1u);
}
float3 SolarWorldAt(uint2 pixel,float depth,uint view){
    return Unproject((float2(pixel)+.5f)/float2(Width,Height),depth,view);
}
// Preserve the same finite ray interval as the fragment visibility path.
float SolarTraceLimit(float3 origin,float3 direction){
    float limit=max(.003f,timing.y-.01f);
    if(solarSun.w>0.f&&sphereScene.z==0u){
        const float3 local=origin-atmosphereSphere.xyz;
        const float along=dot(local,direction);
        const float disc=along*along+atmosphereSphere.w*atmosphereSphere.w-dot(local,local);
        limit=clamp(-along+sqrt(max(0.f,disc))+.003f,.003f,limit);
    }
    return limit;
}
#ifndef OASIS_SOLAR_COMPUTE
bool SolarScreenVisibility(RayDesc ray,float3 normal,float footprint,float2 screen,uint view,
                           uint sampleIndex,out float visibility){
    visibility=0.f;
    const uint descriptor=DomainResource(ResourceDomainLighting,LightingSolarVisibility);
    if(descriptor==0xffffffffu||!isfinite(footprint)||footprint<=0.f)return false;
    Texture2DArray<float4> samples=ResourceDescriptorHeap[descriptor];
    const uint2 cell=uint2(screen)>>1u;
    const float4 a=samples.Load(int4(cell,view*2u,0));
    const float4 b=samples.Load(int4(cell,view*2u+1u,0));
    if(a.x<=0.f||b.x<=0.f||a.w!=b.w||(a.w!=0.f&&a.w!=1.f))return false;
    const float3 na=SolarDecodeNormal(a.yz),nb=SolarDecodeNormal(b.yz);
    if(dot(na,normal)<.999f||dot(nb,normal)<.999f)return false;
    const float3 pa=SolarWorldAt(SolarAnchorPixel(cell,0u),a.x,view);
    const float3 pb=SolarWorldAt(SolarAnchorPixel(cell,1u),b.x,view);
    const float3 p=ray.Origin-normal*.002f;
    const float reach=3.f*footprint+.004f;
    const float planeTolerance=.004f+.05f*footprint;
    if(length(pa-p)>reach||length(pb-p)>reach||
       abs(dot(pa-p,normal))>planeTolerance||abs(dot(pb-p,normal))>planeTolerance)return false;
    visibility=a.w;
    OpticalCount(sampleIndex,26u);
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
