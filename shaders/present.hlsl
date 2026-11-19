#include "common.hlsli"
#include "debris_layout.hlsli"
#include "projectile_overlay.hlsli"
static const uint SphereGeometry=0u,SphereRelief=1u,SphereReliefLite=2u,SphereMean=3u,SpherePoint=4u;
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}

float AtmosphereShadow(float3 p,float3 light,float shadowSlot){
    [branch]if(shadowSlot<0)return 1.0;
    uint slot=(uint)shadowSlot;
    float3 d=p-light,ad=abs(d);
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

bool TraceSphere(float2 uv,float depth,uint view,float4 sphere,out SpherePath path){
    path=(SpherePath)0;
    const float3 origin=eye[view].xyz;
    path.ray=normalize(Unproject(uv,.0001f,view)-origin);
    const float3 relative=origin-sphere.xyz;
    const float b=dot(relative,path.ray);
    const float3 closest=relative-b*path.ray;
    const float discriminant=sphere.w*sphere.w-dot(closest,closest);
    if(discriminant<=0.f)return false;
    const float chord=sqrt(discriminant);
    path.entry=-b-chord;
    path.exit=-b+chord;
    if(path.exit<=0.f)return false;
    path.sceneDistance=depth>0.f?length(Unproject(uv,depth,view)-origin):1.e20f;
    return true;
}
bool TraceAtmosphereSphere(float2 uv,float depth,uint view,out SpherePath path){
    return TraceSphere(uv,depth,view,atmosphereSphere,path);
}

float4 DecodeFloor(uint4 cell){
    const float mass=float(cell.x>>24u)/255.f;
    const float3 tint=float3(cell.x&255u,(cell.x>>8u)&255u,(cell.x>>16u)&255u)/255.f;
    const float metal=saturate(float(cell.y&65535u)/210.f);
    const float soil=saturate(float(cell.y>>16u)/210.f);
    const float melt=max(metal,soil);
    const float since=timing.z-float(max(1u,cell.z>>1u)-1u)/128.f;
    const float heat=cell.z==0u?0.f:saturate(1.f-since*.48f);
    const float3 molten=(cell.z&1u)!=0u?float3(.99f,.14f,.024f):float3(1.25f,.48f,.068f);
    const float3 crust=metal*float3(.17f,.11f,.064f)+soil*float3(.12f,.045f,.025f);
    return float4(tint*(.40f*mass)*(1.f-melt)+crust+molten*(heat*heat*melt),
                  saturate(.46f*mass+.68f*melt));
}

float DepositThickness(uint4 cell){
    const float dust=float(cell.x>>24u)/255.f;
    const float melt=saturate(float((cell.y&65535u)+(cell.y>>16u))/360.f);
    return max(.64f*dust,melt);
}

float4 SampleDeposit(StructuredBuffer<uint4> floorMap,float2 horizontal,uint base,
                     float worldY,out float height){
    const int size=int(DEBRIS_FLOOR_SIZE);
    const float2 grid=(horizontal*.5f+.5f)*float(size)-.5f;
    const int2 origin=clamp(int2(floor(grid)),0,size-2);
    const uint address=base+uint(origin.x+origin.y*size);
    uint4 a=floorMap[address],b=floorMap[address+1u];
    uint4 c=floorMap[address+size],d=floorMap[address+size+1u];
    height=0.f;
    if(((a.x|a.y)|(b.x|b.y)|(c.x|c.y)|(d.x|d.y))==0u)return 0.f;
    if(base==DEBRIS_TERRAIN_BASE){
        const float h=worldY-atmosphereSphere.y+atmosphereSphere.w+1.f;
        a*=abs(asfloat(a.w)-h)<.065f?1u:0u;
        b*=abs(asfloat(b.w)-h)<.065f?1u:0u;
        c*=abs(asfloat(c.w)-h)<.065f?1u:0u;
        d*=abs(asfloat(d.w)-h)<.065f?1u:0u;
    }
    const float2 w=saturate(grid-float2(origin));
    const float4 heights=float4(DepositThickness(a),DepositThickness(b),
        DepositThickness(c),DepositThickness(d));
    height=lerp(lerp(heights.x,heights.y,w.x),lerp(heights.z,heights.w,w.x),w.y);
    const float2 slope=float2(lerp(heights.y-heights.x,heights.w-heights.z,w.y),
                             lerp(heights.z-heights.x,heights.w-heights.y,w.x));
    float4 pile=lerp(lerp(DecodeFloor(a),DecodeFloor(b),w.x),
                     lerp(DecodeFloor(c),DecodeFloor(d),w.x),w.y);
    pile.rgb*=clamp(1.f+dot(slope,float2(-.85f,1.25f)),.46f,1.56f);
    return pile;
}

float3 ShadeDebrisFloor(float3 color,StructuredBuffer<uint4> floorMap,
                        float3 origin,SpherePath path,float distance,bool front){
    const float radius=atmosphereSphere.w;
    const float3 local=origin+path.ray*distance;
    if(local.y>=0.f)return color;
    float height,displacedHeight;
    float4 pile=SampleDeposit(floorMap,local.xz/radius,1u,0.f,height);
    if(height<=0.f)return color;

    // Raise the inner surface under the crust, with a thicker buildup
    // on the membrane than on the island.
    const float inset=radius-min(.26f,.25f*radius)*height;
    const float b=dot(origin,path.ray);
    const float discriminant=b*b+inset*inset-dot(origin,origin);
    if(discriminant>0.f){
        const float t=-b+(front?-1.f:1.f)*sqrt(discriminant);
        if(t>0.f&&t<path.sceneDistance){
            const float3 at=origin+path.ray*t;
            const float4 lifted=SampleDeposit(floorMap,at.xz/radius,1u,0.f,displacedHeight);
            pile=lerp(pile,lifted,.75f);
        }
    }
    const float coverage=saturate(-local.y*5.f/radius);
    return color*(1.f-pile.a*coverage)+pile.rgb*coverage;
}

float3 ApplyDebrisFloor(float3 color,SpherePath path,uint view){
    [branch]if(debrisBindings.y==0xffffffffu)return color;
    const float3 origin=eye[view].xyz-atmosphereSphere.xyz;
    const float inverseRadius=rcp(atmosphereSphere.w);
    const float3 back=(origin+path.ray*path.exit)*inverseRadius;
    const float3 front=(origin+path.ray*path.entry)*inverseRadius;
    const bool seeBack=path.exit<path.sceneDistance&&back.y<0.f;
    const bool seeFront=path.entry>0.f&&path.entry<path.sceneDistance&&front.y<0.f;
    StructuredBuffer<uint4> floorMap=ResourceDescriptorHeap[debrisBindings.y];
    const uint occupied=floorMap[0].x;
    [branch]if(occupied==0u)return color;
    if(seeBack&&(occupied&1u)!=0u)color=ShadeDebrisFloor(color,floorMap,origin,path,path.exit,false);
    if((occupied&2u)!=0u&&path.sceneDistance>=max(0.f,path.entry)&&path.sceneDistance<path.exit){
        const float3 at=origin+path.ray*path.sceneDistance;
        float relief;
        float4 pile=SampleDeposit(floorMap,at.xz*inverseRadius,DEBRIS_TERRAIN_BASE,
                                 at.y+atmosphereSphere.y,relief);
        if(relief>0.f&&path.ray.y<-.12f){
            const float lift=min(.09f,.045f*relief/-path.ray.y);
            float shiftedHeight;
            const float4 raised=SampleDeposit(floorMap,(at-path.ray*lift).xz*inverseRadius,
                DEBRIS_TERRAIN_BASE,at.y+atmosphereSphere.y,shiftedHeight);
            pile=lerp(pile,raised,.65f);
        }
        color=color*(1.f-pile.a)+pile.rgb;
    }
    if(seeFront&&(occupied&1u)!=0u)color=ShadeDebrisFloor(color,floorMap,origin,path,path.entry,true);
    return color;
}

float3 ShadeAtmosphere(float3 scene,SpherePath path,uint view,bool resting,float4 volume,
                       float4 fog,float3 light,float4 emission,float shadowSlot){
    [branch]if(fog.w<=0.f)return scene;
    const float first=max(0.f,path.entry),last=min(path.exit,path.sceneDistance);
    [branch]if(last<=first)return scene;
    const float3 origin=eye[view].xyz;
    float3 samplePosition=origin+path.ray*((first+last)*.5);
    float ground=exp(-max(0.0,samplePosition.y-volume.y+volume.w*.25)*.65);
    float cloud=exp(-abs(samplePosition.y-volume.y-volume.w*.35)*1.1);
    float drift=.88;
    float3 haze=fog.rgb*.035;
    [branch]if(!resting){
        drift+=.12*sin(dot(samplePosition,float3(.83,.23,.61))+timing.z*.16);
        float3 toLight=light-samplePosition;
        float distanceSquared=dot(toLight,toLight);
        float3 eyeToLight=light-origin;
        float phase=.22+.78*pow(saturate(dot(path.ray,eyeToLight*rsqrt(max(dot(eyeToLight,eyeToLight),.000001)))),4);
        float illumination=emission.w*AtmosphereShadow(samplePosition,light,shadowSlot)*phase/(1+distanceSquared);
        haze+=emission.rgb*illumination*.08;
    }
    float opacity=1-exp(-(last-first)*fog.w*(ground+.3*cloud)*drift);
    return scene*(1-opacity)+haze*opacity;
}
float3 ApplyAtmosphere(float3 scene,SpherePath path,uint view,bool resting){
    return ShadeAtmosphere(scene,path,view,resting,atmosphereSphere,atmosphereColor,
                          atmosphereLight.xyz,atmosphereEmission,atmosphereParams.x);
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

MembraneOptics ShadeMembrane(SpherePath path,float2 uv,uint view,bool refract){
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
    const float3 lamp=atmosphereLight.xyz-world;
    const float lampPower=max(atmosphereEmission.w,0.f)/(1.f+dot(lamp,lamp));
    const float ridge=smoothstep(.04f,.28f,length(tangent));
    optics.glow=atmosphereEmission.rgb*lampPower*(.018f+.12f*rim)*ridge;
    optics.glow+=float3(.15f,.23f,.20f)*film.w*(.22f+.20f*rim);

    [branch]if(membraneAnimation.w>0.f&&path.entry>0.f&&path.sceneDistance>path.exit){
        const float chord=(path.exit-path.entry)/atmosphereSphere.w;
        const float3 farSide=surface+path.ray*chord;
        const float farCrest=field.SampleLevel(linearClamp,MembraneUV(farSide),0.f).w;
        optics.glow+=float3(.035f,.15f,.11f)*farCrest*.30f*saturate(chord*5.f);
    }

    [branch]if(refract&&membraneOptics.y>0.f){
        const float gap=path.sceneDistance-surfaceDistance;
        const float strength=saturate(gap/max(.1f,atmosphereSphere.w*.48f))*saturate(facing*6.f);
        const float4 clip=mul(float4(world,1.f),vp[view]);
        const float3 bend=tangent*(1.4f+.9f*ridge)+surface*(.04f*graze);
        const float4 projected=mul(float4(bend,0.f),vp[view]);
        const float2 ndc=uv*float2(2.f,-2.f)+float2(-1.f,1.f);
        const float2 screenShift=(projected.xy-projected.w*ndc)*float2(.5f,-.5f);
        const float pixels=membraneOptics.y*float(outputDimensions.x)/2720.f;
        const float2 shift=screenShift*(7.f*strength);
        optics.shift=pixels*shift*rsqrt(max(1.f,dot(shift,shift)));
        optics.depth=clip.z/max(clip.w,.0001f);
    }
    optics.glow*=membraneTint.rgb;
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

float3 ApplyPassageRefraction(float3 color,float2 pixel,float2 uv,uint view,float4 pulse){
    const float age=pulse.w-1.f;
    const float phase=saturate(age/.18f);
    const float fade=1.f-phase;
    const float2 size=float2(outputDimensions.xy);
    const float2 offset=(uv-.5f)*float2(size.x/size.y,1.f);
    const float distance=length(offset);
    const float ring=distance-(.08f+.63f*phase);
    const float wave=sin(ring*24.f)*saturate(1.f-abs(ring)*3.7f);
    const float2 direction=offset/max(.08f,distance);
    const float3 ray=normalize(Unproject(uv,.0001f,view)-eye[view].xyz);
    const float facing=.7f+.3f*abs(dot(ray,pulse.xyz));
    const float2 shift=direction*(.0045f+.008f*wave)*(fade*fade*facing*size.y);
    const float2 warped=clamp(pixel+shift,.5f,size-.5f);
    const float2 source=PresentSourcePixel(warped);
    const float3 sample=PresentSceneColor(warped,source,PresentSourceCoord(source),view,true);
    return lerp(color,sample,.76f*fade);
}

float2 WarpSphere(float2 uv,SpherePath path,uint view,float4 sphere,float wave){
    if(wave<=0.f||path.entry<=0.f)return 0.f;
    const float4 center=mul(float4(sphere.xyz,1.f),vp[view]);
    if(center.w<=0.f)return 0.f;
    const float2 radial=uv-(center.xy/center.w*float2(.5f,-.5f)+.5f);
    const float radius=max(length(radial),.0001f);
    const float chord=saturate((path.exit-path.entry)/(2.f*sphere.w));
    return radial*(wave*.5f*chord*chord/radius);
}
float2 SphereWarp(float2 uv,SpherePath path,uint view){
    return WarpSphere(uv,path,view,atmosphereSphere,sphereWave.x);
}

struct SphereLayer {
    SpherePath path;
    float3 color;
    float distance;
    uint level;
};
SphereLayer ProbeSphere(Out i,uint owner);
void CompositeSphere(Out i,uint owner,SphereLayer layer,inout float3 color,inout float distance,inout float depth);
float3 PairMarks(Out i,float3 color){
    [branch]if(sphereAtlas.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[sphereAtlas.z];
        return BlendProjectile(color,marks.Load(int4(uint2(i.position.xy),i.view,0)));
    }
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
    [branch]if(atmosphereSphere.w>0.f&&(drawMembrane||atmosphereColor.w>0.f||debrisBindings.y!=0xffffffffu)){
        hasPath=TraceAtmosphereSphere(uv,z,i.view,path);
        [branch]if(hasPath&&drawMembrane){
            optics=ShadeMembrane(path,uv,i.view,true);
            optics.shift+=SphereWarp(uv,path,i.view)*float2(outputDimensions.xy);
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
    const float4 passage=membranePassagePulses[0];
    // After crossing, the local surface is behind the view rays. Keep its
    // refraction beside the eyes until the short passage veil has faded.
    [branch]if(passage.w>=1.f&&passage.w<1.18f)
        color=ApplyPassageRefraction(color,pixel,uv,i.view,passage);
    float depth=z;
    float distance=1.e20f;
    SphereLayer relayed=(SphereLayer)0;
    bool overlayInFront=false;
    uint other=1u-spherePair.y;
    [branch]if(spherePair.x!=0u){
        distance=z>0.f?length(Unproject(uv,z,i.view)-eye[i.view].xyz):1.e20f;
        relayed=ProbeSphere(i,other);
        overlayInFront=hasPath&&relayed.path.entry<max(0.f,path.entry);
        if(!overlayInFront){
            CompositeSphere(i,other,relayed,color,distance,depth);
            if(hasPath)path.sceneDistance=min(path.sceneDistance,distance);
        }
    }
    [branch]if(hasPath){
        color=ApplyDebrisFloor(color,path,i.view);
        color=ApplyAtmosphere(color,path,i.view,false);
    }
    color=color*optics.transmission+optics.glow;
    [branch]if(spherePair.x!=0u){
        if(overlayInFront)CompositeSphere(i,other,relayed,color,distance,depth);
        color=PairMarks(i,color);
    }
    Present o;
    o.color=float4(ACES(color),1.f);
    o.depth=depth;
    return o;
}

static const float PerceptionEdges[7]={0.f,.20f,.40f,.60f,.80f,.94f,1.f};

float2 PerceptionUV(float4 clip){
    return clip.xy/clip.w*float2(.5f,-.5f)+.5f;
}

float PerceptionEccentricity(float2 uv,float2 origin){
    const float2 delta=uv-origin;
    const float2 relative=delta/lerp(origin,1.f-origin,step(0.f,delta));
    return max(abs(relative.x),abs(relative.y));
}

float PerceptionGlyph(float2 glyphUV,uint glyph){
    // Five columns and five rows. A–F, 0–9, m, R, P, H, L, M, space, -, +, degree.
    static const uint masks[26]={
        0x118fe2eu,0xf8be2fu,0x1e0843eu,0xf8c62fu,0x1f0bc3fu,0x10bc3fu,
        0xe9d72eu,0xe210c4u,0x1f2222eu,0xf83a0fu,0x847d29u,
        0xf83c3fu,0xe8bc2eu,0x21111fu,0xe8ba2eu,0xe87a2eu,
        0x15aac00u,0x92be2fu,0x10be2fu,0x118fe31u,0x1f08421u,0x118d771u,
        0u,0x7c00u,0x27c80u,0x1caeu};
    if(glyph>=26u||any(glyphUV<0.f)||any(glyphUV>=5.f))return 0.f;
    uint2 cell=uint2(glyphUV);
    return float((masks[glyph]>>(cell.y*5u+cell.x))&1u);
}

float2 PerceptionOrigin(uint view){
    const float4x4 projection=vp[view];
    const float3 forward=float3(projection[0][3],projection[1][3],projection[2][3]);
    return PerceptionUV(mul(float4(forward,0.f),projection));
}

float3 ApplySphereRangeGuides(Out i,float2 opticalOrigin,float3 color){
    const float3 camera=eye[i.view].xyz;
    const float3 rig=sphereGuideOrigin.xyz;
    const float planeY=rig.y-sphereGuideOrigin.w;
    const float3 ray=normalize(Unproject(i.uv,.0001f,i.view)-camera);
    const float t=(camera.y-planeY)/max(-ray.y,.0001f);
    const float range=length((camera+ray*t).xz-rig.xz);
    const float pixelWidth=max(.001f,1.3f*fwidth(range));
    const float delta=i.uv.x-opticalOrigin.x;
    const float horizontal=abs(delta)/(delta<0.f?opticalOrigin.x:1.f-opticalOrigin.x);
    const float extent=1.f-smoothstep(.575f,.60f,horizontal);
    const float3 ink=float3(.22f,.88f,1.f);
    if(ray.y<-.0001f&&extent>0.f){
        float lines=0.f;
        [unroll]for(uint mark=0u;mark<3u;++mark){
            const float distance=sphereGuideDistances[mark];
            const float width=min(.025f*distance,pixelWidth);
            const float stroke=1.f-smoothstep(0.f,width,abs(range-distance));
            lines=max(lines,stroke*(mark==sphereGuide.y?.80f:.30f));
        }
        color=lerp(color,ink,lines*extent);
    }

    // A single active legend sits beside the corresponding arc in view A–C.
    const float3 forward=normalize(float3(vp[0][0][3],0.f,vp[0][2][3]));
    const float3 right=float3(forward.z,0.f,-forward.x);
    const float distance=sphereGuideDistances[sphereGuide.y];
    const float3 anchor=float3(rig.x,planeY,rig.z)+normalize(forward-.33f*right)*distance;
    const float4 clip=mul(float4(anchor,1.f),vp[i.view]);
    if(clip.w>0.f){
        const float cell=max(2.f,.0036f*outputDimensions.y);
        const float2 anchorPixel=PerceptionUV(clip)*float2(outputDimensions.xy)+float2(0.f,-4.f*cell);
        const float2 p=(i.position.xy-anchorPixel)/cell+float2(24.f,2.5f);
        if(all(p>=float2(-1.f,-1.f))&&all(p<float2(49.f,6.f))){
            const uint meters=uint(distance+.5f);
            const uint representation=sphereGuide.z;
            const uint stageGlyphs[6]={9u,17u,17u,21u,18u,19u};
            const uint suffix=representation==SphereGeometry?3u:representation==SphereReliefLite?20u:22u;
            const uint glyphs[8]={sphereGuide.x,22u,meters>=10u?6u+meters/10u:22u,
                6u+meters%10u,16u,22u,stageGlyphs[representation],suffix};
            const uint index=min(7u,uint(max(0.f,p.x)/6.f));
            const float letter=PerceptionGlyph(p-float2(6.f*index,0.f),glyphs[index]);
            color=lerp(color,float3(.018f,.029f,.039f),.82f);
            color=lerp(color,ink,letter);
        }
    }
    return color;
}

float3 ApplyOrbitGuide(Out i,float2 origin,float3 color){
    const float2 size=float2(outputDimensions.xy);
    const float cell=max(2.f,.003f*size.y);
    const float2 center=origin*size;
    const float2 reticle=abs(i.position.xy-center);
    const float3 ink=float3(.36f,.89f,1.f);
    if(max(reticle.x,reticle.y)<cell&&min(reticle.x,reticle.y)<1.f)
        color=lerp(color,ink,.66f);
    const SphereProjection sprite=sphereProjections[sphereGuide.y];
    const float top=clamp((sprite.bounds[i.view].y-.035f)*size.y,.19f*size.y,center.y-.11f*size.y);
    const float2 p=(i.position.xy-float2(center.x,top))/cell+float2(45.f,2.5f);
    if(any(p<float2(-1.f,-1.f))||any(p>=float2(91.f,6.f)))return color;

    const float3 offset=sprite.sphere.xyz-eye[i.view].xyz;
    const float3 axis=normalize(float3(vp[i.view][0][3],0.f,vp[i.view][2][3]));
    const float side=dot(offset,float3(axis.z,0.f,-axis.x));
    const float degrees=atan2(side,dot(axis,offset))*57.2957795f;
    const uint angle=min(99u,uint(abs(degrees)+.5f));
    const uint meters=min(99u,uint(max(0.f,length(offset)-sprite.sphere.w)+.5f));
    const uint level=sphereGuide.z;
    const uint stageGlyphs[6]={9u,17u,17u,21u,18u,19u};
    const uint glyphs[15]={sphereGuide.y,22u,sphereGuide.x,22u,
        meters>=10u?6u+meters/10u:22u,6u+meters%10u,16u,22u,
        degrees<0.f?23u:24u,angle>=10u?6u+angle/10u:22u,6u+angle%10u,25u,22u,
        stageGlyphs[level],level==SphereGeometry?3u:level==SphereReliefLite?20u:22u};
    const uint index=min(14u,uint(max(0.f,p.x)/6.f));
    const float letter=PerceptionGlyph(p-float2(6.f*index,0.f),glyphs[index]);
    return lerp(lerp(color,float3(.018f,.029f,.039f),.74f),ink,letter);
}

float3 ApplySpherePerception(Out i,float3 color){
    const float2 origin=PerceptionOrigin(i.view);
    if(sphereGuide.w==1u)return ApplyOrbitGuide(i,origin,color);
    const float radius=PerceptionEccentricity(i.uv,origin);
    float edge=1.f;
    [unroll]for(uint n=0;n<5;++n)edge=min(edge,abs(radius-PerceptionEdges[n+1u]));
    const float border=1.f-smoothstep(0.f,max(.0001f,1.5f*fwidth(radius)),edge);
    const float dash=step(.60f,frac((i.uv.x+i.uv.y)*36.f));
    const float3 ink=float3(.17f,.86f,1.f);
    color=lerp(color,ink,.34f*border*dash);

    // Put each letter inside its rectangle, near the horizontal line of sight.
    const float2 extent=float2(outputDimensions.xy);
    const float row=origin.y-.070f*origin.y;
    if(abs(i.uv.y-row)<.023f){
        const bool right=i.uv.x>=origin.x;
        const float width=right?1.f-origin.x:origin.x;
        const float dx=abs(i.uv.x-origin.x)/width;
        uint zone=0u;
        [unroll]for(uint band=0;band<5;++band)zone+=uint(dx>=PerceptionEdges[band+1u]);
        const float lo=PerceptionEdges[zone];
        const float hi=PerceptionEdges[zone+1u];
        const float x=zone==0u?origin.x:origin.x+(right?1.f:-1.f)*width*(lo+hi)*.5f;
        const float cellSize=min(.0065f*extent.y,.14f*(hi-lo)*width*extent.x);
        const float2 label=(i.position.xy-float2(x,row)*extent)/cellSize;
        if(all(abs(label)<3.f)){
            const float letter=PerceptionGlyph(label+2.5f,zone);
            const float3 back=lerp(color,float3(.018f,.029f,.039f),.8f);
            color=lerp(back,ink,letter);
        }
    }
    return ApplySphereRangeGuides(i,origin,color);
}

Present TonemapSpherePerception(Out i){
    Present result=Tonemap(i);
    result.color.rgb=ApplySpherePerception(i,result.color.rgb);
    return result;
}

// Cached RGB is scene-linear. Alpha stores view depth relative to the sphere,
// with zero reserved for uncovered texels.
float4 SphereCaptureTexel(uint2 tile){
    const float2 uv=lerp(sphereRect.xy,sphereRect.zw,(float2(tile)+.5f)/256.f);
    const uint2 pixel=uint2(clamp(uv*float2(Width,Height),0.f,float2(Width-1u,Height-1u)));
    const float z=PresentSceneDepth(pixel,0u);
    const float2 sampleUv=(float2(pixel)+.5f)/float2(Width,Height);
    SpherePath path;
    if(!TraceAtmosphereSphere(sampleUv,z,0u,path))return 0.f;
    Texture2DArray<float4> colorSource=ResourceDescriptorHeap[ColorResolved];
    const float3 rgb=colorSource.Load(int4(pixel,0,0)).rgb;
    float3 color=ApplyDebrisFloor(rgb,path,0u);
    if(z<=0.f&&all(color==rgb))return 0.f;
    float viewZ=LinearDepth(z);
    if(z<=0.f){
        const float3 deposit=eye[0].xyz+path.ray*max(0.f,path.entry);
        viewZ=mul(float4(deposit,1.f),vp[0]).w;
    }
    return float4(color,max(.001f,viewZ-sphereSourceDepth[0].x+2.f*atmosphereSphere.w));
}

[numthreads(8,8,1)]
void CaptureSphereView(uint3 id:SV_DispatchThreadID){
    RWTexture2DArray<float4> relief=ResourceDescriptorHeap[objectIndex];
    relief[uint3(id.xy,passIndex)]=SphereCaptureTexel(id.xy);
}

groupshared float4 sphereMeanTiles[64];

[numthreads(64,1,1)]
void CaptureSphereMean(uint index:SV_GroupIndex){
    Texture2DArray<float4> relief=ResourceDescriptorHeap[objectIndex];
    RWStructuredBuffer<float4> mean=ResourceDescriptorHeap[passIndex];
    const uint2 tile=uint2(index%8u,index/8u)*32u+16u;
    float4 sum=0.f;
    for(uint view=0;view<param1;++view){
        const float4 texel=relief.Load(int4(tile,param0+view,0));
        if(texel.w>0.f)sum+=float4(texel.rgb,1.f);
    }
    sphereMeanTiles[index]=sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint step=32u;step>0u;step>>=1u){
        if(index<step)sphereMeanTiles[index]+=sphereMeanTiles[index+step];
        GroupMemoryBarrierWithGroupSync();
    }
    if(index==0u){
        const float4 total=sphereMeanTiles[0];
        mean[0]=float4(total.rgb/max(1.f,total.w),total.w>0.f?1.f:0.f);
    }
}

Out SphereProxyBounds(uint id:SV_VertexID,uint view:SV_ViewID){
    const float4 bounds=sphereBounds[view];
    const float2 corner=float2(id==1u||id==2u||id==4u,id==2u||id==4u||id==5u);
    Out o;
    o.uv=lerp(bounds.xy,bounds.zw,corner);
    o.position=float4(o.uv*float2(2.f,-2.f)+float2(-1.f,1.f),0.f,1.f);
    o.view=view;
    return o;
}

float4 ReprojectSphere(SpherePath path,float3 origin,uint textureIndex,uint slice,uint steps,
                       float4x4 sourceVP,float4 rect,float sourceDepth,float radius,out float distance){
    distance=path.exit;
    if(slice==0xffffu)return 0.f;
    Texture2DArray<float4> relief=ResourceDescriptorHeap[textureIndex];
    // Transform the ray once, then solve in the capture's clip space.
    const float4 start=mul(float4(origin,1.f),sourceVP);
    const float4 direction=mul(float4(path.ray,0.f),sourceVP);
    if(direction.w<=.001f)return 0.f;
    const float2 cropScale=rcp(rect.zw-rect.xy);
    const float depthOffset=sourceDepth-2.f*radius-start.w;
    float t=.5f*(path.entry+path.exit);
    [unroll]for(uint step=0u;step<3u;++step){
        const float4 p=start+direction*t;
        if(p.w<=0.f)return 0.f;
        const float2 uv=(PerceptionUV(p)-rect.xy)*cropScale;
        if(any(uv<0.f)||any(uv>=1.f))return 0.f;
        const float4 texel=relief.Load(int4(uint2(uv*256.f),slice,0));
        if(texel.w<=0.f)return 0.f;
        const float next=(texel.w+depthOffset)/direction.w;
        if(next<path.entry-.04f||next>path.exit+.04f)return 0.f;
        if(step+1u==steps){
            if(steps==3u&&abs(next-t)>.16f*radius)return 0.f;
            distance=next;
            return float4(texel.rgb,1.f);
        }
        t=next;
    }
    return 0.f;
}

float4 SphereReproject(SpherePath path,float3 origin,uint source,uint slice,uint steps,out float distance){
    return ReprojectSphere(path,origin,sphereAtlas.x,slice,steps,sphereSourceVP[source],
        sphereSourceRect[source],sphereSourceDepth[source].x,atmosphereSphere.w,distance);
}

#include "sphere_pair.hlsli"

Present DrawSphereProxy(Out i){
    const float3 background=float3(.008f,.013f,.021f);
    float3 color=background;
    Present result;
    result.depth=0.f;
    const float4 bounds=sphereBounds[i.view];
    const bool lite=param0==SphereReliefLite;
    const bool mean=param0==SphereMean;
    // param0 is the representation. The inspection variant also draws the
    // background and guides for invisible eyes.
    if(param0==SpherePoint){
        const float4 clip=mul(float4(atmosphereSphere.xyz,1.f),vp[i.view]);
        if(clip.w>0.f&&bounds.z>bounds.x){
            const float2 center=PerceptionUV(clip);
            const float2 sigma=max(float2(3.f,3.f)/float2(outputDimensions.xy),
                .16f*(bounds.zw-bounds.xy));
            const float2 p=(i.uv-center)/sigma;
            color+=float3(.09f,.38f,.30f)*exp2(-dot(p,p)*1.6f);
        }
    }else if((param0==SphereRelief||lite||mean)&&all(i.uv>=bounds.xy)&&all(i.uv<bounds.zw)){
        SpherePath path;
        if(TraceAtmosphereSphere(i.uv,0.f,i.view,path)&&path.entry>0.f){
            if(sphereWave.x>0.f){
                SpherePath warped;
                if(TraceAtmosphereSphere(i.uv+SphereWarp(i.uv,path,i.view),0.f,i.view,warped))
                    path=warped;
            }
            const float3 origin=eye[i.view].xyz;
            float distance=0.f;
            [branch]if(mean){
                StructuredBuffer<float4> average=ResourceDescriptorHeap[sphereAtlas.w];
                const float4 interior=average[0];
                const float facing=saturate((path.exit-path.entry)/max(.001f,2.f*atmosphereSphere.w));
                color=lerp(background,interior.rgb*(.76f+.24f*facing),interior.a);
                distance=path.entry;
            }else{
                const uint first=sphereAtlas.y&0xffffu;
                float hit;
                float4 sampleColor;
                [branch]if(lite)sampleColor=SphereReproject(path,origin,0u,first,2u,hit);
                else{
                    sampleColor=SphereReproject(path,origin,0u,first,3u,hit);
                    [branch]if(sampleColor.a==0.f)
                        sampleColor=SphereReproject(path,origin,1u,sphereAtlas.y>>16u,3u,hit);
                }
                if(sampleColor.a>0.f){
                    distance=hit;
                    color=sampleColor.rgb;
                    path.sceneDistance=hit;
                }
                color=ApplyAtmosphere(color,path,i.view,lite);
                [branch]if(lite){
                    const float rim=1.f-saturate((path.exit-path.entry)/(2.f*atmosphereSphere.w));
                    color*=1.f-membraneOptics.x*(1.f+.30f*rim*rim);
                }else if(membraneField.x!=0xffffffffu){
                    const MembraneOptics optics=ShadeMembrane(path,i.uv,i.view,false);
                    color=color*optics.transmission+optics.glow;
                }
            }
            if(distance>0.f){
                const float4 clip=mul(float4(origin+path.ray*distance,1.f),vp[i.view]);
                result.depth=saturate(clip.z/max(.0001f,clip.w));
            }
        }
    }
    if(sphereAtlas.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[sphereAtlas.z];
        color=BlendProjectile(color,marks.Load(int4(uint2(i.position.xy),i.view,0)));
    }
    result.color=float4(ACES(color),1.f);
    return result;
}

Present SphereProxyPixel(Out i){
    if(spherePair.x!=0u)return DrawSpherePair(i);
    return DrawSphereProxy(i);
}

Present SphereProxyInspect(Out i){
    Present result=SphereProxyPixel(i);
    if(param1!=0u)result.color.rgb=ApplySpherePerception(i,result.color.rgb);
    return result;
}
