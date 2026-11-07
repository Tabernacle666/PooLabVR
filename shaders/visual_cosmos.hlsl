#include "visual_common.hlsli"
struct FullPixel {float4 position:SV_POSITION;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
FullPixel VisualCosmosVS(uint id:SV_VertexID,uint view:SV_ViewID){FullPixel o;float2 p=float2((id<<1)&2,id&2);o.uv=p;o.view=view;o.position=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}
float Hash(float3 p){return frac(sin(dot(p,float3(12.9898,78.233,37.719)))*43758.5453);}
float4 VisualCosmosPS(FullPixel i):SV_TARGET{float2 ndc=i.uv*float2(2,-2)+float2(-1,1);float4 p=mul(float4(ndc,.0001,1),inverseVP[i.view]);float3 direction=normalize(p.xyz/max(abs(p.w),.000001)-eye[i.view].xyz);float3 c=float3(.0003,.0008,.002)+float3(.0002,.0006,.0015)*(.5+.5*direction.y);float3 q=direction*450,cell=floor(q);float star=step(.9997,Hash(cell))*pow(saturate(1-length(frac(q)-.5)*2),12);return float4(c+star*.025,1);}
