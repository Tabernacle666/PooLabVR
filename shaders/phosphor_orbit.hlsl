cbuffer OrbitFrame : register(b0) {
    row_major float4x4 viewProjection[4];
    float4 localEye[4];
    float4 metrics;
    float4 needleDirections;
    uint4 controls;
    uint4 features;
    float4 knobCenters;
    float4 knobDirections;
    float4 travelAndContext;
    float4 marks;
    float4 pointer;
};
cbuffer OrbitDraw : register(b1) {
    uint vertexDescriptor;
    uint indexDescriptor;
    uint plateDescriptor;
    uint firstIndex;
    uint reserved;
};

struct VertexData {
    float3 position;
    float3 normal;
    float2 uv;
    float4 colorRoughness;
    float4 emissionMetallic;
    uint surface;
    uint part;
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
    nointerpolation uint part : TEXCOORD7;
};

static const uint SelectPart=1u,AdjustPart=2u,ColorPart=3u,SetPart=4u;
static const uint FogFeature=1u,DlssFeature=2u,PatternFeature=3u,OpacityFeature=4u;
static const uint DeformationFeature=5u,GlitterFeature=6u,WidthFeature=7u;
static const uint StrengthFeature=8u,WaveSpeedFeature=9u,DurationFeature=10u;

float2 Rotate(float2 p,float2 direction){
    return float2(p.x*direction.x-p.y*direction.y,p.x*direction.y+p.y*direction.x);
}

