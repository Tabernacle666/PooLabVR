#include "common.hlsli"
#ifdef OASIS_DLSS_FRAME
#include "projectile_overlay.hlsli"
#endif
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}

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

struct SpherePath {
    float3 ray;
    float entry,exit,sceneDistance;
};

bool TraceAtmosphereSphere(float2 uv,float depth,uint view,out SpherePath path){
    path=(SpherePath)0;
    const float3 origin=eye[view].xyz;
    path.ray=normalize(Unproject(uv,.0001f,view)-origin);
    const float3 relative=origin-atmosphereSphere.xyz;
    const float b=dot(relative,path.ray);
    const float3 closest=relative-b*path.ray;
    const float discriminant=atmosphereSphere.w*atmosphereSphere.w-dot(closest,closest);
    if(discriminant<=0.f)return false;
    const float chord=sqrt(discriminant);
    path.entry=-b-chord;
    path.exit=-b+chord;
    if(path.exit<=0.f)return false;
    path.sceneDistance=depth>0.f?length(Unproject(uv,depth,view)-origin):1.e20f;
    return true;
}

float3 ApplyAtmosphere(float3 scene,SpherePath path,uint view){
    [branch]if(atmosphereColor.w<=0.f)return scene;
    const float first=max(0.f,path.entry),last=min(path.exit,path.sceneDistance);
    [branch]if(last<=first)return scene;
    const float3 origin=eye[view].xyz;
    float3 samplePosition=origin+path.ray*((first+last)*.5);
    float ground=exp(-max(0.0,samplePosition.y-atmosphereSphere.y+atmosphereSphere.w*.25)*.65);
    float cloud=exp(-abs(samplePosition.y-atmosphereSphere.y-atmosphereSphere.w*.35)*1.1);
    float drift=.88+.12*sin(dot(samplePosition,float3(.83,.23,.61))+timing.z*.16);
    float opacity=1-exp(-(last-first)*atmosphereColor.w*(ground+.3*cloud)*drift);
    float3 toLight=atmosphereLight.xyz-samplePosition;
    float distanceSquared=dot(toLight,toLight);
    float3 eyeToLight=atmosphereLight.xyz-origin;
    float phase=.22+.78*pow(saturate(dot(path.ray,eyeToLight*rsqrt(max(dot(eyeToLight,eyeToLight),.000001)))),4);
    float illumination=atmosphereEmission.w*AtmosphereShadow(samplePosition)*phase/(1+distanceSquared);
    float3 haze=atmosphereColor.rgb*.035+atmosphereEmission.rgb*illumination*.08;
    return scene*(1-opacity)+haze*opacity;
}

float2 MembraneUV(float3 normal){
    float2 p=normal.xy/(abs(normal.x)+abs(normal.y)+abs(normal.z));
    if(normal.z<0.f)p=(1.f-abs(p.yx))*float2(p.x>=0.f?1.f:-1.f,p.y>=0.f?1.f:-1.f);
    const float size=float(membraneField.y);
    return (1.f+(p*.5f+.5f)*(size-2.f))/size;
}

struct MembraneOptics {
    float3 glow;
    float transmission;
    float2 shift;
    float depth;
};

float SunGlitter(float3 surface,SpherePath path,float surfaceDistance){
    const float3 light=float3(-.36f,.80f,-.48f);
    const float alignment=dot(reflect(path.ray,surface),light);
    const float lobe=smoothstep(membraneOptics.w,1.f,alignment);
    [branch]if(lobe<=0.f||membraneOptics.z<=0.f)return 0.f;
    const float time=membraneAnimation.x;
    const float3 sheet=float3(dot(surface,float3(32.f,101.f,21.f)),
        dot(surface,float3(-17.f,38.f,77.f)),dot(surface,float3(61.f,25.f,-28.f)));
    const float3 fold=1.f-abs(2.f*frac(sheet*.073f+time*float3(.027f,-.015f,.019f))-1.f);
    const float3 crest=1.f-abs(2.f*frac(sheet+2.1f*fold+time*float3(.13f,-.09f,.07f))-1.f);
    const float flecks=smoothstep(.47f,.84f,crest.x*crest.y*crest.z);
    const float facing=max(.10f,abs(dot(path.ray,surface)));
    const float footprint=2.f*surfaceDistance/(max(.01f,atmosphereSphere.w)*float(outputDimensions.x)*facing);
    const float detail=1.f-smoothstep(.002f,.012f,footprint);
    return membraneOptics.z*lobe*(.08f+.90f*lerp(.11f,flecks,detail));
}

