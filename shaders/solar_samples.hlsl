#include "common.hlsli"
#define OASIS_SOLAR_COMPUTE 1
#include "solar_visibility.hlsli"

// The depth pass publishes the actual interpolated raster normal and owner.
// Reconstruct position from the matching depth, never normals from neighbours.
float4 SolarReceiverHomogeneous(uint2 pixel,float depth,uint view){
    const float2 uv=(float2(pixel)+.5f)/float2(Width,Height);
    return mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),inverseVP[view]);
}
// Evaluate the same adjacent-pixel footprint using homogeneous differences.
// Avoid two complete unprojections and subtraction of large world positions.
float SolarReceiverFootprint(uint2 pixel,uint view,float4 h,float3 p){
    const float sx=pixel.x+1u<Width?1.f:(pixel.x>0u?-1.f:0.f);
    const float sy=pixel.y+1u<Height?1.f:(pixel.y>0u?-1.f:0.f);
    const float4 dx=inverseVP[view][0]*(2.f*sx/float(Width));
    const float4 dy=inverseVP[view][1]*(-2.f*sy/float(Height));
    const float3 xDelta=(dx.xyz-p*dx.w)/(h.w+dx.w);
    const float3 yDelta=(dy.xyz-p*dy.w)/(h.w+dy.w);
    if(!all(isfinite(xDelta))||!all(isfinite(yDelta)))return -1.f;
    return max(0.00291262136f,sqrt(max(dot(xDelta,xDelta),dot(yDelta,yDelta))));
}
struct SolarReceiver {float3 p,n;float depth,footprint;float value,hit;uint receiver;bool valid;};

