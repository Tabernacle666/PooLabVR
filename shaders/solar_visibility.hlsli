// Current-geometry solar witnesses and adaptive screen-space visibility.
// A cached triangle never implies visibility until it intersects the current ray.
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


static const uint SolarVisibilityMask=(1u<<18u)-1u;
static const uint SolarVisibilityStride=32u;
struct SolarWitnessEntry {
    uint4 header; // frame, packed 2x2 tile and eye, ray slot, primitive
    uint4 identity; // geometry, stable object ID, mesh, reserved
};
uint SolarTileKey(float2 screen,uint view){
    const uint2 tile=uint2(screen)>>1u;
    return tile.x|(tile.y<<14u)|(view<<28u);
}
uint SolarWitnessSlot(uint tile){
    return OpticalHash(uint3(tile,0x9e3779b9u,0u))&SolarVisibilityMask;
}
// Shared by solar and reflection history. Only ordinary opaque rigid triangles
// have a CPU/GPU geometry contract that can certify an intersection here.
bool CurrentOpaqueWitness(RayDesc ray,uint3 hit,uint expectedId,uint expectedMesh,
                          uint mask,out float distance){
    distance=0.f;
    if(hit.x>=RayInstanceCount||hit.z!=0u)return false;
    const uint renderIndex=RenderInstanceForRay(hit.x);
    if(renderIndex==0xffffffffu||renderIndex>=InstanceCount)return false;
    const GpuInstance instance=LoadInstanceStable(renderIndex);
    if(instance.id!=expectedId||(expectedMesh!=0xffffffffu&&instance.mesh!=expectedMesh)||
       (instance.flags&(2u|32u|128u))!=0u||IsForestPage(instance.mesh))return false;
    const uint currentMask=(instance.flags&4u)?4u:(instance.flags&3u)?2u:1u;
    if((currentMask&mask)==0u)return false;
    const MeshRange meshRange=LoadMeshRange(instance.mesh);
    if(hit.y>=meshRange.indexCount/3u)return false;
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    if(materials[instance.material].extra.y!=0u)return false;
    Vertex va,vb,vc;
    LoadRayTriangle(hit.x,hit.y,hit.z,va,vb,vc);
    const float3 e1=vb.position-va.position,e2=vc.position-va.position;
    const float3 h=cross(ray.Direction,e2);
    const float det=dot(e1,h);
    if(!isfinite(det)||abs(det)<1.e-9f)return false;
    const float3 delta=ray.Origin-va.position;
    const float3 q=cross(delta,e1);
    const float2 bary=float2(dot(delta,h),dot(ray.Direction,q))/det;
    distance=dot(e2,q)/det;
    // Refuse numerical/triangle edges and near/far endpoint ambiguity.
    const float edge=1.e-4f;
    return all(bary>edge)&&bary.x+bary.y<1.f-edge&&
           isfinite(distance)&&distance>ray.TMin+.0002f&&distance<ray.TMax-.0002f;
}
void PublishSolarWitness(uint tile,uint3 hit){
    if((solarCacheInfo.w&1u)==0u||IsHelperLane()||hit.x>=RayInstanceCount||hit.z!=0u)return;
    const uint renderIndex=RenderInstanceForRay(hit.x);
    if(renderIndex==0xffffffffu||renderIndex>=InstanceCount)return;
    const GpuInstance instance=LoadInstanceStable(renderIndex);
    if((instance.flags&(2u|4u|32u|128u))!=0u||IsForestPage(instance.mesh))return;
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    if(materials[instance.material].extra.y!=0u)return;
    RWByteAddressBuffer target=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint offset=SolarWitnessSlot(tile)*SolarVisibilityStride;
    const uint stamp=solarCacheInfo.z;
    const uint previous=target.Load(offset);
    if(previous==stamp)return;
    uint observed;
    target.InterlockedCompareExchange(offset,previous,stamp,observed);
    if(observed!=previous)return;
    target.Store3(offset+4u,uint3(tile,hit.xy));
    target.Store4(offset+16u,uint4(hit.z,instance.id,instance.mesh,0u));
}
bool ReuseSolarWitness(RayDesc ray,float2 screen,uint view,uint tile){
    if((solarCacheInfo.w&2u)==0u)return false;
    StructuredBuffer<SolarWitnessEntry> history=ResourceDescriptorHeap[
        DomainResource(ResourceDomainLighting,LightingSolarVisibility)];
    // Prefer reprojected tile. Geometry is verified in current canonical space,
    // so camera motion or a reused ray-slot cannot turn a stale hit into a shadow.
    uint priorTile=tile;
    const float4 clip=mul(float4(ray.Origin,1.f),previousVP[view]);
    if(clip.w>0.f){
        const float2 uv=clip.xy/clip.w*float2(.5f,-.5f)+.5f;
        if(all(uv>=0.f)&&all(uv<1.f))priorTile=SolarTileKey(uv*float2(Width,Height),view);
    }
    const SolarWitnessEntry prior=history[SolarWitnessSlot(priorTile)];
    if(prior.header.x==0xffffffffu||solarCacheInfo.z-prior.header.x!=1u||
       prior.header.y!=priorTile)return false;
    const uint3 hit=uint3(prior.header.zw,prior.identity.x);
    float distance;
    if(!CurrentOpaqueWitness(ray,hit,prior.identity.y,prior.identity.z,3u,distance))return false;
    PublishSolarWitness(tile,hit);
    return true;
}
float TraceAdaptiveSolar(RayDesc ray,uint tile,uint view,float2 screen,uint sampleIndex,bool witness){
    if(witness){
        OpticalCount(sampleIndex,21u);
        if(ReuseSolarWitness(ray,screen,view,tile)){
            OpticalCount(sampleIndex,1u);
            OpticalCount(sampleIndex,8u);
            return 0.f;
        }
    }
    OpticalCount(sampleIndex,2u);
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,3u,ray);
    float transmission=1.f;
    while(query.Proceed()){
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const float2 opacity=RayOpacity(query.CandidateInstanceID(),query.CandidatePrimitiveIndex(),
            query.CandidateGeometryIndex(),query.CandidateTriangleBarycentrics());
        if(opacity.y==2)transmission*=1.f-opacity.x;
        else if(opacity.x>0.f)query.CommitNonOpaqueTriangleHit();
    }
    if(query.CommittedStatus()!=COMMITTED_TRIANGLE_HIT)return transmission;
    if(witness)PublishSolarWitness(tile,uint3(query.CommittedInstanceID(),
        query.CommittedPrimitiveIndex(),query.CommittedGeometryIndex()));
    return 0.f;
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
    const uint tile=SolarTileKey(screen,view);
    if(receiver==0xffffffffu||!isfinite(footprint)||footprint<=0.f||IsHelperLane())
        return TraceAdaptiveSolar(ray,tile,view,screen,sampleIndex,false);
    const uint4 peers=WaveMatch(tile);
    const uint first=SolarFirstLane(peers),last=SolarLastLane(peers);
    const uint lane=WaveGetLaneIndex();
    if(first==last)return TraceAdaptiveSolar(ray,tile,view,screen,sampleIndex,true);
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
    [branch]if(anchor)traceValue=TraceAdaptiveSolar(ray,tile,view,screen,sampleIndex,true);
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
    return TraceAdaptiveSolar(ray,tile,view,screen,sampleIndex,false);
}
