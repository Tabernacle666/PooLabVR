#include "visual_common.hlsli"
#define LAB_VISUAL_HEIGHT_TESS_PIXELS 12.0

struct VisualHeightVSIn { float3 position:POSITION; float3 normal:NORMAL; float3 tangent:TANGENT; float2 uv:TEXCOORD0; };
struct VisualHeightControl { float3 world:TEXCOORD0; float3 normal:TEXCOORD1; float3 tangent:TEXCOORD2; float2 uv:TEXCOORD3; };
struct VisualHeightPatch { float edge[3]:SV_TessFactor; float inside:SV_InsideTessFactor; };
struct VisualHeightRaster {float4 position:SV_POSITION;float3 world:TEXCOORD0;float3 normal:TEXCOORD1;float3 tangent:TEXCOORD2;float2 uv:TEXCOORD3;nointerpolation uint view:TEXCOORD4;};

VisualHeightControl VisualHeightVS(VisualHeightVSIn i)
{
    VisualDrawData d=VisualDraw(); VisualHeightControl o; float3 p=i.position;float3 n=i.normal;float3 tangent=i.tangent;
    if(d.deform.w>0.5){
        float phase=timing.z*max(.05,d.deform.y)+d.deform.z;
        float wobble=sin(dot(p,float3(3.7,5.1,2.9))+phase)+.55*sin(dot(p,float3(-4.3,2.2,6.4))-phase*.73);
        p+=n*wobble*max(0.,d.deform.x)*.055;
    }
    o.world=VisualWorld(p,d);o.normal=VisualNormal(n,d);o.tangent=normalize(VisualRotate(tangent,d.rotation));o.uv=i.uv*max(.001,d.materialParams.x);return o;
}
float VisualEdgePixels(float3 a,float3 b){
    float pixels=0;
    [unroll]for(uint view=0;view<LAB_VISUAL_MAX_VIEWS;++view){
        if(view>=topology.x)break;
        float4 ca=mul(float4(a,1),vp[view]),cb=mul(float4(b,1),vp[view]);
        if(ca.w<=0||cb.w<=0)continue;
        float2 pa=ca.xy/ca.w*float2(dimensions.xy)*.5,pb=cb.xy/cb.w*float2(dimensions.xy)*.5;
        pixels=max(pixels,length(pa-pb));
    }
    return pixels;
}
VisualHeightPatch VisualHeightHSConst(InputPatch<VisualHeightControl,3> p,uint id:SV_PrimitiveID){
    VisualDrawData d=VisualDraw();VisualHeightPatch o;float maximum=max(1.,d.extra.x);
    o.edge[0]=clamp(ceil(VisualEdgePixels(p[1].world,p[2].world)/LAB_VISUAL_HEIGHT_TESS_PIXELS),1.,maximum);
    o.edge[1]=clamp(ceil(VisualEdgePixels(p[2].world,p[0].world)/LAB_VISUAL_HEIGHT_TESS_PIXELS),1.,maximum);
    o.edge[2]=clamp(ceil(VisualEdgePixels(p[0].world,p[1].world)/LAB_VISUAL_HEIGHT_TESS_PIXELS),1.,maximum);
    o.inside=max(o.edge[0],max(o.edge[1],o.edge[2]));return o;
}
[domain("tri")][partitioning("fractional_even")][outputtopology("triangle_cw")][outputcontrolpoints(3)][patchconstantfunc("VisualHeightHSConst")]
VisualHeightControl VisualHeightHS(InputPatch<VisualHeightControl,3> p,uint i:SV_OutputControlPointID,uint id:SV_PrimitiveID){ return p[i]; }
[domain("tri")]
VisualHeightRaster VisualHeightDS(VisualHeightPatch c,const OutputPatch<VisualHeightControl,3> p,float3 b:SV_DomainLocation,uint view:SV_ViewID)
{
    VisualDrawData d=VisualDraw(); VisualHeightRaster o;
    o.world=p[0].world*b.x+p[1].world*b.y+p[2].world*b.z;
    o.normal=normalize(p[0].normal*b.x+p[1].normal*b.y+p[2].normal*b.z);
    o.tangent=normalize(p[0].tangent*b.x+p[1].tangent*b.y+p[2].tangent*b.z);
    o.uv=p[0].uv*b.x+p[1].uv*b.y+p[2].uv*b.z;
    float h=VisualSampleLevel(d.texture0.w,o.uv,0,float4(.5,.5,.5,1)).r;
    o.world+=o.normal*((h-0.5)*2.0)*max(0.0,d.materialParams.w);
    o.view=view;o.position=mul(float4(o.world,1),vp[view]);
    return o;
}

