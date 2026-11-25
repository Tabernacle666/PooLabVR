#include "common.hlsli"
#define OASIS_SOLAR_COMPUTE 1
#include "solar_visibility.hlsli"

// The depth pass publishes the actual interpolated raster normal and owner.
// Reconstruct position from the matching depth, never normals from neighbours.
float SolarReceiverFootprint(uint2 pixel,float depth,uint view,float3 p){
    const uint2 size=uint2(Width,Height);
    const uint x=pixel.x+1u<size.x?pixel.x+1u:(pixel.x>0u?pixel.x-1u:pixel.x);
    const uint y=pixel.y+1u<size.y?pixel.y+1u:(pixel.y>0u?pixel.y-1u:pixel.y);
    return max(.0001f,max(length(SolarWorldAt(uint2(x,pixel.y),depth,view)-p),
                          length(SolarWorldAt(uint2(pixel.x,y),depth,view)-p)));
}
struct SolarReceiver {float3 p,n;float depth,footprint;float value,hit;uint receiver;bool valid;};
bool SolarLightAdmits(float3 p,float3 n){
    if(globalSun.colorPower.w<=0.f||all(globalSun.colorPower.rgb==0.f)||
       (uint(globalSun.parameters.z)&1u)==0u)return false;
    const bool directional=(uint(globalSun.parameters.z)&2u)!=0u;
    const float3 delta=directional?-globalSun.directionOuter.xyz:globalSun.positionRadius.xyz-p;
    if(dot(n,delta)<=0.f)return false;
    if(!directional){
        const float distance2=dot(delta,delta);
        if(distance2>=globalSun.positionRadius.w*globalSun.positionRadius.w)return false;
        if(globalSun.directionOuter.w>-1.f&&
           dot(-delta/max(sqrt(distance2),.001f),globalSun.directionOuter.xyz)<=globalSun.directionOuter.w)
            return false;
    }
    return true;
}
SolarReceiver SolarReadReceiver(Texture2DArray<float> depth,uint2 pixel,uint view,uint sample){
    SolarReceiver r=(SolarReceiver)0;r.value=-1.f;
    Texture2DArray<uint2> receivers=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarReceivers)];
    const uint2 packed=receivers.Load(int4(pixel,view,0));
    if((packed.x&0x80000000u)==0u){
        if(packed.x==1u)OpticalCount(sample,36u);
        return r;
    }
    r.depth=depth.Load(int4(pixel,view,0));
    if(r.depth<=0.f||!isfinite(r.depth))return r;
    r.p=SolarWorldAt(pixel,r.depth,view);r.n=SolarUnpackReceiverNormal(packed.x);
    r.receiver=packed.y;
    if(!all(isfinite(r.p))||!SolarLightAdmits(r.p,r.n)){
        OpticalCount(sample,37u);return r;
    }
    r.footprint=SolarReceiverFootprint(pixel,r.depth,view,r.p);
    r.valid=isfinite(r.footprint);
    if(r.valid)OpticalCount(sample,35u);
    return r;
}
bool SolarCompatible(SolarReceiver a,SolarReceiver b){
    if(!a.valid||!b.valid||dot(a.n,b.n)<.999f)return false;
    const float footprint=max(a.footprint,b.footprint);
    return length(a.p-b.p)<=3.f*footprint+.004f&&
        abs(dot(a.p-b.p,a.n))<=.004f+.05f*footprint&&
        dot(SolarDirection(a.p),SolarDirection(b.p))>.99999f;
}
// A shared light-space table, independent of the projected screen coordinates.
// Quantized cells are candidate lookup only: both current-frame witnesses
// must pass world-position, depth-layer, normal and solar-direction checks.
static const uint SolarSpatialSlots=1u<<20;
uint SolarSpatialHash(float3 p,float footprint){
    const float3 light=SolarDirection(p);
    const float3 axis=abs(light.y)<.9f?float3(0,1,0):float3(1,0,0);
    const float3 x=normalize(cross(axis,light)),y=cross(light,x);
    const float level=clamp(ceil(log2(max(footprint*4.f,.004f))),-8.f,4.f);
    const float cell=exp2(level);
    const int3 coord=int3(floor(float3(dot(p,x),dot(p,y),dot(p,light))/cell));
    return (OpticalHash(asuint(coord))^OpticalHash(uint3(uint(level+8.f),0,0)))&(SolarSpatialSlots-1u);
}
SolarReceiver SolarStored(RWTexture2DArray<float4> output,uint2 pixel){
    SolarReceiver r=(SolarReceiver)0;
    const float4 value=output[uint3(pixel,0u)];
    if(value.x<=0.f)return r;
    r.p=SolarWorldAt(pixel,value.x,0u);r.n=SolarDecodeNormal(value.yz);
    const uint2 next=min(pixel+1u,uint2(Width,Height)-1u);
    r.footprint=max(length(SolarWorldAt(uint2(next.x,pixel.y),value.x,0u)-r.p),
                    length(SolarWorldAt(uint2(pixel.x,next.y),value.x,0u)-r.p));
    r.hit=max(0.f,-value.w);r.value=max(0.f,value.w);
    r.valid=all(isfinite(r.p))&&isfinite(r.footprint);
    return r;
}
[numthreads(256,1,1)]
void SolarSpatialClearMain(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    if(id.x<SolarSpatialSlots)table.Store(id.x*4u,0xffffffffu);
}
[numthreads(8,8,1)]
void SolarSpatialBuildMain(uint3 id:SV_DispatchThreadID){
    const uint2 pixel=id.xy*2u;
    if(any(pixel+1u>=uint2(Width,Height)))return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    SolarReceiver a=SolarStored(output,pixel),b=SolarStored(output,pixel+1u);
    if(!SolarCompatible(a,b)||a.value!=b.value||(a.value!=0.f&&a.value!=1.f))return;
    const uint hash=SolarSpatialHash(a.p,max(a.footprint,b.footprint));
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    uint old;table.InterlockedMin(hash*4u,pixel.y*Width+pixel.x,old);
}
bool SolarSpatialLookup(RWTexture2DArray<float4> output,SolarReceiver r,out float value){
    value=-1.f;
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    const uint index=table.Load(SolarSpatialHash(r.p,r.footprint)*4u);
    if(index==0xffffffffu||index>=Width*Height)return false;
    const uint2 pixel=uint2(index%Width,index/Width);
    if(any(pixel+1u>=uint2(Width,Height)))return false;
    SolarReceiver a=SolarStored(output,pixel),b=SolarStored(output,pixel+1u);
    if(a.value!=b.value||(a.value!=0.f&&a.value!=1.f)||
       !SolarCompatible(r,a)||!SolarCompatible(r,b))return false;
    value=a.value;return true;
}
float3 SolarPreviousWorld(uint2 pixel,float depth,uint view){
    const float2 uv=(float2(pixel)+.5f)/float2(solarHistoryInfo.xy);
    const float4 p=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),solarPreviousInverseVP[view]);
    return p.xyz/p.w;
}
// A prior blocked ray is only a search hint. Confirm a present-frame
// opaque hit in a short interval around the old blocker. No stale occluder
// can darken the scene after motion, destruction or an LOD transition.
// Clear rays still traverse the entire interval; no stale light leaks.
bool SolarTemporalBlocker(SolarReceiver r,uint view,uint sample,out float hit){
    hit=0.f;
    if(solarHistoryInfo.z==0u)return false;
    const uint phase=OpticalHash(asuint(int3(floor(r.p/max(r.footprint,.004f)))))&3u;
    if(phase==(solarHistoryInfo.w&3u))return false; // periodic independent refresh
    const float4 projected=mul(float4(r.p,1),solarPreviousVP[view]);
    if(projected.w<=0.f||!all(isfinite(projected)))return false;
    const float2 screen=(projected.xy/projected.w*float2(.5f,-.5f)+.5f)*float2(solarHistoryInfo.xy);
    if(any(screen<0.f)||any(screen>=float2(solarHistoryInfo.xy)))return false;
    const uint2 pixel=uint2(screen);
    Texture2DArray<float4> previous=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarPrevious)];
    const float4 old=previous.Load(int4(pixel,view,0));
    if(old.x<=0.f||old.w>=0.f)return false;
    const float3 p=SolarPreviousWorld(pixel,old.x,view),n=SolarDecodeNormal(old.yz);
    if(!all(isfinite(p))||dot(n,r.n)<.98f||length(p-r.p)>2.f*r.footprint+.004f)return false;
    RayDesc ray;ray.Origin=r.p+r.n*.002f;ray.Direction=SolarDirection(r.p);
    const float limit=SolarTraceLimit(ray.Origin,ray.Direction);
    const float oldT=-old.w+dot(p-r.p,ray.Direction);
    const float margin=max(.012f,4.f*r.footprint);
    ray.TMin=max(.002f,oldT-margin);
    ray.TMax=min(limit,oldT+margin);
    if(ray.TMax<=ray.TMin)return false;
    OpticalCount(sample,32u);
    const float value=TraceSolarHit(ray,sample,hit);
    if(value==0.f&&hit>0.f){OpticalCount(sample,33u);return true;}
    OpticalCount(sample,34u);return false;
}
float SolarTraceReceiver(inout SolarReceiver r,uint view,uint sample){
    float hit=0.f;
    if(SolarTemporalBlocker(r,view,sample,hit)){r.hit=hit;return 0.f;}
    RayDesc ray;ray.Origin=r.p+r.n*.002f;ray.Direction=SolarDirection(r.p);
    ray.TMin=.002f;ray.TMax=SolarTraceLimit(ray.Origin,ray.Direction);
    OpticalCount(sample,30u);return TraceSolarHit(ray,sample,r.hit);
}
// One pixel per lane. Two diagonal witnesses per 2x2 tile are evaluated
// first, then shared across the group. Keep the wave's ray state small and
// avoid serializing four independent ray queries inside each thread.
groupshared float4 solarGroupPosition[64];
groupshared float4 solarGroupNormal[64];
SolarReceiver SolarGroupReceiver(uint lane){
    SolarReceiver r=(SolarReceiver)0;
    const float4 p=solarGroupPosition[lane],n=solarGroupNormal[lane];
    r.p=p.xyz;r.footprint=p.w;r.n=n.xyz;r.value=n.w;
    r.valid=r.value>=0.f;return r;
}
[numthreads(8,8,1)]
void SolarSamplesMain(uint3 id:SV_DispatchThreadID,uint lane:SV_GroupIndex){
    id.z+=passIndex;
    const bool inside=all(id.xy<uint2(Width,Height))&&id.z<ViewCount;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    SolarReceiver r=(SolarReceiver)0;r.value=-1.f;
    const uint sample=OpticalSampleIndex(float2(id.xy)+.5f,id.z);
    if(inside)r=SolarReadReceiver(depth,id.xy,id.z,sample);
    const bool anchor=(id.x&1u)==(id.y&1u);
    if(r.valid&&id.z>0u){
        float sharedVisibility;
        if(SolarSpatialLookup(output,r,sharedVisibility)){r.value=sharedVisibility;OpticalCount(sample,28u);}
    }
    if(r.valid&&anchor&&r.value<0.f){
        OpticalCount(sample,25u);r.value=SolarTraceReceiver(r,id.z,sample);
    }
    solarGroupPosition[lane]=float4(r.p,r.footprint);
    solarGroupNormal[lane]=float4(r.n,r.valid?r.value:-1.f);
    GroupMemoryBarrierWithGroupSync();
    if(r.valid&&r.value<0.f){
        const uint first=lane&~9u;
        SolarReceiver a=SolarGroupReceiver(first),b=SolarGroupReceiver(first+9u);
        if(a.value==b.value&&(a.value==0.f||a.value==1.f)&&
           SolarCompatible(r,a)&&SolarCompatible(r,b)){
            r.value=a.value;OpticalCount(sample,26u);
        }else r.value=SolarTraceReceiver(r,id.z,sample);
    }
    if(inside)output[uint3(id.xy,id.z)]=r.valid?float4(r.depth,SolarEncodeNormal(r.n),
        r.value==0.f&&r.hit>0.f?-r.hit:r.value):0.f;
}
