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
    result+=ink*.38f*max(DiagramLine(p,float2(34,73),float2(226,73),1.f),
        DiagramLine(p,float2(226,73),float2(213,67),1.f));
    result+=ink*.38f*DiagramLine(p,float2(226,73),float2(213,79),1.f);
    [unroll]for(uint side=0u;side<2u;++side){
        [unroll]for(uint cell=0u;cell<4u;++cell){
            const bool jet=cell==0u||cell==3u;
            const float2 origin=float2(35.f+cell*62.f,48.f+side*50.f);
            const float2 delta=abs(p-origin);
            const float outline=jet?abs(max(delta.x,delta.y)-6.f):abs(length(p-origin)-6.f);
            result+=ink*.64f*(1.f-smoothstep(.8f,1.6f,outline));
            const uint slot=side*2u+(cell==0u?0u:cell==3u?1u:cell==1u?4u:5u);
            const uint load=(payload>>(2u*slot))&3u;
            if(load!=0u){
                float2 direction;
                if(jet)direction=normalize(float2(cell==0u?1.f:-1.f,.44f*(2.f-float(load))));
                else direction=float2(0.f,(side==0u?1.f:-1.f)*(load==1u?1.f:-1.f));
                result+=amber*ThrustArrow(p,origin,direction);
            }
        }
    }
    return result;
}

float3 MixerPanel(float2 p,uint payload,float3 ink,float3 amber){
    static const uint4 labels[8]={
        uint4(0x43414552u,0x52554554u,0x494d2053u,0x0000004eu),
        uint4(0x43414552u,0x52554554u,0x414d2053u,0x00000058u),
        uint4(0x494c4548u,0x20534543u,0x004e494du,0x00000000u),
        uint4(0x494c4548u,0x20534543u,0x0058414du,0x00000000u),
        uint4(0x41504d49u,0x00535443u,0x00000000u,0x00000000u),
        uint4(0x554c4c41u,0x4547414du,0x00000000u,0x00000000u),
        uint4(0x534e4544u,0x20455449u,0x4d534f43u,0x45555149u),
        uint4(0x45545845u,0x55454952u,0x00000052u,0u)
    };
    const uint selected=payload&7u;
    const int value=int((payload>>3u)&127u);
    float3 color=ink*Word(p,float2(13,8),uint4(0x4158494du,0x4e204547u,0x54455641u,0x00004554u),2.25f);
    color+=amber*Word(p,float2(13,36),labels[selected],2.10f);
    color+=ink*SettingNumber(p,float2(153,59),value,false);
    color+=ink*Word(p,float2(234,62),uint4(0x00000025u,0x00000000u,0x00000000u,0x00000000u),1.7f);
    const float2 bar=p-float2(14,65);
    if(all(bar>=0.f)&&bar.x<156.f&&bar.y<10.f)
        color+=ink*(bar.x<1.56f*value?.92f:.12f);
    color+=ink*.66f*Word(p,float2(13,96),uint4(0x41502059u,0x00004547u,0u,0u),1.75f);
    color+=ink*.66f*Word(p,float2(116,96),uint4(0x43494c43u,0x46454420u,0x00545541u,0x00000000u),1.65f);
    return color;
}

