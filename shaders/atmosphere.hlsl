#include "common.hlsli"
struct VolumeSettings {
    uint4 resources;
    float4 centerTime,extent,distribution,optical,controls;
    float4 wake[64];
    float4 windPosition,windDirection,transport,structure;
};
float SceneDepth(uint2 pixel,uint view){
if(topology.y>1){
Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[param0];
    float d=0;[unroll]for(uint s=0;s<4;s++)d=max(d,depth.Load(int3(pixel,view),s));return d;
}else{
Texture2DArray<float> depth=ResourceDescriptorHeap[param0];return depth.Load(int4(pixel,view,0));
}

}
float DensityNoise(float3 p,float time){
    return sin(dot(p,float3(1.7,.8,1.3))+time)*sin(dot(p,float3(-.6,1.4,1.1))-time*.7);
}
float3 VolumeVelocity(float3 p,VolumeSettings s){
    float time=s.centerTime.w*s.optical.w;
    float3 direction=s.windDirection.xyz,delta=p-s.windPosition.xyz;float along=dot(delta,direction);
    float3 radial=delta-direction*along;float width=s.windPosition.w+max(0,along)*.16;
    float plume=exp(-dot(radial,radial)/(width*width))*saturate(along/.15)*exp(-max(0,along)/max(.1,s.transport.z));
    float3 swirl=cross(direction,radial)/max(width,.05)*sin(along*5-time*2);
    float3 turbulence=float3(sin(p.y*2+p.z+time),sin(p.z*2+p.x-time*.7),sin(p.x*2+p.y+time*.8));
    return (direction+swirl*.18+turbulence*s.optical.z*.12)*s.windDirection.w*plume*s.structure.z;
}
float DensityStructure(float3 p,VolumeSettings s){
    // Static world-space seeds become moving structures through the persistent
    // density grid. Time animates velocity, never the density texture coordinates.
    float3 q=p*s.structure.y*6.2831853;
    float lobes=.5+.5*sin(q.x*.73+sin(q.z*.47))*sin(q.y*.57-q.z*.81);
    float ribbons=pow(saturate(.5+.5*sin(q.z*.63+q.x*.31+sin(q.y*.8))),3);
    float structure=lerp(.08,2.4,saturate(lobes*.65+ribbons*.35));
    return lerp(1,structure,s.structure.x);
}
[numthreads(4,4,4)]
void FogDensity(uint3 id:SV_DispatchThreadID){
    StructuredBuffer<VolumeSettings> settings=ResourceDescriptorHeap[objectIndex];VolumeSettings s=settings[0];
    RWTexture3D<float> density=ResourceDescriptorHeap[s.resources.y];uint w,h,d;density.GetDimensions(w,h,d);if(any(id>=uint3(w,h,d)))return;
    float3 p=s.centerTime.xyz-s.extent.xyz+(float3(id)+.5)/float3(w,h,d)*2*s.extent.xyz;
    float sigma=exp(-max(0,p.y-s.distribution.y)*s.distribution.z);
    sigma*=DensityStructure(p,s);
    float disturbance=0;
    [loop]for(uint k=0;k<32;k++){
        float4 end=s.wake[k*2],start=s.wake[k*2+1];float age=s.centerTime.w-end.w;
        if(start.w<=0||age<0||age>2)continue;
        float3 segment=end.xyz-start.xyz;float t=saturate(dot(p-start.xyz,segment)/max(dot(segment,segment),.00001));
        float3 delta=p-(start.xyz+segment*t);float radius=.10+.08*age;
        disturbance+=start.w*exp(-dot(delta,delta)/(radius*radius))*exp(-age/.35);
    }
    float target=sigma*exp(-disturbance*s.controls.x);
    if(s.transport.y>.5){density[id]=target;return;}
    Texture3D<float> history=ResourceDescriptorHeap[s.resources.w];SamplerState sampler=SamplerDescriptorHeap[0];
    float dt=s.transport.x;
    // Persistent semi-Lagrangian RK2 transport, one common world grid for
    // both eyes. Dissipation relaxes to equilibrium rather than replacing it.
    float3 middle=p-VolumeVelocity(p,s)*dt*.5;
    float3 source=p-VolumeVelocity(middle,s)*dt;
    float3 uv=(source-s.centerTime.xyz)/(2*s.extent.xyz)+.5;
    float advected=all(uv>=0)&&all(uv<=1)?history.SampleLevel(sampler,uv,0):sigma;
    // Replenish recognizable filaments only at the physical jet inlet. Their
    // downstream motion is the RK2 backtrace, including orientation and RPM.
    float3 delta=p-s.windPosition.xyz;float along=dot(delta,s.windDirection.xyz);
    float3 radial=delta-along*s.windDirection.xyz;
    float inlet=exp(-dot(radial,radial)/max(.0025,s.windPosition.w*s.windPosition.w))*exp(-abs(along-.15)*7);
    float injection=inlet*saturate(s.windDirection.w)*s.structure.z*s.structure.w;
    // New 3D inlet filaments are modulated at their source. The history field
    // carries them downstream; no time offset is applied to volume sampling.
    float pulse=.5+.5*sin(s.centerTime.w*2.1+dot(radial,float3(7,11,5)));
    float feed=lerp(sigma,sigma*(.12+2.2*pulse*pulse),s.structure.x);
    float recover=1-exp(-s.transport.w*dt);
    float wakeLoss=exp(-disturbance*s.controls.x*dt*6);
    float transported=lerp(advected,sigma,recover);
    density[id]=max(0,lerp(transported,feed,1-exp(-injection*dt))*wakeLoss);
}
float SampleDensity(float3 p,VolumeSettings s){
    Texture3D<float> density=ResourceDescriptorHeap[s.resources.x];SamplerState sampler=SamplerDescriptorHeap[0];
    float3 uv=(p-s.centerTime.xyz)/(2*s.extent.xyz)+.5;
    return all(uv>=0)&&all(uv<=1)?density.SampleLevel(sampler,uv,0)*s.distribution.x:0;
}
float VolumeShadow(float3 p,Light light){
    if(!((uint)light.parameters.z&1))return 1;
    float3 delta=light.positionRadius.xyz-p;float distance=length(delta);if(distance<.025)return 1;
    RaytracingAccelerationStructure scene=ResourceDescriptorHeap[TLAS];RayDesc ray;
    ray.Origin=p;ray.Direction=delta/distance;ray.TMin=.008;ray.TMax=max(.009,distance-.015);
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;query.TraceRayInline(scene,RAY_FLAG_NONE,3,ray);
    while(query.Proceed())if(query.CandidateType()==CANDIDATE_NON_OPAQUE_TRIANGLE)query.CommitNonOpaqueTriangleHit();
    return query.CommittedStatus()==COMMITTED_TRIANGLE_HIT?0:1;
}
[numthreads(8,8,1)]
void FogIntegrate(uint3 id:SV_DispatchThreadID){
    StructuredBuffer<VolumeSettings> settings=ResourceDescriptorHeap[objectIndex];VolumeSettings s=settings[0];
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[passIndex];uint w,h,layers;output.GetDimensions(w,h,layers);if(id.x>=w||id.y>=h||id.z>=layers)return;
    RWTexture2DArray<float> endpoints=ResourceDescriptorHeap[s.resources.z];
    float2 uv=(float2(id.xy)+.5)/float2(w,h);uint2 pixel=min(uint2(uv*float2(Width,Height)),uint2(Width-1,Height-1));
    float3 origin=eye[id.z].xyz;float depth=SceneDepth(pixel,id.z);float3 end=Unproject(uv,depth,id.z),ray=normalize(end-origin);float lengthToSurface=min(length(end-origin),16);
    float3 inverseRay=rcp(ray+float3(1e-8,1e-8,1e-8));
    float3 a=(s.centerTime.xyz-s.extent.xyz-origin)*inverseRay,b=(s.centerTime.xyz+s.extent.xyz-origin)*inverseRay;
    float3 lo=min(a,b),hi=max(a,b);float begin=max(0,max(lo.x,max(lo.y,lo.z))),finish=min(lengthToSurface,min(hi.x,min(hi.y,hi.z)));
    endpoints[id]=lengthToSurface;if(finish<=begin){output[id]=float4(0,0,0,1);return;}
    uint steps=s.controls.z<.5?16:s.controls.z<1.5?32:48;float ds=(finish-begin)/steps;
    float transmittance=1;float3 radiance=0;StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    uint index=(uint)s.controls.w;bool lit=index<LightCount;Light light=(Light)0;if(lit)light=lights[index];
    float visibility=1,g=s.optical.x;
    [loop]for(uint k=0;k<steps;k++){
        float3 p=origin+ray*(begin+(k+.5)*ds);float rho=SampleDensity(p,s);
        float extinction=rho*(s.distribution.w+s.optical.y),attenuation=exp(-extinction*ds);
        float3 incident=float3(.004,.006,.009);
        if(lit&&light.colorPower.w>0){
            float3 delta=light.positionRadius.xyz-p;float distance=length(delta);float3 direction=delta/max(distance,.001);
            // Exact algebraic powers: avoid generic pow() inside each fog
            // raymarch sample without changing step count or light transport.
            float rangeRatio=distance/light.positionRadius.w;
            float rangeRatioSq=rangeRatio*rangeRatio;
            float falloffBase=saturate(1-rangeRatioSq*rangeRatioSq);
            float falloff=(falloffBase*falloffBase)/max(.035,distance*distance);
            if(light.directionOuter.w>-1)falloff*=saturate((dot(-direction,light.directionOuter.xyz)-light.directionOuter.w)/max(.00001,light.parameters.x-light.directionOuter.w));
            if(k%8==0)visibility=VolumeShadow(p,light);
            float phaseDenominator=max(.01,1+g*g-2*g*dot(direction,ray));
            float phase=(1-g*g)/(4*Pi*phaseDenominator*sqrt(phaseDenominator));
            // Four samples of the same world density approximate light-path absorption.
            float opticalDepth=0;[unroll]for(uint j=0;j<4;j++)opticalDepth+=SampleDensity(p+delta*((j+.5)/4),s);
            float lightTransmission=exp(-opticalDepth*.25*distance*(s.distribution.w+s.optical.y))*PopulationTransmission(index,p);
            incident+=light.colorPower.rgb*light.colorPower.w*falloff*phase*visibility*lightTransmission*s.controls.y;
        }
        float integral=extinction>.00001?(1-attenuation)/extinction:ds;
        radiance+=transmittance*rho*s.distribution.w*incident*integral;transmittance*=attenuation;
        if(transmittance<.01)break;
    }
    output[id]=float4(radiance,transmittance);
}
struct FullOut{float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
FullOut FogVS(uint id:SV_VertexID,uint view:SV_ViewID){FullOut o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float4 FogComposite(FullOut i):SV_Target0{
    Texture2DArray<float4> fog=ResourceDescriptorHeap[passIndex];Texture2DArray<float> endpoints=ResourceDescriptorHeap[param1];
    uint w,h,l;fog.GetDimensions(w,h,l);float2 p=i.uv*float2(w,h)-.5;int2 b=int2(floor(p));float2 f=frac(p);
    uint2 pixel=min(uint2(i.position.xy),uint2(Width-1,Height-1));float depth=SceneDepth(pixel,i.view);
    float target=min(16,length(Unproject(i.uv,depth,i.view)-eye[i.view].xyz));float4 sum=0;float weight=0;
    [unroll]for(uint y=0;y<2;y++)[unroll]for(uint x=0;x<2;x++){
        int2 q=clamp(b+int2(x,y),int2(0,0),int2(w-1,h-1));float d=endpoints.Load(int4(q,i.view,0));
        float a=(x?f.x:1-f.x)*(y?f.y:1-f.y);a*=exp(-abs(d-target)*12);
        sum+=fog.Load(int4(q,i.view,0))*a;weight+=a;
    }
    // Depth discontinuities reject unrelated low-resolution rays. Never smear
    // a background volume over foreground geometry or an approaching hand.
    return weight>.0001?sum/weight:float4(0,0,0,1);
}
struct ShadowOut{float4 position:SV_Position;float3 world:POSITION0;float2 uv:TEXCOORD0;nointerpolation float strength:TEXCOORD1;nointerpolation uint view:TEXCOORD2;};
ShadowOut FlyShadowVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uint view:SV_ViewID){
    StructuredBuffer<GpuInstance> shadows=ResourceDescriptorHeap[objectIndex];GpuInstance s=shadows[passIndex+instance];
    const float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    ShadowOut o;o.uv=corners[vertex];o.world=s.positionRadius.xyz+Rotate(float3(o.uv.x*s.scale.x,0,o.uv.y*s.scale.z),s.rotation);
    o.position=mul(float4(o.world,1),vp[view]);o.view=view;o.strength=(s.flags&64)&&s.positionRadius.w>0?s.scale.w:0;
    if(o.strength<=0)o.position=float4(2,2,0,1);return o;
}
float4 FlyShadowPS(ShadowOut i):SV_Target0{
    clip(i.strength-.0001);float radius=dot(i.uv,i.uv);clip(1-radius);
    uint2 pixel=min(uint2(i.position.xy),uint2(Width-1,Height-1));float depth=SceneDepth(pixel,i.view);
    float3 surface=Unproject((float2(pixel)+.5)/float2(Width,Height),depth,i.view);
    clip(.015-length(surface-i.world));float darkness=i.strength*exp(-radius*3)*pow(saturate(1-radius),2);
    return float4((1-darkness).xxx,1);
}
struct ProxyOut {
    float4 position:SV_Position;
    float2 local:TEXCOORD0;
    nointerpolation float4 footprint:TEXCOORD1;
    nointerpolation float3 world:POSITION0;
    nointerpolation uint view:TEXCOORD2;
};
ProxyOut FlyProxyVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uint view:SV_ViewID){
    StructuredBuffer<GpuInstance> insects=ResourceDescriptorHeap[objectIndex];GpuInstance s=insects[passIndex+instance];
    float3 center=s.positionRadius.xyz;float4 clip=mul(float4(center,1),vp[view]);float unit=s.scale.x;
    float blend=saturate(s.scale.w);float3 forward=Rotate(float3(0,0,lerp(.0035,.0018,blend)*unit),s.rotation);
    float4 tip=mul(float4(center+forward,1),vp[view]);
    // Convert unsigned dimensions before applying the Y-axis sign. Negating
    // Height as uint wraps to ~2^32 and collapses the rasterized footprint.
    float2 viewport=float2(Width,Height);
    float2 direction=(tip.xy/max(tip.w,.001)-clip.xy/max(clip.w,.001))*viewport*float2(.5,-.5);
    float major=max(length(direction),.0008*unit*abs(vp[view][1][1])*Height*.5/max(clip.w,.001));
    float minor=lerp(.0012,.00135,blend)*unit*abs(vp[view][1][1])*Height*.5/max(clip.w,.001);
    float2 axis=length(direction)>.0001?normalize(direction):float2(1,0),perpendicular=float2(-axis.y,axis.x);
    // A pixel reconstruction kernel broadens coverage, while its integrated
    // opacity preserves projected area. No minimum opaque insect size.
    float2 sigma=sqrt(float2(major*major,minor*minor)+.12);float2 extent=2.2*sigma;
    const float2 corners[6]={float2(-1,-1),float2(-1,1),float2(1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
    float2 local=corners[vertex]*extent;float2 pixel=axis*local.x+perpendicular*local.y;
    ProxyOut o;o.position=clip;o.position.xy+=pixel/viewport*float2(2,-2)*clip.w;o.local=local;o.world=center;o.view=view;
    o.footprint=float4(sigma,major*minor/max(sigma.x*sigma.y,.00001),unit);
    if(s.positionRadius.w<=0||clip.w<=timing.x)o.position=float4(2,2,0,1);return o;
}
float4 FlyProxyPS(ProxyOut i):SV_Target0{
    uint2 pixel=min(uint2(i.position.xy),uint2(Width-1,Height-1));float depth=SceneDepth(pixel,i.view);
    float3 surface=Unproject((float2(pixel)+.5)/float2(Width,Height),depth,i.view);
    clip(length(surface-eye[i.view].xyz)-length(i.world-eye[i.view].xyz)+.002);
    float opacity=saturate(2*i.footprint.z*exp(-2*dot(i.local/i.footprint.xy,i.local/i.footprint.xy)));
    // Close silhouettes become sharp; distant ones use the same physical area.
    float3 shade=float3(.006,.008,.006);
    StructuredBuffer<Light> lights=ResourceDescriptorHeap[Lights];
    [loop]for(uint k=0;k<LightCount;k++){
        Light light=lights[k];float3 delta=light.positionRadius.xyz-i.world;float distance=length(delta);
        float irradiance=light.colorPower.w*pow(saturate(1-pow(distance/light.positionRadius.w,4)),2)/max(.035,distance*distance);
        shade+=light.colorPower.rgb*irradiance*.014;
    }
    return float4(shade,opacity);
}
