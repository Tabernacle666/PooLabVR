// O1a: persistent spatial hints, never cached visibility without a current proof.
// A cache collision, changed geometry, changed light or reused ray slot is harmless:
// the candidate must still be a current, opaque, ordinary static DXR triangle
// strictly intersecting this ray. All uncertain/unsupported paths fall back to DXR.
static const uint SolarBlockerMask=(1u<<20u)-1u;
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
uint SolarBlockerSlot(RayDesc ray){
    // Cell is only an address for a candidate, never a visibility approximation.
    return OpticalHash(asuint(floor(ray.Origin*4.f)))&SolarBlockerMask;
}
bool SolarOpaqueTriangle(uint rayIndex,uint primitive,bool useCut,out GpuInstance instance){
    instance=(GpuInstance)0;
    if(rayIndex>=RayInstanceCount)return false;
    uint renderIndex=RenderInstanceForRay(rayIndex);
    if(renderIndex==0xffffffffu||renderIndex>=InstanceCount)return false;
    instance=LoadInstanceStable(renderIndex);
    if((instance.flags&(1u|2u|4u|32u|128u))!=0u||IsForestPage(instance.mesh))return false;
    if(useCut&&RayUsesCutReplacement(rayIndex))return false;
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    if(materials[instance.material].extra.y!=0u)return false;
    MeshRange range=LoadMeshRange(instance.mesh);
    return primitive<range.indexCount/3u;
}
bool SolarProveBlocker(RayDesc ray,uint packed,bool useCut){
    if(packed==0xffffffffu)return false;
    GpuInstance instance;
    uint primitive=packed>>24u;
    if(!SolarOpaqueTriangle(packed&0xffffffu,primitive,useCut,instance))return false;
    if(any(abs(instance.scale.xyz)<1.e-5f))return false;
    // Transform the current ray into the same object space as the ordinary BLAS.
    // The direction is deliberately not normalized: t remains in world ray units.
    float4 inverseRotation=float4(-instance.rotation.xyz,instance.rotation.w);
    float3 origin=Rotate(ray.Origin-instance.positionRadius.xyz,inverseRotation)/instance.scale.xyz;
    float3 direction=Rotate(ray.Direction,inverseRotation)/instance.scale.xyz;
    MeshRange range=LoadMeshRange(instance.mesh);
    StructuredBuffer<uint> indices=ResourceDescriptorHeap[Indices];
    StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];
    uint first=range.firstIndex+primitive*3u;
    float3 a=vertices[indices[first]].position;
    float3 e1=vertices[indices[first+1u]].position-a;
    float3 e2=vertices[indices[first+2u]].position-a;
    float3 q=cross(direction,e2);
    float det=dot(e1,q);
    float scale=length(e1)*length(e2)*length(direction);
    if(abs(det)<=max(1.e-12f,scale*1.e-6f))return false;
    float3 delta=origin-a;
    // Do not substitute a numerically fragile intersection for the hardware query.
    if(length(delta)>1.e4f*min(length(e1),length(e2)))return false;
    float3 r=cross(delta,e1);
    float inv=rcp(det);
    float2 uv=float2(dot(delta,q),dot(direction,r))*inv;
    float t=dot(e2,r)*inv;
    return all(uv>.002f)&&uv.x+uv.y<.998f&&t>ray.TMin+.001f&&t<ray.TMax-.001f;
}
bool SolarReuseBlocker(RayDesc ray,bool useCut,uint sampleIndex){
    StructuredBuffer<uint> cache=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarBlockers)];
    uint packed=cache[SolarBlockerSlot(ray)];
    if(SolarProveBlocker(ray,packed,useCut))return true;
    if(packed!=0xffffffffu)OpticalCount(sampleIndex,3u);
    return false;
}
void SolarPublishBlocker(RayDesc ray,uint rayIndex,uint primitive,uint geometry,bool useCut){
    // One publisher per active wave, rather than an atomic for every fragment.
    if(geometry!=0u||primitive>=256u||rayIndex>=0xffffffu)return;
    if(!WaveIsFirstLane())return;
    GpuInstance instance;
    if(!SolarOpaqueTriangle(rayIndex,primitive,useCut,instance))return;
    {
        RWByteAddressBuffer cache=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarBlockersUAV)];
        uint ignored;
        cache.InterlockedExchange(SolarBlockerSlot(ray)*4u,rayIndex|(primitive<<24u),ignored);
    }
}
