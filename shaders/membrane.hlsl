#include "common.hlsli"

float3 DecodeMembraneNormal(float2 p){
    float3 n=float3(p,1.f-abs(p.x)-abs(p.y));
    const float fold=max(-n.z,0.f);
    n.xy+=float2(n.x>=0.f?-fold:fold,n.y>=0.f?-fold:fold);
    return normalize(n);
}

float3 MembraneDirection(uint2 texel,uint size){
    float2 p=(float2(texel)-.5f)*(2.f/float(size-2u))-1.f;
    // The padded texels are reflected across the octahedral seam.
    if(abs(p.x)>1.f)p=float2(sign(p.x)*(2.f-abs(p.x)),-p.y);
    if(abs(p.y)>1.f)p=float2(-p.x,sign(p.y)*(2.f-abs(p.y)));
    return DecodeMembraneNormal(p);
}

[numthreads(8,8,1)]
void BuildMembraneField(uint3 id : SV_DispatchThreadID){
    const uint size=membraneField.y;
    if(any(id.xy>=size))return;
    StructuredBuffer<uint4> events=ResourceDescriptorHeap[objectIndex];
    RWTexture2D<float4> field=ResourceDescriptorHeap[passIndex];
    const float3 normal=MembraneDirection(id.xy,size);
    const float3 driftA=float3(5.4f,3.1f,-3.9f);
    const float3 driftB=float3(-4.2f,6.5f,7.1f);
    float a,da,b,db;
    sincos(dot(normal,driftA)+membraneAnimation.x*.53f,a,da);
    sincos(dot(normal,driftB)-membraneAnimation.x*.37f,b,db);
    float3 gradient=.015f*da*driftA+.007f*db*driftB;
    float crest=0.f;

    uint count,stride;
    events.GetDimensions(count,stride);
    const uint4 history=events[0];
    const uint mask=count-2u;
    const float duration=membraneAnimation.z;
    [loop]for(uint ageIndex=0u;ageIndex<history.y&&membraneAnimation.w>0.f;++ageIndex){
        const uint slot=1u+((history.x-1u-ageIndex)&mask);
        const float4 contact=asfloat(events[slot]);
        const float age=max(timing.z-contact.w,0.f);
        if(age>=duration)break;
        const float travel=.035f+max(0.f,membraneAnimation.y-contact.z)*1.12f;
        if(travel>=2.f)continue;
        const float width=.077f+.053f*age;
        const float nearSide=max(0.f,travel-width),farSide=travel+width;
        const float3 offset=normal-DecodeMembraneNormal(contact.xy);
        const float square=dot(offset,offset);
        if(square>=nearSide*nearSide&&square<farSide*farSide){
            const float distance=sqrt(square);
            const float local=(distance-travel)/width;
            const float skirt=saturate(1.f-local*local);
            const float fade=1.f-age/duration;
            const float envelope=membraneAnimation.w*fade*fade*skirt*saturate((2.f-travel)*7.f);
            crest+=envelope*skirt;
            const float slope=.026f*envelope*(1.f-5.f*local*local)/width;
            gradient+=slope*offset/max(distance,.0001f);
        }
    }
    [unroll]for(uint i=0;i<4u;++i){
        const float4 pulse=membraneHandPulses[i];
        if(pulse.w<=0.f||pulse.w>=1.f+duration)continue;
        const float age=pulse.w-1.f;
        const float travel=.016f+age*.55f;
        const float width=.038f+.036f*age;
        const float3 offset=normal-pulse.xyz;
        const float square=dot(offset,offset);
        const float farSide=travel+width,nearSide=max(0.f,travel-width);
        if(square<nearSide*nearSide||square>=farSide*farSide)continue;
        const float distance=sqrt(square);
        const float local=(distance-travel)/width;
        const float shape=saturate(1.f-local*local);
        const float fade=1.f-age/duration;
        const float envelope=.55f*membraneAnimation.w*fade*fade*shape;
        crest+=envelope*shape;
        gradient+=.019f*envelope*(1.f-5.f*local*local)*offset/(width*max(distance,.0001f));
    }
    [unroll]for(uint hand=0;hand<2u;++hand){
        const float4 end=membraneStrokeEnd[hand];
        if(end.w<=0.f)continue;
        const float4 start=membraneStrokeStart[hand];
        const float3 segment=end.xyz-start.xyz;
        const float u=saturate(dot(normal-start.xyz,segment)/max(dot(segment,segment),1.e-5f));
        const float3 offset=normal-lerp(start.xyz,end.xyz,u);
        const float distance2=dot(offset,offset)/(start.w*start.w);
        if(distance2>=1.f)continue;
        const float wake=end.w*(.38f+.62f*u)*(1.f-distance2);
        crest+=wake*(.18f+.14f*distance2);
        gradient+=wake*(2.1f*distance2-1.f)*offset/start.w*.23f;
    }
    field[id.xy]=float4(gradient,min(crest,4.f));
}
