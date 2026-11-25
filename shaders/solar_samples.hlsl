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

struct SolarReceiver {float3 p,n;float depth,footprint;float value;bool valid;};
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
float SolarTraceReceiver(SolarReceiver r,uint sample){
    RayDesc ray;ray.Origin=r.p+r.n*.002f;ray.Direction=SolarDirection(r.p);
    ray.TMin=.002f;ray.TMax=SolarTraceLimit(ray.Origin,ray.Direction);
    OpticalCount(sample,30u);return TraceAdaptiveSolar(ray,sample);
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
    if(r[0].valid){OpticalCount(samples[0],25u);r[0].value=SolarTraceReceiver(r[0],samples[0]);}
    if(r[3].valid){OpticalCount(samples[3],25u);r[3].value=SolarTraceReceiver(r[3],samples[3]);}
    [unroll]for(uint j=1u;j<=2u;++j)if(r[j].valid){
        if(r[0].value==r[3].value&&(r[0].value==0.f||r[0].value==1.f)&&
           SolarCompatible(r[j],r[0])&&SolarCompatible(r[j],r[3])){
            r[j].value=r[0].value;OpticalCount(samples[j],26u);
        }else r[j].value=SolarTraceReceiver(r[j],samples[j]);
    }
    [unroll]for(uint k=0;k<4u;++k)if(all(pixels[k]<uint2(Width,Height)))
        output[uint3(pixels[k],id.z)]=r[k].valid?float4(r[k].depth,SolarEncodeNormal(r[k].n),r[k].value):0.f;
}
