#include "phosphor_glyphs.hlsli"

float3 CockpitBackground(uint theme){
    switch(theme){
    case 1u:return float3(.067,.027,.010);
    case 2u:return float3(.008,.039,.053);
    case 3u:return float3(.022,.026,.050);
    case 4u:return float3(.048,.015,.050);
    case 5u:return float3(.012,.041,.023);
    default:return 0.f;
    }
}

float SettingNumber(float2 p,float2 origin,int value,bool decimal){
    float2 q=(p-origin)/2.6f;
    if(any(q<0.f)||q.x>=30.f||q.y>=7.f)return 0.f;
    const uint x=uint(q.x)/6u;
    const uint magnitude=uint(abs(value));
    uint digit=32u;
    if(decimal){
        if(x==2u)digit=48u+magnitude/10u;
        if(x==3u)digit=46u;
        if(x==4u)digit=48u+magnitude%10u;
    }else{
        if(x==0u&&value<0)digit=45u;
        if(x==1u&&magnitude>=1000u)digit=48u+magnitude/1000u;
        if(x==2u&&magnitude>=100u)digit=48u+(magnitude/100u)%10u;
        if(x==3u&&magnitude>=10u)digit=48u+(magnitude/10u)%10u;
        if(x==4u)digit=48u+magnitude%10u;
    }
    return Ink(digit,float2(q.x-6.f*x,q.y));
}

float SpeedNumber(float2 p,uint speed){
    float2 q=(p-float2(17.f,48.f))/4.f;
    if(any(q<0.f)||q.x>=30.f||q.y>=7.f)return 0.f;
    const uint column=uint(q.x)/6u;
    uint digit=32u;
    if(column==0u&&speed>=1000u)digit=48u+speed/1000u;
    if(column==1u&&speed>=100u)digit=48u+(speed/100u)%10u;
    if(column==2u)digit=48u+(speed/10u)%10u;
    if(column==3u)digit=46u;
    if(column==4u)digit=48u+speed%10u;
    return Ink(digit,float2(q.x-6.f*column,q.y));
}

float DiagramLine(float2 p,float2 a,float2 b,float w){
    const float2 ab=b-a;
    return 1.f-smoothstep(w,w+.8f,length(p-a-saturate(dot(p-a,ab)/dot(ab,ab))*ab));
}

float ThrustArrow(float2 p,float2 origin,float2 direction){
    const float2 normal=float2(-direction.y,direction.x);
    const float2 start=origin+direction*8.f;
    const float2 end=origin+direction*24.f;
    return max(DiagramLine(p,start,end,1.15f),max(
        DiagramLine(p,end,end-direction*6.f+normal*4.f,1.15f),
        DiagramLine(p,end,end-direction*6.f-normal*4.f,1.15f)));
}

float3 PropulsionDiagram(float2 p,uint payload,float3 ink,float3 amber){
    float3 result=ink*Word(p,float2(13,9),uint4(0x504f5250u,0x45534c55u,0x00535255u,0u),1.85f);
    result+=ink*.38f*max(DiagramLine(p,float2(34,71),float2(226,71),1.f),
        DiagramLine(p,float2(226,71),float2(213,65),1.f));
    result+=ink*.38f*DiagramLine(p,float2(226,71),float2(213,77),1.f);
    [unroll]for(uint side=0u;side<2u;++side){
        [unroll]for(uint cell=0u;cell<4u;++cell){
            const bool jet=cell==0u||cell==3u;
            const float2 origin=float2(35.f+cell*62.f,46.f+side*50.f);
            const float2 delta=abs(p-origin);
            const float outline=jet?abs(max(delta.x,delta.y)-6.f):abs(length(p-origin)-6.f);
            result+=ink*.64f*(1.f-smoothstep(.8f,1.6f,outline));
            const uint slot=side*2u+(cell==0u?0u:cell==3u?1u:cell==1u?4u:5u);
            const uint load=(payload>>(2u*slot))&3u;
            if(load!=0u){
                float2 direction;
                if(jet)direction=normalize(float2(cell==0u?1.f:-1.f,.44f*(2.f-float(load))));
                else direction=float2(0.f,load==1u?-1.f:1.f);
                result+=amber*ThrustArrow(p,origin,direction);
            }
        }
    }
    return result;
}

