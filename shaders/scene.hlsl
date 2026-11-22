#include "common.hlsli"
#ifndef OASIS_MATERIAL_MASK
#define OASIS_MATERIAL_MASK -1
#endif
#define OASIS_SHUTTLE_CRT (OASIS_MATERIAL_MASK == 32)
struct Out {
    float4 position:SV_Position;
    float3 world:POSITION0;
    float3 normal:NORMAL0;
    float2 uv:TEXCOORD0;
    float2 uv1:TEXCOORD3;
    nointerpolation uint material:TEXCOORD1;
    nointerpolation uint view:TEXCOORD2;
#if OASIS_SHUTTLE_CRT
    nointerpolation uint crt:TEXCOORD4;
#endif
};
struct CutTriangle {
    Vertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
float3 ShuttleDustVertex(Vertex vertex,GpuInstance instance,uint view){
    const uint packed=instance.visualData;
    const float density=float(packed>>18u)/63.f;
    const float phase=float(packed&4095u)/4096.f;
    const float speed=float((packed>>12u)&63u)/63.f;
    float3 local=vertex.position;
    local.x=(frac((local.x+20.f)/40.f-phase)-.5f)*40.f;
    const float3 center=World(local,instance);
    const float3 delta=center-eye[view].xyz;
    const float distance2=dot(delta,delta);
    const float3 axis=Rotate(float3(1,0,0),instance.rotation);
    const float3 across=cross(axis,delta);
    const float edge=saturate((20.f-abs(local.x))*.6f)*saturate((distance2-7.f)*.28f);
    const float visible=vertex.uv1.x<density?edge:0.f;
    const float3 side=across*rsqrt(max(1.e-6f,dot(across,across)));
    const float width=.006f+min(.045f,sqrt(distance2)*.00112f);
    return center+visible*(axis*(.085f+.92f*speed)*vertex.uv.x+side*width*vertex.uv.y);
}

[outputtopology("triangle")]
[numthreads(64,1,1)]
void MeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
              out vertices Out verts[64],out indices uint3 tris[124]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    const uint drawOrdinal=objectIndex+group.y;
    const uint instanceIndex=param2==3u?OpaqueRenderIndex(drawOrdinal):drawOrdinal;
    inst=LoadInstance(instanceIndex);
    const bool replaced=generation.y!=0u&&
        instanceIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced&&SphereNativeInstance(instanceIndex)&&(inst.flags&128u)==0u){
        StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];
        MeshRange range=ranges[inst.mesh];
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[Meshlets];
        m=meshlets[range.firstMeshlet+group.x];
        vertexCount=m.vertexCount;
        triangleCount=m.triangleCount;
    }
    // DXIL mesh validation requires exactly one dominating output-count call.
    SetMeshOutputCounts(vertexCount,triangleCount);
    if(tid<vertexCount){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];
        StructuredBuffer<uint> remap=ResourceDescriptorHeap[Remap];
        uint index=remap[m.vertexOffset+tid];
        Vertex v=vertices[index];
        if((inst.flags&2u)!=0u){
            StructuredBuffer<Vertex> deformed=
                ResourceDescriptorHeap[DynamicVertices];
            v=deformed[index];
        }
        float3 world=World(v.position,inst);
#if !OASIS_SHUTTLE_CRT
        if((inst.flags&ShuttleSpeedDustFlag)!=0u)world=ShuttleDustVertex(v,inst,view);
#endif
        verts[tid].position=mul(float4(world,1),vp[view]);
        verts[tid].world=world;
        verts[tid].normal=normalize(Rotate(v.normal/
            (sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),
            inst.rotation));
        verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;
        verts[tid].material=inst.material;verts[tid].view=view;
#if OASIS_SHUTTLE_CRT
        verts[tid].crt=(inst.flags&ShuttleCrtFlag)!=0u?inst.visualData:0u;
