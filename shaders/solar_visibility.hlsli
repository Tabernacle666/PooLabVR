// Adaptive solar visibility: persistent world-space blocker certificates and exact DXR anchors.
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


// A cell supplies a candidate, never a presumed common visibility value.
uint SolarWitnessOffset(RayDesc ray){
    if(solarCacheInfo.x==0u||!all(isfinite(ray.Origin))||
       any(abs(ray.Origin)>100000.f))return OpticalNoSample;
    const int3 cell=int3(floor(ray.Origin*2.f));
    return (OpticalHash(asuint(cell))&(solarCacheInfo.x-1u))*8u;
}

bool SolarBlockerInstance(uint rayIndex,bool useCut,out GpuInstance instance){
    instance=(GpuInstance)0;
    if(rayIndex>=RayInstanceCount)return false;
    const uint source=RenderInstanceForRay(rayIndex);
    if(source==0xffffffffu)return false;
    instance=LoadInstanceStable(source);
    // RayBuildMain excludes flags 4 and 32 from the solar mask.
    // Deforming geometry and alpha/transmission stay on exact DXR.
    if((instance.flags&(2u|4u|32u))!=0u)return false;
    if(useCut&&RayUsesCutReplacement(rayIndex))return false;
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    return materials[instance.material].extra.y==0u;
}

bool SolarTriangleInterior(RayDesc ray,float3 a,float3 b,float3 c){
    // Conservative interior-only Moller-Trumbore. Border, grazing,
    // degenerate and large-coordinate cases stay on hardware DXR.
    const float3 e1=b-a,e2=c-a,e3=c-b;
    const float area=length(cross(e1,e2));
    if(!(area>1.e-12f))return false;
    const float3 h=cross(ray.Direction,e2);
    const float det=dot(e1,h);
    if(abs(det)<.02f*area)return false;
    const float3 s=ray.Origin-a,q=cross(s,e1);
    const float u=dot(s,h)/det,v=dot(ray.Direction,q)/det;
    const float t=dot(e2,q)/det;
    const float3 magnitude=max(max(abs(a),abs(b)),max(abs(c),abs(ray.Origin)));
    const float margin=max(.0002f,2.e-6f*max(magnitude.x,max(magnitude.y,magnitude.z)));
    if(!all(isfinite(float3(u,v,t)))||
       t<=ray.TMin+margin||t>=ray.TMax-margin)return false;
    return u>0.f&&v>0.f&&u+v<1.f&&
        u*area>margin*length(e2)&&v*area>margin*length(e1)&&
        (1.f-u-v)*area>margin*length(e3);
}

bool SolarTryWitness(RayDesc ray,bool useCut,uint sampleIndex){
    const uint offset=SolarWitnessOffset(ray);
    if(offset==OpticalNoSample||IsHelperLane())return false;
    OpticalCount(sampleIndex,21u);
    RWByteAddressBuffer table=ResourceDescriptorHeap[
        DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint2 candidate=table.Load2(offset);
    if(candidate.x!=0u){
        const uint rayIndex=candidate.x-1u;
        GpuInstance instance;
        if(SolarBlockerInstance(rayIndex,useCut,instance)){
            const MeshRange range=LoadMeshRange(instance.mesh);
            if(candidate.y<range.indexCount/3u){
                Vertex a,b,c;
                LoadRayTriangle(rayIndex,candidate.y,0u,a,b,c);
                if(SolarTriangleInterior(ray,a.position,b.position,c.position)){
                    OpticalCount(sampleIndex,1u);
                    OpticalCount(sampleIndex,(instance.flags&1u)!=0u?25u:
                        IsForestPage(instance.mesh)?24u:23u);
                    return true;
                }
            }
        }
        OpticalCount(sampleIndex,3u);
    }
    OpticalCount(sampleIndex,9u);
    return false;
}

void SolarRememberWitness(RayDesc ray,uint rayIndex,uint primitive,uint geometry,bool useCut){
    const uint offset=SolarWitnessOffset(ray);
    if(offset==OpticalNoSample||IsHelperLane())return;
    GpuInstance instance;
    if(!SolarBlockerInstance(rayIndex,useCut,instance))return;
    const bool forest=IsForestPage(instance.mesh);
    if(!forest&&geometry!=0u)return;
    const uint triangleIndex=primitive+(forest?geometry:0u);
    if(triangleIndex<primitive||triangleIndex>=LoadMeshRange(instance.mesh).indexCount/3u)return;
    RWByteAddressBuffer table=ResourceDescriptorHeap[
        DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint2 next=uint2(rayIndex+1u,triangleIndex);
    if(all(table.Load2(offset)==next))return;
    // Concurrent writers may mix words. The reader bounds-checks and
    // re-intersects whatever current triangle it obtains. No lock or age.
    const uint4 peers=WaveMatch(offset);
    const uint first=peers.x?uint(firstbitlow(peers.x)):
        peers.y?32u+uint(firstbitlow(peers.y)):
        peers.z?64u+uint(firstbitlow(peers.z)):96u+uint(firstbitlow(peers.w));
    if(WaveGetLaneIndex()!=first)return;
    uint ignored;
    table.InterlockedExchange(offset+4u,next.y,ignored);
    table.InterlockedExchange(offset,next.x,ignored);
}

float TraceAdaptiveSolar(RayDesc ray,uint sampleIndex){
    if(SolarTryWitness(ray,false,sampleIndex))return 0.f;
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
    SolarRememberWitness(ray,query.CommittedInstanceID(),query.CommittedPrimitiveIndex(),
        query.CommittedGeometryIndex(),false);
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
