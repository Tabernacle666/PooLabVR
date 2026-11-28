#include "common.hlsli"
#include "debris_layout.hlsli"
#include "projectile_overlay.hlsli"
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

float3 ViewRay(float2 uv,uint view){
    return normalize(Unproject(uv,.0001f,view)-eye[view].xyz);
}

bool TraceSphereRay(float2 uv,float depth,uint view,float4 sphere,float3 ray,out SpherePath path){
    path=(SpherePath)0;
    const float3 origin=eye[view].xyz;
    path.ray=ray;
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
bool TraceSphere(float2 uv,float depth,uint view,float4 sphere,out SpherePath path){
    return TraceSphereRay(uv,depth,view,sphere,ViewRay(uv,view),path);
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
        const float h=worldY-atmosphereSphere.y+atmosphereSphere.w+29.1262136f;
        a*=abs(asfloat(a.w)-h)<1.89320388f?1u:0u;
        b*=abs(asfloat(b.w)-h)<1.89320388f?1u:0u;
        c*=abs(asfloat(c.w)-h)<1.89320388f?1u:0u;
        d*=abs(asfloat(d.w)-h)<1.89320388f?1u:0u;
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
    const bool seeBack=sphereScene.z==0u&&path.exit<path.sceneDistance&&back.y<0.f;
    const bool seeFront=sphereScene.z==0u&&path.entry>0.f&&path.entry<path.sceneDistance&&front.y<0.f;
    StructuredBuffer<uint4> floorMap=ResourceDescriptorHeap[debrisBindings.y];
    const uint occupied=floorMap[0].x;
    [branch]if(occupied==0u)return color;
    if(seeBack&&(occupied&1u)!=0u)color=ShadeDebrisFloor(color,floorMap,origin,path,path.exit,false);
    if((occupied&2u)!=0u&&path.sceneDistance>=max(0.f,path.entry)&&path.sceneDistance<path.exit){
        const float3 at=origin+path.ray*path.sceneDistance;
        if(sphereScene.z!=0u&&any(abs(at.xz)>atmosphereSphere.w))return color;
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
        float illumination=emission.w*AtmosphereShadow(samplePosition,light,shadowSlot)*phase/(848.336316f+distanceSquared);
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

float3 LoadMergedSceneColor(Texture2DArray<float4> color,Texture2DArray<float4> reflection,
                            uint2 pixel,uint view){
    const int4 address=int4(pixel,view,0);
    return f16tof32(f32tof16(color.Load(address).rgb+reflection.Load(address).rgb));
}

float3 SampleMergedSceneColor(Texture2DArray<float4> color,Texture2DArray<float4> reflection,
                              float2 pixel,uint view){
    const float2 texel=pixel-.5f;
    const uint2 low=uint2(floor(texel));
    const uint2 high=min(low+1u,uint2(Width-1u,Height-1u));
    const float2 weight=frac(texel);
    const float3 top=lerp(LoadMergedSceneColor(color,reflection,low,view),
                         LoadMergedSceneColor(color,reflection,uint2(high.x,low.y),view),weight.x);
    const float3 bottom=lerp(LoadMergedSceneColor(color,reflection,uint2(low.x,high.y),view),
                            LoadMergedSceneColor(color,reflection,high,view),weight.x);
    return lerp(top,bottom,weight.y);
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
        [branch]if(param1!=0u){
            Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
            [branch]if(bent)color=SampleMergedSceneColor(resolved,reflection,samplePixel,view);
            else color=LoadMergedSceneColor(resolved,reflection,xy,view);
        }else if(bent)color=resolved.SampleLevel(linearClamp,float3(samplePixel/float2(outputDimensions.xy),view),0.f).rgb;
        else color=resolved.Load(int4(xy,view,0)).rgb;
#endif
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
float3 CosmicBackground(float3 ray){
    if(solarSun.w>0.f)ray=Rotate(ray,solarOrientation);
    const float2 oct=ray.xy/(abs(ray.x)+abs(ray.y)+abs(ray.z));
    const float2 mapped=ray.z>=0.f?oct:(1.f-abs(oct.yx))*sign(oct);
    const float2 grid=(mapped*.5f+.5f)*192.f;
    const uint2 cell=uint2(clamp(floor(grid),0.f,191.f));
    uint seed=cell.x*1597334677u^cell.y*3812015801u;
    seed=(seed^(seed>>16u))*2246822519u;
    seed=(seed^(seed>>13u))*3266489917u;
    if((seed&63u)>1u)return float3(.008f,.013f,.021f);
    const float2 center=.25f+.5f*float2((seed>>8u)&255u,(seed>>16u)&255u)/255.f;
    const float star=1.f-smoothstep(.035f,.115f,length(frac(grid)-center));
    const float strength=.06f+float(seed>>24u)*(.24f/255.f);
    return float3(.008f,.013f,.021f)+float3(.77f,.87f,1.f)*(star*strength);
}
void CompositeSun(Out i,float3 ray,inout float3 color,inout float distance,inout float depth){
    [branch]if(solarSun.w<=0.f)return;
    const float3 origin=eye[i.view].xyz;
    const float3 delta=solarSun.xyz-origin;
    const float along=dot(delta,ray);
    const float radial=length(delta-along*ray)/solarSun.w;
    if((along<=0.f&&dot(delta,delta)>=solarSun.w*solarSun.w)||radial>=2.5f)return;
    const float3 glow=float3(1.f,.59f,.27f);
    if(along<distance)color+=glow*(.20f*exp2(-2.3f*radial*radial)*(1.f-smoothstep(1.f,2.5f,radial)));
    if(radial>=1.f)return;
    const float halfChord=solarSun.w*sqrt(max(0.f,1.f-radial*radial));
    const float near=along-halfChord,far=along+halfChord;
    const float hit=near>0.f?near:far;
    if(hit<=0.f||hit>=distance)return;
    const float limb=saturate(halfChord/solarSun.w);
    color=lerp(float3(3.8f,1.56f,.40f),float3(7.2f,4.5f,2.2f),limb);
    distance=hit;
    const float4 clip=mul(float4(origin+hit*ray,1.f),vp[i.view]);
    depth=saturate(clip.z/max(.0001f,clip.w));
}
float3 PairMarks(Out i,float3 color){
    [branch]if(projectileOverlay.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[projectileOverlay.z];
        return BlendProjectile(color,marks.Load(int4(min(uint2(i.uv*float2(outputDimensions.xy)),outputDimensions.xy-1u),i.view,0)));
    }
    return color;
}

#include "target_hud.hlsli"

// Stable world-space ecology for the flat, forested extension. The authored
// atlas remains authoritative wherever its original habitat has coverage.
uint ExtensionFogHash(uint n){
    n^=n>>16u;n*=0x7feb352du;n^=n>>15u;n*=0x846ca68bu;n^=n>>16u;return n;
}
float ExtensionFogNoise(float2 p,uint salt){
    const int2 cell=int2(floor(p));
    float2 a=frac(p);a=a*a*(3.f-2.f*a);
    const uint2 q=asuint(cell);
    const uint x=q.x*0x9e3779b9u,z=q.y*0x85ebca6bu;
    const uint xx=(q.x+1u)*0x9e3779b9u,zz=(q.y+1u)*0x85ebca6bu;
    const float scale=1.f/16777215.f;
    return lerp(lerp(float(ExtensionFogHash(x^z^salt)&0xffffffu),
                     float(ExtensionFogHash(xx^z^salt)&0xffffffu),a.x),
                lerp(float(ExtensionFogHash(x^zz^salt)&0xffffffu),
                     float(ExtensionFogHash(xx^zz^salt)&0xffffffu),a.x),a.y)*scale;
}
struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present Tonemap(Out i){
    // SV_Position is in the full swapchain; UV is local to the cropped scene.
    float2 pixel=i.uv*float2(outputDimensions.xy);
    float2 source=PresentSourcePixel(pixel);
    uint2 xy=PresentSourceCoord(source);
    const float z=PresentSceneDepth(xy,i.view);
    float2 uv=i.uv;
#ifdef OASIS_DLSS_FRAME
    uv+=dlssJitter.xy/float2(Width,Height);
#endif
    const bool dense=spherePair.x==2u;
    const bool fixedWorld=sphereScene.z!=0u;
    const bool stellarSky=dense||fixedWorld;
    const bool drawMembrane=membraneField.x!=0xffffffffu;
    SpherePath path=(SpherePath)0;
    bool hasPath=false,bent=false;
    MembraneOptics optics=(MembraneOptics)0;
    optics.transmission=1.f;
    [branch]if(!fixedWorld&&(!dense||sphereScene.x!=0u)&&atmosphereSphere.w>0.f&&
               (drawMembrane||atmosphereColor.w>0.f||debrisBindings.y!=0xffffffffu)){
        hasPath=TraceAtmosphereSphere(uv,z,i.view,path);
        [branch]if(hasPath&&drawMembrane){
            optics=ShadeMembrane(path,uv,i.view,true);
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
    float3 fieldRay=0.f;
    if(stellarSky)fieldRay=hasPath?path.ray:ViewRay(uv,i.view);
    const bool sky=stellarSky&&(bent?PresentSceneDepth(xy,i.view)==0.f:z==0.f);
    float3 color=sky?CosmicBackground(fieldRay):PresentSceneColor(pixel,source,xy,i.view,bent);
    const float4 passage=membranePassagePulses[0];
    // After crossing, the local surface is behind the view rays. Keep its
    // refraction beside the eyes until the short passage veil has faded.
    [branch]if(passage.w>=1.f&&passage.w<1.18f)
        color=ApplyPassageRefraction(color,pixel,uv,i.view,passage);
    float depth=z;
    float distance=(spherePair.x!=0u||fixedWorld)&&z>0.f?length(Unproject(uv,z,i.view)-eye[i.view].xyz):1.e20f;
    if(stellarSky){
        CompositeSun(i,fieldRay,color,distance,depth);
        // Full native geometry contributes true scene depth and shading;
        // translucent sky objects are blended on top of its actual raster.
        [branch]if(sky&&nativeSky.y>0.f){
            Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
            float4 layer=resolved.Load(int4(xy,i.view,0));
#ifndef OASIS_DLSS_FRAME
            [branch]if(param1!=0u){
                Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
                layer.rgb=LoadMergedSceneColor(resolved,reflection,xy,i.view);
            }
#endif
            if(layer.a>0.f)color=color*(1.f-saturate(layer.a))+layer.rgb;
        }
    }
    [branch]if(hasPath){
        color=ApplyDebrisFloor(color,path,i.view);
        color=ApplyAtmosphere(color,path,i.view,false);
    }else if(fixedWorld&&z>0.f&&debrisBindings.y!=0xffffffffu){
        // Terrain deposits survive; no ray intersection with a shell.
        path.ray=fieldRay;path.entry=0.f;path.exit=1.e20f;
        path.sceneDistance=length(Unproject(uv,z,i.view)-eye[i.view].xyz);
        color=ApplyDebrisFloor(color,path,i.view);
    }
    // Ecological height haze, three world-space samples in the existing composite.
    [branch]if(fixedWorld&&forestAtmosphere.x>0.f&&z>0.f){
        const float units=1.f;
        const float3 origin=eye[i.view].xyz;
        const float3 end=Unproject(uv,z,i.view);
        const float3 segment=(end-origin)/units;
        const float segmentMeters=length(segment);
        const float3 direction=segment/max(segmentMeters,.00001f);
        const bool extended=forestFogExtension.w>0.f;
        float startMeters=0.f,endMeters=min(segmentMeters,110.f);
        if(extended){
            // Sample the actual near-ground height band, even for a distant
            // aerial eye. Integrate the finite visible ray, not the first
            // 110 m of empty air in front of the headset.
            const float lower=min(forestFogStyle.z,forestFogExtension.z);
            const float upper=max(forestFogStyle.z+forestFogStyle.w,forestFogExtension.z)+16.f;
            endMeters=segmentMeters;
            if(abs(direction.y)>1.e-6f){
                const float a=(lower-origin.y)/direction.y,b=(upper-origin.y)/direction.y;
                startMeters=max(0.f,min(a,b));
                endMeters=min(endMeters,max(a,b));
            }else if(origin.y<lower||origin.y>upper)endMeters=0.f;
        }
        const float rayMeters=max(0.f,endMeters-startMeters);
        const float daylight=saturate(normalize(solarSun.xyz-origin).y*2.f);
        const float contrast=forestFogStyle.y;
        float density=0.f;
        [unroll]for(uint j=0u;j<3u;++j){
            const float d=startMeters+rayMeters*(float(j)+.5f)/3.f;
            const float3 p=origin+direction*(d*units);
            float ground=forestAtmosphere.w;
            float horizontal=1.f;
            // Same 10 m cell-center eligibility as TerrainExpansion.
            const float2 tileCenter=(floor(p.xz/10.f)+.5f)*10.f;
            const bool extensionCell=extended&&length(tileCenter-forestFogExtension.xy)<=forestFogExtension.w;
            if(extended){
                ground=forestFogExtension.z;
                horizontal=0.f;
                if(extensionCell){
                    const float broad=ExtensionFogNoise(p.xz/float2(18.f,12.f),0x41544d31u);
                    const float fine=ExtensionFogNoise(p.xz/float2(5.f,8.f),0x464f4732u);
                    // Flat extension has no authored waterway. Preserve the
                    // atlas's broad/fine habitat scales and forest retention.
                    const float retention=.12f+.34f*broad+.16f*fine+.38f*.65f;
                    const float exposure=(1.f-.65f)*(.55f+.45f*broad);
                    horizontal=lerp(1.f,(.35f+1.45f*retention)*(1.f-.42f*exposure),contrast);
                }
            }
            [branch]if(forestFogBinding.x!=0xffffffffu){
                Texture2D<float4> field=ResourceDescriptorHeap[forestFogBinding.x];
                SamplerState linearClamp=SamplerDescriptorHeap[2];
                const float2 fieldUv=(p.xz-forestFogBounds.xy)*forestFogBounds.zw;
                const float4 habitat=field.SampleLevel(linearClamp,saturate(fieldUv),0.f);
                const float authoredGround=forestFogStyle.z+habitat.a*forestFogStyle.w;
                const float edge=saturate(1.f-512.f*max(max(-fieldUv.x,-fieldUv.y),max(fieldUv.x-1.f,fieldUv.y-1.f)));
                // Independent ecological inputs for later weather control:
                // static retention, river humidity and open-canopy exposure.
                const float retention=habitat.r;
                const float humidity=habitat.g;
                const float exposure=habitat.b;
                const float local=(.35f+1.45f*retention+.55f*humidity)*(1.f-.42f*exposure);
                const float authored=lerp(1.f,local,contrast)*edge;
                if(extensionCell){
                    // The old atlas's R channel already includes land
                    // coverage. Fade to extension ecology over its shoreline
                    // rather than fading the whole forest atmosphere to zero.
                    const float weight=smoothstep(0.f,.12f,habitat.r)*edge;
                    ground=lerp(ground,authoredGround,weight);
                    horizontal=lerp(horizontal,authored,weight);
                }else{
                    ground=authoredGround;
                    horizontal=authored;
                }
            }
            const float h=max(0.f,(p.y-ground)/units);
            // Continuous near-ground shoulder, then a long declining tail.
            const float vertical=rcp(1.f+.30f*h*h)*(1.f-smoothstep(9.f,16.f,h));
            density+=vertical*horizontal;
        }
        const float opacity=min(.58f,1.f-exp(-forestAtmosphere.x*.064f*rayMeters*density/3.f));
        const float tone=forestFogStyle.x;
        const float grey=lerp(lerp(.018f,.075f,tone),lerp(.13f,.68f,tone),daylight);
        color=lerp(color,grey.xxx,opacity);
    }
    color=color*optics.transmission+optics.glow;
    if(spherePair.x!=0u)color=PairMarks(i,color);
    Present o;
    float3 displayColor=ACES(color);
    [branch]if(targetOrigin.w>0.f)displayColor=TargetOverlay(displayColor,i.uv,uv,z,i.view);
    o.color=float4(displayColor,1.f);
    o.depth=depth;
    return o;
}
