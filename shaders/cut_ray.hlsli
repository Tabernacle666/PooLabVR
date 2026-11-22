struct CutTriangle {
    Vertex a,b,c;
    uint material,instanceId,pad0,pad1;
};

struct CutRasterBlock {
    float3 lower;uint firstTriangle;
    float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
};

bool CutRayActive(){
    if(resourceAbi.w==0xffffffffu||cutGeometry.y==0xffffffffu||
       cutGeometry.z==0xffffffffu||cutRaster.x==0xffffffffu)return false;
    StructuredBuffer<uint4> cutState=ResourceDescriptorHeap[resourceAbi.w];
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    const uint4 cache=cacheState[0];
    return cutState[0].y!=0u&&cache.y!=0u&&cache.z==1u&&cache.x<=1u;
}

bool RayUsesCutReplacement(uint rayIndex){
    const uint sourceIndex=RenderInstanceForRay(rayIndex);
    if(sourceIndex>=cutGeometry.w)return false;
    const GpuInstance source=LoadInstanceStable(sourceIndex);
    return source.id==0x80000000u+sourceIndex&&(source.flags&19u)==0u&&
        SphereNativeInstance(sourceIndex);
}

struct CutRayNode {float3 lower;uint leftOrBlock;float3 upper;uint rightChild;};
struct CutRayHit {float distance;float2 barycentrics;uint descriptor,triangleIndex;};

bool CutRayIntersects(RayDesc ray,float distance,float3 lower,float3 upper){
    float enter=ray.TMin,leave=distance;
    [unroll]for(uint axis=0;axis<3;++axis){
        const float direction=ray.Direction[axis],origin=ray.Origin[axis];
        if(abs(direction)<1.e-8f){
            if(origin<lower[axis]||origin>upper[axis])return false;
        }else{
            const float a=(lower[axis]-origin)/direction,b=(upper[axis]-origin)/direction;
            enter=max(enter,min(a,b));leave=min(leave,max(a,b));
        }
    }
    return enter<=leave;
}

bool IntersectCutFace(RayDesc ray,CutTriangle face,out float distance,out float2 barycentrics){
    const float3 edge=face.b.position-face.a.position,side=face.c.position-face.a.position;
    const float3 crossRay=cross(ray.Direction,side);
    const float det=dot(edge,crossRay);
    distance=0.f;barycentrics=0.f;
    if(abs(det)<=1.e-10f)return false;
    const float inverse=rcp(det);
    const float3 delta=ray.Origin-face.a.position;
    const float3 crossEdge=cross(delta,edge);
    barycentrics=float2(dot(delta,crossRay),dot(ray.Direction,crossEdge))*inverse;
    distance=dot(side,crossEdge)*inverse;
    return all(barycentrics>=0.f)&&barycentrics.x+barycentrics.y<=1.f&&distance>=ray.TMin;
}

// Cut triangles are already in the active body's canonical render space.
// Take the nearest replacement alongside the ordinary DXR hit.
CutRayHit TraceCutRay(RayDesc ray,uint rayMask,bool anyHit){
    CutRayHit hit;
    hit.distance=ray.TMax;hit.barycentrics=0.f;hit.descriptor=hit.triangleIndex=0xffffffffu;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    const uint blocksDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=ResourceDescriptorHeap[NonUniformResourceIndex(blocksDescriptor)];
    StructuredBuffer<CutRayNode> nodes=ResourceDescriptorHeap[cutGeometry.y];
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    uint pending[32];
    pending[0]=cutGeometry.z;
    uint count=1;
    [loop]while(count>0u){
        const CutRayNode node=nodes[pending[--count]];
        if(!CutRayIntersects(ray,hit.distance,node.lower,node.upper))continue;
        if(node.rightChild!=0xffffffffu){
            // The CPU builds a median-split tree for each body.
            pending[count++]=node.rightChild;
            pending[count++]=node.leftOrBlock;
            continue;
        }
        const CutRasterBlock block=blocks[node.leftOrBlock];
        if(block.geometryState==2u)continue;
        const bool modified=block.geometryState==1u;
        const uint size=modified?block.activeCount:block.triangleCount;
        const uint first=modified?block.activeFirst:block.firstTriangle;
        const uint descriptor=modified?cutRaster.w:cutGeometry.x;
        StructuredBuffer<CutTriangle> faces=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        [loop]for(uint i=0;i<size;++i){
            const CutTriangle face=faces[first+i];
            const uint flags=face.pad0;
            const uint mask=(flags&4u)?4u:(flags&3u)?2u:1u;
            if((mask&rayMask)==0u)continue;
            float distance;
            float2 barycentrics;
            if(!IntersectCutFace(ray,face,distance,barycentrics)||distance>=hit.distance)continue;
            const Surface material=materials[face.material];
            if(material.extra.y!=0u){
                const float3 weights=float3(1.f-barycentrics.x-barycentrics.y,barycentrics);
                const float2 uv0=face.a.uv*weights.x+face.b.uv*weights.y+face.c.uv*weights.z;
                const float2 uv1=face.a.uv1*weights.x+face.b.uv1*weights.y+face.c.uv1*weights.z;
                if(SurfaceRayOpacity(material,uv0,uv1).x<.5f)continue;
            }
            hit.distance=distance;hit.barycentrics=barycentrics;
            hit.descriptor=descriptor;hit.triangleIndex=first+i;
            if(anyHit)return hit;
        }
    }
    return hit;
}
