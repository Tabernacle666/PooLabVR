#include "common.hlsli"
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}
struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present Tonemap(Out i){Texture2DArray<float4> color=ResourceDescriptorHeap[ColorResolved];uint2 xy=min(uint2(i.position.xy*float2(Width,Height)/float2(outputDimensions.xy)),uint2(Width-1,Height-1));
float z;
if(topology.y>1){
Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];z=0;[unroll]for(uint k=0;k<4;k++)z=max(z,depth.Load(int3(xy,i.view),k));
}else{
Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];z=depth.Load(int4(xy,i.view,0));
}

    float3 result=color.Load(int4(xy,i.view,0)).rgb;if(topology.z!=0&&param1==0){Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];result+=reflection.Load(int4(xy,i.view,0)).rgb;}
    Present o;o.color=float4(ACES(result),1);o.depth=z;return o;
}
