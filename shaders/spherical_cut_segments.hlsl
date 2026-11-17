// Intersections between the boundary of a closed solid and each face of the
// faceted impact sphere.  The segments are staging data for cap construction;
// an incomplete or ambiguous result must never be published.
#include "impact_shared.hlsli"
#include "cut_source.hlsli"
struct CutVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct MeshRange {uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;float4 bounds;};
struct CutFace {float3 a;float _pad0;float3 b;float _pad1;float3 c;float _pad2;};
struct CutSegment {float3 a;uint face;float3 b;uint _pad;};

cbuffer SegmentParameters : register(b0) {
    uint inputVertices, inputIndices, inputSolids, inputFaces;
    uint outputSegments, outputControl, rangesDescriptor, triangleCount;
    uint faceCount, segmentCapacity, resolvedImpact, impactIndex;
    uint4 sourceOriginal,sourceFirst,sourceSecond,sourceControl;
};

// control[0]: segment count; control[4]: overflow; control[8]: ambiguous.
void MarkAmbiguous(RWByteAddressBuffer control){
    uint ignored;control.InterlockedOr(8,1,ignored);
}

// Clip a line segment against one inward half-plane in the face's plane.
bool ClipEdge(float3 edgeStart,float3 edgeEnd,float3 normal,
              inout float3 a,inout float3 b,float epsilon){
    float3 edge=edgeEnd-edgeStart;
    float da=dot(cross(edge,a-edgeStart),normal);
    float db=dot(cross(edge,b-edgeStart),normal);
    float tolerance=epsilon*length(edge);
    if(da < -tolerance && db < -tolerance)return false;
    if((da<0)!=(db<0)){
        float t=saturate(da/(da-db));
        float3 crossing=lerp(a,b,t);
        if(da<0)a=crossing;else b=crossing;
    }
    return true;
}

[numthreads(64,1,1)]
void FindCutSegments(uint3 dispatch:SV_DispatchThreadID){
    uint triangleIndex=dispatch.x+dispatch.z*4194240u,faceIndex=dispatch.y;
    if(triangleIndex>=triangleCount||faceIndex>=faceCount)return;
    StructuredBuffer<CutFace> faces=ResourceDescriptorHeap[inputFaces];
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    ResolvedImpact hit=impacts[impactIndex];if(hit.status!=0)return;
    CutSourceChoice src=SelectCutSource(sourceControl.x,hit.instanceId,hit.mesh,sourceControl.y,
                                        sourceOriginal,sourceFirst,sourceSecond);
    StructuredBuffer<CutVertex> vertices=ResourceDescriptorHeap[NonUniformResourceIndex(src.vertices)];
    StructuredBuffer<uint> indices=ResourceDescriptorHeap[NonUniformResourceIndex(src.indices)];
    StructuredBuffer<uint> solids=ResourceDescriptorHeap[NonUniformResourceIndex(src.solids)];
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[NonUniformResourceIndex(src.ranges)];
    MeshRange range=ranges[src.mesh];if(triangleIndex>=range.indexCount/3)return;
    RWStructuredBuffer<CutSegment> segments=ResourceDescriptorHeap[outputSegments];
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    uint first=range.firstIndex+triangleIndex*3;
    if(solids[first/3]!=hit.solid)return;
    float3 p[3]={vertices[indices[first]].position,vertices[indices[first+1]].position,vertices[indices[first+2]].position};
    float3 lo=min(p[0],min(p[1],p[2])),hi=max(p[0],max(p[1],p[2]));
    if(any(hi<hit.localPosition-hit.localRadius)||any(lo>hit.localPosition+hit.localRadius))return;
    CutFace f=faces[faceIndex];
    float3 crossFace=cross(f.b-f.a,f.c-f.a);
    float normalLength=length(crossFace);
    if(normalLength<=0){MarkAmbiguous(control);return;}
    float3 normal=crossFace/normalLength;
    float epsilon=max(hit.localRadius*1e-6,1e-8);
    float d[3]={dot(p[0]-f.a,normal),dot(p[1]-f.a,normal),dot(p[2]-f.a,normal)};
    if((d[0]>epsilon&&d[1]>epsilon&&d[2]>epsilon)||
       (d[0]<-epsilon&&d[1]<-epsilon&&d[2]<-epsilon))return;
    // Coplanar edges are a difficult case, explicitly rejected until supported.
    uint onPlane=(abs(d[0])<=epsilon?1:0)+(abs(d[1])<=epsilon?1:0)+(abs(d[2])<=epsilon?1:0);
    if(onPlane>=2){MarkAmbiguous(control);return;}
    float3 crossings[2];uint hits=0;
    for(uint i=0;i<3;i++){
        uint j=(i+1)%3;
        if(abs(d[i])<=epsilon){if(hits<2)crossings[hits++]=p[i];}
        else if((d[i]>epsilon&&d[j]<-epsilon)||(d[i]<-epsilon&&d[j]>epsilon)){
            if(hits<2)crossings[hits++]=lerp(p[i],p[j],d[i]/(d[i]-d[j]));
        }
    }
    if(hits<2)return;
    float3 a=crossings[0],b=crossings[1];
    if(!ClipEdge(f.a,f.b,normal,a,b,epsilon)||
       !ClipEdge(f.b,f.c,normal,a,b,epsilon)||
       !ClipEdge(f.c,f.a,normal,a,b,epsilon)||dot(a-b,a-b)<=epsilon*epsilon)return;
    uint requested;control.InterlockedAdd(12,1,requested);
    uint slot=control.Load(0);
    for(;;){
        if(slot>=segmentCapacity){uint ignored;control.InterlockedOr(4,1,ignored);return;}
        uint observed;control.InterlockedCompareExchange(0,slot,slot+1,observed);
        if(observed==slot)break;slot=observed;
    }
    CutSegment segment;segment.a=a;segment.b=b;segment.face=faceIndex;segment._pad=0;
    segments[slot]=segment;
}