MembraneOptics ShadeMembrane(SpherePath path,float2 uv,float sceneDepth,uint view){
    MembraneOptics optics=(MembraneOptics)0;
    optics.transmission=1.f;
    const float surfaceDistance=path.entry>0.f?path.entry:path.exit;
    if(path.sceneDistance<=surfaceDistance)return optics;

    const float3 world=eye[view].xyz+path.ray*surfaceDistance;
    const float3 surface=(world-atmosphereSphere.xyz)/atmosphereSphere.w;
    Texture2D<float4> field=ResourceDescriptorHeap[membraneField.x];
    SamplerState linearClamp=SamplerDescriptorHeap[2];
    const float4 film=field.SampleLevel(linearClamp,MembraneUV(surface),0.f);
    const float3 tangent=film.xyz-surface*dot(film.xyz,surface);
    const float3 normal=normalize(surface-tangent);
    const float facing=saturate(abs(dot(path.ray,normal)));
    const float graze=1.f-facing,rim=graze*graze;
    const float idleFacing=saturate(abs(dot(path.ray,surface)));
    const float idleGraze=1.f-idleFacing,idleRim=idleGraze*idleGraze;
    optics.transmission=1.f-membraneOptics.x*(1.f+.30f*idleRim);
    optics.glow=float3(.023f,.108f,.091f)*(.022f+.47f*idleRim);
    optics.glow+=float3(1.f,.72f,.44f)*SunGlitter(surface,path,surfaceDistance);
    optics.glow+=float3(.15f,.23f,.20f)*film.w*(.22f+.20f*rim);

    [branch]if(membraneAnimation.w>0.f&&path.entry>0.f&&path.sceneDistance>path.exit){
        const float chord=(path.exit-path.entry)/atmosphereSphere.w;
        const float3 farSide=surface+path.ray*chord;
        const float farCrest=field.SampleLevel(linearClamp,MembraneUV(farSide),0.f).w;
        optics.glow+=float3(.035f,.15f,.11f)*farCrest*.30f*saturate(chord*5.f);
    }

    [branch]if(sceneDepth>0.f&&membraneOptics.y>0.f){
        const float gap=path.sceneDistance-surfaceDistance;
        const float strength=saturate(gap/max(.1f,atmosphereSphere.w*.48f))*saturate(facing*6.f);
        const float4 clip=mul(float4(world,1.f),vp[view]);
        const float3 bend=tangent+surface*(.14f*graze);
        const float4 projected=mul(float4(bend,0.f),vp[view]);
        const float2 ndc=uv*float2(2.f,-2.f)+float2(-1.f,1.f);
        const float2 screenShift=(projected.xy-projected.w*ndc)*float2(.5f,-.5f);
        const float pixels=membraneOptics.y*float(outputDimensions.x)/2720.f;
        const float2 shift=screenShift*(7.f*strength);
        optics.shift=pixels*shift*rsqrt(max(1.f,dot(shift,shift)));
        optics.depth=clip.z/max(clip.w,.0001f);
    }
    return optics;
}

float2 PresentSourcePixel(float2 outputPixel){
    float2 pixel=outputPixel*float2(Width,Height)/float2(outputDimensions.xy);
#ifdef OASIS_DLSS_FRAME
    pixel+=dlssJitter.xy;
#endif
    return pixel;
}