OrbitFragment PhosphorOrbitVertex(uint vertexId : SV_VertexID, uint view : SV_ViewID) {
    StructuredBuffer<VertexData> vertices = ResourceDescriptorHeap[vertexDescriptor];
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[indexDescriptor];
    const VertexData vertex = vertices[indices[firstIndex + vertexId]];
    float3 local=vertex.position,normal=vertex.normal;
    if(vertex.part==SelectPart||vertex.part==AdjustPart){
        const bool select=vertex.part==SelectPart;
        const float2 center=select?knobCenters.xy:knobCenters.zw;
        const float2 direction=select?knobDirections.xy:knobDirections.zw;
        local.xy=center+Rotate(local.xy-center,direction);
        normal.xy=Rotate(normal.xy,direction);
    }else if(vertex.part==ColorPart){
        local.z+=travelAndContext.x;
    }else if(vertex.part==SetPart){
        local.z+=travelAndContext.y;
    }
    OrbitFragment result;
    result.position = mul(float4(local,1),viewProjection[view]);
    result.local = local;
    result.normal = normal;
    result.uv = vertex.uv;
    result.colorRoughness = vertex.colorRoughness;
    result.emissionMetallic = vertex.emissionMetallic;
    result.surface = vertex.surface;
    result.view = view;
    result.part = vertex.part;
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

uint4 FeatureLabel(uint feature){
    switch(feature){
    case FogFeature:return uint4(0x20474f46u,0x534e4544u,0x00595449u,0x00000000u); // FOG DENSITY
    case DlssFeature:return uint4(0x53534c44u,0x41435320u,0x0000454cu,0x00000000u); // DLSS SCALE
    case PatternFeature:return uint4(0x49544f4du,0x50532046u,0x00444545u,0x00000000u); // MOTIF SPEED
    case OpacityFeature:return uint4(0x4341504fu,0x00595449u,0x00000000u,0x00000000u); // OPACITY
    case DeformationFeature:return uint4(0x4f464544u,0x54414d52u,0x004e4f49u,0x00000000u); // DEFORMATION
    case GlitterFeature:return uint4(0x204e5553u,0x54494c47u,0x00524554u,0x00000000u); // SUN GLITTER
    case WidthFeature:return uint4(0x54494c47u,0x20524554u,0x54444957u,0x00000048u); // GLITTER WIDTH
    case StrengthFeature:return uint4(0x45564157u,0x52545320u,0x54474e45u,0x00000048u); // WAVE STRENGTH
    case WaveSpeedFeature:return uint4(0x45564157u,0x45505320u,0x00004445u,0x00000000u); // WAVE SPEED
    case DurationFeature:return uint4(0x45564157u,0x4d414420u,0x474e4950u,0x00000000u); // WAVE DAMPING
    default:return 0u;
    }
}

float Number(float2 p,float2 origin,float value,float size,uint decimals,bool percent){
    const float2 q=(p-origin)/size;
    if(any(q<0.f)||q.x>=48.f||q.y>=7.f)return 0.f;
    const uint column=uint(q.x)/6u;
    const float2 pixel=float2(q.x-float(column*6u),q.y);
    if(!isfinite(value)||value<-999.f)return Ink(column==0u?45u:32u,pixel);
    const uint scale=decimals==2u?100u:decimals==1u?10u:1u;
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
    if(decimals&&index==digits)return Ink(46u,pixel);
    if(decimals&&index<=digits+decimals){
        const uint divisor=index==digits+decimals?1u:10u;
        return Ink(48u+(packed/divisor)%10u,pixel);
    }
    return Ink(percent&&index==digits+(decimals?decimals+1u:0u)?37u:32u,pixel);
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

float Needle(float2 p,float2 d,float length){
    const float along=dot(p,d);
    const float across=dot(p,float2(-d.y,d.x));
    return Line(across,lerp(4.5f,1.1f,saturate(along/length)))*
           smoothstep(-6.f,-3.f,along)*(1.f-smoothstep(length-2.f,length,along));
}

static const float2 ScreenSize=float2(600.f,306.f);
static const float2 DialPivot=float2(328.f,154.f);

float2 StaticPlate(float2 p){
    float2 intensity=0.f;

    if(p.y<224.f){
        intensity.x+=.31f*(Line(p.x-181.f,.37f)+Line(p.x-476.f,.37f));
        if(p.x<181.f){
            intensity.x+=.84f*Word(p,float2(18,14),uint4(0x54414546u,0x53455255u,0x00000000u,0x00000000u),1.62f);
        }else if(p.x>=478.f){
            intensity.x+=.65f*Word(p,float2(493,25),uint4(0x00535046u,0u,0u,0u),1.67f);
            if(p.x<584.f)intensity.x+=.36f*Line(p.y-72.f,.37f);
        }else{
            const float2 d=p-DialPivot;
            const float r=length(d);
            if(d.y<=10.f&&r>=68.f&&r<=151.f){
                const float u=1.f-atan2(max(-d.y,0.f),d.x)/Pi;
                const float tickDistance=abs(frac(u*(41.f+16.f*u)+.5f)-.5f);
                intensity.x+=(Line(r-101.f,.71f)*.48f+
                         (1.f-smoothstep(.12f,.30f,tickDistance))*smoothstep(94.f,95.f,r)*
                         (1.f-smoothstep(100.f,101.f,r))*.74f)*step(d.y,1.f);
                static const float labels[9]={0.f,10.f,25.f,50.f,100.f,200.f,400.f,700.f,1000.f};
                [unroll]for(uint j=0;j<9u;j++){
                    const float a=Angle(labels[j]);
                    const float2 direction=float2(cos(a),sin(a));
                    const float2 target=DialPivot+direction*119.f;
                    const float2 mark=DialPivot+direction*97.f;
                    const float2 e=p-mark;
                    const uint digits=j==0u?1u:j<4u?2u:j<8u?3u:4u;
                    intensity.x+=.86f*Number(p,target-float2(3.3f*float(digits+1),4.f),
                                                    labels[j],1.1f,0u,true);
                    intensity.x+=.71f*Line(dot(e,float2(-direction.y,direction.x)),.7f)*
                          (1.f-smoothstep(5.f,6.f,abs(dot(e,direction))));
                }
            }
            intensity.x+=.67f*Word(p,float2(219,178),uint4(0x444e4552u,0x00005245u,0x00000000u,0x00000000u),1.43f);
            intensity.y+=.74f*Word(p,float2(358,178),uint4(0x44414548u,0x4d4f4f52u,0x00000000u,0x00000000u),1.43f);
        }
    }

    const float band=Box(p,float2(10,233),float2(588,281));
    intensity.x+=(.030f*step(band,0.f)+.43f*Line(band,.49f));
    return intensity;
}

[numthreads(8,8,1)]
void PhosphorOrbitPlate(uint3 id : SV_DispatchThreadID){
    RWTexture2D<float2> plate=ResourceDescriptorHeap[plateDescriptor];
    uint width,height;
    plate.GetDimensions(width,height);
    if(id.x>=width||id.y>=height)return;
    const float2 uv=(float2(id.xy)+.5f)/float2(width,height);
    plate[id.xy]=StaticPlate(uv*ScreenSize);
}

float FeatureRows(float2 p){
    if(p.y>=205.f&&features.y>=PatternFeature)
        return .54f*Word(p,float2(18,205),uint4(0x424d454du,0x454e4152u,0x00000000u,0x00000000u),1.42f);
    if(p.y<31.f||p.y>=199.f)return 0.f;
    const uint row=uint((p.y-31.f)/21.f);
    const uint feature=(features.x>>(4u*row))&15u;
    if(!feature)return 0.f;
    const float y=21.f*float(row);
    const bool selected=feature==features.y;
    const float border=Box(p,float2(9,32+y),float2(171,51+y));
    return (selected?.085f*step(border,0.f)+.56f*Line(border,.47f):0.f)+
           (selected?.97f:.72f)*Word(p,float2(18,38+y),FeatureLabel(feature),1.42f);
}

float ContextReadings(float2 p){
    if(features.z!=FogFeature&&features.z!=DlssFeature)return 0.f;
    const bool fog=features.z==FogFeature;
    const uint4 top=fog?uint4(0x45522054u,0x20312046u,0x4du,0u): // T REF 1 M
                       uint4(0x45544e49u,0x20454e52u,0x58u,0u); // INTERNE X
    const uint4 bottom=fog?uint4(0x45522054u,0x20352046u,0x4du,0u): // T REF 5 M
                          uint4(0x45544e49u,0x20454e52u,0x59u,0u); // INTERNE Y
    return .65f*(Word(p,float2(493,85),top,1.67f)+Word(p,float2(493,145),bottom,1.67f))+
           .97f*(Number(p,float2(493,104),travelAndContext.z,2.23f,0u,fog)+
                 Number(p,float2(493,164),travelAndContext.w,2.23f,0u,fog))+
           (p.x<584.f?.36f*Line(p.y-132.f,.37f):0.f);
}

float BandMark(float2 p,float mark,float y,float height,float width){
    const float x=lerp(181.f,473.f,mark);
    return Line(p.x-x,width)*Line(p.y-y,height);
}

float3 RadioBand(float2 p,float3 phosphor,float3 amber){
    const uint feature=features.y;
    const bool percent=feature==DlssFeature||feature==OpacityFeature||
        feature==GlitterFeature||feature==StrengthFeature;
    const bool speed=feature==PatternFeature||feature==WaveSpeedFeature;
    const uint decimals=feature==FogFeature||feature==DurationFeature||speed?2u:0u;
    const uint4 unit=speed?uint4(0x00000058u,0x00000000u,0x00000000u,0x00000000u):
        feature==DeformationFeature?uint4(0x00005850u,0x00000000u,0x00000000u,0x00000000u):
        feature==WidthFeature?uint4(0x00474544u,0x00000000u,0x00000000u,0x00000000u):
        feature==DurationFeature?uint4(0x00000053u,0x00000000u,0x00000000u,0x00000000u):0u;
    const float text=Word(p,float2(20,250),FeatureLabel(feature),1.62f)+
        Number(p,float2(507,248),marks.w,2.02f,decimals,percent)+
        Word(p,float2(570,251),unit,1.2f);
    float3 color=phosphor*.84f*text;
    if(p.x>=180.f&&p.x<=474.f){
        color+=phosphor*.38f*Line(p.y-257.f,.51f);
        if(features.w)color+=phosphor*.20f*Line(p.y-257.f,2.7f)*
            Line(frac((p.x-181.f)/29.2f+.5f)*29.2f-14.6f,.49f);
    }
    if(features.w)color+=phosphor*.44f*BandMark(p,0.f,257.f,5.f,.75f);
    color+=phosphor*.44f*BandMark(p,1.f,257.f,5.f,.75f);
    color+=float3(.36f,.75f,1.f)*BandMark(p,marks.y,248.f,3.5f,1.15f);
    if(marks.z>=0.f)color+=amber*BandMark(p,marks.z,267.f,3.5f,1.15f);
    color+=float3(.69f,1.f,.62f)*BandMark(p,marks.x,257.f,6.1f,1.85f);
    return color;
}

float3 Hologram(float2 uv){
    const float2 p=saturate(uv)*ScreenSize;
    const float3 phosphor=float3(.43f,1.f,.45f);
    const float3 amber=float3(1.f,.43f,.08f);
    const float green=metrics.y;
    const float margin=metrics.z;
    Texture2D<float2> plate=ResourceDescriptorHeap[plateDescriptor];
    SamplerState linearClamp=SamplerDescriptorHeap[2];
    const float2 intensity=plate.SampleLevel(linearClamp,uv,0.f);
    float3 color=float3(.007f,.025f,.016f)*(1.f-.10f*cos(p.y*Pi));
    color+=phosphor*(intensity.x+.05f*saturate(1.f-length(uv-.5f)))+amber*intensity.y;

    if(p.y<224.f){
        if(p.x>=478.f){
            color+=phosphor*(.97f*Number(p,float2(493,44),metrics.x>=0.f?metrics.x:-1000.f,2.23f,1u,false)+
                ContextReadings(p));
        }else if(p.x>=181.f){
            const float2 d=p-DialPivot;
            const float r=length(d);
            if(r<99.f){
                if(green>=0.f)color+=phosphor*Needle(d,needleDirections.xy,89.f);
                if(margin>-999.f)color+=amber*Needle(d,needleDirections.zw,86.f);
                color=lerp(color,float3(.023f,.040f,.025f)+phosphor*.24f*Line(r-7.f,.83f),
                           1.f-smoothstep(7.f,8.f,r));
            }
            color+=phosphor*Number(p,float2(219,196),green,2.26f,0u,true);
            color+=amber*Number(p,float2(358,196),margin,2.26f,0u,true);
        }else color+=phosphor*FeatureRows(p);
    }else if(features.y&&p.y<281.f)color+=RadioBand(p,phosphor,amber);
    color*=.94f+.06f*saturate(min(min(p.x,600.f-p.x),min(p.y,306.f-p.y))*.16f);
    return saturate(color);
}

float Grain(float3 p){
    p=frac(p*.1053f);
    p+=dot(p,p.yzx+19.19f);
    return frac((p.x+p.y)*p.z);
}

float PointerGlow(float3 at,float3 normal){
    if(!controls.w||at.z>-.45f||normal.z>-.16f)return 0.f;
    const float2 delta=at.xy-(pointer.xy+pointer.zw*at.z);
    return 1.f-smoothstep(.0016f,.0049f,dot(delta,delta));
}

float4 PhosphorOrbitPixel(OrbitFragment i) : SV_Target0 {
    [branch] if(i.surface==2u)return float4(Hologram(i.uv),1.f);
    const float3 n=normalize(i.normal);
    const float3 view=normalize(localEye[i.view].xyz-i.local);
    const float3 key=normalize(float3(-.5f,.75f,-.6f));
    const float facing=saturate(dot(n,view));
    const float glint=pow(saturate(dot(n,normalize(key+view))),24.f);
    const float spot=PointerGlow(i.local,n);
    const float3 highlight=float3(1.f,.82f,.33f);
    if(i.surface==3u){
        const float edge=pow(1.f-facing,3.f);
        const float scratch=smoothstep(.978f,.995f,Grain(floor(i.local*float3(52.f,3.f,4.f))));
        const float3 reflection=float3(.16f,.26f,.22f)+
            float3(.54f,.68f,.62f)*(.43f*glint+.14f*scratch);
        return float4(lerp(reflection,highlight,spot),
                      saturate(.05f+.30f*edge+.19f*glint+.13f*scratch+.83f*spot));
    }
    float3 base=i.colorRoughness.rgb;
    const float3 grainScale=i.surface>=4u?float3(720.f,720.f,720.f):float3(68.f,63.f,68.f);
    const float grain=Grain(floor(i.local*grainScale));
    if(i.surface==1u){
        static const float3 shellColors[5]={
            float3(.105f,.112f,.068f),float3(.042f,.15f,.18f),float3(.125f,.078f,.19f),
            float3(.21f,.06f,.143f),float3(.25f,.107f,.035f)};
        base=shellColors[min(controls.z,4u)];
        const float edge=smoothstep(.72f,1.f,max(abs(i.local.x)/4.05f,abs(i.local.y)/1.86f));
        const float worn=edge*smoothstep(.81f,.99f,grain);
        base=lerp(base,float3(.34f,.29f,.17f),worn*.64f);
    }
    if(i.surface==4u){
        // UVs follow the fingers and cuff. Filter the weave at arm's length.
        const float2 thread=i.uv*float2(40.f,56.f);
        const float2 footprint=fwidth(thread);
        const float weave=sin(thread.x*2.f*Pi)*sin((thread.y+.5f*thread.x)*2.f*Pi);
        const float contrast=1.f-saturate(max(footprint.x,footprint.y)*1.8f);
        base*=1.f+.19f*weave*contrast;
    }else if(i.surface==5u){
        const float worn=smoothstep(.956f,.997f,grain)*saturate(abs(n.x)+abs(n.y)-.82f);
        base=lerp(base,float3(.33f,.28f,.20f),worn*.68f);
    }
    const float light=.39f+.60f*saturate(dot(n,key));
    const float metal=i.emissionMetallic.w;
    const float specular=lerp(.07f,.32f,metal);
    const float3 emission=i.part>SetPart&&i.part!=SetPart+1u+controls.z?
        i.emissionMetallic.rgb*.13f:i.emissionMetallic.rgb;
    float3 color=base*light*(.91f+.18f*grain)+
        float3(.70f,.60f,.39f)*glint*specular*(i.surface==4u?.32f:1.f)+emission;
    if(i.part&&i.part<=SetPart){
        const float lit=i.part==controls.y?.74f:i.part==controls.x?.44f:
            (i.part==SetPart&&travelAndContext.y>0.f?.51f:0.f);
        color=lerp(color,highlight,lit);
    }
    color=lerp(color,highlight,spot);
    return float4(saturate(color),1.f);
}
