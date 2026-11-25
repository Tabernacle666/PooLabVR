#include "common.hlsli"
#define OASIS_SOLAR_COMPUTE 1
#include "solar_visibility.hlsli"

// Coherent current-frame rays from the completed depth prepass. No persistent
// triangle witness cache, no history of stale blockers, and no cross-eye mixing.
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
[numthreads(8,8,1)]
void SolarSamplesMain(uint3 id:SV_DispatchThreadID){
    if(any(id.xy>=(uint2(Width,Height)+1u)/2u)||id.z>=ViewCount)return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint3 slot0=uint3(id.xy,id.z*2u),slot1=uint3(id.xy,id.z*2u+1u);
    output[slot0]=0.f;output[slot1]=0.f;
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    uint2 pa=SolarAnchorPixel(id.xy,0u),pb=SolarAnchorPixel(id.xy,1u);
    float3 a,b,na,nb;float da,db,fa,fb;
    if(!SolarAnchor(depth,pa,id.z,a,na,da,fa)||!SolarAnchor(depth,pb,id.z,b,nb,db,fb))return;
    if(dot(na,nb)<.999f||length(a-b)>3.f*max(fa,fb)+.004f||
        abs(dot(a-b,na))>.004f+.05f*max(fa,fb))return;
    const float3 sun=normalize(-globalSun.directionOuter.xyz);
    if(dot(na,sun)<=0.f||dot(nb,sun)<=0.f)return;
    RayDesc ray;ray.Direction=sun;ray.TMin=.002f;ray.TMax=max(.003f,timing.y-.01f);
    ray.Origin=a+na*.002f;
    uint sampleA=OpticalSampleIndex(float2(pa)+.5f,id.z);
    OpticalCount(sampleA,25u);
    float va=TraceAdaptiveSolar(ray,sampleA);
    ray.Origin=b+nb*.002f;
    uint sampleB=OpticalSampleIndex(float2(pb)+.5f,id.z);
    OpticalCount(sampleB,25u);
    float vb=TraceAdaptiveSolar(ray,sampleB);
    output[slot0]=float4(da,SolarEncodeNormal(na),va);
    output[slot1]=float4(db,SolarEncodeNormal(nb),vb);
}
