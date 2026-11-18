#define OASIS_DLSS_FRAME
#include "common.hlsli"

[numthreads(8,8,1)]
void PrepareDlssInputs(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height)return;
    const uint view=objectIndex;
    Texture2DArray<float4> src=ResourceDescriptorHeap[ColorResolved];
    Texture2DArray<float> srcDepth=ResourceDescriptorHeap[DepthMS];
    RWTexture2D<float4> color=ResourceDescriptorHeap[passIndex];
    RWTexture2D<float> depth=ResourceDescriptorHeap[param0];
    RWTexture2D<float2> motion=ResourceDescriptorHeap[param1];

    const int4 sampleIndex=int4(id.xy,view,0);
    const float z=srcDepth.Load(sampleIndex);
    color[id.xy]=src.Load(sampleIndex);
    depth[id.xy]=z;
    float2 pixelMotion=0.f;
    if(z>0.f){
        const float2 extent=float2(Width,Height);
        const float2 pixel=float2(id.xy)+0.5f;
        const float3 world=Unproject(pixel/extent,z,view);
        const float4 previous=mul(float4(world,1),prevDlssVP[view]);
        if(previous.w>1e-5f){
            const float2 previousPixel=(previous.xy/previous.w*float2(.5f,-.5f)+.5f)*extent;
            pixelMotion=clamp(previousPixel-pixel+dlssJitter.xy,-2.f*extent,2.f*extent);
        }
    }
    motion[id.xy]=pixelMotion;
}
