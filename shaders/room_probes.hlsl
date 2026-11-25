#include "common.hlsli"
#include "cut_ray.hlsli"

float3 DiffuseRadiance(Light light,RayDesc ray,float distance,uint material,Vertex a,Vertex b,Vertex c,float2 bary){
    const float3 position=ray.Origin+ray.Direction*distance;
    if(!RoomLightReaches(light,position))return 0.f;
    const float3 weights=float3(1.f-bary.x-bary.y,bary);
    const float2 uv0=a.uv*weights.x+b.uv*weights.y+c.uv*weights.z;
    const float2 uv1=a.uv1*weights.x+b.uv1*weights.y+c.uv1*weights.z;
    if(uv1.x<-32768.f)return 0.f;
    StructuredBuffer<Surface> surfaces=ResourceDescriptorHeap[Materials];
    const Surface surface=surfaces[material];
    float3 diffuse=surface.colorRoughness.rgb;
    if(surface.albedo!=0xffffffffu)diffuse*=SampleMaterialLevel(surface.albedo,surface,0,uv0,uv1).rgb;
    diffuse=AuthorColor(surface,diffuse);
    float metal=surface.emissionMetallic.w;
    if(surface.metalRough!=0xffffffffu)
        metal*=SampleMaterialLevel(surface.metalRough,surface,2,uv0,uv1).b;
    float3 normal=a.normal*weights.x+b.normal*weights.y+c.normal*weights.z;
    normal*=rsqrt(max(dot(normal,normal),1.e-8f));
    const float3 toLight=light.positionRadius.xyz-position;
    const float squared=max(dot(toLight,toLight),.02f);
    const float cosine=saturate(dot(normal,toLight)*rsqrt(max(dot(toLight,toLight),1.e-8f)));
    const float radius=light.positionRadius.w;
    const float ratio=dot(toLight,toLight)/max(radius*radius,1.e-5f);
    const float edge=saturate(1.f-ratio*ratio);
    return diffuse*(1.f-saturate(metal+surface.metallicBias))*light.colorPower.rgb*
        (light.colorPower.w*cosine*edge*edge/(Pi*squared));
}

float4 TraceRoomProbe(Light light,float3 direction){
    RayDesc ray;
    ray.Origin=RoomProbeOrigin(light);
    ray.Direction=direction;
    ray.TMin=.002f;
    ray.TMax=light.positionRadius.w;
    const bool useCut=CutRayActive();
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene,useCut?RAY_FLAG_FORCE_NON_OPAQUE:RAY_FLAG_NONE,3u,ray);
    while(query.Proceed()){
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const uint rayIndex=query.CandidateInstanceID();
        if(useCut&&RayUsesCutReplacement(rayIndex))continue;
        const float2 opacity=RayOpacity(rayIndex,query.CandidatePrimitiveIndex(),query.CandidateGeometryIndex(),
                                       query.CandidateTriangleBarycentrics());
        if(opacity.y!=2.f&&opacity.x>0.f)query.CommitNonOpaqueTriangleHit();
    }
    const bool regularHit=query.CommittedStatus()==COMMITTED_TRIANGLE_HIT;
    if(regularHit)ray.TMax=query.CommittedRayT();
    if(useCut){
        const CutRayHit cut=TraceCutRay(ray,3u,false);
        if(cut.descriptor!=0xffffffffu){
            StructuredBuffer<CutTriangle> faces=ResourceDescriptorHeap[NonUniformResourceIndex(cut.descriptor)];
            const CutTriangle face=faces[cut.triangleIndex];
            return float4(DiffuseRadiance(light,ray,cut.distance,face.material,face.a,face.b,face.c,
                cut.barycentrics),cut.distance/light.positionRadius.w);
        }
    }
    if(!regularHit)return float4(0,0,0,1);
    Vertex a,b,c;
    const uint material=LoadRayTriangle(query.CommittedInstanceID(),query.CommittedPrimitiveIndex(),query.CommittedGeometryIndex(),a,b,c);
    return float4(DiffuseRadiance(light,ray,ray.TMax,material,a,b,c,query.CommittedTriangleBarycentrics()),
        ray.TMax/light.positionRadius.w);
}

groupshared float4 probeLobes[6];
groupshared float probeMoments[6];

[numthreads(6,1,1)]
void UpdateRoomProbes(uint3 group:SV_GroupID,uint lobe:SV_GroupIndex){
    const uint index=(objectIndex+group.x)%LightCount;
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    const Light light=lights[index];
    if(!IsRoomLight(light))return;
    const float3 axis=RoomProbeAxis(lobe);
    const float3 tangent=normalize(cross(axis,lobe>=2u&&lobe<4u?float3(1,0,0):float3(0,1,0)));
    const float3 bitangent=cross(axis,tangent);
    const float3 spread=.34f*tangent+.21f*bitangent;
    const float4 a=TraceRoomProbe(light,normalize(axis+spread));
    const float4 b=TraceRoomProbe(light,normalize(axis-spread));
    probeLobes[lobe]=.5f*(a+b);
    probeMoments[lobe]=.5f*(a.w*a.w+b.w*b.w);
    GroupMemoryBarrierWithGroupSync();
    if(lobe==0u){
        RoomProbe probe;
        [unroll]for(uint i=0;i<6u;++i)probe.radianceDistance[i]=probeLobes[i];
        probe.secondMoment[0]=float4(probeMoments[0],probeMoments[1],probeMoments[2],probeMoments[3]);
        probe.secondMoment[1]=float4(probeMoments[4],probeMoments[5],0,0);
        probe.cutVersion=RoomProbeCutVersion();
        probe.reserved=0u;
        RWStructuredBuffer<RoomProbe> probes=ResourceDescriptorHeap[DomainResource(
            ResourceDomainLighting,LightingRoomProbesUAV)];
        probes[index]=probe;
    }
}
