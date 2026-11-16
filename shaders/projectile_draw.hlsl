// Staged R32_UINT marks avoid write races when projectile sprites overlap.
struct Projectile {float3 position;float age;float3 velocity;float radius;float3 acceleration;float lifetime;uint id;uint kind;uint alive;uint padding;};
cbuffer DrawParameters:register(b0){
    uint projectilesDescriptor,maskUav,maskSrv,colorUav;
    uint projectileCapacity,width,height,viewCount;
    row_major float4x4 viewProjection[4];
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
[numthreads(8,8,1)]
void CompositeProjectiles(uint3 id:SV_DispatchThreadID){
    if(id.x>=width||id.y>=height||id.z>=viewCount)return;
    Texture2DArray<uint> marks=ResourceDescriptorHeap[maskSrv];
    uint marker=marks.Load(int4(id,0));
    if(marker==0)return;
    RWTexture2DArray<float4> target=ResourceDescriptorHeap[colorUav];
    float4 original=target[id];
    float3 color=marker==1?float3(.35f,.8f,1.6f):float3(1.6f,.8f,.25f);
    target[id]=float4(original.rgb*.28f+color,original.a);
}
