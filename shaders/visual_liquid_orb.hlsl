#include "visual_common.hlsli"
float Hash31(float3 p)
{
    p = frac(p * 0.1031f);
    p += dot(p, p.yzx + 33.33f);
    return frac((p.x + p.y) * p.z);
}

float ValueNoise(float3 p)
{
    float3 cell = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float n000 = Hash31(cell + float3(0, 0, 0));
    float n100 = Hash31(cell + float3(1, 0, 0));
    float n010 = Hash31(cell + float3(0, 1, 0));
    float n110 = Hash31(cell + float3(1, 1, 0));
    float n001 = Hash31(cell + float3(0, 0, 1));
    float n101 = Hash31(cell + float3(1, 0, 1));
    float n011 = Hash31(cell + float3(0, 1, 1));
    float n111 = Hash31(cell + float3(1, 1, 1));
    float x00 = lerp(n000, n100, f.x);
    float x10 = lerp(n010, n110, f.x);
    float x01 = lerp(n001, n101, f.x);
    float x11 = lerp(n011, n111, f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

float Fbm(float3 p)
{
    float total = 0.0f;
    float weight = 0.55f;
    float norm = 0.0f;
    [unroll]
    for (int octave = 0; octave < 4; ++octave)
    {
        total += ValueNoise(p) * weight;
        norm += weight;
        p = p * 2.03f + float3(7.7f, 3.3f, 9.2f);
        weight *= 0.50f;
    }
    return total / norm;
}

float3 LiquidColour(float value, float feather)
{
    // Target distribution: 20% red, 30% orange, 40% yellow, 10% white.
    const float3 red = float3(0.93f, 0.055f, 0.010f);
    const float3 orange = float3(1.00f, 0.30f, 0.012f);
    const float3 yellow = float3(1.00f, 0.82f, 0.035f);
    const float3 white = float3(1.00f, 0.98f, 0.86f);
    float3 colour = red;
    colour = lerp(colour, orange, smoothstep(0.20f - feather, 0.20f + feather, value));
    colour = lerp(colour, yellow, smoothstep(0.50f - feather, 0.50f + feather, value));
    colour = lerp(colour, white, smoothstep(0.90f - feather, 0.90f + feather, value));
    return colour;
}

float LiquidField0(float3 normal, float phase)
{
    float3 driftA = float3(phase * 0.41f, -phase * 0.30f, phase * 0.35f);
    float3 driftB = float3(-phase * 0.27f, phase * 0.38f, phase * 0.21f);
    float circulation = phase * 0.18f;
    float sineTurn = sin(circulation);
    float cosineTurn = cos(circulation);
    float3 flowingNormal = float3(normal.x * cosineTurn - normal.z * sineTurn, normal.y,
        normal.x * sineTurn + normal.z * cosineTurn);
    float3 basePoint = flowingNormal * VisualDraw().effect0.z;
    float warpA = Fbm(basePoint * 0.82f + driftA);
    float warpB = Fbm(basePoint * 0.67f + driftB + float3(8.0f, 3.0f, 5.0f));
    float3 warped = basePoint + (float3(warpA, warpB, warpA - warpB) - 0.5f) * VisualDraw().effect0.w;
    float liquidA = Fbm(warped + driftA * 0.75f);
    float liquidB = Fbm(warped * 0.52f - driftB * 0.66f + float3(12.0f, 5.0f, 1.0f));
    return frac(liquidA * 1.37f + liquidB * 0.91f + 0.17f);
}

float LiquidField1(float3 normal, float phase)
{
    float3 basePoint = normal * VisualDraw().effect0.z * 1.15f;
    float3 advect = float3(-phase * 0.46f, phase * 0.34f, phase * 0.39f);
    float3 swirl = float3(
        Fbm(basePoint * 0.85f + advect + float3(2.0f, 7.0f, 3.0f)),
        Fbm(basePoint * 0.91f - advect + float3(9.0f, 1.0f, 4.0f)),
        Fbm(basePoint * 0.74f + advect * 1.2f + float3(5.0f, 8.0f, 6.0f)));
    float3 warped = basePoint + (swirl - 0.5f) * (VisualDraw().effect0.w * 1.35f + 0.15f);
    float ridgeA = abs(Fbm(warped * 1.55f + advect * 1.5f) * 2.0f - 1.0f);
    float ridgeB = abs(Fbm(warped * 0.77f - advect * 0.9f + float3(11.0f, 2.0f, 13.0f)) * 2.0f - 1.0f);
    return saturate(1.0f - ridgeA * 0.72f - ridgeB * 0.38f);
}

float LiquidField2(float3 normal, float phase)
{
    float3 basePoint = normal * VisualDraw().effect0.z * 1.35f;
    float3 advect = float3(phase * 0.52f, -phase * 0.24f, phase * 0.33f);
    float low = Fbm(basePoint * 0.48f + advect);
    float high = Fbm(basePoint * 1.75f - advect * 1.3f + float3(10.0f, 4.0f, 8.0f));
    float curlish = Fbm((basePoint + float3(high, low, high - low) * 1.4f) * 0.96f + advect * 0.65f + float3(3.0f, 12.0f, 7.0f));
    float bands = 0.5f + 0.5f * sin((low * 4.6f + high * 2.7f + phase * 0.7f) * 6.283185f);
    return saturate(0.24f + curlish * 0.44f + bands * 0.32f);
}

float4 VisualOrbPS(VisualPixel input) : SV_TARGET
{
    VisualDrawData d=VisualDraw();float3 SunCenter=d.positionEmission.xyz;float Time=d.effect0.x,BlendSoftness=d.effect1.x,Luminosity=d.effect1.y,Alpha=d.effect1.z;int LiquidMode=(int)d.effect1.w;
    float3 normal = normalize(input.world - SunCenter);
    // Time is an already-integrated liquid phase. CPU integration lets END fast-forward while held
    // and restore exactly SUN LIQUID SPEED on release, without rescaling elapsed history.
    float phase = Time;
    float liquid = 0.0f;
    if (LiquidMode == 1) liquid = LiquidField1(normal, phase);
    else if (LiquidMode == 2) liquid = LiquidField2(normal, phase);
    else liquid = LiquidField0(normal, phase);
    float feather = max(BlendSoftness, 0.000001f);
    float3 colour = LiquidColour(liquid, feather);
    float glowPulse = 0.95f + 0.05f * sin(phase * 1.8f + liquid * 6.283185f);
    colour *= Luminosity * glowPulse;
    return float4(saturate(colour), saturate(Alpha * input.color.a));
}


VisualPixel VisualOrbVS(VisualMeshIn i,uint view:SV_ViewID){return VisualProject(VisualWorld(i.position,VisualDraw()),float4(1,1,1,1),view);}
