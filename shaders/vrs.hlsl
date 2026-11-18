cbuffer VrsParameters : register(b0) {
    uint targetWidth, targetHeight, tileSize, imageDescriptor;
    float4 tanFov[2];
};

[numthreads(8, 8, 1)]
void BuildShadingRateImage(uint3 id : SV_DispatchThreadID) {
    uint2 targetSize = uint2(targetWidth, targetHeight);
    uint2 firstPixel = id.xy * tileSize;
    if (any(firstPixel >= targetSize)) return;

    float2 uvFirst = float2(firstPixel) / targetSize;
    float2 uvLast = float2(min(firstPixel + tileSize, targetSize)) / targetSize;
    float nearestSquared = 3.402823e38f;
    [unroll] for (uint eye = 0; eye < 2; ++eye) {
        float2 a = lerp(tanFov[eye].xy, tanFov[eye].zw, uvFirst);
        float2 b = lerp(tanFov[eye].xy, tanFov[eye].zw, uvLast);
        float2 nearest = max(0.0, max(min(a, b), -max(a, b)));
        nearestSquared = min(nearestSquared, dot(nearest, nearest));
    }

    static const uint rate1x1 = 0x0u;
    static const uint rate1x2 = 0x1u;
    static const uint rate2x2 = 0x5u;
    const float fineRadius = 0.60;
    const float transitionRadius = 0.82;
    RWTexture2D<uint> image = ResourceDescriptorHeap[imageDescriptor];
    image[id.xy] = nearestSquared <= fineRadius * fineRadius ? rate1x1 :
                   nearestSquared <= transitionRadius * transitionRadius ? rate1x2 : rate2x2;
}
