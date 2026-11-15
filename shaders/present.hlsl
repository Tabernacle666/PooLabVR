#include "common.hlsli"
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}

uint2 HudGlyph(uint c){
    switch(c){
    case 48u:return uint2(0x239ace2eu,0x0eu);
    case 49u:return uint2(0x08421184u,0x0eu);
    case 50u:return uint2(0x1041062eu,0x1fu);
    case 51u:return uint2(0x0217043eu,0x1eu);
    case 52u:return uint2(0x05f928c2u,0x02u);
    case 53u:return uint2(0x021f421fu,0x1eu);
    case 54u:return uint2(0x231f420eu,0x0eu);
    case 55u:return uint2(0x1082083fu,0x08u);
    case 56u:return uint2(0x2317462eu,0x0eu);
    case 57u:return uint2(0x0217c62eu,0x0eu);
    case 65u:return uint2(0x231fc62eu,0x11u);
    case 66u:return uint2(0x231f463eu,0x1eu);
    case 67u:return uint2(0x2108420fu,0x0fu);
    case 68u:return uint2(0x2318c63eu,0x1eu);
    case 69u:return uint2(0x210f421fu,0x1fu);
    case 70u:return uint2(0x210f421fu,0x10u);
    case 71u:return uint2(0x231bc20fu,0x0fu);
    case 73u:return uint2(0x0842109fu,0x1fu);
    case 77u:return uint2(0x231ad771u,0x11u);
    case 79u:return uint2(0x2318c62eu,0x0eu);
    case 80u:return uint2(0x210f463eu,0x10u);
    case 82u:return uint2(0x254f463eu,0x11u);
    case 83u:return uint2(0x0217420fu,0x1eu);
    case 84u:return uint2(0x0842109fu,0x04u);
    case 85u:return uint2(0x2318c631u,0x0eu);
    case 86u:return uint2(0x1518c631u,0x04u);
    case 46u:return uint2(0x0c000000u,0x06u);
    case 47u:return uint2(0x10820841u,0x10u);
    case 45u:return uint2(0x000f8000u,0x00u);
    default:return uint2(0u,0u);
    }
}

uint HudWord(uint2 word,uint index){
    if(index>=8u)return 32u;
    uint packed=index<4u?word.x:word.y;
    uint c=(packed>>((index&3u)*8u))&255u;
    return c==0u?32u:c;
}

uint HudNumber(float value,uint index,bool wide){
    uint width=wide?6u:5u;
    if(index>=width)return 32u;
    if(!(value>=0.0))return index==width-3u||index==width-2u?45u:32u;
    uint n=(uint)min(wide?99999.0:9999.0,floor(value*10.0+0.5));
    uint p=wide?index:index+1u;
    if(p==0u)return n>=10000u?48u+n/10000u:32u;
    if(p==1u)return n>=1000u?48u+(n/1000u)%10u:32u;
    if(p==2u)return n>=100u?48u+(n/100u)%10u:32u;
    if(p==3u)return 48u+(n/10u)%10u;
    if(p==4u)return 46u;
    return 48u+n%10u;
}

float HudGlyphCoverage(uint c,uint2 pixel){
    if(pixel.x>=5u||pixel.y>=7u)return 0.0;
    uint2 glyph=HudGlyph(c);
    uint row=pixel.y==6u?glyph.y:(glyph.x>>(pixel.y*5u))&31u;
    return float((row>>(4u-pixel.x))&1u);
}

float HudBox(float2 p,float2 size,float radius){
    float2 d=abs(p-size*0.5)-(size*0.5-radius);
    return length(max(d,0.0))+min(max(d.x,d.y),0.0)-radius;
}

