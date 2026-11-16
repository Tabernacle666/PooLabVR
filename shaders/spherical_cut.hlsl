// Polygon clipping for a single convex, faceted impact volume.
// Output is staged separately; the caller must not publish it if overflow is set.
#include "impact_shared.hlsli"
#include "cut_source.hlsli"
struct CutVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct MeshRange {uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;float4 bounds;};
struct CutPlane {float3 normal;float offset;};

cbuffer CutParameters : register(b0) {
    uint inputVertices, inputIndices, inputPlanes, outputVertices;
    uint outputIndices, outputControl, rangesDescriptor, triangleCount;
    uint planeCount, outputTriangleCapacity, inputSolids, outputSolids;
    uint resolvedImpact,impactIndex,_reserved0,_reserved1;
    uint4 sourceOriginal,sourceFirst,sourceSecond,sourceControl;
};

static const uint MaxCutPlanes=80;
static const uint MaxPolygonVertices=MaxCutPlanes+3;

float3 Point(float3 bary,CutVertex a,CutVertex b,CutVertex c){
    return a.position*bary.x+b.position*bary.y+c.position*bary.z;
}
CutVertex InterpolateCutVertex(float3 bary,CutVertex a,CutVertex b,CutVertex c){
    CutVertex v;
    v.position=a.position*bary.x+b.position*bary.y+c.position*bary.z;
    v.normal=a.normal*bary.x+b.normal*bary.y+c.normal*bary.z;
    v.uv=a.uv*bary.x+b.uv*bary.y+c.uv*bary.z;
    v.uv1=a.uv1*bary.x+b.uv1*bary.y+c.uv1*bary.z;
    return v;
}

void ClipPolygon(in float3 source[MaxPolygonVertices],uint count,
                 CutPlane plane,CutVertex a,CutVertex b,CutVertex c,bool inside,
                 out float3 destination[MaxPolygonVertices],out uint resultCount){
    resultCount=0;
    if(count==0)return;
    for(uint i=0;i<count;i++){
        float3 x=source[i],y=source[(i+1)%count];
        float dx=dot(plane.normal,Point(x,a,b,c))-plane.offset;
        float dy=dot(plane.normal,Point(y,a,b,c))-plane.offset;
        bool keepX=inside?dx<=0:dx>0;
        bool keepY=inside?dy<=0:dy>0;
        if(keepX)destination[resultCount++]=x;
        if(keepX!=keepY)destination[resultCount++]=lerp(x,y,saturate(dx/(dx-dy)));
    }
}

// Count is derived from non-degenerate triangles, exactly as in EmitPolygon.
uint CountPolygon(in float3 poly[MaxPolygonVertices],uint count,
                  CutVertex a,CutVertex b,CutVertex c){
    uint result=0;
    if(count<3)return result;
    float3 p=Point(poly[0],a,b,c);
    for(uint j=1;j+1<count;j++){
        float3 n=cross(Point(poly[j],a,b,c)-p,Point(poly[j+1],a,b,c)-p);
        if(dot(n,n)>1e-18)result++;
    }
    return result;
}

void EmitPolygon(in float3 poly[MaxPolygonVertices],uint count,
                 CutVertex a,CutVertex b,CutVertex c,uint solid,uint baseTriangle,inout uint written){
    if(count<3)return;
    RWStructuredBuffer<CutVertex> vertices=ResourceDescriptorHeap[outputVertices];
    RWStructuredBuffer<uint> indices=ResourceDescriptorHeap[outputIndices];
    RWStructuredBuffer<uint> emittedSolids=ResourceDescriptorHeap[outputSolids];
    float3 p=Point(poly[0],a,b,c);
    for(uint j=1;j+1<count;j++){
        float3 n=cross(Point(poly[j],a,b,c)-p,Point(poly[j+1],a,b,c)-p);
        if(dot(n,n)<=1e-18)continue;
        uint outputTriangle=baseTriangle+written++;uint first=outputTriangle*3;
        vertices[first]=InterpolateCutVertex(poly[0],a,b,c);
        vertices[first+1]=InterpolateCutVertex(poly[j],a,b,c);
        vertices[first+2]=InterpolateCutVertex(poly[j+1],a,b,c);
        indices[first]=first;indices[first+1]=first+1;indices[first+2]=first+2;
        emittedSolids[outputTriangle]=solid;
    }
}

void InitialTriangle(out float3 poly[MaxPolygonVertices],out uint count){
    poly[0]=float3(1,0,0);poly[1]=float3(0,1,0);poly[2]=float3(0,0,1);count=3;
}

