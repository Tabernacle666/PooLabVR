#include "common.hlsli"
struct SnowParticle {float4 position,velocity;};
[numthreads(8,8,1)]
void SnowClear(uint3 id:SV_DispatchThreadID){
    RWTexture2DArray<uint> coverage=ResourceDescriptorHeap[objectIndex];
    uint w,h,views;coverage.GetDimensions(w,h,views);
    if(id.x<w&&id.y<h&&id.z<views)coverage[id]=0;
}
float SceneSnowDepth(uint2 pixel,uint view){
    if(topology.y>1){
        Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[param0];float d=0;
        [unroll]for(uint sample=0;sample<4;sample++)d=max(d,depth.Load(int3(pixel,view),sample));return d;
    }
    Texture2DArray<float> depth=ResourceDescriptorHeap[param0];return depth.Load(int4(pixel,view,0));
}
[numthreads(256,1,1)]
void SnowSplat(uint3 id:SV_DispatchThreadID){
    uint index=id.x,view=id.y;if(index>=param1||view>=ViewCount)return;
    StructuredBuffer<SnowParticle> states=ResourceDescriptorHeap[objectIndex];SnowParticle s=states[index];
    float3 p=s.position.xyz;
    p.x+=floor((eye[view].x-p.x+128.f)/256.f)*256.f;
    p.z+=floor((eye[view].z-p.z+128.f)/256.f)*256.f;
    // The 1950s lab is roofed: snowfall must not appear inside the room.
    // Roof-top flakes remain visible, with the roof/depth geometry taking over.
    if(abs(p.x)<4.2f&&abs(p.z)<4.2f&&p.y<4.05f)return;
    float3 delta=p-eye[view].xyz;
    if(abs(delta.x)>35.f||abs(delta.z)>35.f||dot(delta,delta)>1225.f)return;
    float4 clip=mul(float4(p,1),vp[view]);
    if(clip.w<=.05f||clip.z<0||clip.z>clip.w)return;
    float2 uv=clip.xy/clip.w*float2(.5f,-.5f)+.5f;
    if(any(uv<=0)||any(uv>=1))return;
    uint2 depthPixel=min(uint2(uv*float2(Width,Height)),uint2(Width-1,Height-1));
    // Reverse-Z: geometry with larger depth is closer than the flake.
    if(SceneSnowDepth(depthPixel,view)>clip.z/clip.w+.00025f)return;
    RWTexture2DArray<uint> coverage=ResourceDescriptorHeap[passIndex];
    uint w,h,views;coverage.GetDimensions(w,h,views);
    int2 pixel=int2(uv*float2(w,h));if(any(pixel<0)||any(pixel>=int2(w,h)))return;
    float distance=length(delta);
    uint strength=max(1u,(uint)(330.f*saturate((35.f-distance)/15.f)*lerp(.4f,1.f,s.position.w)));
    InterlockedAdd(coverage[uint3(pixel,view)],strength);
    if(distance<9.f&&pixel.x+1<int(w))
        InterlockedAdd(coverage[uint3(pixel+int2(1,0),view)],strength/3u);
}
struct SnowOut{float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
SnowOut SnowCompositeVS(uint index:SV_VertexID,uint view:SV_ViewID){
    SnowOut o;o.uv=float2((index<<1)&2,index&2);
    o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;
}
float4 SnowCompositePS(SnowOut i):SV_Target0{
    Texture2DArray<uint> coverage=ResourceDescriptorHeap[objectIndex];
    uint w,h,views;coverage.GetDimensions(w,h,views);
    float2 p=i.uv*float2(w,h)-.5f;int2 base=int2(floor(p));float2 f=frac(p);float coverageLevel=0;
    [unroll]for(int y=0;y<2;y++)[unroll]for(int x=0;x<2;x++){
        int2 q=clamp(base+int2(x,y),int2(0,0),int2(int(w)-1,int(h)-1));
        coverageLevel+=(x?f.x:1-f.x)*(y?f.y:1-f.y)*float(coverage.Load(int4(q,i.view,0)));
    }
    return float4(.67f,.76f,.91f,saturate(coverageLevel/850.f));
}
