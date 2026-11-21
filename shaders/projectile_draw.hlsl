#include "projectile_overlay.hlsli"
// Staged R32_UINT marks avoid write races when projectile sprites overlap.
struct Projectile {float3 position;float age;float3 velocity;float radius;float3 acceleration;float lifetime;uint id;uint kind;uint alive;float visualRadius;};
static const uint ScoriaProjectileKind=2u;
static const uint LaserLineCount=8u;
cbuffer DrawParameters:register(b0){
    uint projectilesDescriptor,maskUav,maskSrv,colorUav;
    uint projectileCapacity,width,height,viewCount;
    row_major float4x4 viewProjection[4];
    float4 laserStart[LaserLineCount],laserEnd[LaserLineCount];
    uint tileSeenUav,tileActiveUav,tilePreviousUav,tileCountUav;
    uint tileArgsUav,tileGeneration,tileCountX,tileCountY;
    uint sceneDepth,depthWidth,depthHeight,depthSamples;
    float4 depthJitter;
    float4 viewScale; // scene units per metre, near clip in scene units
};
// One R32_UINT mark per pixel; exactly one list entry per touched 16x16 tile.
void RegisterMarkedTile(uint2 pixel,uint eye){
    uint tile=(eye*tileCountY+(pixel.y>>4))*tileCountX+(pixel.x>>4);
    RWByteAddressBuffer seen=ResourceDescriptorHeap[tileSeenUav];
    uint previous;
    seen.InterlockedExchange(tile*4u,tileGeneration,previous);
    if(previous!=tileGeneration){
        RWByteAddressBuffer count=ResourceDescriptorHeap[tileCountUav];
        uint slot;
        count.InterlockedAdd(0,1u,slot);
        RWByteAddressBuffer active=ResourceDescriptorHeap[tileActiveUav];
        active.Store(slot*4u,tile);
    }
}

void StampMark(float2 xy,uint view,uint marker){
    if(any(xy<0.f)||xy.x>=width||xy.y>=height)return;
    const uint2 pixel=uint2(xy);
    RWTexture2DArray<uint> marks=ResourceDescriptorHeap[maskUav];
    uint ignored;
    InterlockedMax(marks[uint3(pixel,view)],marker,ignored);
    RegisterMarkedTile(pixel,view);
}

