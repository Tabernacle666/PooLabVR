#include "visual_common.hlsli"
struct GOut{float4 position:SV_POSITION;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
GOut VisualBloomVS(uint id:SV_VertexID,uint view:SV_ViewID){GOut o;float2 p=float2((id<<1)&2,id&2);o.uv=p;o.view=view;o.position=float4(p*float2(2,-2)+float2(-1,1),0,1);return o;}
float2 VisualBloomTexel(){
    uint width,height,layers;
    if(visualFlags&4u){Texture2D<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(visualSceneSnapshot)];t.GetDimensions(width,height);}
    else{Texture2DArray<float4> t=ResourceDescriptorHeap[NonUniformResourceIndex(visualSceneSnapshot)];t.GetDimensions(width,height,layers);}
    return 1./max(float2(width,height),1.);
}
float3 VisualBloomBright(float3 color,float threshold){float l=max(color.r,max(color.g,color.b));return color*saturate((l-threshold)/max(.001,l));}
float4 VisualBloomExtractPS(GOut i):SV_TARGET{
    float2 texel=VisualBloomTexel()*.5;float threshold=VisualDraw().effect0.y;
    float3 sum=VisualBloomBright(VisualSnapshot(i.uv+texel*float2(-1,-1),i.view),threshold);
    sum+=VisualBloomBright(VisualSnapshot(i.uv+texel*float2(1,-1),i.view),threshold);
    sum+=VisualBloomBright(VisualSnapshot(i.uv+texel*float2(-1,1),i.view),threshold);
    sum+=VisualBloomBright(VisualSnapshot(i.uv+texel*float2(1,1),i.view),threshold);
    return float4(sum*.25,0);
}
float4 VisualBloomBlurPS(GOut i):SV_TARGET{
    float2 texel=VisualBloomTexel();float radius=max(.001,VisualDraw().effect0.x)*.5;
    float2 delta=(visualFlags&8u)?float2(0,texel.y*radius):float2(texel.x*radius,0);
    float3 sum=VisualSnapshot(i.uv,i.view)*.227027;
    sum+=(VisualSnapshot(i.uv-delta*1.384615,i.view)+VisualSnapshot(i.uv+delta*1.384615,i.view))*.316216;
    sum+=(VisualSnapshot(i.uv-delta*3.230769,i.view)+VisualSnapshot(i.uv+delta*3.230769,i.view))*.070270;
    return float4(sum,0);
}
float4 VisualBloomCompositePS(GOut i):SV_TARGET{return float4(VisualSnapshot(i.uv,i.view)*VisualDraw().effect0.z,0);}
