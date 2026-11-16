// A sphere face is partitioned by the supporting lines of its intersection
// segments. Every resulting convex cell lies wholly on one side of the solid
// boundary. A parity query chooses the cells that become the new inner wall.
// All allocations are staging allocations. On overflow or ambiguity the
// caller must discard the entire result and retry with larger buffers or defer.
#include "impact_shared.hlsli"
struct CutVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct CutFace {float3 a;float _pad0;float3 b;float _pad1;float3 c;float _pad2;};
struct CutSegment {float3 a;uint face;float3 b;uint _pad;};
struct CapPolygon {uint first;uint count;uint next;uint face;uint active;uint _pad0;uint _pad1;uint _pad2;};

cbuffer CapParameters : register(b0) {
    uint inputFaces,inputSegments,segmentControl,polygonBuffer;
    uint vertexBuffer,faceHeads,capControl,sourceVertices;
    uint sourceIndices,sourceSolids,outputVertices,outputIndices;
    uint faceCount,polygonCapacity,vertexCapacity,outputTriangleCapacity;
    uint sourceTriangleCount,outputSolids,resolvedImpact,impactIndex;
};
static const uint Invalid=0xffffffff;
// capControl byte offsets: 0 polygon reservations, 4 vertex reservations,
// 8 output triangles, 12 overflow, 16 numerical ambiguity.
void FailCap(RWByteAddressBuffer control,uint byteOffset){uint ignored;control.InterlockedOr(byteOffset,1,ignored);}

bool Allocate(RWByteAddressBuffer control,uint offset,uint count,uint capacity,out uint first){
    uint expected=control.Load(offset);
    for(;;){
        if(expected>capacity||count>capacity-expected){first=0;FailCap(control,12);return false;}
        uint observed;control.InterlockedCompareExchange(offset,expected,expected+count,observed);
        if(observed==expected){first=expected;return true;}expected=observed;
    }
}

float LineDistance(float3 p,float3 lineOrigin,float3 lineNormal,float epsilon){
    float d=dot(p-lineOrigin,lineNormal);return abs(d)<=epsilon?0:d;
}

uint ClipCount(RWStructuredBuffer<float3> vertices,CapPolygon poly,
               float3 lineOrigin,float3 lineNormal,float epsilon,bool positive){
    uint count=0;
    for(uint i=0;i<poly.count;i++){
        float3 a=vertices[poly.first+i],b=vertices[poly.first+(i+1)%poly.count];
        float da=LineDistance(a,lineOrigin,lineNormal,epsilon),db=LineDistance(b,lineOrigin,lineNormal,epsilon);
        if(positive?da>=0:da<=0)count++;
        if((da<0&&db>0)||(da>0&&db<0))count++;
    }
    return count;
}

void ClipWrite(RWStructuredBuffer<float3> vertices,CapPolygon poly,uint output,
               float3 lineOrigin,float3 lineNormal,float epsilon,bool positive){
    uint cursor=0;
    for(uint i=0;i<poly.count;i++){
        float3 a=vertices[poly.first+i],b=vertices[poly.first+(i+1)%poly.count];
        float da=LineDistance(a,lineOrigin,lineNormal,epsilon),db=LineDistance(b,lineOrigin,lineNormal,epsilon);
        if(positive?da>=0:da<=0)vertices[output+cursor++]=a;
        if((da<0&&db>0)||(da>0&&db<0))vertices[output+cursor++]=lerp(a,b,da/(da-db));
    }
}

