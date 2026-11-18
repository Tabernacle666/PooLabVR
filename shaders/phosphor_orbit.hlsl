cbuffer OrbitFrame : register(b0) {
    row_major float4x4 viewProjection[4];
    float4 localEye[4];
    float4 metrics;
    float4 display;
};
cbuffer OrbitDraw : register(b1) {
    uint vertexDescriptor;
    uint indexDescriptor;
    uint3 reserved;
};

struct VertexData {
    float3 position;
    float3 normal;
    float2 uv;
    float4 colorRoughness;
    float4 emissionMetallic;
    uint surface;
};

struct OrbitFragment {
    float4 position : SV_Position;
    float3 local : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    nointerpolation float4 colorRoughness : TEXCOORD3;
    nointerpolation float4 emissionMetallic : TEXCOORD4;
    nointerpolation uint surface : TEXCOORD5;
    nointerpolation uint view : TEXCOORD6;
};

OrbitFragment PhosphorOrbitVertex(uint vertexId : SV_VertexID, uint view : SV_ViewID) {
    StructuredBuffer<VertexData> vertices = ResourceDescriptorHeap[vertexDescriptor];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[indexDescriptor];
    const VertexData vertex = vertices[indices[vertexId]];
    OrbitFragment result;
    result.position = mul(float4(vertex.position,1),viewProjection[view]);
    result.local = vertex.position;
    result.normal = vertex.normal;
    result.uv = vertex.uv;
    result.colorRoughness = vertex.colorRoughness;
    result.emissionMetallic = vertex.emissionMetallic;
    result.surface = vertex.surface;
    result.view = view;
    return result;
}

uint2 Glyph(uint c){
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
    case 72u:return uint2(0x231fc631u,0x11u);
    case 81u:return uint2(0x2558c62eu,0xdu);
    case 87u:return uint2(0x2b5ac631u,0xau);
    case 88u:return uint2(0x22a22a31u,0x11u);
    case 89u:return uint2(0x8422a31u,0x4u);
    default:return uint2(0u,0u);
    }
}

float Ink(uint code,float2 pixel){
    if(any(pixel<0.f)||pixel.x>=5.f||pixel.y>=7.f)return 0.f;
    const uint2 g=Glyph(code);
    const uint2 q=uint2(pixel);
    const uint bits=q.y==6u?g.y:(g.x>>(q.y*5u));
    return float((bits>>(4u-q.x))&1u);
}

float Word(float2 p,float2 origin,uint4 text,float size){
    float2 q=(p-origin)/size;
    if(any(q<0.f)||q.x>=96.f||q.y>=7.f)return 0.f;
    const uint character=uint(q.x)/6u;
    const uint code=(text[character/4u]>>((character&3u)*8u))&255u;
    return Ink(code,float2(q.x-float(character*6u),q.y));
}

static const uint featureWords[8][5] = {
    {0x444e4552u,0x53205245u,0x454c4143u,0x00000000u,0x00000000u},
    {0x45564f46u,0x4f495441u,0x0000004eu,0x00000000u,0x00000000u},
    {0x44414853u,0x20474e49u,0x4c415551u,0x00595449u,0x00000000u},
    {0x44414853u,0x5220574fu,0x4c4f5345u,0x4f495455u,0x0000004eu},
    {0x54584554u,0x20455255u,0x45525453u,0x4e494d41u,0x00000047u},
    {0x00474f46u,0x00000000u,0x00000000u,0x00000000u,0x00000000u},
    {0x4c464552u,0x49544345u,0x00004e4fu,0x00000000u,0x00000000u},
    {0x50504952u,0x0000454cu,0x00000000u,0x00000000u,0x00000000u}
};
float FeatureText(float2 p,uint row){
    const float2 q=(p-float2(18,38+float(row)*21.f))/1.42f;
    if(any(q<0.f)||q.x>=120.f||q.y>=7.f)return 0.f;
    const uint character=uint(q.x)/6u;
    const uint code=(featureWords[row][character/4u]>>((character&3u)*8u))&255u;
    return Ink(code,float2(q.x-float(character*6u),q.y));
}

float Number(float2 p,float2 origin,float value,float size,bool decimal,bool percent){
    const float2 q=(p-origin)/size;
    if(any(q<0.f)||q.x>=48.f||q.y>=7.f)return 0.f;
    const uint column=uint(q.x)/6u;
    const float2 pixel=float2(q.x-float(column*6u),q.y);
    if(!isfinite(value)||value<-999.f)return Ink(column==0u?45u:32u,pixel);
    const uint scale=decimal?10u:1u;
    const uint packed=uint(round(min(abs(value),9999.f)*scale));
    const uint integer=packed/scale;
    const uint digits=integer>=1000u?4u:integer>=100u?3u:integer>=10u?2u:1u;
    const uint sign=value<0.f?1u:0u;
    if(sign&&column==0u)return Ink(45u,pixel);
    const uint index=column-sign;
    if(index<digits){
        const uint place=digits-index-1u;
        const uint divisor=place==3u?1000u:place==2u?100u:place==1u?10u:1u;
        return Ink(48u+(integer/divisor)%10u,pixel);
    }
    if(decimal&&index==digits)return Ink(46u,pixel);
    if(decimal&&index==digits+1u)return Ink(48u+packed%10u,pixel);
    return Ink(percent&&index==digits+(decimal?2u:0u)?37u:32u,pixel);
}

