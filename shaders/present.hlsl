#include "common.hlsli"
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}

float AtmosphereShadow(float3 p){
    [branch]if(atmosphereParams.x<0)return 1.0;
    uint slot=(uint)atmosphereParams.x;
    float3 d=p-atmosphereLight.xyz,ad=abs(d);
    uint face=ad.x>=ad.y&&ad.x>=ad.z?(d.x>=0?0:1):ad.y>=ad.z?(d.y>=0?2:3):(d.z>=0?4:5);
    float4 clip=mul(float4(p,1),shadowVP[slot*6+face]);
    if(clip.w<=0)return 1.0;
    float3 ndc=clip.xyz/clip.w;
    if(ndc.z<0||ndc.z>1||any(abs(ndc.xy)>1))return 1.0;
    float2 uv=ndc.xy*float2(.5,-.5)+.5,tile=float2((slot%2)*3+face%3,(slot/2)*2+face/3);
    Texture2D<float> atlas=ResourceDescriptorHeap[ShadowAtlas];
    SamplerComparisonState comparison=SamplerDescriptorHeap[1];
    return atlas.SampleCmpLevelZero(comparison,(clamp(uv,.5/512,1-.5/512)+tile)/float2(6,4),ndc.z+.0006);
}

float3 ApplyAtmosphere(float3 scene,float2 uv,float depth,uint view){
    [branch]if(atmosphereSphere.w<=0||atmosphereColor.w<=0)return scene;
    float3 origin=eye[view].xyz;
    float3 ray=normalize(Unproject(uv,0.0001,view)-origin);
    float3 relative=origin-atmosphereSphere.xyz;
    float b=dot(relative,ray),discriminant=b*b-dot(relative,relative)+atmosphereSphere.w*atmosphereSphere.w;
    [branch]if(discriminant<=0)return scene;
    float chord=sqrt(discriminant),first=max(0.0,-b-chord),last=-b+chord;
    if(depth>0)last=min(last,length(Unproject(uv,depth,view)-origin));
    [branch]if(last<=first)return scene;
    float3 samplePosition=origin+ray*((first+last)*.5);
    float ground=exp(-max(0.0,samplePosition.y-atmosphereSphere.y+atmosphereSphere.w*.25)*.65);
    float cloud=exp(-abs(samplePosition.y-atmosphereSphere.y-atmosphereSphere.w*.35)*1.1);
    float drift=.88+.12*sin(dot(samplePosition,float3(.83,.23,.61))+timing.z*.16);
    float opacity=1-exp(-(last-first)*atmosphereColor.w*(ground+.3*cloud)*drift);
    float3 toLight=atmosphereLight.xyz-samplePosition;
    float distanceSquared=dot(toLight,toLight);
    float3 eyeToLight=atmosphereLight.xyz-origin;
    float phase=.22+.78*pow(saturate(dot(ray,eyeToLight*rsqrt(max(dot(eyeToLight,eyeToLight),.000001)))),4);
    float illumination=atmosphereEmission.w*AtmosphereShadow(samplePosition)*phase/(1+distanceSquared);
    float3 haze=atmosphereColor.rgb*.035+atmosphereEmission.rgb*illumination*.08;
    return scene*(1-opacity)+haze*opacity;
}

struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present FinishPresent(Out i,float3 color,float2 uv,float z){
    Present o;
    o.color=float4(ACES(ApplyAtmosphere(color,uv,z,i.view)),1);
    o.depth=z;
    return o;
}

Present Tonemap(Out i){
    const uint2 xy=min(uint2(i.position.xy*float2(Width,Height)/float2(outputDimensions.xy)),
        uint2(Width-1,Height-1));
    float z=0.f;
    if(topology.y>1){
        Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];
        [unroll]for(uint k=0;k<4;k++)z=max(z,depth.Load(int3(xy,i.view),k));
    }else{
        Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
        z=depth.Load(int4(xy,i.view,0));
    }
    Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
    float3 color=resolved.Load(int4(xy,i.view,0)).rgb;
    if(topology.z!=0&&param1==0){
        Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
        color+=reflection.Load(int4(xy,i.view,0)).rgb;
    }
    return FinishPresent(i,color,i.uv,z);
}
