


float3 ReflectedLightContribution(float3 hitPosition,float3 n,float3 v,
                                  float3 base,float roughness,float metal,Light light,uint index,bool indoors,float dielectricF0,float diffuseTransmission){

        if(light.colorPower.w<=0||all(light.colorPower.rgb==0))return 0;
        const bool room=IsRoomLight(light);
        if((indoors&&!room)||(room&&!RoomLightReaches(light,hitPosition)))return 0;
        const float3 bounce=room?RoomDiffuseBounce(light,index,hitPosition,n,base,metal):0.f;
        bool directional=((uint)light.parameters.z&2)!=0;
        float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-hitPosition;
        if(diffuseTransmission<=0.f&&dot(n,delta)<=0.f)return bounce;
        float squared=dot(delta,delta);
        if(!directional&&squared>=light.positionRadius.w*light.positionRadius.w)return bounce;
        float distance=sqrt(squared);
        float3 direction=delta/max(distance,.001);
        float falloff=1;
        if(!directional) {
            float ratio=distance/light.positionRadius.w;
            float ratio2=ratio*ratio;
            float fade=saturate(1-ratio2*ratio2);
            falloff=fade*fade/max(.02,squared);
            if(!room&&light.directionOuter.w>-1)
                falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/
                    max(.00001,light.parameters.x-light.directionOuter.w));
            if(falloff<=0)return bounce;
        }


        float shadow=1;
        if(((uint)light.parameters.z&1)!=0&&light.parameters.y>=0)
            shadow=StaticShadow(hitPosition+(diffuseTransmission>0.f&&dot(n,direction)<0.f?-n:n)*.002,light,(uint)light.parameters.y);
        if(shadow<=0)return bounce;
        return bounce+ThinSurfaceBRDF(base,roughness,metal,n,v,direction,
                    light.colorPower.rgb*light.colorPower.w*falloff,dielectricF0,diffuseTransmission)*shadow;
    }

// Convert a ray-cone diameter in world units to the footprint of the actual
// triangle UV mapping. This works for scaled instances, cut faces, rotated
// texture maps and both UV sets, without assuming that a metre equals a texel.
struct ReflectionFootprint {
    float3 dual1,dual2;
    float width;
};
ReflectionFootprint MakeReflectionFootprint(Vertex a,Vertex b,Vertex c,float3 rayDirection,float diameter){
    const float3 e1=b.position-a.position,e2=c.position-a.position;
    const float3 area=cross(e1,e2);
    const float area2=dot(area,area);
    ReflectionFootprint f;
    const float inverseArea=area2>1.e-24f?rcp(area2):0.f;
    f.dual1=cross(e2,area)*inverseArea;
    f.dual2=cross(area,e1)*inverseArea;
    const float incidence=abs(dot(area*sqrt(inverseArea),rayDirection));
    f.width=diameter/max(.12f,incidence);
    return f;
}
float ReflectionTextureLod(uint descriptor,ReflectionFootprint footprint,float2 a,float2 b,float2 c){
    Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    uint width,height,levels;
    image.GetDimensions(0u,width,height,levels);
    const float2 uv1=b-a,uv2=c-a;
    const float3 u=footprint.dual1*uv1.x+footprint.dual2*uv2.x;
    const float3 v=footprint.dual1*uv1.y+footprint.dual2*uv2.y;
    const float texels=footprint.width*max(length(u)*float(width),length(v)*float(height));
    return clamp(log2(max(1.f,texels)),0.f,float(max(1u,levels)-1u));
}
float4 SampleReflectedMaterial(uint descriptor,Surface s,uint slot,float2 uv0,float2 uv1,
                               Vertex a,Vertex b,Vertex c,ReflectionFootprint footprint){
    Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
    SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)s.mapping[slot].rotationSet.w)];
    const float lod=ReflectionTextureLod(descriptor,footprint,
        MaterialUV(s,slot,a.uv,a.uv1),MaterialUV(s,slot,b.uv,b.uv1),MaterialUV(s,slot,c.uv,c.uv1));
    return image.SampleLevel(sampleState,MaterialUV(s,slot,uv0,uv1),lod);
}