#endif
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        StructuredBuffer<uint3> triangles=
            ResourceDescriptorHeap[Triangles];
        tris[t]=triangles[m.triangleOffset+t];
    }
}
[outputtopology("triangle")]
[numthreads(64,1,1)]
void ShadowMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,
                out vertices Out verts[64],out indices uint3 tris[124]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    inst=LoadInstanceStable(objectIndex);
    const bool replaced=generation.y!=0u&&
        objectIndex<cutGeometry.w&&
        (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
    if(!replaced){
        StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];
        StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[Meshlets];
        m=meshlets[ranges[inst.mesh].firstMeshlet+group.x];
        vertexCount=m.vertexCount;
        triangleCount=m.triangleCount;
    }
    // One call dominates every possible mesh output store.
    SetMeshOutputCounts(vertexCount,triangleCount);
    if(tid<vertexCount){
        StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];
        StructuredBuffer<uint> remap=ResourceDescriptorHeap[Remap];
        Vertex v=vertices[remap[m.vertexOffset+tid]];
        float3 world=World(v.position,inst);
        verts[tid].position=mul(float4(world,1),shadowVP[passIndex]);
        verts[tid].world=world;
        verts[tid].normal=Rotate(v.normal,inst.rotation);
        verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;
        verts[tid].material=inst.material;verts[tid].view=0;
    }
    for(uint t=tid;t<triangleCount;t+=64u){
        StructuredBuffer<uint3> triangles=
            ResourceDescriptorHeap[Triangles];
        tris[t]=triangles[m.triangleOffset+t];
    }
}


struct CutRasterBlock {
    float3 lower;uint firstTriangle;
    float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
};
// Dedicated step-4d raster path. Each indirect command supplies a stable
// block ID and its triangle count. Unselected owners emit no geometry.
[outputtopology("triangle")]
[numthreads(64,1,1)]
void CutMeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
                 out vertices Out verts[64],out indices uint3 tris[124]){
    uint local=(group.x+group.y*65535u)*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    const uint faceCount=cacheState[0].y!=0u&&SphereNativeInstance(block.instanceId)&&local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);
    uint descriptor=block.geometryState==0u?cutGeometry.x:cutRaster.w;
    uint start=(block.geometryState==0u?block.firstTriangle:block.activeFirst)+local;
    if(tid<faceCount*3u){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        CutTriangle cutFace=source[start+tid/3u];
        Vertex v=cutFace.a;
        if(tid%3u==1u)v=cutFace.b;
        else if(tid%3u==2u)v=cutFace.c;
        verts[tid].position=mul(float4(v.position,1),vp[view]);
        verts[tid].world=v.position;
        verts[tid].normal=normalize(v.normal);
        verts[tid].uv=v.uv;
        verts[tid].uv1=v.uv1;
        verts[tid].material=cutFace.material;
        verts[tid].view=view;
    }
    if(tid<faceCount)
        tris[tid]=uint3(tid*3u,tid*3u+1u,tid*3u+2u);
}

[outputtopology("triangle")]
[numthreads(64,1,1)]
void CutShadowMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,
                   out vertices Out verts[64],out indices uint3 tris[124]){
    uint local=(group.x+group.y*65535u)*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint faceCount=cacheState[0].y!=0u&&local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    uint descriptor=block.geometryState==0u?cutGeometry.x:cutRaster.w;
    uint start=(block.geometryState==0u?block.firstTriangle:block.activeFirst)+local;
    if(tid<faceCount*3u){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        CutTriangle cutFace=source[start+tid/3u];
        Vertex v=cutFace.a;
        if(tid%3u==1u)v=cutFace.b;
        else if(tid%3u==2u)v=cutFace.c;
        verts[tid].position=mul(float4(v.position,1),shadowVP[passIndex]);
        verts[tid].world=v.position;
        verts[tid].normal=normalize(v.normal);
        verts[tid].uv=v.uv;
        verts[tid].uv1=v.uv1;
        verts[tid].material=cutFace.material;
        verts[tid].view=0u;
    }
    if(tid<faceCount){
        StructuredBuffer<CutTriangle> source=
            ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
        tris[tid]=(source[start+tid].pad0&4u)!=0u?
            uint3(0u,0u,0u):uint3(3u*tid,3u*tid+1u,3u*tid+2u);
    }
}

