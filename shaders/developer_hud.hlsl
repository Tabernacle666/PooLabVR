#include "common.hlsli"

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
    case 76u:return uint2(0x21084210u,0x1fu);
    case 77u:return uint2(0x231ad771u,0x11u);
    case 78u:return uint2(0x2319d731u,0x11u);
    case 79u:return uint2(0x2318c62eu,0x0eu);
    case 80u:return uint2(0x210f463eu,0x10u);
    case 82u:return uint2(0x254f463eu,0x11u);
    case 83u:return uint2(0x0217420fu,0x1eu);
    case 84u:return uint2(0x0842109fu,0x04u);
    case 85u:return uint2(0x2318c631u,0x0eu);
    case 86u:return uint2(0x1518c631u,0x04u);
    case 37u:return uint2(0x26b21359u,0x0u);
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

float4 DeveloperHud(float4 position:SV_Position):SV_Target0{
    const float scale=asfloat(param0),panelHeight=asfloat(param1);
    const float2 p=(position.xy-float2(asfloat(objectIndex),asfloat(passIndex)))/scale;
    float distance=HudBox(p,float2(137.0,panelHeight),4.0);
    float aa=0.7/scale;
    [branch]if(distance>aa)return 0.0;
    float panel=1.0-smoothstep(-aa,aa,distance);
    float border=panel-(1.0-smoothstep(-aa,aa,distance+0.35));
    const float opacity=panel*0.87;
    float4 result=float4(float3(0.008,0.019,0.027)*opacity,opacity);
    result=lerp(result,float4(0.035,0.22,0.23,1.0),saturate(border)*0.75);
    if(p.x>=9.0&&p.x<10.5&&p.y>=8.0&&p.y<15.0)result=float4(0.08,0.72,0.65,1.0);
    if(p.x>=9.0&&p.x<128.0&&p.y>=18.0&&p.y<18.25)result=float4(0.025,0.15,0.16,1.0);

    int2 gridPos=int2(floor(p));
    int textRow=-1;
    if(gridPos.y>=8&&gridPos.y<15)textRow=0;
    else if(gridPos.y>=22&&gridPos.y<29)textRow=1;
    else if(gridPos.y>=32&&gridPos.y<39)textRow=2;
    else if(gridPos.y>=42&&gridPos.y<49)textRow=3;
    else if(gridPos.y>=52&&gridPos.y<59)textRow=4;
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
    else if(textRow==2&&index<6u)character=HudWord(uint2(0x00545345u,0u),index);
    else if(textRow==3&&index<6u)character=HudWord(uint2(0x4752414du,0x00000045u),index);
    else if(textRow==4&&index<6u)character=HudWord(uint2(0x53534c44u,0u),index);
    else{
        label=false;
        if(textRow==1)character=HudNumber(hud.x,index-6u,true);
        else if(textRow==2)character=HudNumber(hud.y,index-6u,true);
        else if(textRow==4){
            uint c=index-6u;
            if(param2==0xffffffffu)character=HudWord(uint2(0x00412f4eu,0u),c);
            else if(param2==0u)character=HudWord(uint2(0x0046464fu,0u),c);
            else if(c<5u)character=HudNumber(float(param2)*.1f,c,false);
            else if(c==5u)character=37u;
        }
        else if(textRow==3){
            uint c=index-6u;
            if(hud.y<0.0)character=HudNumber(-1.0,c,true);
            else if(hud.z<0.0){
                if(c==0u)character=45u;
                else if(c<7u)character=HudNumber(-hud.z,c-1u,true);
                else if(c==7u)character=37u;
            }else{
                if(c<6u)character=HudNumber(hud.z,c,true);
                else if(c==6u)character=37u;
            }
        }
    }
    float ink=HudGlyphCoverage(character,uint2(column,(uint)row));
    float3 color=textRow==0?float3(0.48,0.95,0.88):(label?float3(0.24,0.58,0.57):float3(0.83,0.92,0.93));
    return lerp(result,float4(color,1.0),ink*panel);
}