float3 ReflectedSurfaceRadiance(uint material,Vertex a,Vertex b,Vertex c,float2 barycentric,
                                 float3 hitPosition,float3 viewDirection,float diameter){
    float3 w=float3(1-barycentric.x-barycentric.y,barycentric.x,barycentric.y);
    float2 uv0=a.uv*w.x+b.uv*w.y+c.uv*w.z;
    float2 uv1=a.uv1*w.x+b.uv1*w.y+c.uv1*w.z;
    // Newly exposed cut walls are black in reflections as well as raster.
    if(uv1.x<-32768.f)return float3(.003,.003,.003);
    float3 n=normalize(a.normal*w.x+b.normal*w.y+c.normal*w.z);
    float3 v=normalize(viewDirection);
    if(dot(n,v)<0)n=-n;
    const ReflectionFootprint footprint=MakeReflectionFootprint(a,b,c,v,diameter);

    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    Surface s=materials[material];
    const float diffuseTransmission=s.extra.z!=0u?saturate(s.opticalLayers.w):0.f;
    float3 base=s.colorRoughness.rgb;
    float3 emission=s.emissionMetallic.rgb;
    float roughness=s.colorRoughness.w,metal=s.emissionMetallic.w;
    float surfaceAo=1.;
    if(s.albedo!=0xffffffff)base*=SampleReflectedMaterial(s.albedo,s,0u,uv0,uv1,a,b,c,footprint).rgb;
    base=AuthorColor(s,base);
    if(s.metalRough!=0xffffffff) {
        float4 properties=SampleReflectedMaterial(s.metalRough,s,2u,uv0,uv1,a,b,c,footprint);
        roughness*=properties.g;metal*=properties.b;
        if(s.extra.w==2)surfaceAo=lerp(1.,properties.r,.82);
    }
    if(s.emissive!=0xffffffff)emission*=SampleReflectedMaterial(s.emissive,s,3u,uv0,uv1,a,b,c,footprint).rgb;
    if(s.parameters.w>0){
        float variation=UrbanMacro(hitPosition)*s.parameters.w;
        base*=1.+variation;
        roughness=saturate(roughness*(1.-variation*.45));
    }
    float overrideLod=0.f;
    if(s.roughnessOverride!=0xffffffffu)
        overrideLod=ReflectionTextureLod(s.roughnessOverride,footprint,
            a.uv*s.overrideUv,b.uv*s.overrideUv,c.uv*s.overrideUv);
    roughness=clamp(AuthorRoughness(s,roughness,uv0,true,overrideLod),s.opticalPolicy.z,1.f);
    if(s.extra.w==9u)roughness=max(roughness,.82f);
    metal=min(s.opticalPolicy.w,saturate(metal+s.metallicBias));


    if(s.normal!=0xffffffff) {
        float2 xy=(SampleReflectedMaterial(s.normal,s,1u,uv0,uv1,a,b,c,footprint).xy*2-1)*s.parameters.x;
        float3 dp1=b.position-a.position,dp2=c.position-a.position;
        float2 t0=MaterialUV(s,1,a.uv,a.uv1);
        float2 dt1=MaterialUV(s,1,b.uv,b.uv1)-t0,dt2=MaterialUV(s,1,c.uv,c.uv1)-t0;
        float det=dt1.x*dt2.y-dt1.y*dt2.x;
        if(abs(det)>1.e-7) {
            float orientation=det<0?-1:1;
            float3 t=normalize((dp1*dt2.y-dp2*dt1.y)*orientation);
            float3 b=normalize((dp2*dt1.x-dp1*dt2.x)*orientation);
            n=normalize(t*xy.x+b*xy.y+n*sqrt(saturate(1-dot(xy,xy))));
        }
    }

    float occlusion=(s.extra.x==0xffffffff?1:
        lerp(1,SampleReflectedMaterial(s.extra.x,s,4u,uv0,uv1,a,b,c,footprint).r,s.opacity.z))*surfaceAo;
    if(s.extra.w==6u)return base*.12f+emission;
    if(s.extra.w==1)return base+emission;
    float3 color=base*(.026+.018*saturate(n.y))*occlusion+emission;
    if(s.extra.w==9u)color+=base*(.075f+.035f*saturate(n.y))*occlusion;
    if(s.extra.w==2) {
        float hemisphere=.27+.49*saturate(n.y);
        color+=base*occlusion*hemisphere*float3(.32,.39,.52);
    }


    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    uint cursor=1u;
    [loop]while(true){
        const uint index=LightBvhNextLight(cursor,LightBvhLeafCount,
                                          hitPosition,0.0f);
        if(index==0xffffffffu)break;
        color+=ReflectedLightContribution(hitPosition,n,v,base,roughness,metal,
                                          lights[index],index,(s.extra.w==7u||s.extra.w==9u),s.opticalPolicy.y,diffuseTransmission);
    }
    if(HasGlobalSun)
        color+=ReflectedLightContribution(hitPosition,n,v,base,roughness,metal,
                                          globalSun,LightCount,s.extra.w==7u||s.extra.w==9u,s.opticalPolicy.y,diffuseTransmission);
    return color;
}

