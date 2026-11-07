#include "common.hlsli"
float3 Perceptual(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}
float Luma(float3 x){return dot(Perceptual(max(0,x)),float3(.299,.587,.114));}
[numthreads(8,8,1)]
void FxaaMain(uint3 id:SV_DispatchThreadID){
    if(id.x>=Width||id.y>=Height||id.z>=topology.x)return;
    Texture2DArray<float4> source=ResourceDescriptorHeap[objectIndex];RWTexture2DArray<float4> target=ResourceDescriptorHeap[passIndex];SamplerState sampler=SamplerDescriptorHeap[0];
    int2 p=int2(id.xy),limit=int2(Width-1,Height-1);float4 center=source.Load(int4(p,id.z,0));
    float n=Luma(source.Load(int4(clamp(p+int2(0,-1),0,limit),id.z,0)).rgb),s=Luma(source.Load(int4(clamp(p+int2(0,1),0,limit),id.z,0)).rgb);
    float w=Luma(source.Load(int4(clamp(p+int2(-1,0),0,limit),id.z,0)).rgb),e=Luma(source.Load(int4(clamp(p+int2(1,0),0,limit),id.z,0)).rgb),m=Luma(center.rgb);
    float low=min(m,min(min(n,s),min(e,w))),high=max(m,max(max(n,s),max(e,w))),range=high-low;
    if(range<max(param0==1?.055:.035,high*(param0==1?.166:.125))){target[id]=center;return;}
    // Opposite gradients identify isolated one-pixel wires/dots. Keep their
    // area and contrast, including fine meshes and transparent insect wings.
    if(((m-n)*(m-s)>0&&abs(n-s)<range*.15)&&((m-w)*(m-e)>0&&abs(w-e)<range*.15)){target[id]=center;return;}
    bool vertical=(m-w)*(m-e)>0&&abs(w-e)<range*.15&&abs(n-s)<range*.25&&abs(m-(n+s)*.5)<range*.25;
    bool horizontal=(m-n)*(m-s)>0&&abs(n-s)<range*.15&&abs(w-e)<range*.25&&abs(m-(w+e)*.5)<range*.25;
    if(vertical||horizontal){target[id]=center;return;}
    float nw=Luma(source.Load(int4(clamp(p+int2(-1,-1),0,limit),id.z,0)).rgb),ne=Luma(source.Load(int4(clamp(p+int2(1,-1),0,limit),id.z,0)).rgb);
    float sw=Luma(source.Load(int4(clamp(p+int2(-1,1),0,limit),id.z,0)).rgb),se=Luma(source.Load(int4(clamp(p+int2(1,1),0,limit),id.z,0)).rgb);
    float2 direction=float2(-((nw+ne)-(sw+se)),(nw+sw)-(ne+se));
    float reduce=max((nw+ne+sw+se)*.03125,.0078125);direction=clamp(direction/(min(abs(direction.x),abs(direction.y))+reduce),-(param0==1?4:8),(param0==1?4:8))/float2(Width,Height);
    float2 uv=(float2(id.xy)+.5)/float2(Width,Height);
    float3 a=.5*(source.SampleLevel(sampler,float3(uv+direction*(1./3-.5),id.z),0).rgb+source.SampleLevel(sampler,float3(uv+direction*(2./3-.5),id.z),0).rgb);
    if(param0==2){float3 b=a*.5+.25*(source.SampleLevel(sampler,float3(uv-direction*.5,id.z),0).rgb+source.SampleLevel(sampler,float3(uv+direction*.5,id.z),0).rgb);float l=Luma(b);if(l>=low&&l<=high)a=b;}
    target[id]=float4(a,center.a);
}