float3 ShuttleCrt(Surface surface,float2 uv,float2 uv1,uint packed){
    if(packed==0u)return float3(.004,.007,.005);
    const uint theme=(packed&7u)-1u;
    const uint page=(packed>>3u)&7u;
    const uint payload=packed>>6u;
    const float2 p=uv*float2(1024.f,256.f)-float2((page&3u)*256u,(page>>2u)*128u);
    const float3 amber=float3(1.,.57,.24);
    const float3 inkColor=theme==0u?amber:float3(.53,.90,.56);
    const float3 background=CockpitBackground(theme);
    const float scan=.91+.09*cos(p.y*3.14159265f);

    if(page==3u){
        const uint selected=payload&15u;
        int value=int((payload>>4u)&127u);
        const uint progress=(payload>>11u)&31u;
        const bool alignedG=(payload&65536u)!=0u,alignedD=(payload&131072u)!=0u;
        const bool running=progress>20u;
        float3 ink=inkColor*Word(p,float2(12,9),uint4(0x54535953u,0x53454d45u,0x00000000u,0x00000000u),2.35);
        ink+=(running?inkColor:amber)*Word(p,float2(12,35),
            running?uint4(0x45544f4du,0x20535255u,0x54455250u,0x00000053u):uint4(0x47494f50u,0x5354454eu,0x53203220u,0x00004345u),1.92);
        if(!running){
            ink+=(alignedG?inkColor:amber)*Word(p,float2(189,37),uint4(0x00000047u,0x00000000u,0x00000000u,0x00000000u),1.9);
            ink+=(alignedD?inkColor:amber)*Word(p,float2(218,37),uint4(0x00000044u,0x00000000u,0x00000000u,0x00000000u),1.9);
            if(p.y>=52.f&&p.y<56.f&&p.x>=12.f&&p.x<12.f+float(progress)*10.f)ink+=inkColor*.8;
        }
        uint4 label=uint4(0x454e4f5au,0x55454e20u,0x00455254u,0x00000000u);
        if(selected==0u)label=uint4(0x4c474e41u,0x00472045u,0x00000000u,0x00000000u);
        else if(selected==1u)label=uint4(0x4c474e41u,0x00442045u,0x00000000u,0x00000000u);
        else if(selected==3u)label=uint4(0x4c554f43u,0x00525545u,0x00000000u,0x00000000u);
        else if(selected==4u)label=uint4(0x41434552u,0x5242494cu,0x00005245u,0x00000000u);
        else if(selected==5u)label=uint4(0x53554f50u,0x20454553u,0x0058414du,0x00000000u);
        else if(selected==6u)label=uint4(0x49474552u,0x5220454du,0x524f544fu,0x00000053u);
        else if(selected==7u)label=uint4(0x544e4f4du,0x52204545u,0x524f544fu,0x00000053u);
        else if(selected==8u)label=uint4(0x49535341u,0x00004553u,0x00000000u,0x00000000u);
        ink+=amber*Word(p,float2(12,68),label,2.2);
        if(selected==4u)ink+=inkColor*Word(p,float2(18,98),uint4(0x43494c43u,0x49545320u,0x47204b43u,0x00000000u),1.95);
        else{
            if(selected<2u)value-=40;
            if(selected==6u)value*=50;
            ink+=inkColor*SettingNumber(p,float2(3,96),value,selected==7u);
            uint4 unit=0u;
            if(selected<2u)unit=uint4(0x00474544u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==2u||selected==5u)unit=uint4(0x00000025u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==6u)unit=uint4(0x4d2f5254u,0x00004e49u,0x00000000u,0x00000000u);
            else if(selected==7u)unit=uint4(0x00000053u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==8u)unit=uint4(0x0030312fu,0x00000000u,0x00000000u,0x00000000u);
            ink+=inkColor*Word(p,float2(86,98),unit,1.65);
            ink+=inkColor*.6*Word(p,float2(143,99),uint4(0x43495453u,0x0047204bu,0x00000000u,0x00000000u),1.7);
        }
        return background+ink*scan;
    }

    if(page==1u)return background+scan*PropulsionDiagram(p,payload,inkColor,amber);
    if(page==2u)
        return background+inkColor*.72*Word(p,float2(15,12),uint4(0x45534956u,0x2f205255u,0x49455620u,0x00454c4cu),2.25)*scan;
    if(page==4u)
        return background+inkColor*scan*(Word(p,float2(13,10),uint4(0x45534552u,0x494f5652u,0x00000052u,0x00000000u),2.3)+
            Word(p,float2(16,60),uint4(0x20303231u,0x0000474bu,0x00000000u,0x00000000u),3.45));
    if(page==5u){
        const uint speed=payload&4095u;
        const int vertical=int(payload>>12u)-31;
        return background+inkColor*scan*(Word(p,float2(13,11),uint4(0x45544956u,0x00455353u,0u,0u),2.4f)+
            SpeedNumber(p,speed)+Word(p,float2(151,59),uint4(0x00532f4du,0u,0u,0u),2.1f)+
            Word(p,float2(15,99),uint4(0x59u,0u,0u,0u),2.f)+
            SettingNumber(p,float2(35,96),vertical,false)+
            Word(p,float2(126,99),uint4(0x00532f4du,0u,0u,0u),1.7f));
    }
    if(page==6u)
        return background+inkColor*.72*Word(p,float2(13,10),uint4(0x494e554du,0x4e4f4954u,0x00000053u,0x00000000u),2.2)*scan;

    const float4 ink=SampleMaterial(surface.emissive,surface,3,uv,uv1);
    if(p.x>=115.f&&p.y>=34.f){
        const float3 marks=theme==0u?amber*dot(ink.rgb,float3(.21,.72,.07)):ink.rgb;
        return lerp(background,marks,ink.a)*scan;
    }
    const uint4 body=payload==0u?uint4(0x4953414fu,0x00000053u,0x00000000u,0x00000000u):payload==9u?uint4(0x454e554cu,0x00000000u,0x00000000u,0x00000000u):
                     payload==15u?uint4(0x4d534f43u,0x0000534fu,0x00000000u,0x00000000u):uint4(0x52545341u,0x00000045u,0x00000000u,0x00000000u);
    float label=Word(p,float2(13,10),uint4(0x2056414eu,0x4143202fu,0x00455452u,0x00000000u),2.2)+
                Word(p,float2(13,44),body,2.15)+Word(p,float2(13,80),uint4(0x2056494eu,0x00003130u,0x00000000u,0x00000000u),1.95);
    if(payload>0u&&payload<9u)
        label+=Ink(48u+payload,(p-float2(90,44))/2.15f);
    return background+inkColor*label*scan;
}