float3 ReflectedHitRadiance(uint instanceIndex,uint primitive,uint geometry,float2 barycentric,
                           float3 hitPosition,float3 viewDirection,float diameter) {
    Vertex a,b,c;
    const uint material=LoadRayTriangle(instanceIndex,primitive,geometry,a,b,c);
    return ReflectedSurfaceRadiance(material,a,b,c,barycentric,hitPosition,viewDirection,diameter);
}

/* O2: share only broad reflected incident radiance between adjacent, compatible
   opaque receivers in the current eye and invocation wave. No history or
   extra dispatch. The caller still evaluates its own BRDF and environment. */
float4 TraceReflectionIncident(RayDesc ray,bool useCut,float pixelWidth,float coneAngle,uint opticalSample){
    OpticalCount(opticalSample,5u);
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene,RAY_FLAG_NONE,HardwareRayMask(7u,useCut),ray);
    while(query.Proceed()){
        if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
        const uint rayIndex=query.CandidateInstanceID();
        if(useCut&&RayUsesCutReplacement(rayIndex))continue;
        OpticalCount(opticalSample,24u);
        const float2 opacity=RayOpacity(rayIndex,query.CandidatePrimitiveIndex(),query.CandidateGeometryIndex(),
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
        const float3 p=ray.Origin+ray.Direction*cut.distance;
        return float4(ReflectedSurfaceRadiance(face.material,face.a,face.b,face.c,cut.barycentrics,
            p,-ray.Direction,pixelWidth+coneAngle*cut.distance),cut.distance);
    }
    if(regularHit){
        const float t=query.CommittedRayT();
        const float3 p=ray.Origin+ray.Direction*t;
        return float4(ReflectedHitRadiance(query.CommittedInstanceID(),query.CommittedPrimitiveIndex(),
            query.CommittedGeometryIndex(),query.CommittedTriangleBarycentrics(),p,-ray.Direction,
            pixelWidth+coneAngle*t),t);
    }
    return float4(0.f,0.f,0.f,-1.f);
}
// Retain exact traversal and post-trace history. Pre-trace reuse needs a complete local ray-corridor dependency contract.
float4 SharedReflectionIncident(RayDesc ray,bool useCut,uint receiver,uint material,uint view,
    float2 screen,float3 normal,float roughness,float pixelWidth,float coneAngle,
    float coneSpread,float footprintPixels,bool opaque,uint opticalSample){
    // Derivatives were evaluated by the caller; helpers have no color output.
    if(IsHelperLane())return float4(0.f,0.f,0.f,-1.f);
    // Smooth/narrow lobes, transparent media and cuts retain independent traces.
    if(useCut||!opaque||receiver==0xffffffffu||roughness<.15f||footprintPixels<8.f||
        pixelWidth<=0.f||!isfinite(pixelWidth)||!isfinite(coneSpread)||
        !all(isfinite(ray.Origin))||!all(isfinite(ray.Direction)))
        return TraceReflectionIncident(ray,useCut,pixelWidth,coneAngle,opticalSample);
    const uint2 tile=uint2(screen)>>1u;
    const uint key=OpticalHash(uint3(tile,view))^OpticalHash(uint3(receiver,material,0u));
    const uint4 peers=WaveMatch(key);
    if(countbits(peers.x)+countbits(peers.y)+countbits(peers.z)+countbits(peers.w)==1u)
        return TraceReflectionIncident(ray,false,pixelWidth,coneAngle,opticalSample);
    const uint leader=peers.x?uint(firstbitlow(peers.x)):
        peers.y?32u+uint(firstbitlow(peers.y)):
        peers.z?64u+uint(firstbitlow(peers.z)):96u+uint(firstbitlow(peers.w));
    const bool own=WaveGetLaneIndex()==leader;
    // Hash equality alone is never sufficient. Validate the actual identity,
    // screen tile, normal, lobe and world-space origin before sharing.
    const uint leaderReceiver=WaveReadLaneAt(receiver,leader);
    const uint leaderMaterial=WaveReadLaneAt(material,leader);
    const uint leaderView=WaveReadLaneAt(view,leader);
    const uint2 leaderTile=WaveReadLaneAt(tile,leader);
    const float3 leaderOrigin=WaveReadLaneAt(ray.Origin,leader);
    const float3 leaderDirection=WaveReadLaneAt(ray.Direction,leader);
    const float3 leaderNormal=WaveReadLaneAt(normal,leader);
    const float leaderWidth=WaveReadLaneAt(pixelWidth,leader);
    const float leaderRoughness=WaveReadLaneAt(roughness,leader);
    const float leaderSpread=WaveReadLaneAt(coneSpread,leader);
    const float originDelta=length(ray.Origin-leaderOrigin);
    const float directionDelta=length(ray.Direction-leaderDirection);
    const float commonSpread=min(coneSpread,leaderSpread);
    const bool compatible=receiver==leaderReceiver&&material==leaderMaterial&&view==leaderView&&
        all(tile==leaderTile)&&dot(normal,leaderNormal)>.999f&&
        abs(roughness-leaderRoughness)<=.05f*min(roughness,leaderRoughness)&&
        max(pixelWidth,leaderWidth)<=1.5f*min(pixelWidth,leaderWidth)&&
        originDelta<=2.f*min(pixelWidth,leaderWidth)&&directionDelta<=.5f*commonSpread;
    float4 incident=float4(0.f,0.f,0.f,-1.f);
    [branch]if(own)incident=TraceReflectionIncident(ray,false,pixelWidth,coneAngle,opticalSample);
    // Every participating leader is active again at this reconvergence.
    const float4 leaderIncident=WaveReadLaneAt(incident,leader);
    if(own)return incident;
    OpticalCount(opticalSample,10u);
    // Broad miss samples carry no borrowed RGB: the receiver evaluates its
    // own environment. Near hits still require a small projected lobe error.
    if(compatible&&((leaderIncident.w<0.f&&
        originDelta+directionDelta*ray.TMax<=.5f*commonSpread*ray.TMax)||
        (leaderIncident.w>0.f&&
        originDelta+directionDelta*leaderIncident.w<=.5f*commonSpread*leaderIncident.w))){
        OpticalCount(opticalSample,11u);
        return leaderIncident;
    }
    OpticalCount(opticalSample,12u);
    return TraceReflectionIncident(ray,false,pixelWidth,coneAngle,opticalSample);
}
