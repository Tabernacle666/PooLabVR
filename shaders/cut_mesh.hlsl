// Persistent GPU triangle soup.  One impact rebuilds the current geometry;
// no fragment-time sphere list, spatial pages, or CPU physics/readback.
struct CutVertex {
    float3 position,normal;
    float2 uv,uv1;
};
struct CutTriangle {
    CutVertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
struct CutImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId,primitiveIndex,kind,padding;
};
cbuffer CutParameters:register(b0) {
    uint originalTriangles,triangleA,triangleB,triangleAWrite;
    uint triangleBWrite,stateRead,stateWrite,impactBuffer;
    uint impactControl,outputControl,dispatchArguments,drawArguments;
    uint originalCount,capacity,planeCount,pad;
};
static const uint MaxPolygon=32;
static const uint PlaneCount=26;
struct PolyVertex {
    float3 position,normal;
    float2 uv,uv1;
};
PolyVertex Blend(PolyVertex a,PolyVertex b,float t){
    PolyVertex v;
    v.position=lerp(a.position,b.position,t);
    v.normal=normalize(lerp(a.normal,b.normal,t));
    v.uv=lerp(a.uv,b.uv,t);
    v.uv1=lerp(a.uv1,b.uv1,t);
    return v;
}
float3 PlaneNormal(uint k){
    if(k<6){
        float3 n=0;
        uint axis=k/2;
        n[axis]=(k&1u)!=0?-1.f:1.f;
        return n;
    }
    if(k<18){
        k-=6u;
        uint pair=k/4u,signs=k%4u;
        float3 n=0;
        if(pair==0){n.x=(signs&1u)?-1:1;n.y=(signs&2u)?-1:1;}
        else if(pair==1){n.x=(signs&1u)?-1:1;n.z=(signs&2u)?-1:1;}
        else{n.y=(signs&1u)?-1:1;n.z=(signs&2u)?-1:1;}
        return normalize(n);
    }
    k-=18u;
    return normalize(float3((k&1u)?-1:1,(k&2u)?-1:1,(k&4u)?-1:1));
}
// Both polygons preserve source winding. The kept (inside) polygon is
// subsequently clipped by the next supporting plane; the outside is final.
uint ClipPolygon(PolyVertex src[MaxPolygon],uint count,
                 float3 normal,float3 center,float radius,bool inside,
                 out PolyVertex dst[MaxPolygon]){
    uint written=0;
    if(count==0)return 0;
    for(uint i=0;i<count;i++){
        PolyVertex a=src[i],b=src[(i+1u)%count];
        float da=dot(normal,a.position-center)-radius;
        float db=dot(normal,b.position-center)-radius;
        bool keepA=inside?da<=0.f:da>0.f;
        bool keepB=inside?db<=0.f:db>0.f;
        if(keepA&&written<MaxPolygon)dst[written++]=a;
        if(keepA!=keepB&&written<MaxPolygon){
            float denom=da-db;
            float t=abs(denom)>1e-12?saturate(da/denom):.5f;
            dst[written++]=Blend(a,b,t);
        }
    }
    return written;
}
PolyVertex ToPoly(CutVertex src){
    PolyVertex v;
    v.position=src.position;v.normal=src.normal;v.uv=src.uv;v.uv1=src.uv1;
    return v;
}
CutVertex ToVertex(PolyVertex src){
    CutVertex v;
    v.position=src.position;v.normal=src.normal;v.uv=src.uv;v.uv1=src.uv1;
    return v;
}
void EmitTriangle(PolyVertex a,PolyVertex b,PolyVertex c,
                  uint material,uint instanceId,uint flags,uint outputDescriptor){
    float3 crossValue=cross(b.position-a.position,c.position-a.position);
    if(dot(crossValue,crossValue)<=1e-16)return;
    RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
    uint index;
    counter.InterlockedAdd(0u,1u,index);
    if(index>=capacity){
        uint ignored;counter.InterlockedOr(4u,1u,ignored);
        return;
    }
    RWStructuredBuffer<CutTriangle> target=ResourceDescriptorHeap[NonUniformResourceIndex(outputDescriptor)];
    CutTriangle triangle;
    triangle.a=ToVertex(a);triangle.b=ToVertex(b);triangle.c=ToVertex(c);
    triangle.material=material;triangle.instanceId=instanceId;
    triangle.pad0=flags;triangle.pad1=0;
    target[index]=triangle;
}
void EmitPolygon(PolyVertex poly[MaxPolygon],uint count,
                 uint material,uint instanceId,uint flags,uint outputDescriptor){
    if(count<3)return;
    for(uint j=1;j+1<count;j++)
        EmitTriangle(poly[0],poly[j],poly[j+1],material,instanceId,flags,outputDescriptor);
}
[numthreads(1,1,1)]
void BeginCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer control=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer dispatch=ResourceDescriptorHeap[dispatchArguments];
    RWByteAddressBuffer hits=ResourceDescriptorHeap[impactControl];
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint4 version=state[0];
    control.Store4(0,uint4(0,0,0,0));
    uint hitCount=hits.Load(0);
    uint count=version.y==0?originalCount:version.x;
    uint groups=(hitCount>0&&hits.Load(4)==0&&hits.Load(8)==0&&
                 count>0&&count<=capacity)?(count+63u)/64u:0u;
    dispatch.Store3(0,uint3(groups,1,1));
}
[numthreads(64,1,1)]
void RebuildCutTriangles(uint3 id:SV_DispatchThreadID){
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[stateRead];
    uint4 version=state[0];
    uint count=version.y==0?originalCount:version.x;
    if(id.x>=count)return;
    StructuredBuffer<CutImpact> hits=ResourceDescriptorHeap[impactBuffer];
    CutImpact hit=hits[0];
    uint sourceDescriptor=version.y==0?originalTriangles:
                          (version.y==1?triangleA:triangleB);
    uint destDescriptor=version.y==1?triangleBWrite:triangleAWrite;
    StructuredBuffer<CutTriangle> source=ResourceDescriptorHeap[NonUniformResourceIndex(sourceDescriptor)];
    CutTriangle tri=source[id.x];
    PolyVertex inside[MaxPolygon],next[MaxPolygon],outside[MaxPolygon];
    inside[0]=ToPoly(tri.a);inside[1]=ToPoly(tri.b);inside[2]=ToPoly(tri.c);
    uint countInside=3;
    bool touched=false;
    float radius=max(hit.radius,1e-6);
    for(uint p=0;p<PlaneCount&&countInside>=3;p++){
        float3 normal=PlaneNormal(p);
        uint outsideCount=ClipPolygon(inside,countInside,normal,hit.position,radius,false,outside);
        EmitPolygon(outside,outsideCount,tri.material,tri.instanceId,tri.pad0,destDescriptor);
        uint nextCount=ClipPolygon(inside,countInside,normal,hit.position,radius,true,next);
        for(uint k=0;k<nextCount;k++)inside[k]=next[k];
        countInside=nextCount;
    }
    // A non-degenerate portion inside the 26-plane impact has been removed.
    if(countInside>=3){
        float3 normal=cross(inside[1].position-inside[0].position,
                            inside[2].position-inside[0].position);
        if(dot(normal,normal)>1e-16){
            RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
            uint old;counter.InterlockedAdd(8u,1u,old);
        }
    }
}
[numthreads(1,1,1)]
void CommitCut(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer counter=ResourceDescriptorHeap[outputControl];
    RWByteAddressBuffer impacts=ResourceDescriptorHeap[impactControl];
    if(impacts.Load(0)==0||impacts.Load(4)!=0||impacts.Load(8)!=0)return;
    uint count=counter.Load(0u);
    if(counter.Load(4u)!=0||counter.Load(8u)==0||count>capacity)return;
    StructuredBuffer<uint4> current=ResourceDescriptorHeap[stateRead];
    RWStructuredBuffer<uint4> state=ResourceDescriptorHeap[stateWrite];
    uint4 previous=current[0];
    uint slot=previous.y==1u?2u:1u;
    state[0]=uint4(count,slot,previous.z+1u,0u);
    RWByteAddressBuffer args=ResourceDescriptorHeap[drawArguments];
    args.Store4(0,uint4(0u,(count+20u)/21u,1u,1u));
}
