#include "common.hlsli"
struct Out {float4 position:SV_Position;float3 world:POSITION0;float3 normal:NORMAL0;float2 uv:TEXCOORD0;float2 uv1:TEXCOORD3;nointerpolation uint material:TEXCOORD1;nointerpolation uint view:TEXCOORD2;nointerpolation uint sourceId:TEXCOORD4;};
[outputtopology("triangle")]
[numthreads(64,1,1)]
void MeshMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,uint view:SV_ViewID,out vertices Out verts[64],out indices uint3 tris[124]){
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[Meshlets];StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];StructuredBuffer<uint> remap=ResourceDescriptorHeap[Remap];StructuredBuffer<uint3> triangles=ResourceDescriptorHeap[Triangles];GpuInstance inst=LoadInstance(objectIndex);MeshRange range=ranges[inst.mesh];Meshlet m=meshlets[range.firstMeshlet+group.x];SetMeshOutputCounts(m.vertexCount,m.triangleCount);
    if(tid<m.vertexCount){StructuredBuffer<Vertex> deformed=ResourceDescriptorHeap[DynamicVertices];Vertex v=vertices[remap[m.vertexOffset+tid]];if(inst.flags&2)v=deformed[remap[m.vertexOffset+tid]];float3 world=World(v.position,inst);verts[tid].position=mul(float4(world,1),vp[view]);verts[tid].world=world;verts[tid].normal=normalize(Rotate(v.normal/(sign(inst.scale.xyz)*max(abs(inst.scale.xyz),.000001)),inst.rotation));verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;verts[tid].material=inst.material;verts[tid].view=view;verts[tid].sourceId=inst.id;}for(uint t=tid;t<m.triangleCount;t+=64)tris[t]=triangles[m.triangleOffset+t];
}
[outputtopology("triangle")]
[numthreads(64,1,1)]
void ShadowMain(uint tid:SV_GroupIndex,uint3 group:SV_GroupID,out vertices Out verts[64],out indices uint3 tris[124]){
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];StructuredBuffer<Meshlet> meshlets=ResourceDescriptorHeap[Meshlets];StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];StructuredBuffer<uint> remap=ResourceDescriptorHeap[Remap];StructuredBuffer<uint3> triangles=ResourceDescriptorHeap[Triangles];GpuInstance instance=LoadInstanceStable(objectIndex);Meshlet meshlet=meshlets[ranges[instance.mesh].firstMeshlet+group.x];SetMeshOutputCounts(meshlet.vertexCount,meshlet.triangleCount);
    if(tid<meshlet.vertexCount){Vertex v=vertices[remap[meshlet.vertexOffset+tid]];float3 world=World(v.position,instance);verts[tid].position=mul(float4(world,1),shadowVP[passIndex]);verts[tid].world=world;verts[tid].normal=Rotate(v.normal,instance.rotation);verts[tid].uv=v.uv;verts[tid].uv1=v.uv1;verts[tid].material=instance.material;verts[tid].view=0;verts[tid].sourceId=instance.id;}
    for(uint i=tid;i<meshlet.triangleCount;i+=64)tris[i]=triangles[meshlet.triangleOffset+i];
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
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;q.TraceRayInline(scene,RAY_FLAG_NONE,slot>=0?2:3,ray);float transmission=1;while(q.Proceed()){if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE){float2 opacity=RayOpacity(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics());if(opacity.y==2)transmission*=1-opacity.x;else if(opacity.x>0)q.CommitNonOpaqueTriangleHit();}}
    return q.CommittedStatus()==COMMITTED_TRIANGLE_HIT?0:transmission*(slot>=0?StaticShadow(p+n*.002,light,(uint)slot):1);
}
float3 BRDF(float3 base,float roughness,float metal,float3 n,float3 v,float3 l,float3 irradiance){float3 h=normalize(v+l);float nl=saturate(dot(n,l)),nv=saturate(dot(n,v)),nh=saturate(dot(n,h)),vh=saturate(dot(v,h));float a=max(.045,roughness*roughness),a2=a*a;float denominator=nh*nh*(a2-1)+1;float d=a2/max(Pi*denominator*denominator,.0001);float k=(roughness+1)*(roughness+1)/8;float g=(nv/(nv*(1-k)+k))*(nl/(nl*(1-k)+k));float3 fresnel=lerp(.04.xxx,base,metal)+(1-lerp(.04.xxx,base,metal))*pow(1-vh,5);return ((1-fresnel)*(1-metal)*base/Pi+fresnel*d*g/max(4*nv*nl,.001))*irradiance*nl;}
float3 ShadeLight(float3 p,float3 base,float roughness,float metal,float3 n,float3 v,Light light,uint index){
    // Reject zero-contribution lights before DXR visibility and optical
    // transmission. BRDF ends in *NdotL; for NdotL<=0 the exact contribution
    // is zero regardless of the shadow ray or the material's roughness.
    if(light.colorPower.w<=0)return 0;
    bool directional=((uint)light.parameters.z&2)!=0;
    float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-p;
    if(dot(n,delta)<=0)return 0;

    // Outside a point light's finite range the original smooth attenuation
    // is exactly zero. Avoid the square root, shadow query and BRDF entirely.
    float distanceSquared=dot(delta,delta);
    if(!directional&&distanceSquared>=light.positionRadius.w*light.positionRadius.w)return 0;
    float distance=sqrt(distanceSquared);
    float3 direction=delta/max(distance,.001);
    float falloff=1;
    if(!directional){
        // Exact polynomial powers (r^4 and (1-r^4)^2), not generic pow().
        // The falloff law, range, spot cone and shading response are preserved.
        float ratio=distance/light.positionRadius.w;
        float ratioSquared=ratio*ratio;
        float fade=saturate(1-ratioSquared*ratioSquared);
        falloff=fade*fade/max(.02,distanceSquared);
        if(light.directionOuter.w>-1)
            falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/max(.00001,light.parameters.x-light.directionOuter.w));
        if(falloff<=0)return 0;
    }
    float shadow=Visibility(p,n,light,index)*PopulationTransmission(index,p);
    return BRDF(base,roughness,metal,n,v,direction,light.colorPower.rgb*light.colorPower.w*falloff)*shadow;
}
// Urban surfaces use the metric height in ORM alpha. Parallax is in
// texture UV units derived from material tile size, never arbitrary mesh scale.
// Only the micro relief is parallax-mapped: road camber/repair depressions
// already have real triangles and appear consistently in depth/shadows/DXR.
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
[earlydepthstencil]
ColorOut PixelMain(Out i,bool front:SV_IsFrontFace){
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];Surface s=materials[i.material];float3 base=s.colorRoughness.rgb,emission=s.emissionMetallic.rgb;float roughness=s.colorRoughness.a,metal=s.emissionMetallic.a,alpha=s.opacity.x;float3 n=normalize(i.normal);if(s.extra.z&&!front)n=-n;float3 v=normalize(eye[i.view].xyz-i.world);
    float2 surfaceUV=UrbanHeightUV(s,i.uv,i.world,n,v);
    if(s.albedo!=0xffffffff){float4 color=SampleMaterial(s.albedo,s,0,surfaceUV,i.uv1);base*=color.rgb;alpha*=color.a;}if(s.extra.y==1)clip(alpha-s.opacity.y);if(s.extra.y!=2)alpha=1;
    if(s.normal!=0xffffffff){float2 xy=(SampleMaterial(s.normal,s,1,surfaceUV,i.uv1).xy*2-1)*s.parameters.x;float3 dp1=ddx(i.world),dp2=ddy(i.world);float2 uv=MaterialUV(s,1,surfaceUV,i.uv1),duv1=ddx(uv),duv2=ddy(uv);float determinant=duv1.x*duv2.y-duv1.y*duv2.x;if(abs(determinant)>.0000001){float signUV=determinant<0?-1:1;float3 tangent=normalize((dp1*duv2.y-dp2*duv1.y)*signUV);float3 bitangent=normalize((dp2*duv1.x-dp1*duv2.x)*signUV);n=normalize(tangent*xy.x+bitangent*xy.y+n*sqrt(saturate(1-dot(xy,xy))));}}
    if(s.procedural.y>0){
        float3 q=i.world*s.procedural.y*6.2831853;float footprint=max(length(ddx(q)),length(ddy(q)));float filtered=exp(-footprint*footprint*.08);
        float3 gradient=float3(cos(q.x)*sin(q.z*.79),cos(q.y*.83)*sin(q.x*.71),cos(q.z*.79)*sin(q.y*.83));
        n=normalize(n+(gradient-n*dot(gradient,n))*s.procedural.x*filtered);
        base*=1+.035*filtered*sin(q.x)*sin(q.y*.83)*sin(q.z*.79);
        // Box UV borders provide a continuous, darkened seam without added
        // geometry or rays. Derivatives keep distant junctions stable.
        float edge=min(min(i.uv.x,1-i.uv.x),min(i.uv.y,1-i.uv.y));float aa=max(fwidth(edge),.0001);
        float seam=1-smoothstep(s.procedural.z-aa,s.procedural.z+aa,edge);base*=1-seam*s.procedural.w;
        roughness=clamp(roughness+seam*.08,.04,1);
    }
    float surfaceAo=1.;
    if(s.metalRough!=0xffffffff){
        float4 sample=SampleMaterial(s.metalRough,s,2,surfaceUV,i.uv1);
        roughness*=sample.g;metal*=sample.b;
        if(s.extra.w==2)surfaceAo=lerp(1.,sample.r,.82);
    }
    if(s.parameters.w>0){
        float variation=UrbanMacro(i.world)*s.parameters.w;
        base*=1.+variation;
        roughness=saturate(roughness*(1.-variation*.45));
    }
    if(s.emissive!=0xffffffff)emission*=SampleMaterial(s.emissive,s,3,i.uv,i.uv1).rgb;
    emission*=PopulationSourceTransmission(i.sourceId,eye[i.view].xyz);
    float occlusion=(s.extra.x==0xffffffff?1:
        lerp(1,SampleMaterial(s.extra.x,s,4,surfaceUV,i.uv1).r,s.opacity.z))*surfaceAo;
    float3 color=base*(.026+.018*saturate(n.y))*occlusion+emission;
    // unlit=2 is a skylit exterior material, not an unlit surface.
    // Diffuse overcast fill works even under a wall or forest canopy;
    // the sodium point lights remain independent accents.
    if(s.extra.w==2){
        // Moonless overcast winter night: diffuse sky bounce, not sunlight.
        // Preserve physically located sodium lamp lights below.
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
        [loop]for(uint j=0;j<span.y;++j){
            uint index=indices[span.x+j];
            color+=ShadeLight(i.world,base,roughness,metal,n,v,lights[index],index);
        }
    }
    // topology.z is set only when the CPU can prove at least one submitted surface may reflect.
    // The wet winter street only needs glossy ray-query reflections within
    // 10 m of the headset. Indoor reflection quality remains unrestricted.
    bool closeExterior=s.extra.w!=2||dot(i.world-eye[i.view].xyz,
        i.world-eye[i.view].xyz)<=100.;
    bool reflective=topology.z!=0&&closeExterior&&s.extra.w!=1&&((metal>.55&&roughness<.32)||(Quality>0&&roughness<(Quality==1?.18:Quality==2?.28:.38)))&&max(emission.x,max(emission.y,emission.z))<1;float3 reflection=0;
    if(reflective){if(RayInstanceCount==0)reflection=float3(.025,.035,.055);else{RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];RayDesc ray;ray.Origin=i.world+n*.003;ray.Direction=reflect(-v,n);ray.TMin=.003;ray.TMax=Quality<=1?5:12;RayQuery<RAY_FLAG_NONE> q;q.TraceRayInline(scene,RAY_FLAG_NONE,7,ray);while(q.Proceed()){if(q.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE){float2 opacity=RayOpacity(q.CandidateInstanceID(),q.CandidatePrimitiveIndex(),q.CandidateTriangleBarycentrics());if(opacity.x>=.5)q.CommitNonOpaqueTriangleHit();}}
        if(q.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
            float3 hitPosition=ray.Origin+ray.Direction*q.CommittedRayT();
            reflection=ReflectedHitRadiance(q.CommittedInstanceID(),q.CommittedPrimitiveIndex(),
                q.CommittedTriangleBarycentrics(),hitPosition,-ray.Direction,i.world);
        }else reflection=float3(.025,.035,.055);}reflection*=lerp(.04.xxx,base,metal)+(1-lerp(.04.xxx,base,metal))*pow(1-saturate(dot(n,v)),5);reflection*=1-roughness;
    }
    ColorOut output;output.direct=float4(color,saturate(alpha));output.reflection=float4(reflection,s.extra.y==2?saturate(alpha):(reflective?1:0));return output;
}
