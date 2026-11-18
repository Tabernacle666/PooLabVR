#include "common.hlsli"

float3 MembraneDirection(uint2 texel,uint size){
    float2 p=(float2(texel)-.5f)*(2.f/float(size-2u))-1.f;
    // The padded texels are reflected across the octahedral seam.
    if(abs(p.x)>1.f)p=float2(sign(p.x)*(2.f-abs(p.x)),-p.y);
    if(abs(p.y)>1.f)p=float2(-p.x,sign(p.y)*(2.f-abs(p.y)));
    float3 n=float3(p,1.f-abs(p.x)-abs(p.y));
    const float fold=max(-n.z,0.f);
    n.xy+=float2(n.x>=0.f?-fold:fold,n.y>=0.f?-fold:fold);
    return normalize(n);
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
    sincos(dot(normal,driftA)+timing.z*.53f,a,da);
    sincos(dot(normal,driftB)-timing.z*.37f,b,db);
    float3 gradient=.015f*da*driftA+.007f*db*driftB;
    float thickness=.27f+.08f*a+.055f*b;

    uint count,stride;
    events.GetDimensions(count,stride);
    const uint4 history=events[0];
    const uint mask=count-2u;
    const float duration=1.05f;
    [loop]for(uint ageIndex=0u;ageIndex<history.y;++ageIndex){
        const uint slot=1u+((history.x-1u-ageIndex)&mask);
        const float4 contact=asfloat(events[slot]);
        const float age=max(timing.z-contact.w,0.f);
        if(age>=duration)break;
        const float travel=.035f+age*1.12f;
        const float width=.077f+.053f*age;
        const float nearSide=max(0.f,travel-width),farSide=travel+width;
        const float3 offset=normal-contact.xyz;
        const float square=dot(offset,offset);
        if(square>=nearSide*nearSide&&square<farSide*farSide){
            const float distance=sqrt(square);
            const float local=(distance-travel)/width;
            const float skirt=saturate(1.f-local*local);
            const float fade=1.f-age/duration;
            const float envelope=fade*fade*skirt;
            thickness+=.36f*envelope*skirt;
            const float slope=.026f*envelope*(1.f-5.f*local*local)/width;
            gradient+=slope*offset/max(distance,.0001f);
        }
    }
    field[id.xy]=float4(gradient,saturate(thickness));
}
