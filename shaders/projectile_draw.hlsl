#include "projectile_overlay.hlsli"
// Staged R32_UINT marks avoid write races when projectile sprites overlap.
struct Projectile {float3 position;float age;float3 velocity;float radius;float3 acceleration;float lifetime;uint id;uint kind;uint alive;uint padding;};
static const uint BasaltProjectileKind=2u;
cbuffer DrawParameters:register(b0){
    uint projectilesDescriptor,maskUav,maskSrv,colorUav;
    uint projectileCapacity,width,height,viewCount;
    row_major float4x4 viewProjection[4];
    float4 laserStart[2],laserEnd[2];
    uint tileSeenUav,tileActiveUav,tilePreviousUav,tileCountUav;
    uint tileArgsUav,tileGeneration,tileCountX,tileCountY;
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
    Projectile p=projectiles[group.x];if(p.alive==0)return;
    float4 clip=mul(float4(p.position,1),viewProjection[group.y]);
    if(clip.w<=.01f||clip.z<0||clip.z>clip.w)return;
    float2 center=(clip.xy/clip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
    if(p.kind==BasaltProjectileKind){
        // Small projected sphere with a brighter upper core. The simulation's
        // cut radius is independent of this visual footprint.
        const float radius=clamp(.008f*float(height)/clip.w,3.5f,7.6f);
        [unroll]for(uint quadrant=0u;quadrant<4u;++quadrant){
            const float2 offset=float2(thread.xy+8u*uint2(quadrant&1u,quadrant>>1u))-7.5f;
            const float2 sample=offset/radius;
            if(dot(sample,sample)<=1.f){
                const float2 core=sample+float2(.18f,.22f);
                StampMark(center+offset,group.y,
                    dot(core,core)<.46f?BasaltOrbCoreMark:BasaltOrbRimMark);
            }
        }
    }else{
        const float2 offset=float2(thread.xy)-3.5f;
        if(dot(offset,offset)<=16.f)
            StampMark(center+offset,group.y,p.kind==0?1u:2u);
    }
}
// Thin controller-space aiming lines, stamped into the existing projectile mask.
[numthreads(64,1,1)]
void StampAimLasers(uint3 id:SV_DispatchThreadID){
    uint hand=id.y,view=id.z;
    if(hand>=2||view>=viewCount||laserStart[hand].w==0)return;
    float4 nearClip=mul(float4(laserStart[hand].xyz,1),viewProjection[view]);
    float4 farClip=mul(float4(laserEnd[hand].xyz,1),viewProjection[view]);
    if(nearClip.w<=.01f||farClip.w<=.01f)return;
    float2 start=(nearClip.xy/nearClip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
    float2 finish=(farClip.xy/farClip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
    float2 displacement=finish-start;
    float span=length(displacement);
    const bool basalt=laserStart[hand].w==2.f;
    if(span<1.f&&!basalt)return;
    uint count=max(1u,uint(min(ceil(span),float(max(width,height)))));
    if(id.x>=count)return;
    float2 xy=start+displacement*((float(id.x)+.5f)/float(count));
    StampMark(xy,view,basalt?BasaltLaserMark:(hand==0?3u:4u));
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