float Line(float distance,float width){
    return 1.f-smoothstep(width-.7f,width+.7f,abs(distance));
}

float Box(float2 p,float2 minimum,float2 maximum){
    const float2 d=max(minimum-p,p-maximum);
    return max(d.x,d.y);
}

static const float Pi=3.14159265f;
float Angle(float value){
    return Pi+Pi*log2(1.f+.08f*clamp(value,0.f,1000.f))/log2(81.f);
}

float Needle(float2 p,float angle,float length){
    const float2 d=float2(cos(angle),sin(angle));
    const float along=dot(p,d);
    const float across=dot(p,float2(-d.y,d.x));
    return Line(across,lerp(4.5f,1.1f,saturate(along/length)))*
           smoothstep(-6.f,-3.f,along)*(1.f-smoothstep(length-2.f,length,along));
}

float3 Hologram(float2 uv){
    const float2 p=saturate(uv)*float2(600.f,306.f);
    const float3 phosphor=float3(.43f,1.f,.45f);
    const float3 amber=float3(1.f,.43f,.08f);
    const float green=metrics.x>=0.f?100.f*metrics.x/display.x:-1000.f;
    const float margin=metrics.y>0.f&&metrics.x>0.f?metrics.z:-1000.f;
    float3 color=float3(.007f,.025f,.016f)*(1.f-.10f*cos(p.y*Pi));
    color+=phosphor*.05f*saturate(1.f-length(uv-.5f));

    if(p.y<224.f){
        color+=phosphor*.31f*(Line(p.x-181.f,.37f)+Line(p.x-476.f,.37f));
        if(p.x<181.f){
            color+=phosphor*.84f*Word(p,float2(18,14),uint4(0x54414546u,0x53455255u,0x00000000u,0x00000000u),1.62f);
            if(p.y>=31.f&&p.y<207.f){
                const uint row=min(uint((p.y-31.f)/21.f),7u);
                const float border=Box(p,float2(9,32),float2(171,52));
                if(row==0u)color+=phosphor*(.085f*step(border,0.f)+.56f*Line(border,.47f));
                color+=phosphor*FeatureText(p,row)*(row==0u?.97f:.72f);
            }
        }else if(p.x>=478.f){
            color+=phosphor*.65f*(Word(p,float2(493,25),uint4(0x00535046u,0x00000000u,0x00000000u,0x00000000u),1.67f)+
                Word(p,float2(493,85),uint4(0x00545345u,0x00000000u,0x00000000u,0x00000000u),1.67f)+
                Word(p,float2(493,145),uint4(0x4752414du,0x00000045u,0x00000000u,0x00000000u),1.67f));
            if(p.x<584.f)color+=phosphor*.36f*(Line(p.y-72.f,.37f)+Line(p.y-132.f,.37f));
            color+=phosphor*.97f*(Number(p,float2(493,44),metrics.x>=0.f?metrics.x:-1000.f,2.23f,true,false)+
                Number(p,float2(493,104),metrics.y>=0.f?metrics.y:-1000.f,2.23f,true,false));
            color+=amber*.95f*Number(p,float2(493,164),margin,2.23f,false,true);
        }else{
            const float2 pivot=float2(328,154);
            const float2 d=p-pivot;
            const float r=length(d);
            if(d.y<=10.f&&r>=68.f&&r<=151.f){
                const float u=1.f-atan2(max(-d.y,0.f),d.x)/Pi;
                const float tickDistance=abs(frac(u*52.f+.5f)-.5f);
                color+=phosphor*(Line(r-101.f,.71f)*.48f+
                         (1.f-smoothstep(.12f,.30f,tickDistance))*smoothstep(94.f,95.f,r)*
                         (1.f-smoothstep(100.f,101.f,r))*.74f)*step(d.y,1.f);
                static const float labels[9]={0.f,10.f,25.f,50.f,100.f,200.f,400.f,700.f,1000.f};
                [unroll]for(uint j=0;j<9u;j++){
                    const float a=Angle(labels[j]);
                    const float2 direction=float2(cos(a),sin(a));
                    const float2 target=pivot+direction*119.f;
                    const float2 mark=pivot+direction*97.f;
                    const float2 e=p-mark;
                    const uint digits=j==0u?1u:j<4u?2u:j<8u?3u:4u;
                    color+=phosphor*.86f*Number(p,target-float2(3.3f*float(digits+1),4.f),
                                                    labels[j],1.1f,false,true);
                    color+=phosphor*.71f*Line(dot(e,float2(-direction.y,direction.x)),.7f)*
                          (1.f-smoothstep(5.f,6.f,abs(dot(e,direction))));
                }
            }
            if(r<99.f){
                if(green>=0.f)color+=phosphor*Needle(d,Angle(green),89.f);
                if(margin>-999.f)color+=amber*Needle(d,Angle(margin),86.f);
                color=lerp(color,float3(.023f,.040f,.025f)+phosphor*.24f*Line(r-7.f,.83f),
                           1.f-smoothstep(7.f,8.f,r));
            }
            color+=phosphor*.67f*Word(p,float2(219,178),uint4(0x444e4552u,0x00005245u,0x00000000u,0x00000000u),1.43f);
            color+=amber*.74f*Word(p,float2(358,178),uint4(0x44414548u,0x4d4f4f52u,0x00000000u,0x00000000u),1.43f);
            color+=phosphor*Number(p,float2(219,196),green,2.26f,false,true);
            color+=amber*Number(p,float2(358,196),margin,2.26f,false,true);
        }
    }

    const float band=Box(p,float2(10,233),float2(588,281));
    color+=phosphor*(.030f*step(band,0.f)+.43f*Line(band,.49f));
    color+=phosphor*.78f*Word(p,float2(20,248),uint4(0x444e4552u,0x53205245u,0x454c4143u,0x00000000u),1.53f);
    color+=phosphor*.70f*(Word(p,float2(190,240),uint4(0x004e494du,0x00000000u,0x00000000u,0x00000000u),1.14f)+
        Word(p,float2(333,240),uint4(0x54434146u,0x0059524fu,0x00000000u,0x00000000u),1.14f)+
        Word(p,float2(447,240),uint4(0x52455355u,0x00000000u,0x00000000u,0x00000000u),1.14f));
    color+=phosphor*.49f*Word(p,float2(451,258),uint4(0x0000002du,0x00000000u,0x00000000u,0x00000000u),1.42f);
    if(p.x>184.f&&p.x<428.f)color+=phosphor*.41f*Line(p.y-267.f,.42f);
    const float cursor=lerp(186.f,425.f,saturate(display.y/100.f));
    color+=amber*(1.f-smoothstep(2.f,3.f,abs(p.x-cursor)))*Line(p.y-267.f,4.6f);
    color+=phosphor*.88f*Number(p,float2(536,246),display.y,1.47f,false,true);
    color*=.94f+.06f*saturate(min(min(p.x,600.f-p.x),min(p.y,306.f-p.y))*.16f);
    return saturate(color);
}