[numthreads(64,1,1)]
void CutSurface(uint3 dispatch:SV_DispatchThreadID){
    uint triangleIndex=dispatch.x+dispatch.y*4194240u;if(triangleIndex>=triangleCount)return;
    StructuredBuffer<CutPlane> planes=ResourceDescriptorHeap[inputPlanes];
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    ResolvedImpact hit=impacts[impactIndex];if(hit.status!=0)return;
    CutSourceChoice src=SelectCutSource(sourceControl.x,hit.instanceId,hit.mesh,
                                        sourceOriginal,sourceFirst,sourceSecond);
    StructuredBuffer<CutVertex> sourceVertices=ResourceDescriptorHeap[NonUniformResourceIndex(src.vertices)];
    StructuredBuffer<uint> sourceIndices=ResourceDescriptorHeap[NonUniformResourceIndex(src.indices)];
    StructuredBuffer<uint> solids=ResourceDescriptorHeap[NonUniformResourceIndex(src.solids)];
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[NonUniformResourceIndex(src.ranges)];
    MeshRange range=ranges[src.mesh];if(triangleIndex>=range.indexCount/3)return;
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    if(planeCount==0||planeCount>MaxCutPlanes){uint ignored;control.InterlockedOr(4,1,ignored);return;}
    uint first=range.firstIndex+triangleIndex*3;
    uint solid=solids[first/3];
    CutVertex a=sourceVertices[sourceIndices[first]],b=sourceVertices[sourceIndices[first+1]],c=sourceVertices[sourceIndices[first+2]];
    float3 low=min(a.position,min(b.position,c.position));
    float3 high=max(a.position,max(b.position,c.position));
    uint selectedSolid=hit.solid==0xfffffffeu?0xffffffffu:hit.solid;
    bool candidate=solid==selectedSolid&&all(high>=hit.localPosition-hit.localRadius)&&all(low<=hit.localPosition+hit.localRadius);
    float3 inner[MaxPolygonVertices],temp[MaxPolygonVertices],outside[MaxPolygonVertices];uint count;
    InitialTriangle(inner,count);
    if(candidate){
        for(uint k=0;k<planeCount&&count>=3;k++){
            uint nextCount;ClipPolygon(inner,count,planes[k],a,b,c,true,temp,nextCount);
            count=nextCount;for(uint j=0;j<count;j++)inner[j]=temp[j];
        }
        candidate=CountPolygon(inner,count,a,b,c)!=0;
    }
    uint needed=0;
    if(!candidate)needed=1;
    else{
        InitialTriangle(inner,count);
        for(uint k=0;k<planeCount&&count>=3;k++){
            uint outsideCount,nextCount;
            ClipPolygon(inner,count,planes[k],a,b,c,false,outside,outsideCount);
            needed+=CountPolygon(outside,outsideCount,a,b,c);
            ClipPolygon(inner,count,planes[k],a,b,c,true,temp,nextCount);
            count=nextCount;for(uint j=0;j<count;j++)inner[j]=temp[j];
        }
    }
    // The output contains unmodified triangles too. Count actual clipped
    // source triangles separately to reject a false-positive generation.
    // A triangle completely inside the impact has no surviving output, but
    // its removal still makes the cut a real change.
    if(candidate){
        uint ignored;control.InterlockedAdd(12,1,ignored);
    }
    if(needed==0)return;
    // The requested total is independent of the usable capacity; an
    // overflowing generation can report how much GPU space it needed.
    uint requested;control.InterlockedAdd(16,needed,requested);
    uint base=control.Load(0);
    for(;;){
        if(base>outputTriangleCapacity||needed>outputTriangleCapacity-base){uint ignored;control.InterlockedOr(4,1,ignored);return;}
        uint observed;control.InterlockedCompareExchange(0,base,base+needed,observed);
        if(observed==base)break;base=observed;
    }
    uint written=0;InitialTriangle(inner,count);
    if(!candidate)EmitPolygon(inner,count,a,b,c,solid,base,written);
    else for(uint k=0;k<planeCount&&count>=3;k++){
        uint outsideCount,nextCount;
        ClipPolygon(inner,count,planes[k],a,b,c,false,outside,outsideCount);
        EmitPolygon(outside,outsideCount,a,b,c,solid,base,written);
        ClipPolygon(inner,count,planes[k],a,b,c,true,temp,nextCount);
        count=nextCount;for(uint j=0;j<count;j++)inner[j]=temp[j];
    }
}
