#ifndef LAB_VISUAL_HLSLI
#define LAB_VISUAL_HLSLI
#define LAB_VISUAL_MAX_VIEWS 4
cbuffer LabVisualFrame : register(b0)
{
    row_major float4x4 vp[LAB_VISUAL_MAX_VIEWS];
    row_major float4x4 inverseVP[LAB_VISUAL_MAX_VIEWS];
    row_major float4x4 previousVP[LAB_VISUAL_MAX_VIEWS];
    row_major float4x4 shadowVP[24];
    float4 eye[LAB_VISUAL_MAX_VIEWS];
    uint4 resourceAbi;
    uint4 dimensions;
    uint4 outputDimensions;
    uint4 counts;
    uint4 topology;
    float4 timing;
};
cbuffer LabVisualRoot : register(b1)
{
    uint visualDrawIndex;
    uint visualDrawTable;
    uint visualSceneSnapshot;
    uint visualResourceTable;
    uint visualFlags;
};
uint VisualResource(uint slot)
{
    StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(visualResourceTable)];
    return table[slot].x;
}
#define VisualSceneSnapshot visualSceneSnapshot

struct VisualDrawData {
    float4 positionEmission;
    float4 rotation;
    float4 scale;
    float4 tint;
    float4 materialParams;
    float4 detailParams;
    float4 advancedParams;
    float4 deform;
    float4 extra;
    uint4 texture0;
    uint4 texture1;
    float4 effect0;
    float4 effect1;
};
VisualDrawData VisualDraw(){StructuredBuffer<VisualDrawData> table=ResourceDescriptorHeap[NonUniformResourceIndex(visualDrawTable)];return table[visualDrawIndex];}
float3 VisualRotate(float3 v,float4 q){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
float3 VisualWorld(float3 v,VisualDrawData d){return VisualRotate(v*d.scale.xyz,d.rotation)+d.positionEmission.xyz;}
float3 VisualNormal(float3 n,VisualDrawData d){return normalize(VisualRotate(n/max(abs(d.scale.xyz),.00001),d.rotation));}
float4 VisualSampleLevel(uint slot,float2 uv,float lod,float4 fallback){
    if(slot==0xffffffffu)return fallback;
    Texture2D<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(VisualResource(slot))];SamplerState s=SamplerDescriptorHeap[0];return t.SampleLevel(s,uv,lod);
}
float4 VisualSampleGrad(uint slot,float2 uv,float2 dx,float2 dy,float4 fallback){
    if(slot==0xffffffffu)return fallback;
    Texture2D<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(VisualResource(slot))];SamplerState s=SamplerDescriptorHeap[0];return t.SampleGrad(s,uv,dx,dy);
}
float3 VisualSnapshot(float2 uv,uint view){SamplerState s=SamplerDescriptorHeap[2];
    if(visualFlags&4u){Texture2D<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(visualSceneSnapshot)];return t.SampleLevel(s,uv,0).rgb;}
    Texture2DArray<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(visualSceneSnapshot)];return t.SampleLevel(s,float3(uv,view),0).rgb;
}
struct VisualLightData {float4 positionRadius;float4 colorPower;};
float3 VisualLighting(VisualDrawData d,float3 world,float3 n,float3 vd,float3 albedo,float rough,float wet,float ao){
    StructuredBuffer<VisualLightData> lights=ResourceDescriptorHeap[NonUniformResourceIndex(d.texture1.y)];float3 rgb=albedo*.012*ao;
    [loop]for(uint k=0;k<d.texture1.z;++k){VisualLightData l=lights[k];float3 delta=l.positionRadius.xyz-world;float distance=length(delta);float3 direction=delta/max(distance,.0001);float a=saturate(1-distance/max(l.positionRadius.w,.001));a=a*a/(1+.10*distance*distance);float ndl=saturate(dot(n,direction));float spec=pow(saturate(dot(n,normalize(direction+vd))),lerp(90.,7.,rough));rgb+=(albedo*ndl+spec*lerp(.06,.60,wet))*l.colorPower.rgb*l.colorPower.w*a*ao;}
    return rgb;
}
struct VisualMeshIn {float3 position:POSITION;float3 normal:NORMAL;float3 tangent:TANGENT;float2 uv:TEXCOORD0;};
struct VisualStreamIn {float3 position:POSITION;float4 color:COLOR;};
struct VisualPixel {float4 position:SV_POSITION;float4 clip:TEXCOORD0;float3 world:TEXCOORD1;float3 normal:TEXCOORD2;float3 tangent:TEXCOORD3;float2 uv:TEXCOORD4;float4 color:COLOR;float2 pointCoord:TEXCOORD5;nointerpolation uint view:TEXCOORD6;};
VisualPixel VisualProject(float3 world,float4 color,uint v){VisualPixel o=(VisualPixel)0;o.world=world;o.color=color;o.view=v;o.position=mul(float4(world,1),vp[v]);o.clip=o.position;return o;}
#endif
