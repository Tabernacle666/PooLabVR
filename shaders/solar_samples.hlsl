#include "common.hlsli"
#define OASIS_SOLAR_COMPUTE 1
#include "solar_visibility.hlsli"

// Current-frame rays from completed depth. Eye 0 is published before eye 1.
// Reuse only mutually agreeing binary anchors on a compatible world-space surface.
bool SolarStereoVisibility(RWTexture2DArray<float4> output,float3 p,float3 n,
                           float footprint,out float value){
    value=0.f;
    const float4 projected=mul(float4(p,1.f),vp[0]);
    if(projected.w<=0.f||!all(isfinite(projected)))return false;
    const float2 screen=(projected.xy/projected.w*float2(.5f,-.5f)+.5f)*float2(Width,Height);
    if(any(screen<0.f)||any(screen>=float2(Width,Height)))return false;
    const uint2 cell=uint2(screen)>>1u;
    const float4 a=output[uint3(cell,0u)],b=output[uint3(cell,1u)];
    if(a.x<=0.f||b.x<=0.f||a.w!=b.w||(a.w!=0.f&&a.w!=1.f))return false;
    const float3 na=SolarDecodeNormal(a.yz),nb=SolarDecodeNormal(b.yz);
    if(dot(na,n)<.999f||dot(nb,n)<.999f)return false;
    const float3 pa=SolarWorldAt(SolarAnchorPixel(cell,0u),a.x,0u);
    const float3 pb=SolarWorldAt(SolarAnchorPixel(cell,1u),b.x,0u);
    // Reject disparity across silhouettes, thin geometry and distinct depth layers.
    const float reach=2.5f*footprint+.002f;
    const float planeTolerance=.002f+.025f*footprint;
    if(!all(isfinite(pa))||!all(isfinite(pb))||
       length(pa-p)>reach||length(pb-p)>reach||
       abs(dot(pa-p,n))>planeTolerance||abs(dot(pb-p,n))>planeTolerance)return false;
    value=a.w;
    return true;
}
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
    id.z+=passIndex;
    if(any(id.xy>=(uint2(Width,Height)+1u)/2u)||id.z>=ViewCount)return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    const uint3 slot0=uint3(id.xy,id.z*2u),slot1=uint3(id.xy,id.z*2u+1u);
    output[slot0]=0.f;output[slot1]=0.f;
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    uint2 pa=SolarAnchorPixel(id.xy,0u),pb=SolarAnchorPixel(id.xy,1u);
    float3 a,b,na,nb;float da,db,fa,fb;
    if(all(pa==pb)||!SolarAnchor(depth,pa,id.z,a,na,da,fa)||!SolarAnchor(depth,pb,id.z,b,nb,db,fb))return;
    if(dot(na,nb)<.999f||length(a-b)>3.f*max(fa,fb)+.004f||
        abs(dot(a-b,na))>.004f+.05f*max(fa,fb))return;
    const float3 sun=normalize(-globalSun.directionOuter.xyz);
    if(dot(na,sun)<=0.f||dot(nb,sun)<=0.f)return;
    RayDesc ray;ray.Direction=sun;ray.TMin=.002f;
    uint sampleA=OpticalSampleIndex(float2(pa)+.5f,id.z);
    uint sampleB=OpticalSampleIndex(float2(pb)+.5f,id.z);
    float va=0.f,vb=0.f;
    // Both eye-1 anchors must agree with independently validated eye-0 pairs.
    // Otherwise trace both current-eye anchors; no fractional transmission reuse.
    const bool stereo=id.z==1u&&
        SolarStereoVisibility(output,a,na,fa,va)&&
        SolarStereoVisibility(output,b,nb,fb,vb)&&va==vb;
    if(stereo){
        OpticalCount(sampleA,28u);OpticalCount(sampleB,28u);
    }else{
        ray.Origin=a+na*.002f;ray.TMax=SolarTraceLimit(ray.Origin,sun);
        OpticalCount(sampleA,25u);va=TraceAdaptiveSolar(ray,sampleA);
        ray.Origin=b+nb*.002f;ray.TMax=SolarTraceLimit(ray.Origin,sun);
        OpticalCount(sampleB,25u);vb=TraceAdaptiveSolar(ray,sampleB);
    }
    output[slot0]=float4(da,SolarEncodeNormal(na),va);
    output[slot1]=float4(db,SolarEncodeNormal(nb),vb);
}
