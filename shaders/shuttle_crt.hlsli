#include "phosphor_glyphs.hlsli"

float3 CockpitBackground(uint theme){
    switch(theme){
    case 1u:return float3(.067,.027,.010);
    case 2u:return float3(.008,.039,.053);
    case 3u:return float3(.022,.026,.050);
    case 4u:return float3(.048,.015,.050);
    default:return float3(.012,.041,.023);
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

float3 ShuttleCrt(Surface surface,float2 uv,float2 uv1,uint packed){
    if(packed==0u)return float3(.004,.007,.005);
    const uint theme=(packed&7u)-1u;
    const uint page=(packed>>3u)&7u;
    const uint payload=packed>>6u;
    const float2 p=uv*float2(1024.f,256.f)-float2((page&3u)*256u,(page>>2u)*128u);
    const float3 green=float3(.53,.90,.56),amber=float3(1.,.57,.24);
    const float3 background=CockpitBackground(theme);
    const float scan=.91+.09*cos(p.y*3.14159265f);

    if(page==3u){
        const uint selected=payload&15u;
        int value=int((payload>>4u)&127u);
        const uint progress=(payload>>11u)&31u;
        const bool alignedG=(payload&65536u)!=0u,alignedD=(payload&131072u)!=0u;
        const bool running=progress>20u;
        float3 ink=green*Word(p,float2(12,9),uint4(0x54535953u,0x53454d45u,0x00000000u,0x00000000u),2.35);
        ink+=(running?green:amber)*Word(p,float2(12,35),
            running?uint4(0x45544f4du,0x20535255u,0x54455250u,0x00000053u):uint4(0x47494f50u,0x5354454eu,0x53203220u,0x00004345u),1.92);
        if(!running){
            ink+=(alignedG?green:amber)*Word(p,float2(189,37),uint4(0x00000047u,0x00000000u,0x00000000u,0x00000000u),1.9);
            ink+=(alignedD?green:amber)*Word(p,float2(218,37),uint4(0x00000044u,0x00000000u,0x00000000u,0x00000000u),1.9);
            if(p.y>=52.f&&p.y<56.f&&p.x>=12.f&&p.x<12.f+float(progress)*10.f)ink+=green*.8;
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
        if(selected==4u)ink+=green*Word(p,float2(18,98),uint4(0x43494c43u,0x49545320u,0x47204b43u,0x00000000u),1.95);
        else{
            if(selected<2u)value-=40;
            if(selected==6u)value*=50;
            ink+=green*SettingNumber(p,float2(3,96),value,selected==7u);
            uint4 unit=0u;
            if(selected<2u)unit=uint4(0x00474544u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==2u||selected==5u)unit=uint4(0x00000025u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==6u)unit=uint4(0x4d2f5254u,0x00004e49u,0x00000000u,0x00000000u);
            else if(selected==7u)unit=uint4(0x00000053u,0x00000000u,0x00000000u,0x00000000u);
            else if(selected==8u)unit=uint4(0x0030312fu,0x00000000u,0x00000000u,0x00000000u);
            ink+=green*Word(p,float2(86,98),unit,1.65);
            ink+=green*.6*Word(p,float2(143,99),uint4(0x43495453u,0x0047204bu,0x00000000u,0x00000000u),1.7);
        }
        return background+ink*scan;
    }

    if(page==1u)
        return background+green*scan*(Word(p,float2(13,10),uint4(0x004c4f56u,0x00000000u,0x00000000u,0x00000000u),2.3)+
            SpeedNumber(p,payload)+Word(p,float2(151,59),uint4(0x00532f4du,0x00000000u,0x00000000u,0x00000000u),2.25));
    if(page==2u)
        return background+green*.72*Word(p,float2(15,12),uint4(0x45534956u,0x2f205255u,0x49455620u,0x00454c4cu),2.25)*scan;
    if(page==4u)
        return background+green*scan*(Word(p,float2(13,10),uint4(0x45534552u,0x494f5652u,0x00000052u,0x00000000u),2.3)+
            Word(p,float2(16,60),uint4(0x20303231u,0x0000474bu,0x00000000u,0x00000000u),3.45));
    if(page==5u){
        float3 ink=green*Word(p,float2(13,10),uint4(0x504f5250u,0x45534c55u,0x00535255u,0x00000000u),2.2);
        if(p.y>=48.f&&p.y<100.f&&p.x>=21.f&&p.x<211.f){
            const uint side=uint(p.y-48.f)/26u;
            const uint cell=uint(p.x-21.f)/47u;
            const float2 block=p-float2(21u+cell*47u,48u+side*26u);
            const uint slot=side*2u+(cell==0u?1u:cell==3u?0u:(5u-cell+1u));
            if(cell<4u&&block.x<26.f&&block.y<12.f){
                const bool on=(payload&(1u<<slot))!=0u;
                ink+=on?green:green*.19;
            }
        }
        return background+ink*scan;
    }
    if(page==6u)
        return background+green*.72*Word(p,float2(13,10),uint4(0x494e554du,0x4e4f4954u,0x00000053u,0x00000000u),2.2)*scan;

    const float4 ink=SampleMaterial(surface.emissive,surface,3,uv,uv1);
    if(p.x>=115.f&&p.y>=34.f)return lerp(background,ink.rgb,ink.a)*scan;
    const uint4 body=payload==0u?uint4(0x4953414fu,0x00000053u,0x00000000u,0x00000000u):payload==9u?uint4(0x454e554cu,0x00000000u,0x00000000u,0x00000000u):
                     payload==15u?uint4(0x4d534f43u,0x0000534fu,0x00000000u,0x00000000u):uint4(0x52545341u,0x00000045u,0x00000000u,0x00000000u);
    float label=Word(p,float2(13,10),uint4(0x2056414eu,0x4143202fu,0x00455452u,0x00000000u),2.2)+
                Word(p,float2(13,44),body,2.15)+Word(p,float2(13,80),uint4(0x2056494eu,0x00003130u,0x00000000u,0x00000000u),1.95);
    if(payload>0u&&payload<9u)
        label+=Ink(48u+payload,(p-float2(90,44))/2.15f);
    return background+green*label*scan;
}
