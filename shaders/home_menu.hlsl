#include "common.hlsli"
struct HomeMenuLine {float4 geometry;float4 surface;uint4 chars[6];uint4 meta;float4 right;float4 up;float4 normal;};
struct HomeMenuVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint lineIndex:TEXCOORD1;nointerpolation uint viewIndex:TEXCOORD2;nointerpolation uint reverse:TEXCOORD3;};
uint PackedChar(HomeMenuLine l,uint i){return (l.chars[i>>4][(i>>2)&3]>>((i&3)*8))&255;}
HomeMenuVertex HomeMenuVS(uint vertexId:SV_VertexID,uint instanceId:SV_InstanceID,uint view:SV_ViewID){
    StructuredBuffer<HomeMenuLine> lines=ResourceDescriptorHeap[objectIndex];HomeMenuLine l=lines[instanceId];
    static const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    float2 uv=corners[vertexId%6];uv.x=(vertexId/6+uv.x)/16.;float2 c=float2(uv.x*2-1,1-uv.y*2);
    float3 center=float3(l.surface.x,l.geometry.x,l.geometry.w);
    float3 p=center+l.right.xyz*c.x*l.geometry.y+l.up.xyz*c.y*l.geometry.z;
    if((l.meta.z&1)&&!(l.meta.z&2)){float curve=(p.x-l.surface.z)/l.surface.w;p.z-=l.surface.y*saturate(1-curve*curve);}
    HomeMenuVertex o;o.position=mul(float4(p,1),vp[view]);o.uv=uv;o.lineIndex=instanceId;o.viewIndex=view;
    o.reverse=(l.meta.z&2)&&dot(eye[view].xyz-center,l.normal.xyz)>0?1:0;return o;
}
float4 HomeMenuPS(HomeMenuVertex input):SV_Target0{
    StructuredBuffer<HomeMenuLine> lines=ResourceDescriptorHeap[objectIndex];StructuredBuffer<uint> atlas=ResourceDescriptorHeap[passIndex];HomeMenuLine l=lines[input.lineIndex];
    if(l.meta.z&1){
float z;
if(topology.y>1){
Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[param0];z=0;
        for(uint sample=0;sample<4;sample++)z=max(z,depth.Load(int3(int2(input.position.xy),input.viewIndex),sample));
}else{
Texture2DArray<float> depth=ResourceDescriptorHeap[param0];z=depth.Load(int4(int2(input.position.xy),input.viewIndex,0));
}

        if(input.position.z+1e-6<z)discard;
    }
    bool panel=(l.meta.z&2)!=0;
    // Inactive face is empty except the face escape control, readable from either side.
    if(panel&&input.reverse&&!(l.meta.z&16))discard;
    float2 uv=input.uv;if(panel&&input.reverse)uv.x=1-uv.x;
    float pixel=l.right.w;float2 local=float2((uv.x-.5)*l.geometry.y*2,(.5-uv.y)*l.geometry.z*2);
    float cell=(local.x+max(pixel,l.meta.x*6.*pixel-pixel)*.5)/pixel;int row=(int)floor(3.5-local.y/pixel);float ink=0;
    if(cell>=0&&row>=0&&row<7){uint n=(uint)floor(cell),ch=n/6,col=n%6;if(ch<l.meta.x&&col<5)ink=(atlas[min(127u,PackedChar(l,ch))*7+row]>>(4-col))&1;}
    float2 a=abs(local);float border=step(min(l.geometry.y-a.x,l.geometry.z-a.y),panel?.0045:.0035);
    float w=l.geometry.y*2,h=l.geometry.z*2,perimeter=2*(w+h);
    float edge;if(a.y/l.geometry.z>a.x/l.geometry.y)edge=local.y>0?local.x+w*.5:w+h+w*.5-local.x;
    else edge=local.x>0?w+h*.5-local.y:2*w+h+h*.5+local.y;
    uint contour=(l.meta.z>>8)&255;
    float all=panel?perimeter:perimeter*max(1u,l.meta.z>>16);
    float head=fmod(asfloat(l.meta.w)*7.0+l.normal.w,all),at=(panel?0:contour*perimeter)+edge;
    float behind=fmod(head-at+all,all);float spark=border*(exp(-behind*19)*2.5+exp(-behind*100)*5);
    float scan=.93+.07*sin(input.position.y*3.14159);
    if(l.meta.z&1){
        float3 phosphor=(panel&&(l.meta.z&4))?float3(1,.48,.035):float3(.025,1,.15);
        float layer=panel?l.up.w:.07;float active=(l.meta.y&2)?.32:(l.meta.y&1)?.12:0;float fade=panel&&(l.meta.z&8)?.25:1;
        float opacity=panel?saturate(layer+active+ink*.87+border*.68+min(spark,.8)):(ink>0||border>0?.97:.82);
        return float4(phosphor*(layer+active+border*((l.meta.y&2)?.9:.36)+ink*1.3+spark)*scan*fade,opacity*fade);
    }
    return ink?float4(.18,1,.32,.95):float4(.008,.05,.019,l.meta.y?.78:.7);
}