// H3: one compact tile list per eye. The first four words per eye are
// count, indirect dispatch X, Y, Z. Entries after the fixed header are
// screen tile ordinals, partitioned by eye and current render extent.
static const uint SolarTileHeaderWords=MaxViews*4u;
uint2 SolarTileGrid(){return (uint2(Width,Height)+7u)/8u;}
[numthreads(4,1,1)]
void SolarTileResetMain(uint3 id:SV_DispatchThreadID){
    RWStructuredBuffer<uint> work=ResourceDescriptorHeap[solarCacheInfo.x];
    if(id.x<MaxViews){
        const uint base=id.x*4u;
        work[base]=0u;work[base+1u]=0u;work[base+2u]=1u;work[base+3u]=1u;
    }
}
groupshared uint solarTileAdmitted;
[numthreads(8,8,1)]
void SolarTileCompactMain(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    if(lane==0u)solarTileAdmitted=0u;
    GroupMemoryBarrierWithGroupSync();
    const bool inside=all(id.xy<uint2(Width,Height))&&id.z<ViewCount;
    Texture2DArray<uint2> receivers=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarReceivers)];
    uint packed=0u;
    if(inside)packed=receivers.Load(int4(id.xy,id.z,0)).x;
    const bool anyReceiver=WaveActiveAnyTrue((packed&0x80000000u)!=0u);
    if(WaveIsFirstLane()&&anyReceiver)InterlockedOr(solarTileAdmitted,1u);
    GroupMemoryBarrierWithGroupSync();
    if(solarTileAdmitted!=0u){
        if(lane==0u){
            const uint2 grid=SolarTileGrid();
            const uint capacity=grid.x*grid.y;
            RWStructuredBuffer<uint> work=ResourceDescriptorHeap[solarCacheInfo.x];
            uint slot;InterlockedAdd(work[group.z*4u],1u,slot);
            if(slot<capacity)
                work[SolarTileHeaderWords+group.z*capacity+slot]=group.y*grid.x+group.x;
        }
    }else if(inside){
        // Inactive tiles still invalidate their previous history.
        RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
        output[id]=0.f;
        if(packed==1u)OpticalCount(OpticalSampleIndex(float2(id.xy)+.5f,id.z),36u);
    }
}
[numthreads(4,1,1)]
void SolarTileFinalizeMain(uint3 id:SV_DispatchThreadID){
    if(id.x>=ViewCount)return;
    RWStructuredBuffer<uint> work=ResourceDescriptorHeap[solarCacheInfo.x];
    const uint base=id.x*4u;
    const uint2 grid=SolarTileGrid();
    const uint count=min(work[base],grid.x*grid.y);
    const uint rows=max(1u,(count+65534u)/65535u);
    work[base]=count;
    work[base+1u]=(count+rows-1u)/rows;
    work[base+2u]=rows;
    work[base+3u]=1u;
}
bool SolarActiveTile(uint3 group,uint view,out uint2 tile){
    tile=0u;
    StructuredBuffer<uint> work=ResourceDescriptorHeap[solarCacheInfo.y];
    const uint base=view*4u;
    const uint index=group.x+group.y*work[base+1u];
    if(index>=work[base])return false;
    const uint2 grid=SolarTileGrid();
    const uint ordinal=work[SolarTileHeaderWords+view*grid.x*grid.y+index];
    tile=uint2(ordinal%grid.x,ordinal/grid.x);
    return true;
}
bool SolarLightAdmits(float3 p,float3 n){
    if(globalSun.colorPower.w<=0.f||all(globalSun.colorPower.rgb==0.f)||
       (uint(globalSun.parameters.z)&1u)==0u)return false;
    const bool directional=(uint(globalSun.parameters.z)&2u)!=0u;
    const float3 delta=directional?-globalSun.directionOuter.xyz:globalSun.positionRadius.xyz-p;
    if(dot(n,delta)<=0.f)return false;
    if(!directional){
        const float distance2=dot(delta,delta);
        if(distance2>=globalSun.positionRadius.w*globalSun.positionRadius.w)return false;
        if(globalSun.directionOuter.w>-1.f&&
           dot(-delta/max(sqrt(distance2),0.0291262136f),globalSun.directionOuter.xyz)<=globalSun.directionOuter.w)
            return false;
    }
    return true;
}
SolarReceiver SolarReadReceiver(Texture2DArray<float> depth,uint2 pixel,uint view,uint sample){
    SolarReceiver r=(SolarReceiver)0;r.value=-1.f;
    Texture2DArray<uint2> receivers=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarReceivers)];
    const uint2 packed=receivers.Load(int4(pixel,view,0));
    if((packed.x&0x80000000u)==0u){
        if(packed.x==1u)OpticalCount(sample,36u);
        return r;
    }
    r.depth=depth.Load(int4(pixel,view,0));
    if(r.depth<=0.f||!isfinite(r.depth))return r;
    const float4 h=SolarReceiverHomogeneous(pixel,r.depth,view);
    r.p=h.xyz/h.w;r.n=SolarUnpackReceiverNormal(packed.x);
    if((packed.x&0x40000000u)!=0u&&dot(r.n,SolarDirection(r.p))<0.f)r.n=-r.n;
    r.receiver=packed.y;
    if(!all(isfinite(r.p))||!SolarLightAdmits(r.p,r.n)){
        OpticalCount(sample,37u);return r;
    }
    r.footprint=SolarReceiverFootprint(pixel,view,h,r.p);
    r.valid=r.footprint>0.f&&isfinite(r.footprint);
    if(r.valid)OpticalCount(sample,35u);
    return r;
}
bool SolarCompatible(SolarReceiver a,SolarReceiver b){
    if(!a.valid||!b.valid||a.receiver==0xffffffffu||a.receiver!=b.receiver||dot(a.n,b.n)<.999f)return false;
    const float footprint=max(a.footprint,b.footprint);
    const float3 delta=a.p-b.p;
    const float limit=3.f*footprint+0.116504854f;
    return dot(delta,delta)<=limit*limit&&
        abs(dot(delta,a.n))<=0.116504854f+.05f*footprint&&
        dot(SolarDirection(a.p),SolarDirection(b.p))>.99999f;
}
// A shared light-space table, independent of the projected screen coordinates.
// Quantized cells are candidate lookup only: both current-frame witnesses
// must pass world-position, depth-layer, normal and solar-direction checks.
static const uint SolarSpatialSlots=1u<<20;
uint SolarSpatialHash(float3 p,float footprint,uint receiver){
    const float3 light=SolarDirection(p);
    const float3 axis=abs(light.y)<.9f?float3(0,1,0):float3(1,0,0);
    const float3 x=normalize(cross(axis,light)),y=cross(light,x);
    const float baseCell=0.113774272f; // metres
    const float level=clamp(ceil(log2(max(footprint*4.f/baseCell,1.f))),0.f,12.f);
    const float cell=baseCell*exp2(level);
    const int3 coord=int3(floor(float3(dot(p,x),dot(p,y),dot(p,light))/cell));
    return (OpticalHash(asuint(coord))^OpticalHash(uint3(uint(level),receiver,0)))&(SolarSpatialSlots-1u);
}
SolarReceiver SolarStored(RWTexture2DArray<float4> output,uint2 pixel){
    SolarReceiver r=(SolarReceiver)0;
    const float4 value=output[uint3(pixel,0u)];
    if(value.x<=0.f)return r;
    Texture2DArray<uint2> receivers=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarReceivers)];
    const uint2 packed=receivers.Load(int4(pixel,0u,0));
    if((packed.x&0x80000000u)==0u)return r;
    r.receiver=packed.y;
    const float4 h=SolarReceiverHomogeneous(pixel,value.x,0u);
    r.p=h.xyz/h.w;r.n=SolarUnpackReceiverNormal(packed.x);
    if((packed.x&0x40000000u)!=0u&&dot(r.n,SolarDirection(r.p))<0.f)r.n=-r.n;
    r.footprint=SolarReceiverFootprint(pixel,0u,h,r.p);
    r.hit=max(0.f,-value.w);r.value=max(0.f,value.w);
    r.valid=all(isfinite(r.p))&&r.footprint>0.f&&isfinite(r.footprint);
    return r;
}
[numthreads(256,1,1)]
void SolarSpatialClearMain(uint3 id:SV_DispatchThreadID){
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    if(id.x<SolarSpatialSlots)table.Store(id.x*4u,0xffffffffu);
}
[numthreads(8,8,1)]
void SolarSpatialBuildMain(uint3 id:SV_DispatchThreadID){
    const uint2 pixel=id.xy*2u;
    if(any(pixel+1u>=uint2(Width,Height)))return;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    SolarReceiver a=SolarStored(output,pixel),b=SolarStored(output,pixel+1u);
    if(!SolarCompatible(a,b)||a.value!=b.value||(a.value!=0.f&&a.value!=1.f))return;
    const uint hash=SolarSpatialHash(a.p,a.footprint,a.receiver);
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    uint old;table.InterlockedMin(hash*4u,pixel.y*Width+pixel.x,old);
}
bool SolarSpatialWitness(RWTexture2DArray<float4> output,SolarReceiver r,uint2 pixel,out float value){
    value=-1.f;
    if(any(pixel+1u>=uint2(Width,Height)))return false;
    SolarReceiver a=SolarStored(output,pixel),b=SolarStored(output,pixel+1u);
    if(a.value!=b.value||(a.value!=0.f&&a.value!=1.f)||
       !SolarCompatible(r,a)||!SolarCompatible(r,b))return false;
    value=a.value;return true;
}
bool SolarSpatialLookup(RWTexture2DArray<float4> output,SolarReceiver r,out float value){
    value=-1.f;
    if(r.receiver==0xffffffffu)return false;
    RWByteAddressBuffer table=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarSpatialUAV)];
    const uint index=table.Load(SolarSpatialHash(r.p,r.footprint,r.receiver)*4u);
    if(index!=0xffffffffu&&index<Width*Height&&
       SolarSpatialWitness(output,r,uint2(index%Width,index/Width),value))return true;
    // Hash collisions or a footprint-bin transition must not prevent a
    // geometrically matching pair from being found in the reference eye.
    const float4 projected=mul(float4(r.p,1.f),vp[0]);
    if(projected.w<=0.f||!all(isfinite(projected)))return false;
    const float2 screen=(projected.xy/projected.w*float2(.5f,-.5f)+.5f)*float2(Width,Height);
    if(any(screen<0.f)||any(screen>=float2(Width,Height)))return false;
    return SolarSpatialWitness(output,r,(uint2(screen)>>1u)*2u,value);
}
float3 SolarPreviousWorld(uint2 pixel,float depth,uint view){
    const float2 uv=(float2(pixel)+.5f)/float2(solarHistoryInfo.xy);
    const float4 p=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),solarPreviousInverseVP[view]);
    return p.xyz/p.w;
}
// A prior blocked ray is only a search hint. Confirm a present-frame
// opaque hit in a short interval around the old blocker. No stale occluder
// can darken the scene after motion, destruction or an LOD transition.
// Clear rays still traverse the entire interval; no stale light leaks.
bool SolarTemporalHint(SolarReceiver r,uint view,out float3 delta,out float previousT){
    delta=0.f;previousT=0.f;
    if(solarHistoryInfo.z==0u)return false;
    const uint phase=OpticalHash(asuint(int3(floor(r.p/max(r.footprint,0.116504854f)))))&3u;
    if(phase==(solarHistoryInfo.w&3u))return false; // periodic independent refresh
    const float4 projected=mul(float4(r.p,1),solarPreviousVP[view]);
    if(projected.w<=0.f||!all(isfinite(projected)))return false;
    const float2 screen=(projected.xy/projected.w*float2(.5f,-.5f)+.5f)*float2(solarHistoryInfo.xy);
    if(any(screen<0.f)||any(screen>=float2(solarHistoryInfo.xy)))return false;
    const uint2 pixel=uint2(screen);
    Texture2DArray<float4> previous=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarPrevious)];
    const float4 old=previous.Load(int4(pixel,view,0));
    if(old.x<=0.f||old.w>=0.f)return false;
    const float3 p=SolarPreviousWorld(pixel,old.x,view),n=SolarDecodeNormal(old.yz);
    delta=p-r.p;
    if(!all(isfinite(p))||dot(n,r.n)<.98f||length(delta)>2.f*r.footprint+0.116504854f)return false;
    previousT=-old.w;return true;
}
float SolarTraceReceiver(inout SolarReceiver r,uint view,uint sample){
    // Validate the history before building the shared ray. Both the short
    // interval and its full fallback use exactly the same origin/direction/limit.
    float3 delta;float previousT;
    const bool hasHint=SolarTemporalHint(r,view,delta,previousT);
    RayDesc ray;ray.Origin=r.p+r.n*0.0582524271f;ray.Direction=SolarDirection(r.p);
    ray.TMin=0.0582524271f;ray.TMax=SolarTraceLimit(ray.Origin,ray.Direction);
    if(hasHint){
        const float oldT=previousT+dot(delta,ray.Direction);
        const float margin=max(0.349514563f,4.f*r.footprint);
        RayDesc local=ray;
        local.TMin=max(0.0582524271f,oldT-margin);
        local.TMax=min(ray.TMax,oldT+margin);
        if(!(local.TMax<=local.TMin)){
            float hit;
            OpticalCount(sample,32u);
            const float value=TraceSolarHit(local,sample,hit);
            if(value==0.f&&hit>0.f){OpticalCount(sample,33u);r.hit=hit;return 0.f;}
            OpticalCount(sample,34u);
        }
    }
    OpticalCount(sample,30u);return TraceSolarHit(ray,sample,r.hit);
}
// One pixel per lane. Two diagonal witnesses per 2x2 tile are evaluated
// first, then shared across the group. Keep the wave's ray state small and
// avoid serializing four independent ray queries inside each thread.
groupshared float4 solarGroupPosition[64];
groupshared float4 solarGroupNormal[64];
groupshared uint solarGroupOwner[64];
SolarReceiver SolarGroupReceiver(uint lane){
    SolarReceiver r=(SolarReceiver)0;
    const float4 p=solarGroupPosition[lane],n=solarGroupNormal[lane];
    r.p=p.xyz;r.footprint=p.w;r.n=n.xyz;r.value=n.w;
    r.receiver=solarGroupOwner[lane];
    r.valid=r.value>=0.f;return r;
}
[numthreads(8,8,1)]
void SolarSamplesMain(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID,uint lane:SV_GroupIndex){
    const uint view=passIndex;
    uint2 tile;
    if(view>=ViewCount||!SolarActiveTile(group,view,tile))return;
    const uint3 id=uint3(tile*8u+thread.xy,view);
    const bool inside=all(id.xy<uint2(Width,Height))&&id.z<ViewCount;
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[DomainResource(ResourceDomainLighting,LightingSolarVisibilityUAV)];
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    SolarReceiver r=(SolarReceiver)0;r.value=-1.f;
    const uint sample=OpticalSampleIndex(float2(id.xy)+.5f,id.z);
    if(inside)r=SolarReadReceiver(depth,id.xy,id.z,sample);
    const bool anchor=(id.x&1u)==(id.y&1u);
    if(r.valid&&id.z>0u){
        float sharedVisibility;
        if(SolarSpatialLookup(output,r,sharedVisibility)){r.value=sharedVisibility;OpticalCount(sample,28u);}
    }
    if(r.valid&&anchor&&r.value<0.f){
        OpticalCount(sample,25u);r.value=SolarTraceReceiver(r,id.z,sample);
    }
    solarGroupPosition[lane]=float4(r.p,r.footprint);
    solarGroupNormal[lane]=float4(r.n,r.valid?r.value:-1.f);
    solarGroupOwner[lane]=r.receiver;
    GroupMemoryBarrierWithGroupSync();
    if(r.valid&&r.value<0.f){
        const uint first=lane&~9u;
        SolarReceiver a=SolarGroupReceiver(first),b=SolarGroupReceiver(first+9u);
        if(a.value==b.value&&(a.value==0.f||a.value==1.f)&&
           SolarCompatible(r,a)&&SolarCompatible(r,b)){
            r.value=a.value;OpticalCount(sample,26u);
        }else r.value=SolarTraceReceiver(r,id.z,sample);
    }
    if(inside)output[uint3(id.xy,id.z)]=r.valid?float4(r.depth,SolarEncodeNormal(r.n),
        r.value==0.f&&r.hit>0.f?-r.hit:r.value):0.f;
}
