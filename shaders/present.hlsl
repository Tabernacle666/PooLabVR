#include "common.hlsli"
#include "projectile_overlay.hlsli"
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

float3 HudOverlay(float3 scene,float2 pixel){
    // First wrist-console pass.  The display is deliberately self-contained in
    // the existing present pass.  Controller bindings and audio remain unused.
    float scale=max(min(float(OutputWidth)/560.0,float(OutputHeight)/360.0),1.0);
    float2 p=(pixel-float2((OutputWidth-300.0*scale)*0.5,OutputHeight*0.27))/scale;
    float panelHeight=156.0;
    [branch]if(any(p<float2(-1.0,-1.0))||any(p>float2(301.0,panelHeight+1.0)))return scene;
    float distance=HudBox(p,float2(300.0,panelHeight),10.0);
    float aa=0.7/scale;
    [branch]if(distance>aa)return scene;
    float panel=1.0-smoothstep(-aa,aa,distance);
    float border=panel-(1.0-smoothstep(-aa,aa,distance+0.35));
    float3 result=lerp(scene,float3(0.004,0.016,0.012),panel*0.9);
    result=lerp(result,float3(0.10,0.48,0.23),saturate(border)*0.85);
    // Outer military housing, recessed glass and two physical control wells.
    float housing=1.0-smoothstep(0.0,1.0,abs(HudBox(p+float2(0.0,2.0),float2(294.0,150.0),8.0)));
    result+=float3(0.025,0.09,0.04)*housing*panel;
    float2 knobA=p-float2(22.0,43.0),knobB=p-float2(22.0,112.0);
    float ka=1.0-smoothstep(7.0,9.0,length(knobA)),kb=1.0-smoothstep(7.0,9.0,length(knobB));
    result+=float3(0.20,0.72,0.30)*(ka+kb)*panel;
    float2 gauge=p-float2(211.0,70.0);float gr=length(gauge);
    float ring=1.0-smoothstep(0.9,2.1,abs(gr-48.0));
    result+=float3(0.18,0.86,0.38)*ring*panel;
    // Exponential-style tachometer arc and the two live needles.
    float angle=atan2(gauge.y,gauge.x),arc=smoothstep(-2.45,-2.40,angle)*(1.0-smoothstep(2.35,2.40,angle));
    result+=float3(0.18,0.75,0.32)*arc*(1.0-smoothstep(44.0,49.0,gr))*panel;
    float greenRatio=saturate(hud.x/max(hud.y,1.0)),orangeRatio=saturate(hud.z/100.0);
    float2 ng=float2(cos(2.45-greenRatio*4.9),sin(2.45-greenRatio*4.9));
    float2 no=float2(cos(2.45-orangeRatio*4.9),sin(2.45-orangeRatio*4.9));
    float needleG=(1.0-smoothstep(1.0,2.5,abs(gauge.x*ng.y-gauge.y*ng.x)))*(1.0-smoothstep(4.0,52.0,-dot(gauge,ng)));
    float needleO=(1.0-smoothstep(1.0,2.5,abs(gauge.x*no.y-gauge.y*no.x)))*(1.0-smoothstep(5.0,51.0,-dot(gauge,no)));
    result+=float3(0.34,1.0,0.42)*needleG*panel+float3(1.0,0.42,0.06)*needleO*panel;
    // Recessed radio band and the pale set button.
    float band=1.0-smoothstep(0.7,1.8,abs(p.y-139.0))*smoothstep(18.0,28.0,p.x)*smoothstep(285.0,275.0,p.x);
    result+=float3(0.08,0.45,0.20)*band*panel;
    float2 sb=p-float2(278.0,105.0);float sbutton=1.0-smoothstep(9.0,11.0,length(sb));
    result+=float3(0.72,0.55,0.12)*sbutton*panel;

    int2 gridPos=int2(floor(p));
    int textRow=-1;
    if(gridPos.y>=10&&gridPos.y<17)textRow=0;
    else if(gridPos.y>=34&&gridPos.y<41)textRow=1;
    else if(gridPos.y>=49&&gridPos.y<56)textRow=2;
    else if(gridPos.y>=64&&gridPos.y<71)textRow=3;
    else if(gridPos.y>=79&&gridPos.y<86)textRow=4;
    if(textRow<0)return result;
    int row=textRow==0?gridPos.y-10:gridPos.y-(textRow*15+19);
    int start=textRow==0?38:31;
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
    return lerp(result,color,ink*panel);
}

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
    float3 mapped=ACES(ApplyAtmosphere(color,uv,z,i.view));
    [branch]if(hud.w>0.5f)mapped=HudOverlay(mapped,i.position.xy);
    Present o;
    o.color=float4(mapped,1);
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