float Grain(float3 p){
    p=frac(p*.1053f);
    p+=dot(p,p.yzx+19.19f);
    return frac((p.x+p.y)*p.z);
}

float4 PhosphorOrbitPixel(OrbitFragment i) : SV_Target0 {
    if(i.surface==2u)return float4(Hologram(i.uv),1.f);
    const float3 n=normalize(i.normal);
    const float3 view=normalize(localEye[i.view].xyz-i.local);
    const float3 key=normalize(float3(-.5f,.75f,-.6f));
    const float facing=saturate(dot(n,view));
    const float glint=pow(saturate(dot(n,normalize(key+view))),24.f);
    if(i.surface==3u){
        const float edge=pow(1.f-facing,3.f);
        const float scratch=smoothstep(.978f,.995f,Grain(floor(i.local*float3(52.f,3.f,4.f))));
        const float3 reflection=float3(.16f,.26f,.22f)+
            float3(.54f,.68f,.62f)*(.43f*glint+.14f*scratch);
        return float4(reflection,saturate(.05f+.30f*edge+.19f*glint+.13f*scratch));
    }
    float3 base=i.colorRoughness.rgb;
    const float grain=Grain(floor(i.local*float3(68.f,63.f,68.f)));
    if(i.surface==1u){
        const float edge=smoothstep(.72f,1.f,max(abs(i.local.x)/4.05f,abs(i.local.y)/1.86f));
        const float worn=edge*smoothstep(.81f,.99f,grain);
        base=lerp(base,float3(.34f,.29f,.17f),worn*.64f);
    }
    const float light=.39f+.60f*saturate(dot(n,key));
    const float metal=i.emissionMetallic.w;
    float3 color=base*light*(.91f+.18f*grain)+
        float3(.70f,.60f,.39f)*glint*lerp(.07f,.32f,metal)+i.emissionMetallic.rgb;
    return float4(saturate(color),1.f);
}