float3 HudOverlay(float3 scene,float2 pixel){
    float scale=max(min(float(OutputWidth)/360.0,float(OutputHeight)/300.0),1.0);
    float2 p=(pixel-float2((OutputWidth-137.0*scale)*0.5,OutputHeight*0.30))/scale;
    float panelHeight=54.0;
    [branch]if(any(p<float2(-1.0,-1.0))||any(p>float2(138.0,panelHeight+1.0)))return scene;
    float distance=HudBox(p,float2(137.0,panelHeight),4.0);
    float aa=0.7/scale;
    [branch]if(distance>aa)return scene;
    float panel=1.0-smoothstep(-aa,aa,distance);
    float border=panel-(1.0-smoothstep(-aa,aa,distance+0.35));
    float3 result=lerp(scene,float3(0.008,0.019,0.027),panel*0.87);
    result=lerp(result,float3(0.035,0.22,0.23),saturate(border)*0.75);
    if(p.x>=9.0&&p.x<10.5&&p.y>=8.0&&p.y<15.0)result=float3(0.08,0.72,0.65);
    if(p.x>=9.0&&p.x<128.0&&p.y>=18.0&&p.y<18.25)result=float3(0.025,0.15,0.16);

    int2 gridPos=int2(floor(p));
    int textRow=-1;
    if(gridPos.y>=8&&gridPos.y<15)textRow=0;
    else if(gridPos.y>=22&&gridPos.y<29)textRow=1;
    else if(gridPos.y>=32&&gridPos.y<39)textRow=2;
    else if(gridPos.y>=42&&gridPos.y<49)textRow=3;
    if(textRow<0)return result;
    int row=textRow==0?gridPos.y-8:gridPos.y-(textRow*10+12);
    int start=textRow==0?14:9;
    if(gridPos.x<start)return result;
    int x=gridPos.x-start;
    uint index=(uint)(x/6);
    uint column=(uint)(x%6);
    uint character=32u;
    bool label=true;
    if(textRow==0)character=HudWord(uint2(0x4953414fu,0x00000053u),index);
    else if(textRow==1&&index<6u)character=HudWord(uint2(0x00535046u,0u),index);
    else if(textRow==2&&index<6u)character=HudWord(uint2(0x4d415246u,0x00000045u),index);
    else if(textRow==3&&index<6u)character=HudWord(uint2(0x4d415256u,0u),index);
    else{
        label=false;
        if(textRow==1)character=HudNumber(hud.x,index-6u,true);
        else if(textRow==2){
            uint c=index-6u;
            if(c<6u)character=HudNumber(hud.x>0.0?1000.0/max(hud.x,0.0001):-1.0,c,true);
            else if(c==6u)character=77u;
            else if(c==7u)character=83u;
        }else if(textRow==3){
            uint c=index-6u;
            if(c<5u)character=HudNumber(hud.y,c,false);
            else if(c==5u)character=47u;
            else if(c<11u)character=HudNumber(hud.z,c-6u,false);
            else if(c==11u)character=71u;
            else if(c==12u)character=73u;
            else if(c==13u)character=79u;
        }
    }
    float ink=HudGlyphCoverage(character,uint2(column,(uint)row));
    float3 color=textRow==0?float3(0.48,0.95,0.88):(label?float3(0.24,0.58,0.57):float3(0.83,0.92,0.93));
    return lerp(result,color,ink*panel);
}

struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present Tonemap(Out i){Texture2DArray<float4> color=ResourceDescriptorHeap[ColorResolved];uint2 xy=min(uint2(i.position.xy*float2(Width,Height)/float2(outputDimensions.xy)),uint2(Width-1,Height-1));
float z;
if(topology.y>1){
Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];z=0;[unroll]for(uint k=0;k<4;k++)z=max(z,depth.Load(int3(xy,i.view),k));
}else{
Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];z=depth.Load(int4(xy,i.view,0));
}

    float3 result=color.Load(int4(xy,i.view,0)).rgb;if(topology.z!=0&&param1==0){Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];result+=reflection.Load(int4(xy,i.view,0)).rgb;}
    Present o;o.color=float4(HudOverlay(ACES(result),i.position.xy),1);o.depth=z;return o;
}
