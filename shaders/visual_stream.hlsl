#include "visual_common.hlsli"
VisualPixel VisualStreamVS(VisualStreamIn i,uint view:SV_ViewID){return VisualProject(i.position,i.color,view);}
VisualPixel VisualPointVS(VisualStreamIn input,uint corner:SV_VertexID,uint view:SV_ViewID){
    static const float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,1),float2(-1,-1),float2(1,1),float2(1,-1)};
    VisualPixel center=VisualProject(input.position,input.color,view);
    float2 delta=max(1.,VisualDraw().extra.y)/max(float2(dimensions.xy),1.)*center.position.w;
    float2 cornerValue=corners[corner];center.position.xy+=cornerValue*delta;center.pointCoord=cornerValue;return center;
}
float4 VisualStreamPS(VisualPixel i):SV_TARGET{return i.color*VisualDraw().tint;}
float4 VisualPointPS(VisualPixel i):SV_TARGET{float r=dot(i.pointCoord,i.pointCoord);if(r>1)discard;return float4(i.color.rgb*(1+(1-r)*.18),i.color.a*saturate(1.15-r))*VisualDraw().tint;}
