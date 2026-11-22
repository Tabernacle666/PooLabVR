


float3 ReflectedLightContribution(float3 hitPosition,float3 n,float3 v,
                                  float3 base,float roughness,float metal,Light light,uint index,bool indoors){

        if(light.colorPower.w<=0||all(light.colorPower.rgb==0))return 0;
        const bool room=IsRoomLight(light);
        if((indoors&&!room)||(room&&!RoomLightReaches(light,hitPosition)))return 0;
        const float3 bounce=room?RoomDiffuseBounce(light,index,hitPosition,n,base,metal):0.f;
        bool directional=((uint)light.parameters.z&2)!=0;
        float3 delta=directional?-light.directionOuter.xyz:light.positionRadius.xyz-hitPosition;
        if(dot(n,delta)<=0)return bounce;
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
            shadow=StaticShadow(hitPosition+n*.002,light,(uint)light.parameters.y);
        if(shadow<=0)return bounce;
        return bounce+BRDF(base,roughness,metal,n,v,direction,
                    light.colorPower.rgb*light.colorPower.w*falloff)*shadow;
    }

float3 ReflectedSurfaceRadiance(uint material,Vertex a,Vertex b,Vertex c,float2 barycentric,
                                 float3 hitPosition,float3 viewDirection){
    float3 w=float3(1-barycentric.x-barycentric.y,barycentric.x,barycentric.y);
    float2 uv0=a.uv*w.x+b.uv*w.y+c.uv*w.z;
    float2 uv1=a.uv1*w.x+b.uv1*w.y+c.uv1*w.z;
    // Newly exposed cut walls are black in reflections as well as raster.
    if(uv1.x<-32768.f)return float3(.003,.003,.003);
    float3 n=normalize(a.normal*w.x+b.normal*w.y+c.normal*w.z);
    float3 v=normalize(viewDirection);
    if(dot(n,v)<0)n=-n;

    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];
    Surface s=materials[material];
    float3 base=s.colorRoughness.rgb;
    float3 emission=s.emissionMetallic.rgb;
    float roughness=s.colorRoughness.w,metal=s.emissionMetallic.w;
    float surfaceAo=1.;
    if(s.albedo!=0xffffffff)base*=SampleMaterialLevel(s.albedo,s,0,uv0,uv1).rgb;
    base=AuthorColor(s,base);
    if(s.metalRough!=0xffffffff) {
        float4 properties=SampleMaterialLevel(s.metalRough,s,2,uv0,uv1);
        roughness*=properties.g;metal*=properties.b;
        if(s.extra.w==2)surfaceAo=lerp(1.,properties.r,.82);
    }
    if(s.emissive!=0xffffffff)emission*=SampleMaterialLevel(s.emissive,s,3,uv0,uv1).rgb;
    if(s.parameters.w>0){
        float variation=UrbanMacro(hitPosition)*s.parameters.w;
        base*=1.+variation;
        roughness=saturate(roughness*(1.-variation*.45));
    }
    roughness=clamp(AuthorRoughness(s,roughness,uv0,true),.04f,1.f);
    metal=saturate(metal+s.metallicBias);


    if(s.normal!=0xffffffff) {
        float2 xy=(SampleMaterialLevel(s.normal,s,1,uv0,uv1).xy*2-1)*s.parameters.x;
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
        lerp(1,SampleMaterialLevel(s.extra.x,s,4,uv0,uv1).r,s.opacity.z))*surfaceAo;
    if(s.extra.w==6u)return base*.12f+emission;
    if(s.extra.w==1)return base+emission;
    float3 color=base*(.026+.018*saturate(n.y))*occlusion+emission;
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
                                          lights[index],index,s.extra.w==7u);
    }
    return color;
}

float3 ReflectedHitRadiance(uint instanceIndex,uint primitive,float2 barycentric,
                           float3 hitPosition,float3 viewDirection) {
    Vertex a,b,c;
    const uint material=LoadRayTriangle(instanceIndex,primitive,a,b,c);
    return ReflectedSurfaceRadiance(material,a,b,c,barycentric,hitPosition,viewDirection);
}