float3 BlendN(float3 a,float3 b,float s){b.xy*=s;return normalize(float3(a.xy+b.xy,max(0.04,a.z*b.z)));}
float2 Parallax(VisualDrawData d,float2 uv,float2 uvDx,float2 uvDy,float3 viewT,out float hs){
    hs=.5;if(d.texture0.w==0xffffffffu)return uv;
    Texture2D<float4> heightTexture=ResourceDescriptorHeap[NonUniformResourceIndex(VisualResource(d.texture0.w))];SamplerState sampler=SamplerDescriptorHeap[0];
    hs=heightTexture.SampleGrad(sampler,uv,uvDx,uvDy).r;
    float lod=max(0.,heightTexture.CalculateLevelOfDetail(sampler,uv));
    if(lod>=5.||d.materialParams.w<=.00001||viewT.z<=.035)return uv;
    float lodDetail=saturate(1.-lod/5.);
    float requested=lerp(d.detailParams.z,d.detailParams.w,1-saturate(viewT.z));
    int steps=(int)clamp(round(lerp(min(4.,requested),requested,lodDetail)),1.,96.);
    float layer=1./steps;float2 delta=(viewT.xy/max(.09,viewT.z))*d.materialParams.w*d.materialParams.x/steps;
    float2 cuv=uv;float depth=0,h=hs;
    [loop]for(int i=0;i<96;++i){if(i>=steps||depth>=h)break;cuv-=delta;depth+=layer;h=heightTexture.SampleGrad(sampler,cuv,uvDx,uvDy).r;}
    hs=h;return cuv;
}
[earlydepthstencil]
float4 VisualHeightPS(VisualHeightRaster i,bool front:SV_IsFrontFace):SV_TARGET
{
    VisualDrawData d=VisualDraw(); SamplerState samp=SamplerDescriptorHeap[0];
    float3 gn=normalize(i.normal);if(!front)gn=-gn;float3 t=normalize(i.tangent-gn*dot(gn,i.tangent));float3 b=normalize(cross(gn,t));float3 vd=normalize(eye[i.view].xyz-i.world);float3 vt=float3(dot(vd,t),dot(vd,b),abs(dot(vd,gn)));
    float2 uvDx=ddx(i.uv),uvDy=ddy(i.uv);float height=0.5;float2 uv=Parallax(d,i.uv,uvDx,uvDy,vt,height);
    if(d.deform.w>0.5){float2 drift=float2(sin(uv.y*31+timing.z*(0.16+d.deform.y*0.09)),cos(uv.x*27-timing.z*(0.13+d.deform.y*0.07)));uv+=drift*max(0.0,d.advancedParams.x)*0.009;}
    float3 nm=VisualSampleGrad(d.texture0.y,uv,uvDx,uvDy,float4(.5,.5,1,1)).xyz*2-1;nm.xy*=max(0.0,d.materialParams.y);nm.z=max(0.04,nm.z);float3 micro=VisualSampleGrad(d.texture1.x,uv*max(.01,d.detailParams.x),uvDx*max(.01,d.detailParams.x),uvDy*max(.01,d.detailParams.x),float4(.5,.5,1,1)).xyz*2-1;nm=BlendN(nm,micro,max(0.0,d.detailParams.y));float3 n=normalize(t*nm.x+b*nm.y+gn*nm.z);
    float3 albedo=VisualSampleGrad(d.texture0.x,uv,uvDx,uvDy,float4(1,1,1,1)).rgb*d.tint.rgb;float3 orm=VisualSampleGrad(d.texture0.z,uv,uvDx,uvDy,float4(1,.5,0,1)).rgb;float rough=clamp(orm.g+d.materialParams.z,0.025,0.98);float ao=saturate(orm.r);float cavity=smoothstep(0.80,0.16,height);albedo*=lerp(1.0,0.55,cavity*0.34);ao*=lerp(1.0,0.55,cavity*0.42);float wet=saturate(max(0.0,d.advancedParams.x)*((1-rough)*0.75+cavity*0.35));rough=lerp(rough,0.035,wet*0.72);
    float3 rgb=VisualLighting(d,i.world,n,vd,albedo,rough,wet,ao);
    rgb+=albedo*d.advancedParams.y*d.positionEmission.w*(d.deform.w>.5?(0.55+0.45*sin(timing.z*(.72+d.deform.y))):1.);
    return float4(max(rgb,0),1);
}