void DepthPixel(Out i){StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface surface=materials[i.material];if(surface.extra.y){float alpha=surface.opacity.x;if(surface.albedo!=0xffffffff)alpha*=SampleMaterial(surface.albedo,surface,0,i.uv,i.uv1).a;clip(alpha-surface.opacity.y);}}
float StaticShadow(float3 p,Light light,uint slot) {
    float3 d=p-light.positionRadius.xyz;float3 ad=abs(d);uint face=ad.x>=ad.y&&ad.x>=ad.z?(d.x>=0?0:1):ad.y>=ad.z?(d.y>=0?2:3):(d.z>=0?4:5);
    float4 clip=mul(float4(p,1),shadowVP[slot*6+face]);if(clip.w<=0)return 1;float3 ndc=clip.xyz/clip.w;if(ndc.z<0||ndc.z>1)return 1;
    float2 uv=ndc.xy*float2(.5,-.5)+.5,tile=float2((slot%2)*3+face%3,(slot/2)*2+face/3);float result=0;
    Texture2D<float> atlas=ResourceDescriptorHeap[ShadowAtlas];SamplerComparisonState comparison=SamplerDescriptorHeap[1];
    [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){float2 q=clamp(uv+float2(x,y)/512,.5/512,1-.5/512);result+=atlas.SampleCmpLevelZero(comparison,(q+tile)/float2(6,4),ndc.z+.0006);}return result/9;
}
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

float Visibility(float3 p,float3 n,Light light,uint index) {
    if(!((uint)light.parameters.z&1))return 1;
    bool directional=((uint)light.parameters.z&2)!=0;float3 direction=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;float distance=directional?timing.y:length(direction);
    int slot=(int)light.parameters.y;
    if((Quality==0&&index>=4)||RayInstanceCount==0)return slot>=0?StaticShadow(p+n*.002,light,(uint)slot):1;
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];RayDesc ray;ray.Origin=p+n*.002;ray.Direction=normalize(direction);ray.TMin=.002;ray.TMax=max(.003,distance-.01);
    if(solarSun.w>0.f){
        // Only the active body's geometry occupies this canonical space.
        const float3 local=ray.Origin-atmosphereSphere.xyz;
        const float along=dot(local,ray.Direction);
        const float disc=along*along+atmosphereSphere.w*atmosphereSphere.w-dot(local,local);
        ray.TMax=clamp(-along+sqrt(max(0.f,disc))+.003f,.003f,ray.TMax);
    }
    const uint rayMask=slot>=0?2u:3u;
    const bool useCut=CutRayActive();
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(scene,useCut?RAY_FLAG_FORCE_NON_OPAQUE:RAY_FLAG_NONE,rayMask,ray);
    float transmission=1.f;
    while(q.Proceed()){
        if(q.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const uint rayIndex=q.CandidateInstanceID();
        if(useCut&&RayUsesCutReplacement(rayIndex))continue;
        const float2 opacity=RayOpacity(rayIndex,q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics());
        if(opacity.y==2)transmission*=1.f-opacity.x;
        else if(opacity.x>0.f)q.CommitNonOpaqueTriangleHit();
    }
    if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT)return 0.f;
    if(useCut&&slot<0&&TraceCutRay(ray,rayMask,true).descriptor!=0xffffffffu)return 0.f;
    return transmission*(slot>=0?StaticShadow(p+n*.002,light,(uint)slot):1);
}
float3 BRDF(float3 base,float roughness,float metal,float3 n,float3 v,float3 l,float3 irradiance){float3 h=normalize(v+l);float nl=saturate(dot(n,l)),nv=saturate(dot(n,v)),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));float a=max(.045,roughness*roughness),a2=a*a;float denominator=nh*nh*(a2-1)+1;float d=a2/max(Pi*denominator*denominator,.0001);float k=(roughness+1)*(roughness+1)/8;float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));float3 fresnel=lerp(.04.xxx,base,metal)+(1-lerp(.04.xxx,base,metal))*pow(1-vh,5);return ((1-fresnel)*(1-metal)*base/Pi+fresnel*d*g/max(4*nv*nl,.001))*irradiance*nl;}
float3 ShadeLight(float3 p,float3 base,float roughness,float metal,float3 n,float3 v,Light light,uint index,bool indoors){


    if(light.colorPower.w<=0||all(light.colorPower.rgb==0))return 0;
    const bool room=IsRoomLight(light);
    if((indoors&&!room)||(room&&!RoomLightReaches(light,p)))return 0;
    const float3 bounce=room?RoomDiffuseBounce(light,p,n,base,metal):0.f;
    bool directional=((uint)light.parameters.z&2)!=0;
    float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;
    if(dot(n,delta)<=0)return bounce;


    float distanceSquared=dot(delta,delta);
    if(!directional&&distanceSquared>=light.positionRadius.w*light.positionRadius.w)return bounce;
    float distance=sqrt(distanceSquared);
    float3 direction=delta/max(distance,.001);
    float falloff=1;
    if(!directional){


        float ratio=distance/light.positionRadius.w;
        float ratioSquared=ratio*ratio;
        float fade=saturate(1-ratioSquared*ratioSquared);
        falloff=fade*fade/max(.02,distanceSquared);
        if(!room&&light.directionOuter.w>-1)
            falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/max(.00001,light.parameters.x-light.directionOuter.w));
        if(falloff<=0)return bounce;
    }
    float shadow=Visibility(p,n,light,index);
    if(shadow<=0)return bounce;
    return bounce+BRDF(base,roughness,metal,n,v,direction,light.colorPower.rgb*light.colorPower.w*falloff)*shadow;
}