[numthreads(64,1,1)]
void PartitionCap(uint3 dispatch:SV_DispatchThreadID){
    uint faceIndex=dispatch.x;if(faceIndex>=faceCount)return;
    StructuredBuffer<CutFace> faces=ResourceDescriptorHeap[inputFaces];
    StructuredBuffer<CutSegment> segments=ResourceDescriptorHeap[inputSegments];
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    ResolvedImpact hit=impacts[impactIndex];if(hit.status!=0)return;
    ByteAddressBuffer segmentCounts=ResourceDescriptorHeap[segmentControl];
    RWStructuredBuffer<CapPolygon> polygons=ResourceDescriptorHeap[polygonBuffer];
    RWStructuredBuffer<float3> vertices=ResourceDescriptorHeap[vertexBuffer];
    RWStructuredBuffer<uint> heads=ResourceDescriptorHeap[faceHeads];
    RWByteAddressBuffer control=ResourceDescriptorHeap[capControl];
    if(segmentCounts.Load(4)||segmentCounts.Load(8)){FailCap(control,16);return;}
    CutFace face=faces[faceIndex];float3 crossFace=cross(face.b-face.a,face.c-face.a);
    float faceArea=length(crossFace);if(faceArea<=0){FailCap(control,16);return;}
    float3 faceNormal=crossFace/faceArea;float epsilon=max(hit.localRadius*1e-6,1e-8);
    uint head,firstVertex;
    if(!Allocate(control,0,1,polygonCapacity,head)||!Allocate(control,4,3,vertexCapacity,firstVertex))return;
    vertices[firstVertex]=face.a;vertices[firstVertex+1]=face.b;vertices[firstVertex+2]=face.c;
    CapPolygon initial;initial.first=firstVertex;initial.count=3;initial.next=Invalid;initial.face=faceIndex;
    initial.active=0;initial._pad0=initial._pad1=initial._pad2=0;polygons[head]=initial;
    DeviceMemoryBarrier();
    uint segmentCount=segmentCounts.Load(0);
    for(uint s=0;s<segmentCount;s++){
        CutSegment segment=segments[s];if(segment.face!=faceIndex)continue;
        float3 raw=cross(faceNormal,segment.b-segment.a);float len=length(raw);
        if(len<=epsilon)continue;
        float3 lineNormal=raw/len;uint nextHead=Invalid;
        while(head!=Invalid){
            CapPolygon poly=polygons[head];uint following=poly.next;
            float lo=1e30,hi=-1e30;
            for(uint j=0;j<poly.count;j++){
                float d=LineDistance(vertices[poly.first+j],segment.a,lineNormal,epsilon);
                lo=min(lo,d);hi=max(hi,d);
            }
            if(lo<0&&hi>0){
                uint negativeCount=ClipCount(vertices,poly,segment.a,lineNormal,epsilon,false);
                uint positiveCount=ClipCount(vertices,poly,segment.a,lineNormal,epsilon,true);
                if(negativeCount<3||positiveCount<3){FailCap(control,16);return;}
                uint newPolygons,newVertices;
                if(!Allocate(control,0,2,polygonCapacity,newPolygons)||
                   !Allocate(control,4,negativeCount+positiveCount,vertexCapacity,newVertices))return;
                ClipWrite(vertices,poly,newVertices,segment.a,lineNormal,epsilon,false);
                ClipWrite(vertices,poly,newVertices+negativeCount,segment.a,lineNormal,epsilon,true);
                CapPolygon neg=initial;neg.first=newVertices;neg.count=negativeCount;neg.next=nextHead;
                CapPolygon pos=initial;pos.first=newVertices+negativeCount;pos.count=positiveCount;pos.next=newPolygons;
                polygons[newPolygons]=neg;polygons[newPolygons+1]=pos;nextHead=newPolygons+1;
            }else{poly.next=nextHead;polygons[head]=poly;nextHead=head;}
            head=following;
        }
        head=nextHead;DeviceMemoryBarrier();
    }
    heads[faceIndex]=head;
    while(head!=Invalid){CapPolygon poly=polygons[head];poly.active=1;polygons[head]=poly;head=poly.next;}
}

