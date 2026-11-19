static const uint SphereHidden=5u;

SphereLayer ProbeSphere(Out i,uint owner,float2 uv,float3 ray){
    SphereLayer layer=(SphereLayer)0;
    layer.level=SphereHidden;
    layer.path.entry=layer.distance=1.e20f;
    const SphereProjection sprite=sphereProjections[owner];
    const uint level=sprite.atlas.w;
    [branch]if(level==SphereGeometry||level==SphereHidden)return layer;
    const float4 bounds=sprite.bounds[i.view];
    [branch]if(any(i.uv<bounds.xy)||any(i.uv>=bounds.zw))return layer;

    const float3 origin=eye[i.view].xyz;
    if(level==SpherePoint){
        const float4 clip=mul(float4(sprite.sphere.xyz,1.f),vp[i.view]);
        if(clip.w<=0.f)return layer;
        const float2 sigma=max(3.f/float2(outputDimensions.xy),.16f*(bounds.zw-bounds.xy));
        const float2 p=(i.uv-PerceptionUV(clip))/sigma;
        layer.path.entry=length(sprite.sphere.xyz-origin);
        layer.level=level;
        layer.color=float3(.09f,.38f,.30f)*sprite.tint.rgb*exp2(-dot(p,p)*1.6f);
        return layer;
    }

    SpherePath path;
    if(!TraceSphereRay(uv,0.f,i.view,sprite.sphere,ray,path)||path.entry<=0.f)return layer;
    if(sprite.parameters.y>0.f){
        SpherePath warped;
        if(TraceSphere(uv+WarpSphere(uv,path,i.view,sprite.sphere,sprite.parameters.y),
                       0.f,i.view,sprite.sphere,warped))path=warped;
    }
    layer.path=path;
    layer.level=level;
    if(level==SphereMean){
        StructuredBuffer<float4> average=ResourceDescriptorHeap[NonUniformResourceIndex(sprite.atlas.z)];
        const float4 interior=average[0];
        if(interior.a>0.f){
            const float facing=saturate((path.exit-path.entry)/max(.001f,2.f*sprite.sphere.w));
            layer.color=interior.rgb*(.76f+.24f*facing);
            layer.distance=path.entry;
        }
        return layer;
    }

    const uint steps=level==SphereReliefLite?2u:3u;
    float hit;
    float4 sampleColor=ReprojectSphere(path,origin,NonUniformResourceIndex(sprite.atlas.x),
        sprite.atlas.y&0xffffu,steps,sprite.sourceVP[0],sprite.sourceRect[0],
        sprite.sourceDepth[0].x,sprite.sphere.w,hit);
    [branch]if(level==SphereRelief&&sampleColor.a==0.f){
        sampleColor=ReprojectSphere(path,origin,NonUniformResourceIndex(sprite.atlas.x),
            sprite.atlas.y>>16u,3u,sprite.sourceVP[1],sprite.sourceRect[1],
            sprite.sourceDepth[1].x,sprite.sphere.w,hit);
    }
    if(sampleColor.a>0.f){layer.color=sampleColor.rgb;layer.distance=hit;}
    return layer;
}

