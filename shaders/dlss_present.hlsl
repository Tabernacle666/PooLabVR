#define OASIS_DLSS_FRAME
#include "present.hlsl"

Present TonemapDlss(Out i){
    const uint2 extent=uint2(Width,Height);
    const float2 activePixel=i.position.xy*float2(extent)/float2(outputDimensions.xy)
        +dlssJitter.xy;
    const uint2 xy=uint2(clamp(activePixel,0.0,float2(extent)-1.0));
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    const float z=depth.Load(int4(xy,i.view,0));

    float3 color;
    [branch]if(param2!=0u&&param2!=0xffffffffu){
        Texture2D<float4> upscaled=ResourceDescriptorHeap[NonUniformResourceIndex(dlssOutput[i.view])];
        color=upscaled.Load(int3(uint2(i.position.xy),0)).rgb;
    }else{
        Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
        SamplerState linearSampler=SamplerDescriptorHeap[2];
        float2 samplePixel=clamp(activePixel,.5f,float2(extent)-.5f);
        color=resolved.SampleLevel(linearSampler,
            float3(samplePixel/float2(outputDimensions.xy),i.view),0.f).rgb;
        if(topology.z!=0&&param1==0){
            Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
            color+=reflection.Load(int4(xy,i.view,0)).rgb;
        }
    }

    [branch]if(dlssOutput.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[dlssOutput.z];
        const uint mark=marks.Load(int4(uint2(i.position.xy),i.view,0));
        color=BlendProjectile(color,mark);
    }
    return FinishPresent(i,color,i.uv+dlssJitter.xy/float2(extent),z);
}