float3 FeedbackPanel(float2 p,uint payload,float3 ink,float3 amber){
    // Matches ShuttlePilot::DisplayData for the right-hand CRT.
    const uint page=payload>>16u;
    if(page==0u)return MixerPanel(p,payload,ink,amber);
    if(page==3u){
        float3 result=ink*Word(p,float2(12,38),uint4(0x5245564fu,0x54495257u,0x00000045u,0x00000000u),2.f)+
                      ink*Word(p,float2(12,58),uint4(0x54434146u,0x2059524fu,0x41464544u,0x00544c55u),2.f);
        if((payload&4u)!=0u)
            result+=ink*Word(p,float2(12,94),uint4(0x2053494du,0x4d204e45u,0x494f4d45u,0x00004552u),1.9f);
        else if((payload&1u)!=0u)
            result+=amber*Word(p,float2(12,94),(payload&2u)!=0u?uint4(0x43494c43u,0x4e4f4320u,0x4d524946u,0x00005245u):uint4(0x45545441u,0x4f49544eu,0x202f204eu,0x43494c43u),1.85f);
        return result;
    }
    static const uint4 events[17]={
        uint4(0x54504143u,0x00455255u,0x00000000u,0x00000000u),
        uint4(0x52544e45u,0x4d204545u,0x52424d45u,0x00454e41u),
        uint4(0x544f5246u,0x454d4554u,0x0000544eu,0x00000000u),
        uint4(0x54524f53u,0x4d204549u,0x52424d45u,0x00454e41u),
        uint4(0x414d4544u,0x47415252u,0x00000045u,0x00000000u),
        uint4(0x494e4749u,0x4e4f4954u,0x00000000u,0x00000000u),
        uint4(0x45564e49u,0x4f495352u,0x0000004eu,0x00000000u),
        uint4(0x43414552u,0x52554554u,0x00000053u,0x00000000u),
        uint4(0x494c4548u,0x00534543u,0x00000000u,0x00000000u),
        uint4(0x434f4843u,0x54495620u,0x00004552u,0x00000000u),
        uint4(0x434f4843u,0x514f4320u,0x00004555u,0x00000000u),
        uint4(0x4e415243u,0x4e454d20u,0x00000055u,0x00000000u),
        uint4(0x49535546u,0x554c204cu,0x5249414eu,0x00000045u),
        uint4(0x4f4e4143u,0x554c204eu,0x5249414eu,0x00000045u),
        uint4(0x41504d49u,0x4c205443u,0x49414e55u,0x00004552u),
        uint4(0x454d4f44u,0x4e554c20u,0x45524941u,0x00000000u),
        uint4(0x45544f4du,0x20535255u,0x414e554cu,0x53455249u)
    };
    static const uint4 fields[8]={
        uint4(0x4e455645u,0x4e454d45u,0x00000054u,0x00000000u),
        uint4(0x45444f4du,0x00000000u,0x00000000u,0x00000000u),
        uint4(0x43524f46u,0x00252045u,0x00000000u,0x00000000u),
        uint4(0x45525544u,0x534d2045u,0x00000000u,0x00000000u),
        uint4(0x414c4544u,0x534d2049u,0x00000000u,0x00000000u),
        uint4(0x4f484345u,0x524f4620u,0x25204543u,0x00000000u),
        uint4(0x4f484345u,0x52554420u,0x4d204545u,0x00000053u),
        uint4(0x45544e49u,0x4c415652u,0x4d20454cu,0x00000053u)
    };
    const bool haptic=page==2u;
    const uint cue=min(payload&31u,16u);
    const uint field=(payload>>5u)&7u;
    const uint value=(payload>>8u)&255u;
    float3 result=ink*Word(p,float2(12,8),haptic?uint4(0x54504148u,0x45555149u,0x00000000u,0x00000000u):uint4(0x49445541u,0x5645204fu,0x4d454e45u,0x00544e45u),2.f);
    result+=(field==0u?amber:ink)*Word(p,float2(12,35),events[cue],2.f);
    const uint4 label=haptic||field==0u?fields[field]:field==1u?uint4(0x554c4f56u,0x2520454du,0x00000000u,0x00000000u):uint4(0x414e4f54u,0x4554494cu,0x00002520u,0x00000000u);
    result+=amber*Word(p,float2(12,65),label,1.7f);
    if(haptic&&field==1u){
        const uint mode=value==1u?84u:value==2u?83u:value==3u?68u:45u;
        result+=ink*Ink(mode,(p-float2(215,63))/2.6f);
    }else if(field!=0u){
        const bool milliseconds=haptic&&(field==3u||field==4u||field>=6u);
        result+=ink*SettingNumber(p,float2(171,61),int(milliseconds?10u*value:value),false);
    }
    result+=ink*.65f*Word(p,float2(12,99),field==0u?uint4(0x20442f47u,0x4e455645u,0x4e454d45u,0x00000054u):uint4(0x43494c43u,0x46454420u,0x00545541u,0x00000000u),1.7f);
    result+=ink*.65f*Word(p,float2(175,99),uint4(0x41502059u,0x00004547u,0u,0u),1.45f);
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
    if(page==2u)return background+scan*FeedbackPanel(p,payload,inkColor,amber);
    if(page==4u)
        return background+inkColor*scan*(Word(p,float2(13,10),uint4(0x45534552u,0x494f5652u,0x00000052u,0x00000000u),2.3)+
            Word(p,float2(16,60),uint4(0x20303231u,0x0000474bu,0x00000000u,0x00000000u),3.45));
    if(page==5u){
        const uint packedSpeed=payload&1023u;
        const uint speed=packedSpeed<=255u?packedSpeed:255u+5u*(packedSpeed-255u);
        const int climb=int((payload>>10u)&127u);
        const int vertical=(climb<=63?climb:63+4*(climb-63))*((payload&131072u)!=0u?-1:1);
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