float2 UrbanHeightUV(Surface s,float2 uv,float3 world,float3 n,float3 view){
    if(s.extra.w!=2||s.parameters.y<=0||s.metalRough==0xffffffffu)
        return uv;
    float3 dpdx=ddx(world),dpdy=ddy(world);
    float2 duvdx=ddx(uv),duvdy=ddy(uv);
    float det=duvdx.x*duvdy.y-duvdx.y*duvdy.x;
    float facing=abs(dot(view,n));
    if(abs(det)<1.e-7||facing<.09||facing>.985)
        return uv;
    float orientation=det<0?-1:1;
    float3 t=normalize((dpdx*duvdy.y-dpdy*duvdx.y)*orientation);
    float3 b=normalize((dpdy*duvdx.x-dpdx*duvdy.x)*orientation);
    float2 shift=float2(dot(view,t),dot(view,b))*
        s.parameters.yz/max(.12,facing);
    float steps=ceil(lerp(6.,14.,1.-facing));
    float2 delta=shift/steps;
    Texture2D<float4> orm=ResourceDescriptorHeap[NonUniformResourceIndex(s.metalRough)];
    SamplerState sampler=SamplerDescriptorHeap[NonUniformResourceIndex((uint)s.mapping[2].rotationSet.w)];
    float2 current=uv;
    float depth=0,stepDepth=1./steps;
    [loop]for(int index=0;index<14;index++){
        float height=orm.SampleGrad(sampler,MaterialUV(s,2,current,current),duvdx,duvdy).a;
        if(index>=int(steps)||depth>=1.-height)break;
        current-=delta;
        depth+=stepDepth;
    }
    return current;
}
float UrbanMacro(float3 p){
    return .62*sin(dot(p,float3(.113,.037,.086)))*
               sin(dot(p,float3(-.048,.091,.061)))+
           .38*sin(dot(p,float3(.29,.076,-.22)));
}
struct ColorOut{float4 direct:SV_Target0;float4 reflection:SV_Target1;};
#if OASIS_SHUTTLE_CRT
#include "shuttle_crt.hlsli"
[earlydepthstencil]
ColorOut PixelMain(Out i){
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    ColorOut screen;
    screen.direct=float4(ShuttleCrt(materials[i.material],i.uv,i.uv1,i.crt),1.f);
    screen.reflection=0.f;
    return screen;
}
#else
float3 MappedSurfaceNormal(Surface s,float3 n,float3 world,float2 uv0,float2 uv1){
    float2 xy=(SampleMaterial(s.normal,s,1,uv0,uv1).xy*2-1)*s.parameters.x;
    float3 dp1=ddx(world),dp2=ddy(world);
    float2 uv=MaterialUV(s,1,uv0,uv1),duv1=ddx(uv),duv2=ddy(uv);
    float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
    if(abs(determinant)<=.0000001f)return n;
    float direction=determinant<0?-1:1;
    float3 tangent=normalize((dp1*duv2.y-dp2*duv1.y)*direction);
    float3 bitangent=normalize((dp2*duv1.x-dp1*duv2.x)*direction);
    return normalize(tangent*xy.x+bitangent*xy.y+n*sqrt(saturate(1-dot(xy,xy))));
}
#include "reflection_hit.hlsli"
ColorOut PixelMain(Out i,bool front:SV_IsFrontFace){
    
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface s=materials[i.material];float3 base=s.colorRoughness.rgb,emission=s.emissionMetallic.rgb;float roughness=s.colorRoughness.a,metal=s.emissionMetallic.a,alpha=s.opacity.x;float3 n=normalize(i.normal);if(s.extra.z&&!front)n=-n;float3 v=normalize(eye[i.view].xyz-i.world);
    float2 surfaceUV=i.uv;

#if OASIS_MATERIAL_MASK < 0
    if(s.extra.w==5u){
        const float fresnel=.04f+.96f*pow(1.f-saturate(abs(dot(n,v))),5.f);
        const float3 mirror=reflect(-v,n);
        const float spread=.25f+.75f*saturate(AuthorRoughness(s,roughness,i.uv,false));
        const float sky=smoothstep(-spread,spread,mirror.y);
        const float3 tint=AuthorColor(s,base);
        const float3 environment=lerp(float3(.047f,.079f,.087f),float3(.19f,.28f,.36f),sky);
        const float opacity=saturate(fresnel+alpha*(1.f-fresnel)*.55f);
        ColorOut pane;
        pane.direct=float4((environment*fresnel+tint*(opacity-fresnel))/max(opacity,.001f),opacity);
        pane.reflection=float4(0,0,0,opacity);
        return pane;
    }
    if(s.extra.w==3u||s.extra.w==4u){
        const bool crtGlass=s.extra.w==4u;
        if(s.normal!=0xffffffffu)n=MappedSurfaceNormal(s,n,i.world,i.uv,i.uv1);
        const float sharpness=exp2(-2.f*(AuthorRoughness(s,roughness,i.uv,false)-roughness));
        float4 glassTexture=1.f;
        if(s.albedo!=0xffffffffu)glassTexture=SampleMaterial(s.albedo,s,0,i.uv,i.uv1);
        if(s.emissive!=0xffffffffu)emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
        const float facing=saturate(abs(dot(n,v)));
        const float edge=pow(1.f-facing,5.f);
        const float3 reflected=reflect(-v,n);
        const float sky=smoothstep(-.40f,.70f,reflected.y);
        const float3 sun=normalize(solarSun.xyz-i.world+float3(.0001f,.0002f,0.f));
        const float sunGlint=pow(saturate(dot(reflected,sun)),27.f*sharpness);
        const float panelGlint=pow(saturate(dot(reflected,normalize(float3(-.19f,.87f,.46f)))),13.f*sharpness);
        const float horizon=1.f-smoothstep(.03f,.52f,abs(reflected.y-.09f));
        const float3 cool=float3(.15f,.24f,.34f)*(.32f+.68f*sky);
        const float3 amber=float3(.42f,.24f,.085f);
        const float3 sheen=crtGlass?cool*.54f+amber*panelGlint*.50f:
            cool+float3(.14f,.18f,.23f)*horizon+amber*sunGlint;
        ColorOut glass;
        glass.direct=float4(AuthorColor(s,sheen*glassTexture.rgb)*(.40f+.60f*edge)*(1.f+.4f*s.metallicBias)+emission,
            saturate(glassTexture.a*s.opacity.w*(crtGlass?.073f+.36f*edge+.12f*panelGlint:.085f+.53f*edge+.18f*sunGlint)));
        glass.reflection=float4(0,0,0,glass.direct.a);
        return glass;
    }
#endif

    if(s.extra.w==6u){
        ColorOut fitting;
        fitting.direct=float4(base*.12f+emission,1.f);
        fitting.reflection=0;
        return fitting;
    }

    if(s.extra.w==2u&&s.extra.y==2u&&s.colorRoughness.w<.12)
        surfaceUV+=float2(-.031,.012)*timing.z;
#if OASIS_MATERIAL_MASK < 0 || (OASIS_MATERIAL_MASK & 4)
    surfaceUV=UrbanHeightUV(s,surfaceUV,i.world,n,v);
#endif
#if OASIS_MATERIAL_MASK < 0
    if(s.albedo!=0xffffffff){float4 color=SampleMaterial(s.albedo,s,0,surfaceUV,i.uv1);base*=color.rgb;alpha*=color.a;}
#elif (OASIS_MATERIAL_MASK & 1)
    {float4 color=SampleMaterial(s.albedo,s,0,surfaceUV,i.uv1);base*=color.rgb;alpha*=color.a;}
#endif
    base=AuthorColor(s,base);
    if(s.extra.y==1)clip(alpha-s.opacity.y);if(s.extra.y!=2)alpha=1;
#if OASIS_MATERIAL_MASK < 0
    if(s.normal!=0xffffffff)n=MappedSurfaceNormal(s,n,i.world,surfaceUV,i.uv1);
#elif (OASIS_MATERIAL_MASK & 2)
    n=MappedSurfaceNormal(s,n,i.world,surfaceUV,i.uv1);
#endif
    if(s.procedural.y>0){
        float3 q=i.world*s.procedural.y*6.2831853;float footprint=max(length(ddx(q)),length(ddy(q)));float filtered=exp(-footprint*footprint*.08);
        float3 gradient=float3(cos(q.x)*sin(q.z*.79),cos(q.y*.83)*sin(q.x*.71),cos(q.z*.79)*sin(q.y*.83));
        n=normalize(n+(gradient-n*dot(gradient,n))*s.procedural.x*filtered);
        base*=1+.035*filtered*sin(q.x)*sin(q.y*.83)*sin(q.z*.79);


        float edge=min(min(i.uv.x,1-i.uv.x),min(i.uv.y,1-i.uv.y));float aa=max(fwidth(edge),.0001);
        float seam=1-smoothstep(s.procedural.z-aa,s.procedural.z+aa,edge);base*=1-seam*s.procedural.w;
        roughness=clamp(roughness+seam*.08,.04,1);
    }
    float surfaceAo=1.;
#if OASIS_MATERIAL_MASK < 0
    if(s.metalRough!=0xffffffff){
#elif (OASIS_MATERIAL_MASK & 4)
    {
#endif
#if OASIS_MATERIAL_MASK < 0 || (OASIS_MATERIAL_MASK & 4)
        float4 sample=SampleMaterial(s.metalRough,s,2,surfaceUV,i.uv1);
        roughness*=sample.g;metal*=sample.b;
        if(s.extra.w==2)surfaceAo=lerp(1.,sample.r,.82);
    }
#endif
    if(s.parameters.w>0){
        float variation=UrbanMacro(i.world)*s.parameters.w;
        base*=1.+variation;
        roughness=saturate(roughness*(1.-variation*.45));
    }
#if OASIS_MATERIAL_MASK < 0
    if(s.emissive!=0xffffffff)emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
#elif (OASIS_MATERIAL_MASK & 8)
    emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
#endif
    roughness=AuthorRoughness(s,roughness,surfaceUV,false);
    metal=saturate(metal+s.metallicBias);
    if(atmosphereSphere.w>0&&atmosphereLight.w>0&&any(emission>0)){
        float3 delta=atmosphereLight.xyz-i.world;
        float3 incoming=delta*rsqrt(max(dot(delta,delta),.000001));
        float3 boundary=dot(n,incoming)>=0?n:-n;
        float3 refracted=refract(-incoming,boundary,1.0/1.45);
        float alignment=saturate(dot(-refracted,v));
        emission*=atmosphereParams.y*(.9+.1*alignment);
    }
#if OASIS_MATERIAL_MASK < 0
    float occlusion=(s.extra.x==0xffffffff?1:
        lerp(1,SampleMaterial(s.extra.x,s,4,surfaceUV,i.uv1).r,s.opacity.z))*surfaceAo;
#elif (OASIS_MATERIAL_MASK & 16)
    float occlusion=lerp(1,SampleMaterial(s.extra.x,s,4,surfaceUV,i.uv1).r,s.opacity.z)*surfaceAo;
#else
    float occlusion=surfaceAo;
#endif
    float3 color=base*(.026+.018*saturate(n.y))*occlusion+emission;
    if(sphereScene.y!=0u&&s.extra.w!=7u)color+=base*(.095f*occlusion);


    if(s.extra.w==2){


        float hemisphere=.27+.49*saturate(n.y);
        color+=base*occlusion*hemisphere*float3(.32,.39,.52);
    }
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    if(s.extra.w==1)color=base+emission;
    else if(LightCount<=4){[loop]for(uint index=0;index<LightCount;index++)color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index,s.extra.w==7u);}
    else{
        StructuredBuffer<uint> directionalCount=ResourceDescriptorHeap[DirectionalCount];
        StructuredBuffer<uint> directional=ResourceDescriptorHeap[DirectionalIndices];
        [loop]for(uint j=0;j<directionalCount[0];++j){
            uint index=directional[j];
            color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index,s.extra.w==7u);
        }
        StructuredBuffer<uint2> clusters=ResourceDescriptorHeap[Clusters];
        StructuredBuffer<uint> indices=ResourceDescriptorHeap[ClusterIndices];
        uint2 span=clusters[ClusterIndex(uint2(i.position.xy),i.position.z,i.view)];
        if(span.y==ClusterOverflow){


            uint cursor=1u;
            [loop]while(true){
                uint index=LightBvhNextLight(cursor,LightBvhLeafCount,
                                            i.world,0.0f);
                if(index==0xffffffffu)break;
                if(((uint)lights[index].parameters.z&2u)==0u)
                    color+=ShadeLight(i.world,base,roughness,metal,n,v,
                                      lights[index],index,s.extra.w==7u);
            }
        }else{
            [loop]for(uint j=0;j<span.y;++j){
                uint index=indices[span.x+j];
                color+=ShadeLight(i.world,base,roughness,metal,n,v,
                                  lights[index],index,s.extra.w==7u);
            }
        }
    }


    bool closeExterior=s.extra.w!=2||dot(i.world-eye[i.view].xyz,
        i.world-eye[i.view].xyz)<=100.;
    bool reflective=topology.z!=0&&closeExterior&&s.extra.w!=1&&s.extra.w!=7u&&((metal>.55&&roughness<.32)||(Quality>0&&roughness<(Quality==1?.18:Quality==2?.28:.38)))&&max(emission.x,max(emission.y,emission.z))<1;float3 reflection=0;
    if(reflective){
        reflection=float3(.025,.035,.055);
        if(RayInstanceCount!=0){
            const bool useCut=CutRayActive();
            RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
            RayDesc ray;
            ray.Origin=i.world+n*.003f;ray.Direction=reflect(-v,n);
            ray.TMin=.003f;ray.TMax=Quality<=1?5.f:12.f;
            RayQuery<RAY_FLAG_NONE> query;
            query.TraceRayInline(scene,useCut?RAY_FLAG_FORCE_NON_OPAQUE:RAY_FLAG_NONE,7u,ray);
            while(query.Proceed()){
                if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
                const uint rayIndex=query.CandidateInstanceID();
                if(useCut&&RayUsesCutReplacement(rayIndex))continue;
                const float2 opacity=RayOpacity(rayIndex,query.CandidatePrimitiveIndex(),
                                                query.CandidateTriangleBarycentrics());
                if(opacity.x>=.5f)query.CommitNonOpaqueTriangleHit();
            }
            const bool regularHit=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
            if(regularHit)ray.TMax=query.CommittedRayT();
            CutRayHit cut;
            cut.descriptor=0xffffffffu;
            if(useCut)cut=TraceCutRay(ray,7u,false);
            if(cut.descriptor!=0xffffffffu){
                StructuredBuffer<CutTriangle> triangles=ResourceDescriptorHeap[NonUniformResourceIndex(cut.descriptor)];
                const CutTriangle face=triangles[cut.triangleIndex];
                const float3 hitPosition=ray.Origin+ray.Direction*cut.distance;
                reflection=ReflectedSurfaceRadiance(face.material,face.a,face.b,face.c,cut.barycentrics,
                    hitPosition,-ray.Direction);
            }else if(regularHit){
                const float3 hitPosition=ray.Origin+ray.Direction*query.CommittedRayT();
                reflection=ReflectedHitRadiance(query.CommittedInstanceID(),query.CommittedPrimitiveIndex(),
                    query.CommittedTriangleBarycentrics(),hitPosition,-ray.Direction);
            }
        }
        reflection*=lerp(.04.xxx,base,metal)+(1-lerp(.04.xxx,base,metal))*pow(1-saturate(dot(n,v)),5);
        reflection*=1-roughness;
    }
    ColorOut output;output.direct=float4(color,saturate(alpha));output.reflection=float4(reflection,s.extra.y==2?saturate(alpha):(reflective?1:0));return output;
}
#endif