void CompositeSphere(Out i,uint owner,SphereLayer layer,inout float3 color,inout float distance,inout float depth){
    [branch]if(layer.level==SphereHidden||layer.path.entry>=distance)return;
    if(layer.level==SpherePoint){color+=layer.color;return;}
    if(layer.distance<distance){
        distance=layer.distance;
        color=layer.color;
        const float3 world=eye[i.view].xyz+layer.path.ray*distance;
        const float4 clip=mul(float4(world,1.f),vp[i.view]);
        depth=saturate(clip.z/max(.0001f,clip.w));
    }
    if(layer.level==SphereMean)return;
    const SphereProjection sprite=sphereProjections[owner];
    const bool lite=layer.level==SphereReliefLite;
    SpherePath path=layer.path;
    path.sceneDistance=distance;
    color=ShadeAtmosphere(color,path,i.view,lite,sprite.sphere,sprite.fog,
                         sprite.light.xyz,sprite.emission,sprite.parameters.x);
    [branch]if(sprite.optics.x>0.f){
        if(!lite&&owner==spherePair.y&&membraneField.x!=0xffffffffu){
            const MembraneOptics optics=ShadeMembrane(path,i.uv,i.view,false);
            color=color*optics.transmission+optics.glow;
        }else{
            const float facing=saturate((path.exit-path.entry)/(2.f*sprite.sphere.w));
            const float rim=1.f-facing;
            color*=1.f-sprite.optics.x*(1.f+.30f*rim*rim);
            // The neighbour's medium-detail film has the same idle pattern as
            // the native field. Contact ripples belong to its native render.
            if(!lite){
                const float3 world=eye[i.view].xyz+path.ray*path.entry;
                const float3 normal=(world-sprite.sphere.xyz)/sprite.sphere.w;
                const float3 a=float3(5.4f,3.1f,-3.9f),b=float3(-4.2f,6.5f,7.1f);
                const float3 gradient=.015f*cos(dot(normal,a)+sprite.parameters.z*.53f)*a+
                    .007f*cos(dot(normal,b)-sprite.parameters.z*.37f)*b;
                const float3 tangent=gradient-normal*dot(gradient,normal);
                const float3 lamp=sprite.light.xyz-world;
                const float power=max(sprite.emission.w,0.f)/(1.f+dot(lamp,lamp));
                color+=sprite.tint.rgb*sprite.emission.rgb*power*(.018f+.12f*rim*rim)*
                    smoothstep(.04f,.28f,length(tangent));
            }
        }
    }
}

Present DrawSpherePair(Out i){
    float3 color=float3(.008f,.013f,.021f);
    float distance=1.e20f,depth=0.f;
    const float3 ray=ViewRay(i.uv,i.view);
    const SphereLayer a=ProbeSphere(i,0u,i.uv,ray),b=ProbeSphere(i,1u,i.uv,ray);
    if(a.path.entry<b.path.entry){
        CompositeSphere(i,1u,b,color,distance,depth);
        CompositeSphere(i,0u,a,color,distance,depth);
    }else{
        CompositeSphere(i,0u,a,color,distance,depth);
        CompositeSphere(i,1u,b,color,distance,depth);
    }
    Present result;
    result.color=float4(ACES(PairMarks(i,color)),1.f);
    result.depth=depth;
    return result;
}

float3 ComposeNativeSphere(Out i,SpherePath path,MembraneOptics optics,float3 color,float distance){
    path.sceneDistance=min(path.sceneDistance,distance);
    color=ApplyDebrisFloor(color,path,i.view);
    color=ApplyAtmosphere(color,path,i.view,false);
    return color*optics.transmission+optics.glow;
}

void CompositeSphereField(Out i,float2 uv,float3 ray,SpherePath nativePath,bool nativeVisible,
    MembraneOptics optics,inout float3 color,inout float distance,inout float depth){
    const uint2 tile=min(uint2(i.uv*float(SphereTileAxis)),SphereTileAxis-1u);
    const uint4 mask=sphereTiles[(i.view*SphereTileAxis+tile.y)*SphereTileAxis+tile.x];
    [unroll]for(uint word=0u;word<4u;++word){
        uint bits=mask[word];
        while(bits!=0u){
            const uint rank=32u*word+uint(firstbitlow(bits));
            bits&=bits-1u;
            const uint owner=sphereOrder[i.view*SphereOrderStride+rank/4u][rank%4u];
            const SphereLayer layer=ProbeSphere(i,owner,uv,ray);
            if(nativeVisible&&layer.path.entry<max(0.f,nativePath.entry)){
                color=ComposeNativeSphere(i,nativePath,optics,color,distance);
                nativeVisible=false;
            }
            CompositeSphere(i,owner,layer,color,distance,depth);
        }
    }
    if(nativeVisible)color=ComposeNativeSphere(i,nativePath,optics,color,distance);
}

Present DrawSphereField(Out i){
    const float3 ray=ViewRay(i.uv,i.view);
    float3 color=CosmicBackground(ray);
    float distance=1.e20f,depth=0.f;
    CompositeSphereField(i,i.uv,ray,(SpherePath)0,false,(MembraneOptics)0,color,distance,depth);
    Present result;
    result.color=float4(ACES(PairMarks(i,color)),1.f);
    result.depth=depth;
    return result;
}