[numthreads(8,8,1)]
void StampProjectiles(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
    if(group.x>=projectileCapacity||group.y>=viewCount)return;
    StructuredBuffer<Projectile> projectiles=ResourceDescriptorHeap[projectilesDescriptor];
    Projectile p=projectiles[group.x];
    if(p.alive==0u||((p.kind>>8u)&255u)!=uint(viewScale.z))return;
    float4 clip=mul(float4(p.position,1),viewProjection[group.y]);
    if(clip.w<=viewScale.y||clip.z<0||clip.z>clip.w)return;
    float2 center=(clip.xy/clip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
    const uint kind=p.kind&255u;
    if(kind==ScoriaProjectileKind){
        // Small projected sphere with a brighter upper core. The simulation's
        // cut radius is independent of this visual footprint.
        const float radius=clamp(p.visualRadius*float(height)/clip.w,3.5f,7.6f);
        [unroll]for(uint quadrant=0u;quadrant<4u;++quadrant){
            const float2 offset=float2(thread.xy+8u*uint2(quadrant&1u,quadrant>>1u))-7.5f;
            const float2 sample=offset/radius;
            if(dot(sample,sample)<=1.f){
                const float2 core=sample+float2(.18f,.22f);
                StampMark(center+offset,group.y,
                    dot(core,core)<.46f?ScoriaOrbCoreMark:ScoriaOrbRimMark);
            }
        }
    }else{
        const float2 offset=float2(thread.xy)-3.5f;
        if(dot(offset,offset)<=16.f)
            StampMark(center+offset,group.y,kind==0u?1u:2u);
    }
}
float2 BeamPixel(float4 clip){
    return (clip.xy/clip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
}
float BeamSceneDepth(float2 xy,uint view){
    if(sceneDepth==0xffffffffu)return 0.f;
    const float2 size=float2(depthWidth,depthHeight);
    const uint2 pixel=uint2(clamp(xy*size/float2(width,height)+depthJitter.xy,0.f,size-1.f));
    if(depthSamples>1u){
        Texture2DMSArray<float,4> texture=ResourceDescriptorHeap[sceneDepth];
        float z=0.f;
        [unroll]for(uint i=0u;i<4u;++i)z=max(z,texture.Load(int3(pixel,view),i));
        return z;
    }
    Texture2DArray<float> depth=ResourceDescriptorHeap[sceneDepth];
    return depth.Load(int4(pixel,view,0));
}
void StampBeam(float2 xy,float z,uint view,uint kind,float intensity){
    if(any(xy<0.f)||xy.x>=width||xy.y>=height||z<=0.f||z>1.f)return;
    if(z+1.e-5f<BeamSceneDepth(xy,view))return;
    const uint light=uint(round(255.f*saturate(intensity)));
    if(light)StampMark(xy,view,(kind<<8u)|light);
}
float DustRandom(uint value){
    value^=value>>16u;value*=0x7feb352du;value^=value>>15u;
    return float(value&4095u)/4095.f;
}
void StampLaserDust(uint index,uint view,float3 start,float3 end,float strength){
    const float3 direction=normalize(end-start);
    const float3 axis=normalize(cross(direction,abs(direction.y)<.9f?float3(0,1,0):float3(1,0,0)));
    const uint seed=asuint(start.x)^asuint(start.y)^asuint(start.z)^(index*0x9e3779b9u);
    const float t=(float(index)+.12f+.76f*DustRandom(seed))/96.f;
    const float3 scatter=(.085f*viewScale.x)*((DustRandom(seed+1u)-.5f)*axis+
                                (DustRandom(seed+2u)-.5f)*cross(direction,axis));
    const float4 clip=mul(float4(lerp(start,end,t)+scatter,1.f),viewProjection[view]);
    if(clip.w<viewScale.y*1.025f)return;
    const float2 xy=BeamPixel(clip);
    const float z=clip.z/clip.w;
    const float atSurface=1.f-smoothstep(.00004f,.0004f,abs(BeamSceneDepth(xy,view)-z));
    StampBeam(xy,z,view,1u,strength*(.25f+.32f*DustRandom(seed+3u)+.33f*atSurface));
}

// Depth-tested BASALT beam, trails and world-space particles reuse the
// existing sparse overlay in both native and DLSS output.
[numthreads(64,1,1)]
void StampAimLasers(uint3 id:SV_DispatchThreadID){
    const uint beamIndex=id.y,view=id.z;
    if(beamIndex>=LaserLineCount||view>=viewCount)return;
    const float4 beam=laserStart[beamIndex],end=laserEnd[beamIndex];
    const uint style=uint(beam.w);
    if(!style)return;
    if(style>=4u&&id.x<96u)
        StampLaserDust(id.x,view,beam.xyz,end.xyz,end.w*(style==4u?.94f:.56f));
    float4 a=mul(float4(beam.xyz,1),viewProjection[view]);
    float4 b=mul(float4(end.xyz,1),viewProjection[view]);
    const float nearDepth=viewScale.y*1.025f;
    if(max(a.w,b.w)<nearDepth)return;
    if(a.w<nearDepth)a=lerp(a,b,(nearDepth-a.w)/(b.w-a.w));
    if(b.w<nearDepth)b=lerp(b,a,(nearDepth-b.w)/(a.w-b.w));
    const float2 start=BeamPixel(a),finish=BeamPixel(b);
    const float2 delta=finish-start;
    float first=0.f,last=1.f;
    const float2 size=float2(width,height);
    for(uint axis=0u;axis<2u;++axis){
        if(abs(delta[axis])<1.e-5f){
            if(start[axis]<-2.f||start[axis]>size[axis]+2.f)return;
            continue;
        }
        const float low=(-2.f-start[axis])/delta[axis];
        const float high=(size[axis]+2.f-start[axis])/delta[axis];
        first=max(first,min(low,high));
        last=min(last,max(low,high));
    }
    if(first>last)return;
    const float span=length(delta)*(last-first);
    const uint count=max(1u,uint(min(ceil(span),float(width+height))));
    if(id.x>=count)return;
    const float t=lerp(first,last,(float(id.x)+.5f)/float(count));
    const float2 xy=start+delta*t;
    if(style<3u){
        const uint mark=style==2u?ScoriaGuideMark:(beamIndex==0u?3u:4u);
        StampMark(xy,view,mark);
        return;
    }
    const float z=lerp(a.z/a.w,b.z/b.w,t);
    if(style==3u){
        StampBeam(xy,z,view,2u,.32f);
    }else if(style==4u){
        const float2 normal=length(delta)>.1f?normalize(float2(-delta.y,delta.x)):float2(0,1);
        StampBeam(xy-normal,z,view,3u,.54f);
        StampBeam(xy+normal,z,view,3u,.54f);
        StampBeam(xy,z,view,4u,1.f);
    }else StampBeam(xy,z,view,2u,.42f*end.w);
}
// The previous list was produced by the last overlay frame, not the last XR frame.
[numthreads(16,16,1)]
void ClearMarkedTiles(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
    RWByteAddressBuffer previousTiles=ResourceDescriptorHeap[tilePreviousUav];
    uint tile=previousTiles.Load(group.x*4u);
    uint tilesPerEye=tileCountX*tileCountY;
    uint eye=tile/tilesPerEye;
    uint local=tile%tilesPerEye;
    uint2 pixel=uint2((local%tileCountX)*16u+thread.x,
                      (local/tileCountX)*16u+thread.y);
    if(eye>=viewCount||pixel.x>=width||pixel.y>=height)return;
    RWTexture2DArray<uint> marks=ResourceDescriptorHeap[maskUav];
    marks[uint3(pixel,eye)]=0u;
}
// Build exactly one D3D12_DISPATCH_ARGUMENTS directly on the GPU.
[numthreads(1,1,1)]
void BuildTileDispatchArgs(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer count=ResourceDescriptorHeap[tileCountUav];
    RWByteAddressBuffer args=ResourceDescriptorHeap[tileArgsUav];
    uint offset=(tileGeneration&1u)*12u;
    args.Store(offset,count.Load(0));
    args.Store(offset+4u,1u);
    args.Store(offset+8u,1u);
}
[numthreads(16,16,1)]
void CompositeProjectiles(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
    RWByteAddressBuffer tiles=ResourceDescriptorHeap[tileActiveUav];
    uint tile=tiles.Load(group.x*4u);
    uint tilesPerEye=tileCountX*tileCountY;
    uint eye=tile/tilesPerEye;
    uint local=tile%tilesPerEye;
    uint2 pixel=uint2((local%tileCountX)*16u+thread.x,
                      (local/tileCountX)*16u+thread.y);
    if(eye>=viewCount||pixel.x>=width||pixel.y>=height)return;
    uint3 id=uint3(pixel,eye);
    Texture2DArray<uint> marks=ResourceDescriptorHeap[maskSrv];
    uint marker=marks.Load(int4(id,0));
    if(marker==0)return;
    RWTexture2DArray<float4> target=ResourceDescriptorHeap[colorUav];
    float4 original=target[id];
    target[id]=float4(BlendProjectile(original.rgb,marker),original.a);
}
