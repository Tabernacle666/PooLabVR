#include "common.hlsli"
#define OASIS_SOLAR_COMPUTE 1
#include "solar_visibility.hlsli"

bool SolarDepthPoint(Texture2DArray<float> depth,int2 pixel,uint view,out float3 p,out float d){
    p=0.f;d=0.f;
    if(any(pixel<0)||any(pixel>=int2(Width,Height)))return false;
    d=depth.Load(int4(pixel,view,0));
    if(d<=0.f||!isfinite(d))return false;
    p=SolarWorldAt(uint2(pixel),d,view);
    return all(isfinite(p));
}
bool SolarAnchor(Texture2DArray<float> depth,uint2 pixel,uint view,out float3 p,out float3 n,out float d,out float footprint){
    n=0.f;footprint=0.f;
    if(!SolarDepthPoint(depth,int2(pixel),view,p,d))return false;
    float3 left,right,up,down;float dl,dr,du,dd;
    bool l=SolarDepthPoint(depth,int2(pixel)+int2(-1,0),view,left,dl);
    bool r=SolarDepthPoint(depth,int2(pixel)+int2(1,0),view,right,dr);
    bool u=SolarDepthPoint(depth,int2(pixel)+int2(0,-1),view,up,du);
    bool b=SolarDepthPoint(depth,int2(pixel)+int2(0,1),view,down,dd);
    if((!l&&!r)||(!u&&!b))return false;
    float3 dx=r&&(!l||dot(right-p,right-p)<=dot(p-left,p-left))?right-p:p-left;
    float3 dy=b&&(!u||dot(down-p,down-p)<=dot(p-up,p-up))?down-p:p-up;
    footprint=max(length(dx),length(dy));
    float3 area=cross(dx,dy);float area2=dot(area,area);
    const float3 flatX=SolarWorldAt(min(pixel+uint2(1,0),uint2(Width,Height)-1u),d,view)-p;
    const float3 flatY=SolarWorldAt(min(pixel+uint2(0,1),uint2(Width,Height)-1u),d,view)-p;
    const float reference=max(length(flatX),length(flatY));
    if(area2<=1.e-18f||!isfinite(footprint)||footprint>6.f*max(reference,.0001f))return false;
    n=area*rsqrt(area2);if(dot(n,eye[view].xyz-p)<0.f)n=-n;
    return true;
}

struct SolarReceiver {float3 p,n;float depth,footprint;float value,hit;bool valid;};
SolarReceiver SolarReadReceiver(Texture2DArray<float> depth,uint2 pixel,uint view){
    SolarReceiver r=(SolarReceiver)0;
    r.valid=SolarAnchor(depth,pixel,view,r.p,r.n,r.depth,r.footprint);
    r.value=-1.f;
    if(r.valid)r.valid=dot(r.n,SolarDirection(r.p))>0.f;
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
[numthreads(8,8,1)]
void SolarSamplesMain(uint3 id:SV_DispatchThreadID){
    id.z+=passIndex;
    if(any(id.xy>=(uint2(Width,Height)+1u)/2u)||id.z>=ViewCount)return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    // One lane owns a 2x2 receiver tile; no duplicate anchor or fallback rays.
    uint2 pixels[4];SolarReceiver r[4];uint samples[4];
    [unroll]for(uint i=0;i<4u;++i){
        pixels[i]=id.xy*2u+uint2(i&1u,i>>1u);
        r[i]=SolarReadReceiver(depth,pixels[i],id.z);
        samples[i]=OpticalSampleIndex(float2(pixels[i])+.5f,id.z);
    }
    if(id.z>0u){
        [unroll]for(uint k=0u;k<4u;++k)if(r[k].valid){
            float shared;
            if(SolarSpatialLookup(output,r[k],shared)){r[k].value=shared;OpticalCount(samples[k],28u);}
        }
    }
    if(r[0].valid&&r[0].value<0.f){OpticalCount(samples[0],25u);r[0].value=SolarTraceReceiver(r[0],id.z,samples[0]);}
    if(r[3].valid&&r[3].value<0.f){OpticalCount(samples[3],25u);r[3].value=SolarTraceReceiver(r[3],id.z,samples[3]);}
    [unroll]for(uint j=1u;j<=2u;++j)if(r[j].valid&&r[j].value<0.f){
        if(r[0].value==r[3].value&&(r[0].value==0.f||r[0].value==1.f)&&
           SolarCompatible(r[j],r[0])&&SolarCompatible(r[j],r[3])){
            r[j].value=r[0].value;OpticalCount(samples[j],26u);
        }else r[j].value=SolarTraceReceiver(r[j],id.z,samples[j]);
    }
    [unroll]for(uint k=0;k<4u;++k)if(all(pixels[k]<uint2(Width,Height)))
        output[uint3(pixels[k],id.z)]=r[k].valid?float4(r[k].depth,SolarEncodeNormal(r[k].n),
            r[k].value==0.f&&r[k].hit>0.f?-r[k].hit:r[k].value):0.f;
}