// A parity ray uses all triangles of the same welded solid, including triangles
// belonging to other materials or render regions. Edge hits retry another ray.
int SolidParity(float3 origin,float3 direction,float epsilon,uint solid){
    StructuredBuffer<CutVertex> vertices=ResourceDescriptorHeap[sourceVertices];
    StructuredBuffer<uint> indices=ResourceDescriptorHeap[sourceIndices];
    StructuredBuffer<uint> solids=ResourceDescriptorHeap[sourceSolids];
    uint hits=0;
    for(uint i=0;i<sourceTriangleCount;i++){
        if(solids[i]!=solid)continue;
        uint first=i*3;float3 a=vertices[indices[first]].position;
        float3 e1=vertices[indices[first+1]].position-a,e2=vertices[indices[first+2]].position-a;
        float3 h=cross(direction,e2);float determinant=dot(e1,h);
        if(abs(determinant)<1e-12)continue;
        float inverse=1/determinant;float3 s=origin-a;
        float u=dot(s,h)*inverse;float3 q=cross(s,e1);float v=dot(direction,q)*inverse;
        if(u<0||v<0||u+v>1)continue;
        float t=dot(e2,q)*inverse;if(t<=epsilon)continue;
        if(min(u,min(v,1-u-v))<1e-6)return -1;
        hits++;
    }
    return int(hits&1);
}

[numthreads(64,1,1)]
void EmitCap(uint3 dispatch:SV_DispatchThreadID){
    uint index=dispatch.x+dispatch.y*4194240u;
    RWByteAddressBuffer control=ResourceDescriptorHeap[capControl];
    uint polygonCount=control.Load(0);
    if(index>=polygonCount||index>=polygonCapacity||control.Load(12)||control.Load(16))return;
    RWStructuredBuffer<CapPolygon> polygons=ResourceDescriptorHeap[polygonBuffer];
    RWStructuredBuffer<float3> vertices=ResourceDescriptorHeap[vertexBuffer];
    StructuredBuffer<CutFace> faces=ResourceDescriptorHeap[inputFaces];
    StructuredBuffer<ResolvedImpact> impacts=ResourceDescriptorHeap[resolvedImpact];
    ResolvedImpact hit=impacts[impactIndex];if(hit.status!=0)return;
    RWStructuredBuffer<CutVertex> output=ResourceDescriptorHeap[outputVertices];
    RWStructuredBuffer<uint> indices=ResourceDescriptorHeap[outputIndices];
    RWStructuredBuffer<uint> emittedSolids=ResourceDescriptorHeap[outputSolids];
    CapPolygon poly=polygons[index];if(!poly.active||poly.count<3)return;
    float3 center=0;for(uint i=0;i<poly.count;i++)center+=vertices[poly.first+i];center/=poly.count;
    float epsilon=max(hit.localRadius*1e-6,1e-8);
    int parity=SolidParity(center,normalize(float3(.803,.421,.422)),epsilon,hit.solid);
    if(parity<0)parity=SolidParity(center,normalize(float3(.317,.911,.263)),epsilon,hit.solid);
    if(parity<0)parity=SolidParity(center,normalize(float3(.517,.183,.836)),epsilon,hit.solid);
    if(parity<0){FailCap(control,16);return;}if(parity==0)return;
    float3 a=vertices[poly.first];uint needed=0;
    for(uint j=1;j+1<poly.count;j++){
        float3 b=vertices[poly.first+j],c=vertices[poly.first+j+1];
        if(dot(cross(b-a,c-a),cross(b-a,c-a))>1e-18)needed++;
    }
    if(!needed)return;uint base;
    if(!Allocate(control,8,needed,outputTriangleCapacity,base))return;
    CutFace face=faces[poly.face];float3 normal=-normalize(cross(face.b-face.a,face.c-face.a));
    uint written=0;
    for(uint j=1;j+1<poly.count;j++){
        float3 b=vertices[poly.first+j],c=vertices[poly.first+j+1];
        if(dot(cross(b-a,c-a),cross(b-a,c-a))<=1e-18)continue;
        uint outputTriangle=base+written++;uint first=outputTriangle*3;CutVertex va,vb,vc;
        va.position=a;vb.position=c;vc.position=b;va.normal=vb.normal=vc.normal=normal;
        va.uv=vb.uv=vc.uv=0;va.uv1=vb.uv1=vc.uv1=0;
        output[first]=va;output[first+1]=vb;output[first+2]=vc;
        indices[first]=first;indices[first+1]=first+1;indices[first+2]=first+2;
        emittedSolids[outputTriangle]=hit.solid;
    }
}
