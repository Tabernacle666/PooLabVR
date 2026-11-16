// Staged R32_UINT marks avoid write races when projectile sprites overlap.
struct Projectile {float3 position;float age;float3 velocity;float radius;float3 acceleration;float lifetime;uint id;uint kind;uint alive;uint padding;};
cbuffer DrawParameters:register(b0){
    uint projectilesDescriptor,maskUav,maskSrv,colorUav;
    uint projectileCapacity,width,height,viewCount;
    row_major float4x4 viewProjection[4];
    float4 laserStart[2],laserEnd[2];
};
[numthreads(8,8,1)]
void StampProjectiles(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID){
    if(group.x>=projectileCapacity||group.y>=viewCount)return;
    StructuredBuffer<Projectile> projectiles=ResourceDescriptorHeap[projectilesDescriptor];
    Projectile p=projectiles[group.x];if(p.alive==0)return;
    float4 clip=mul(float4(p.position,1),viewProjection[group.y]);
    if(clip.w<=.01f||clip.z<0||clip.z>clip.w)return;
    float2 center=(clip.xy/clip.w*float2(.5f,-.5f)+.5f)*float2(width,height);
    float2 offset=float2(thread.xy)-3.5f;
    float2 xy=center+offset;
    if(dot(offset,offset)>16.f||any(xy<0)||xy.x>=width||xy.y>=height)return;
    RWTexture2DArray<uint> marks=ResourceDescriptorHeap[maskUav];
    uint ignored;
    InterlockedMax(marks[uint3(uint2(xy),group.y)],p.kind==0?1u:2u,ignored);
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
    if(span<1.f)return;
    uint count=min(uint(ceil(span)),max(width,height));
    if(id.x>=count)return;
    float2 xy=start+displacement*((float(id.x)+.5f)/float(count));
    if(any(xy<0.f)||xy.x>=width||xy.y>=height)return;
    uint2 pixel=uint2(xy);
    RWTexture2DArray<uint> marks=ResourceDescriptorHeap[maskUav];
    uint ignored;
    InterlockedMax(marks[uint3(pixel,view)],hand==0?3u:4u,ignored);
}
[numthreads(8,8,1)]
void CompositeProjectiles(uint3 id:SV_DispatchThreadID){
    if(id.x>=width||id.y>=height||id.z>=viewCount)return;
    Texture2DArray<uint> marks=ResourceDescriptorHeap[maskSrv];
    uint marker=marks.Load(int4(id,0));
    if(marker==0)return;
    RWTexture2DArray<float4> target=ResourceDescriptorHeap[colorUav];
    float4 original=target[id];
    float3 color=(marker==1||marker==3)?float3(.35f,.8f,1.6f):float3(1.6f,.8f,.25f);
    float baseScale=marker>=3?.55f:.28f;
    target[id]=float4(original.rgb*baseScale+color*(marker>=3?.70f:1.f),original.a);
}
