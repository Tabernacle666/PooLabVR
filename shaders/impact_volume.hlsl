// Construct the 80 outward-facing triangles and clipping planes of the
// faceted impact sphere directly from the GPU projectile impact buffer.
#include "impact_shared.hlsli"
struct CutFace {float3 a;float _pad0;float3 b;float _pad1;float3 c;float _pad2;};
struct CutPlane {float3 normal;float offset;};
cbuffer VolumeParameters:register(b0){uint impactsDescriptor,facesDescriptor,planesDescriptor,impactIndex;};
static const float GoldenRatio=1.618033988749895;
static const float3 Icosahedron[12]={
    float3(-1,GoldenRatio,0),float3(1,GoldenRatio,0),float3(-1,-GoldenRatio,0),float3(1,-GoldenRatio,0),
    float3(0,-1,GoldenRatio),float3(0,1,GoldenRatio),float3(0,-1,-GoldenRatio),float3(0,1,-GoldenRatio),
    float3(GoldenRatio,0,-1),float3(GoldenRatio,0,1),float3(-GoldenRatio,0,-1),float3(-GoldenRatio,0,1)
};
static const uint3 FaceIndices[20]={
    uint3(6,2,10),uint3(11,0,10),uint3(11,10,2),uint3(11,2,4),uint3(3,4,2),
    uint3(3,6,8),uint3(3,2,6),uint3(7,10,0),uint3(7,6,10),uint3(7,8,6),
    uint3(7,0,1),uint3(7,1,8),uint3(5,1,0),uint3(5,11,4),uint3(5,0,11),
    uint3(9,3,8),uint3(9,8,1),uint3(9,4,3),uint3(9,1,5),uint3(9,5,4)
};
[numthreads(64,1,1)]
void BuildImpactVolume(uint3 dispatch:SV_DispatchThreadID){
    uint index=dispatch.x;if(index>=80)return;
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[impactsDescriptor];
    RWStructuredBuffer<CutFace> faces=ResourceDescriptorHeap[facesDescriptor];
    RWStructuredBuffer<CutPlane> planes=ResourceDescriptorHeap[planesDescriptor];
    ResolvedImpact hit=impacts[impactIndex];if(hit.status!=0)return;uint3 ids=FaceIndices[index/4];
    float3 a=normalize(Icosahedron[ids.x]),b=normalize(Icosahedron[ids.y]),c=normalize(Icosahedron[ids.z]);
    float3 ab=normalize(a+b),bc=normalize(b+c),ca=normalize(c+a);
    float3 p,q,r;
    switch(index%4){
        case 0:p=a;q=ab;r=ca;break;
        case 1:p=ab;q=b;r=bc;break;
        case 2:p=ca;q=bc;r=c;break;
        default:p=ab;q=bc;r=ca;break;
    }
    p=hit.localPosition+hit.localRadius*p;q=hit.localPosition+hit.localRadius*q;r=hit.localPosition+hit.localRadius*r;
    CutFace face;face.a=p;face.b=q;face.c=r;face._pad0=face._pad1=face._pad2=0;faces[index]=face;
    CutPlane plane;plane.normal=normalize(cross(q-p,r-p));plane.offset=dot(plane.normal,p);planes[index]=plane;
}