uint2 PresentSourceCoord(float2 pixel){
    return uint2(clamp(pixel,0.f,float2(Width,Height)-1.f));
}

float PresentSceneDepth(uint2 xy,uint view){
#ifndef OASIS_DLSS_FRAME
    if(topology.y>1){
        Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];
        float z=0.f;
        [unroll]for(uint k=0;k<4;k++)z=max(z,depth.Load(int3(xy,view),k));
        return z;
    }
#endif
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    return depth.Load(int4(xy,view,0));
}

float3 PresentSceneColor(float2 outputPixel,float2 sourcePixel,uint2 xy,uint view,bool bent){
    SamplerState linearClamp=SamplerDescriptorHeap[2];
    float3 color;
#ifdef OASIS_DLSS_FRAME
    [branch]if(param2!=0u&&param2!=0xffffffffu){
        Texture2D<float4> upscaled=ResourceDescriptorHeap[NonUniformResourceIndex(dlssOutput[view])];
        [branch]if(bent)color=upscaled.SampleLevel(linearClamp,outputPixel/float2(outputDimensions.xy),0.f).rgb;
        else color=upscaled.Load(int3(uint2(outputPixel),0)).rgb;
    }else
#endif
    {
        Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
        const float2 samplePixel=clamp(sourcePixel,.5f,float2(Width,Height)-.5f);
#ifdef OASIS_DLSS_FRAME
        color=resolved.SampleLevel(linearClamp,float3(samplePixel/float2(outputDimensions.xy),view),0.f).rgb;
#else
        [branch]if(bent)color=resolved.SampleLevel(linearClamp,float3(samplePixel/float2(outputDimensions.xy),view),0.f).rgb;
        else color=resolved.Load(int4(xy,view,0)).rgb;
#endif
        if(topology.z!=0&&param1==0u){
            Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
            color+=reflection.Load(int4(xy,view,0)).rgb;
        }
    }
#ifdef OASIS_DLSS_FRAME
    [branch]if(dlssOutput.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[dlssOutput.z];
        color=BlendProjectile(color,marks.Load(int4(uint2(outputPixel),view,0)));
    }
#endif
    return color;
}

struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present Tonemap(Out i){
    float2 pixel=i.position.xy;
    float2 source=PresentSourcePixel(pixel);
    uint2 xy=PresentSourceCoord(source);
    const float z=PresentSceneDepth(xy,i.view);
    float2 uv=i.uv;
#ifdef OASIS_DLSS_FRAME
    uv+=dlssJitter.xy/float2(Width,Height);
#endif
    const bool drawMembrane=membraneField.x!=0xffffffffu;
    SpherePath path;
    bool hasPath=false,bent=false;
    MembraneOptics optics=(MembraneOptics)0;
    optics.transmission=1.f;
    [branch]if(atmosphereSphere.w>0.f&&(drawMembrane||atmosphereColor.w>0.f)){
        hasPath=TraceAtmosphereSphere(uv,z,i.view,path);
        [branch]if(hasPath&&drawMembrane){
            optics=ShadeMembrane(path,uv,z,i.view);
            [branch]if(dot(optics.shift,optics.shift)>.25f){
                const float2 candidate=clamp(pixel+optics.shift,.5f,float2(outputDimensions.xy)-.5f);
                const float2 candidateSource=PresentSourcePixel(candidate);
                const uint2 candidateCoord=PresentSourceCoord(candidateSource);
                if(PresentSceneDepth(candidateCoord,i.view)<=optics.depth+.00002f){
                    pixel=candidate;
                    source=candidateSource;
                    xy=candidateCoord;
                    bent=true;
                }
            }
        }
    }
    float3 color=PresentSceneColor(pixel,source,xy,i.view,bent);
    [branch]if(hasPath)color=ApplyAtmosphere(color,path,i.view);
    Present o;
    o.color=float4(ACES(color*optics.transmission+optics.glow),1.f);
    o.depth=z;
    return o;
}
