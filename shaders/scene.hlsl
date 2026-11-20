#include "common.hlsli"
#ifndef OASIS_MATERIAL_MASK
#define OASIS_MATERIAL_MASK -1
#endif
struct Out {
    float4 position:SV_Position;
    float3 world:POSITION0;
    float3 normal:NORMAL0;
    float2 uv:TEXCOORD0;
    float2 uv1:TEXCOORD3;
    nointerpolation uint material:TEXCOORD1;
    nointerpolation uint view:TEXCOORD2;
};
struct CutTriangle {
    Vertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
[outputtopology("triangle")]
[numthreads(64,1,1)]
void MeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,
              out vertices Out verts[64],out indices uint3 tris[124]){
    uint4 generation=0;
    if(resourceAbi.w!=0xffffffffu){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
        generation=state[0];
    }
    const bool isCut=param2==1u;
    const uint base=group.x*21u;
    uint cutCount=0u;
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    if(isCut){
        cutCount=generation.y!=0u&&base<generation.x?
            min(21u,generation.x-base):0u;
        vertexCount=cutCount*3u;
        triangleCount=cutCount;
    }else{
        const uint instanceIndex=objectIndex+group.y;
        inst=LoadInstance(instanceIndex);
        const bool replaced=generation.y!=0u&&
            instanceIndex<cutGeometry.w&&
            (inst.id&0x80000000u)!=0u&&(inst.flags&19u)==0u;
        if(!replaced&&SphereNativeInstance(instanceIndex)){
            StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];
            MeshRange range=ranges[inst.mesh];
            StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[Meshlets];
            m=meshlets[range.firstMeshlet+group.x];
            vertexCount=m.vertexCount;
            triangleCount=m.triangleCount;
        }
    }
    // DXIL mesh validation requires exactly one dominating output-count call.
    SetMeshOutputCounts(vertexCount,triangleCount);
    if(isCut){
        if(tid<vertexCount){
            uint descriptor=generation.y==1u?cutGeometry.y:cutGeometry.z;
            StructuredBuffer<CutTriangle> source=
                ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
            CutTriangle cutFace=source[base+tid/3u];
            Vertex v=cutFace.a;
            if(tid%3u==1u)v=cutFace.b;
            else if(tid%3u==2u)v=cutFace.c;
            verts[tid].position=mul(float4(v.position,1),vp[view]);
            verts[tid].world=v.position;
            verts[tid].normal=normalize(v.normal);
            verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;
            verts[tid].material=cutFace.material;verts[tid].view=view;
        }
        if(tid<triangleCount)
            tris[tid]=uint3(tid*3u,tid*3u+1u,tid*3u+2u);
    }else{
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
            verts[tid].position=mul(float4(world,1),vp[view]);
            verts[tid].world=world;
            verts[tid].normal=normalize(Rotate(v.normal/
                (sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),
                inst.rotation));
            verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;
            verts[tid].material=inst.material;verts[tid].view=view;
        }
        for(uint t=tid;t<triangleCount;t+=64u){
            StructuredBuffer<uint3> triangles=
                ResourceDescriptorHeap[Triangles];
            tris[t]=triangles[m.triangleOffset+t];
        }
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
    const bool isCut=param2==1u;
    const uint base=group.x*21u;
    uint cutCount=0u;
    uint vertexCount=0u,triangleCount=0u;
    GpuInstance inst=(GpuInstance)0;
    Meshlet m=(Meshlet)0;
    if(isCut){
        cutCount=generation.y!=0u&&base<generation.x?
            min(21u,generation.x-base):0u;
        vertexCount=cutCount*3u;
        triangleCount=cutCount;
    }else{
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
    }
    // One call dominates every possible mesh output store.
    SetMeshOutputCounts(vertexCount,triangleCount);
    if(isCut){
        if(tid<vertexCount){
            uint descriptor=generation.y==1u?cutGeometry.y:cutGeometry.z;
            StructuredBuffer<CutTriangle> source=
                ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
            CutTriangle cutFace=source[base+tid/3u];
            Vertex v=cutFace.a;
            if(tid%3u==1u)v=cutFace.b;
            else if(tid%3u==2u)v=cutFace.c;
            verts[tid].position=mul(float4(v.position,1),shadowVP[passIndex]);
            verts[tid].world=v.position;
            verts[tid].normal=normalize(v.normal);
            verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;
            verts[tid].material=cutFace.material;verts[tid].view=0;
        }
        if(tid<triangleCount){
            uint descriptor=generation.y==1u?cutGeometry.y:cutGeometry.z;
            StructuredBuffer<CutTriangle> source=
                ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
            tris[tid]=(source[base+tid].pad0&4u)!=0u?
                uint3(0u,0u,0u):
                uint3(3u*tid,3u*tid+1u,3u*tid+2u);
        }
    }else{
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
    uint local=group.x*21u;
    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
    uint blockDescriptor=cacheState[0].x==0u?cutRaster.y:cutRaster.z;
    StructuredBuffer<CutRasterBlock> blocks=
        ResourceDescriptorHeap[NonUniformResourceIndex(blockDescriptor)];
    CutRasterBlock block=blocks[objectIndex];
    const uint faceCount=SphereNativeInstance(block.instanceId)&&local<param0?min(21u,param0-local):0u;
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
    uint local=group.x*21u;
    uint faceCount=local<param0?min(21u,param0-local):0u;
    SetMeshOutputCounts(faceCount*3u,faceCount);

    StructuredBuffer<uint4> cacheState=ResourceDescriptorHeap[cutRaster.x];
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
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;q.TraceRayInline(scene,RAY_FLAG_NONE,slot>=0?2:3,ray);float transmission=1;while(q.Proceed()){if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE){float2 opacity=RayOpacity(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics());if(opacity.y==2)transmission*=1-opacity.x;else if(opacity.x>0)q.CommitNonOpaqueTriangleHit();}}
    return q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?0:transmission*(slot>=0?StaticShadow(p+n*.002,light,(uint)slot):1);
}
float3 BRDF(float3 base,float roughness,float metal,float3 n,float3 v,float3 l,float3 irradiance){float3 h=normalize(v+l);float nl=saturate(dot(n,l)),nv=saturate(dot(n,v)),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));float a=max(.045,roughness*roughness),a2=a*a;float denominator=nh*nh*(a2-1)+1;float d=a2/max(Pi*denominator*denominator,.0001);float k=(roughness+1)*(roughness+1)/8;float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));float3 fresnel=lerp(.04.xxx,base,metal)+(1-lerp(.04.xxx,base,metal))*pow(1-vh,5);return ((1-fresnel)*(1-metal)*base/Pi+fresnel*d*g/max(4*nv*nl,.001))*irradiance*nl;}
float3 ShadeLight(float3 p,float3 base,float roughness,float metal,float3 n,float3 v,Light light,uint index){


    if(light.colorPower.w<=0||all(light.colorPower.rgb==0))return 0;
    bool directional=((uint)light.parameters.z&2)!=0;
    float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;
    if(dot(n,delta)<=0)return 0;


    float distanceSquared=dot(delta,delta);
    if(!directional&&distanceSquared>=light.positionRadius.w*light.positionRadius.w)return 0;
    float distance=sqrt(distanceSquared);
    float3 direction=delta/max(distance,.001);
    float falloff=1;
    if(!directional){


        float ratio=distance/light.positionRadius.w;
        float ratioSquared=ratio*ratio;
        float fade=saturate(1-ratioSquared*ratioSquared);
        falloff=fade*fade/max(.02,distanceSquared);
        if(light.directionOuter.w>-1)
            falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/max(.00001,light.parameters.x-light.directionOuter.w));
        if(falloff<=0)return 0;
    }
    float shadow=Visibility(p,n,light,index);
    if(shadow<=0)return 0;
    return BRDF(base,roughness,metal,n,v,direction,light.colorPower.rgb*light.colorPower.w*falloff)*shadow;
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
#include "reflection_hit.hlsli"
struct ColorOut{float4 direct:SV_Target0;float4 reflection:SV_Target1;};
ColorOut PixelMain(Out i,bool front:SV_IsFrontFace){
    
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface s=materials[i.material];float3 base=s.colorRoughness.rgb,emission=s.emissionMetallic.rgb;float roughness=s.colorRoughness.a,metal=s.emissionMetallic.a,alpha=s.opacity.x;float3 n=normalize(i.normal);if(s.extra.z&&!front)n=-n;float3 v=normalize(eye[i.view].xyz-i.world);
    float2 surfaceUV=i.uv;


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
    if(s.extra.y==1)clip(alpha-s.opacity.y);if(s.extra.y!=2)alpha=1;
#if OASIS_MATERIAL_MASK < 0
    if(s.normal!=0xffffffff){float2 xy=(SampleMaterial(s.normal,s,1,surfaceUV,i.uv1).xy*2-1)*s.parameters.x;float3 dp1=ddx(i.world),dp2=ddy(i.world);float2 uv=MaterialUV(s,1,surfaceUV,i.uv1),duv1=ddx(uv),duv2=ddy(uv);float determinant=duv1.x*duv2.y-duv1.y*duv2.x;if(abs(determinant)>.0000001){float signUV=determinant<0?-1:1;float3 tangent=normalize((dp1*duv2.y-dp2*duv1.y)*signUV);float3 bitangent=normalize((dp2*duv1.x-dp1*duv2.x)*signUV);n=normalize(tangent*xy.x+bitangent*xy.y+n*sqrt(saturate(1-dot(xy,xy))));}}
#elif (OASIS_MATERIAL_MASK & 2)
    {float2 xy=(SampleMaterial(s.normal,s,1,surfaceUV,i.uv1).xy*2-1)*s.parameters.x;float3 dp1=ddx(i.world),dp2=ddy(i.world);float2 uv=MaterialUV(s,1,surfaceUV,i.uv1),duv1=ddx(uv),duv2=ddy(uv);float determinant=duv1.x*duv2.y-duv1.y*duv2.x;if(abs(determinant)>.0000001){float signUV=determinant<0?-1:1;float3 tangent=normalize((dp1*duv2.y-dp2*duv1.y)*signUV);float3 bitangent=normalize((dp2*duv1.x-dp1*duv2.x)*signUV);n=normalize(tangent*xy.x+bitangent*xy.y+n*sqrt(saturate(1-dot(xy,xy))));}}
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
    if(sphereScene.y!=0u)color+=base*(.095f*occlusion);


    if(s.extra.w==2){


        float hemisphere=.27+.49*saturate(n.y);
        color+=base*occlusion*hemisphere*float3(.32,.39,.52);
    }
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    if(s.extra.w==1)color=base+emission;
    else if(LightCount<=4){[loop]for(uint index=0;index<LightCount;index++)color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index);}
    else{
        StructuredBuffer<uint> directionalCount=ResourceDescriptorHeap[DirectionalCount];
        StructuredBuffer<uint> directional=ResourceDescriptorHeap[DirectionalIndices];
        [loop]for(uint j=0;j<directionalCount[0];++j){
            uint index=directional[j];
            color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index);
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
                                      lights[index],index);
            }
        }else{
            [loop]for(uint j=0;j<span.y;++j){
                uint index=indices[span.x+j];
                color+=ShadeLight(i.world,base,roughness,metal,n,v,
                                  lights[index],index);
            }
        }
    }


    bool closeExterior=s.extra.w!=2||dot(i.world-eye[i.view].xyz,
        i.world-eye[i.view].xyz)<=100.;
    bool reflective=topology.z!=0&&closeExterior&&s.extra.w!=1&&((metal>.55&&roughness<.32)||(Quality>0&&roughness<(Quality==1?.18:Quality==2?.28:.38)))&&max(emission.x,max(emission.y,emission.z))<1;float3 reflection=0;
    if(reflective){
        reflection=float3(.025,.035,.055);
        if(RayInstanceCount!=0){
            bool cutPublished=false;
            if(resourceAbi.w!=0xffffffffu){
                StructuredBuffer<uint4> cutState=ResourceDescriptorHeap[resourceAbi.w];
                cutPublished=cutState[0].y!=0u;
            }
            RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
            RayDesc ray;
            ray.Origin=i.world+n*.003f;ray.Direction=reflect(-v,n);
            ray.TMin=.003f;ray.TMax=Quality<=1?5.f:12.f;
            RayQuery<RAY_FLAG_NONE> query;
            // The TLAS still holds the original solid. Until cut faces have
            // a ray publication, use the environment where they would reflect.
            const uint flags=cutPublished?RAY_FLAG_FORCE_NON_OPAQUE:RAY_FLAG_NONE;
            query.TraceRayInline(scene,flags,7u,ray);
            while(query.Proceed()){
                if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
                const uint rayIndex=query.CandidateInstanceID();
                if(cutPublished){
                    const uint sourceIndex=RenderInstanceForRay(rayIndex);
                    if(sourceIndex<cutGeometry.w){
                        const GpuInstance source=LoadInstanceStable(sourceIndex);
                        if(source.id==(0x80000000u+sourceIndex)&&(source.flags&19u)==0u)continue;
                    }
                }
                const float2 opacity=RayOpacity(rayIndex,query.CandidatePrimitiveIndex(),
                                                query.CandidateTriangleBarycentrics());
                if(opacity.x>=.5f)query.CommitNonOpaqueTriangleHit();
            }
            if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
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
