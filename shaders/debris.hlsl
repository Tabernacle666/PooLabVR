#include "common.hlsli"
#include "debris_layout.hlsli"
#include "cut_surface.hlsli"

static const uint Projectile=0u, HotMetal=1u, HotSoil=2u, Crust=3u;
static const float DustDuration=11.f, MeltDuration=3.f;

uint DebrisHash(uint x){
    x=(x^(x>>16u))*0x7feb352du;
    x=(x^(x>>15u))*0x846ca68bu;
    return x^(x>>16u);
}
float Random(uint x){return float(DebrisHash(x)>>8u)*(1.f/16777216.f);}

struct Grain {
    float3 offset,velocity;
    float gravity,period,drag;
    bool fleeting;
};
Grain MakeGrain(uint kind,uint eventSeed,uint id){
    const uint seed=eventSeed+id*5u;
    const float3 noise=float3(Random(seed),Random(seed+1u),Random(seed+2u));
    const float r=Random(seed+3u);
    Grain g;
    const uint local=id%DEBRIS_DRAW_BLOCK;
    g.fleeting=kind==Projectile?local<20u:local>=12u;
    g.drag=kind==Projectile?.49f:0.f;
    if(kind==Crust){
        g.fleeting=false;
        g.velocity=(noise-.5f)*float3(.20f,.18f,.20f);
        g.offset=(noise-.5f)*.046f;
        g.gravity=2.6f;
        g.period=MeltDuration;
    }else if(kind==Projectile){
        // Fine grey dust rolls out quickly. Heavier colored dust spreads and falls.
        g.velocity=(noise*2.f-1.f)*float3(.96f,.47f,.96f);
        g.velocity.y+=.32f;
        g.velocity*=.35f+.65f*r;
        g.offset=(noise-.5f)*.070f;
        g.gravity=g.fleeting?.055f:.47f;
        g.period=g.fleeting?(.38f+1.00f*r):DustDuration;
    }else if(g.fleeting){
        g.velocity=(noise-.5f)*float3(3.4f,1.1f,3.4f);
        g.velocity.y+=.57f;
        g.offset=(noise-.5f)*.027f;
        g.gravity=2.2f;
        g.period=.10f+.29f*r;
    }else{
        g.velocity=float3((noise.x-.5f)*.048f,-.08f,(noise.z-.5f)*.048f);
        g.offset=(noise-.5f)*.050f;
        g.gravity=3.9f;
        g.period=MeltDuration;
    }
    return g;
}

float3 GrainPosition(Grain g,float3 origin,float age){
    float3 p=origin+g.offset+g.velocity*(age/(1.f+.29f*g.drag*age));
    p.y-=g.gravity*age*age/(1.f+g.drag*age);
    return p;
}

// Column queries use the published triangles, including open holes.
static const float NoTerrain=-1.e10f;
float TerrainBelow(float3 origin,uint4 cache,float lowest){
    if(debrisBindings.z==0xffffffffu||debrisBindings.w==0xffffffffu||
       cache.z!=1u||cache.x>1u)return NoTerrain;
    StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[debrisBindings.z];
    StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[NonUniformResourceIndex(
        cache.x==0u?cutRaster.y:cutRaster.z)];
    uint pending[32];
    pending[0]=debrisBindings.w;
    uint count=1u;
    float highest=lowest;
    while(count>0u){
        const CutSpatialNode node=nodes[pending[--count]];
        if(any(origin.xz<node.lower.xz)||any(origin.xz>node.upper.xz)||
           node.lower.y>origin.y||node.upper.y<=highest)continue;
        if(node.rightChild!=0xffffffffu){
            if(count+2u>32u)return NoTerrain;
            pending[count++]=node.leftOrBlock;
            pending[count++]=node.rightChild;
            continue;
        }
        const CutSpatialBlock block=blocks[node.leftOrBlock];
        if(block.geometryState==2u)continue;
        const bool changed=block.geometryState==1u;
        const uint first=changed?block.activeFirst:block.firstTriangle;
        const uint length=changed?block.activeCount:block.triangleCount;
        StructuredBuffer<CutTriangle> faces=ResourceDescriptorHeap[
            NonUniformResourceIndex(changed?cutRaster.w:cutGeometry.x)];
        for(uint i=0u;i<length;++i){
            const CutTriangle face=faces[first+i];
            if(((face.pad0>>16u)&3u)!=2u)continue;
            if(face.a.normal.y+face.b.normal.y+face.c.normal.y<=.35f)continue;
            const float2 a=face.a.position.xz;
            const float2 e=face.b.position.xz-a,f=face.c.position.xz-a,p=origin.xz-a;
            const float det=e.x*f.y-e.y*f.x;
            if(abs(det)<1.e-9f)continue;
            const float u=(p.x*f.y-p.y*f.x)/det,v=(e.x*p.y-e.y*p.x)/det;
            if(u<0.f||v<0.f||u+v>1.f)continue;
            const float y=face.a.position.y+u*(face.b.position.y-face.a.position.y)+
                v*(face.c.position.y-face.a.position.y);
            if(y<=origin.y)highest=max(highest,y);
        }
    }
    return highest>lowest?highest:NoTerrain;
}

bool GrainOverSupport(Grain g,float3 localOrigin,float support){
    if(support<=NoTerrain)return false;
    const float drop=max(0.f,localOrigin.y+g.offset.y-support);
    const float fall=sqrt(drop/max(g.gravity,.001f));
    const float age=fall*(1.f+g.drag*fall*.42f);
    const float2 drift=g.offset.xz+g.velocity.xz*age/(1.f+.29f*g.drag*age);
    return dot(drift,drift)<.34f*.34f;
}

uint PackDust(float3 rgb,uint mass){
    uint3 c=(uint3)round(saturate(rgb)*255.f);
    return c.x|(c.y<<8u)|(c.z<<16u)|(mass<<24u);
}
float3 DustColor(uint packed){
    return float3(packed&255u,(packed>>8u)&255u,(packed>>16u)&255u)/255.f;
}

[numthreads(64,1,1)]
void ResetDebris(uint3 id:SV_DispatchThreadID){
    RWStructuredBuffer<uint4> events=ResourceDescriptorHeap[objectIndex];
    RWStructuredBuffer<uint4> floorMap=ResourceDescriptorHeap[passIndex];
    if(id.x==0u)events[0]=0u;
    if(id.x<DEBRIS_FLOOR_CELLS)floorMap[id.x]=0u;
}

void StampFloor(RWStructuredBuffer<uint4> floorMap,float3 local,float3 tint,
                uint kind,float landed,bool terrain){
    const int size=int(DEBRIS_FLOOR_SIZE);
    const float2 uv=local.xz/(2.f*atmosphereSphere.w)+.5f;
    const int2 cell=int2(uv*float(size));
    [unroll]for(int y=-1;y<=1;++y){
        [unroll]for(int x=-1;x<=1;++x){
            const int2 p=cell+int2(x,y);
            if(any(p<0)||any(p>=size))continue;
            const uint address=(terrain?DEBRIS_TERRAIN_BASE:1u)+uint(p.x+p.y*size);
            if(terrain){
                const uint level=asuint(local.y+atmosphereSphere.w+1.f);
                uint old;
                InterlockedMax(floorMap[address].w,level,old);
            }
            const uint distance=uint(abs(x)+abs(y));
            if(kind==Projectile||kind==Crust){
                const uint mass=distance==0u?18u:(distance==1u?10u:4u);
                uint old;
                InterlockedOr(floorMap[address].x,0u,old);
                [allow_uav_condition]for(uint retry=0u;retry<12u;++retry){
                    const uint previous=old>>24u;
                    const uint value=PackDust(lerp(DustColor(old),tint,
                        float(mass)/float(previous+mass)),min(255u,previous+mass));
                    uint seen;
                    InterlockedCompareExchange(floorMap[address].x,old,value,seen);
                    if(seen==old)break;
                    old=seen;
                }
            }else{
                const uint mass=distance==0u?38u:(distance==1u?18u:6u);
                uint old;
                InterlockedOr(floorMap[address].y,0u,old);
                [allow_uav_condition]for(uint retry=0u;retry<12u;++retry){
                    const uint shift=kind==HotMetal?0u:16u;
                    const uint mask=65535u<<shift;
                    const uint value=(old&~mask)|(min(65535u,((old>>shift)&65535u)+mass)<<shift);
                    uint seen;
                    InterlockedCompareExchange(floorMap[address].y,old,value,seen);
                    if(seen==old)break;
                    old=seen;
                }
                const uint timestamp=(1u+(uint)(max(0.f,landed)*128.f))*2u+(kind==HotSoil?1u:0u);
                InterlockedMax(floorMap[address].z,timestamp);
            }
        }
    }
}

uint DepositEvent(RWStructuredBuffer<uint4> floorMap,float3 origin,float born,
                  float age,float previousAge,uint kind,uint seed,float3 tint,float support){
    const float radius=atmosphereSphere.w;
    const float3 localOrigin=origin-atmosphereSphere.xyz;
    uint settled=0u;
    const uint samples=kind==Projectile?12u:4u;
    // Sample the same trajectories as the visible grains, once at their crossing.
    for(uint n=0u;n<samples;++n){
        const uint id=kind==Projectile?20u+n*DEBRIS_DRAW_BLOCK:3u*n;
        const Grain g=MakeGrain(kind,seed,id);
        const float3 current=GrainPosition(g,localOrigin,age);
        const float3 previous=GrainPosition(g,localOrigin,previousAge);
        const bool onIsland=GrainOverSupport(g,localOrigin,support);
        if(onIsland&&current.y<=support&&(previous.y>support||previousAge==0.f)){
            float3 at=lerp(previous,current,saturate((previous.y-support)/
                max(previous.y-current.y,.00001f)));
            at.y=support;
            StampFloor(floorMap,at,tint,kind,born+age,true);
            settled|=2u;
            continue;
        }
        if(onIsland&&current.y<=support)continue;
        if(sphereScene.z!=0u)continue; // no membrane deposits
        if(current.y>=0.f||dot(current,current)<radius*radius)continue;
        if(previous.y<0.f&&dot(previous,previous)>=radius*radius)continue;
        float lo=previousAge,hi=age;
        [unroll]for(uint step=0u;step<5u;++step){
            const float mid=(lo+hi)*.5f;
            const float3 p=GrainPosition(g,localOrigin,mid);
            if(p.y<0.f&&dot(p,p)>=radius*radius)hi=mid;
            else lo=mid;
        }
        const float3 at=GrainPosition(g,localOrigin,hi);
        StampFloor(floorMap,at*(radius/max(length(at),radius)),tint,kind,born+hi,false);
        settled|=1u;
    }
    return settled;
}

uint BlocksForEvent(uint kind,float age){
    if(kind!=Projectile)return age<MeltDuration?1u:0u;
    if(age<1.1f)return 64u;
    if(age<3.8f)return 24u;
    return age<DustDuration?8u:0u;
}

groupshared uint detachedCells[64];

void DetachDebris(RWStructuredBuffer<uint4> events,RWStructuredBuffer<uint4> floorMap,
                  uint4 header,uint4 cache,uint lane){
    const uint cursor=header.y;
    const uint cell=cursor+lane;
    detachedCells[lane]=0u;
    const float radius=atmosphereSphere.w;
    const float size=float(DEBRIS_FLOOR_SIZE);
    if(cell<DEBRIS_MAP_CELLS){
        const uint4 pile=floorMap[DEBRIS_TERRAIN_BASE+cell];
        if((pile.x|pile.y)!=0u&&pile.w!=0u){
            const float y=atmosphereSphere.y+asfloat(pile.w)-radius-1.f;
            const float2 xz=(float2(cell%DEBRIS_FLOOR_SIZE,cell/DEBRIS_FLOOR_SIZE)+.5f)
                *(2.f*radius/size)-radius+atmosphereSphere.xz;
            if(TerrainBelow(float3(xz.x,y+.11f,xz.y),cache,y-.11f)==NoTerrain)
                detachedCells[lane]=1u;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane!=0u)return;
    uint4 ring=events[0];
    uint emitted=0u;
    for(uint i=0u;i<64u;++i){
        if(detachedCells[i]==0u)continue;
        if(emitted==4u)break;
        const uint index=cursor+i;
        const uint address=DEBRIS_TERRAIN_BASE+index;
        const uint4 pile=floorMap[address];
        const float y=atmosphereSphere.y+asfloat(pile.w)-radius-1.f;
        const float2 xz=(float2(index%DEBRIS_FLOOR_SIZE,index/DEBRIS_FLOOR_SIZE)+.5f)
            *(2.f*radius/size)-radius+atmosphereSphere.xz;
        const uint target=DEBRIS_RING_BASE+(ring.x&(DEBRIS_RING_SIZE-1u))*DEBRIS_RING_STRIDE;
        const uint tint=pile.x!=0u?pile.x:PackDust(float3(.31f,.14f,.072f),0u);
        events[target]=asuint(float4(xz.x,y,xz.y,timing.z));
        events[target+1u]=uint4(Crust,tint,0u,0u);
        ++ring.x;
        ++emitted;
        floorMap[address]=0u;
    }
    if(emitted!=0u)events[0]=ring;
    // Drain a busy group over successive frames before continuing the sweep.
    if(emitted==4u)return;
    const uint targetRevision=cursor==0u?cache.y:header.w;
    const uint next=cursor+64u;
    floorMap[0]=uint4(header.x,next<DEBRIS_MAP_CELLS?next:0u,
        next<DEBRIS_MAP_CELLS?header.z:targetRevision,targetRevision);
}

groupshared uint liveEvents;
groupshared uint drawBlocks;
groupshared uint drawRanges[DEBRIS_RING_SIZE];
groupshared uint floorChanged;

[numthreads(64,1,1)]
void BuildDebris(uint lane:SV_GroupIndex){
    RWStructuredBuffer<uint4> events=ResourceDescriptorHeap[objectIndex];
    RWStructuredBuffer<uint4> floorMap=ResourceDescriptorHeap[passIndex];
    RWByteAddressBuffer args=ResourceDescriptorHeap[param0];
    const uint4 floorHeader=floorMap[0];
    if((floorHeader.x&2u)!=0u){
        StructuredBuffer<uint4> state=ResourceDescriptorHeap[cutRaster.x];
        const uint4 cache=state[0];
        if(cache.z==1u&&(floorHeader.y!=0u||floorHeader.z!=cache.y)){
            DetachDebris(events,floorMap,floorHeader,cache,lane);
            DeviceMemoryBarrierWithGroupSync();
        }
    }
    const uint4 header=events[0];
    const uint head=header.x;
    const float previous=asfloat(header.w);
    const float now=max(previous,timing.z);
    if(lane==0u){
        liveEvents=0u;
        drawBlocks=0u;
        floorChanged=0u;
        // Expired records are left in the ring. Skip them as soon as its live tail ends.
        for(uint i=0u;i<min(head,DEBRIS_RING_SIZE);++i){
            const uint slot=(head-1u-i)&(DEBRIS_RING_SIZE-1u);
            const uint address=DEBRIS_RING_BASE+slot*DEBRIS_RING_STRIDE;
            const float born=asfloat(events[address].w);
            if(previous-born>=DustDuration)break;
            ++liveEvents;
            // Reserve the newest bursts first and keep the selection stable across frames.
            const uint blocks=drawBlocks<DEBRIS_MAX_DRAW_BLOCKS?
                min(BlocksForEvent(events[address+1u].x,max(0.f,now-born)),
                    DEBRIS_MAX_DRAW_BLOCKS-drawBlocks):0u;
            drawRanges[i]=(drawBlocks<<7u)|blocks;
            drawBlocks+=blocks;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(liveEvents==0u){
        if(lane==0u)args.Store4(0u,uint4(6u,0u,0u,0u));
        return;
    }
    for(uint i=lane;i<liveEvents;i+=64u){
        const uint sequence=head-1u-i;
        const uint slot=sequence&(DEBRIS_RING_SIZE-1u);
        const uint address=DEBRIS_RING_BASE+slot*DEBRIS_RING_STRIDE;
        const float4 source=asfloat(events[address]);
        const uint4 info=events[address+1u];
        const uint kind=info.x;
        // One exact support column per burst. The broader dust skirts it.
        float support=NoTerrain;
        if(info.w==0u){
            StructuredBuffer<uint4> state=ResourceDescriptorHeap[cutRaster.x];
            const float lowest=atmosphereSphere.y-atmosphereSphere.w;
            float3 probe=source.xyz;
            probe.y-=.004f;
            support=TerrainBelow(probe,state[0],lowest)-atmosphereSphere.y;
            if(support<=NoTerrain)support=NoTerrain;
            events[address+1u].zw=uint2(asuint(support),1u);
        }else support=asfloat(info.z);
        const float age=max(0.f,now-source.w);
        const float previousAge=max(0.f,previous-source.w);
        const float duration=kind==Projectile?DustDuration:MeltDuration;
        if(previousAge>=duration)continue;

        float3 tint;
        if(kind==Crust)tint=DustColor(info.y);
        else{
            StructuredBuffer<Surface> surfaces=ResourceDescriptorHeap[Materials];
            tint=lerp(float3(.18f,.16f,.13f),saturate(surfaces[info.y].colorRoughness.rgb),.78f);
        }
        const uint seed=DebrisHash(sequence+1u);
        if(age>previousAge){
            const uint deposits=DepositEvent(floorMap,source.xyz,source.w,
                min(age,duration),previousAge,kind,seed,tint,support);
            if(deposits!=0u)InterlockedOr(floorChanged,deposits);
        }
        const uint first=drawRanges[i]>>7u;
        const uint blocks=drawRanges[i]&127u;
        for(uint block=0u;block<blocks;++block)
            events[DEBRIS_DRAW_BASE+first+block]=uint4(slot,seed,block,PackDust(tint,0u));
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane==0u){
        if(floorChanged!=0u)floorMap[0].x|=floorChanged;
        if(head!=0u)events[0].w=asuint(now);
        args.Store4(0u,uint4(6u,drawBlocks*DEBRIS_DRAW_BLOCK,0u,0u));
    }
}

struct DebrisVertex {
    float4 position:SV_Position;
    float2 uv:TEXCOORD0;
    nointerpolation float4 tint:TEXCOORD1;
};

DebrisVertex DebrisVS(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uint view:SV_ViewID){
    StructuredBuffer<uint4> events=ResourceDescriptorHeap[debrisBindings.x];
    const uint4 block=events[DEBRIS_DRAW_BASE+instance/DEBRIS_DRAW_BLOCK];
    const uint address=DEBRIS_RING_BASE+block.x*DEBRIS_RING_STRIDE;
    const float4 source=asfloat(events[address]);
    const uint4 info=events[address+1u];
    const uint kind=info.x;
    const uint id=block.z*DEBRIS_DRAW_BLOCK+(instance%DEBRIS_DRAW_BLOCK);
    const Grain g=MakeGrain(kind,block.y,id);
    const float age=max(0.f,timing.z-source.w);
    const float3 p=GrainPosition(g,source.xyz,age);
    const float3 local=p-atmosphereSphere.xyz;
    const float radius=atmosphereSphere.w;
    const float distanceSquared=dot(local,local);
    DebrisVertex o=(DebrisVertex)0;
    o.position=float4(2.f,2.f,0.f,1.f);
    if(age>=g.period||(sphereScene.z==0u&&distanceSquared>=radius*radius))return o;
    const float support=asfloat(info.z);
    if(info.w!=0u&&local.y<=support&&
       GrainOverSupport(g,source.xyz-atmosphereSphere.xyz,support))return o;
    const float4 clip=mul(float4(p,1.f),vp[view]);
    if(clip.w<=0.f)return o;

    const bool hot=kind==HotMetal||kind==HotSoil;
    const float random=Random(block.y+id*7u+13u);
    const float size=kind==Crust?.009f+.016f*random:hot?(g.fleeting?.0065f:.012f):
        (g.fleeting ? (.009f+.024f*age) : (.004f+.007f*random));
    const float minDimension=float(min(outputDimensions.x,outputDimensions.y));
    const float radiusPx=clamp(size*minDimension*.55f/clip.w,.65f,13.f);
    const float2 halfSize=radiusPx*2.f/float2(outputDimensions.xy);
    const float fade=saturate((g.period-age)*(hot?6.f:1.6f));
    const float edgeFade=sphereScene.z!=0u?1.f:saturate((radius*radius-distanceSquared)/max(.01f,radius*.09f));
    const float reveal=hot?1.f:saturate(age*(block.z>=8u?9.f:35.f));
    float opacity=(kind==Crust?.68f:hot?(g.fleeting?.78f:.88f):(g.fleeting?.038f:.12f))*fade*edgeFade*reveal;
    if(!hot){
        // Fade high-density blocks into the dispersing cloud before drawing fewer.
        if(block.z>=24u)opacity*=saturate((1.1f-age)*3.5f);
        else if(block.z>=8u)opacity*=saturate((3.8f-age)*1.7f);
    }
    float3 tint;
    if(hot){
        const float heat=saturate(1.f-age*(g.fleeting?2.9f:1.1f));
        tint=lerp(float3(.53f,.054f,.012f),
                  kind==HotSoil&&!g.fleeting?float3(1.f,.31f,.08f):float3(1.f,.78f,.32f),heat);
    }else{
        tint=g.fleeting?float3(.54f,.54f,.52f):
            lerp(DustColor(block.w)*.76f,DustColor(block.w)+.25f,random);
    }

    const uint corners[6]={0u,2u,1u,1u,2u,3u};
    const uint corner=corners[vertex];
    o.uv=float2((corner&1u)!=0u?1.f:-1.f,(corner&2u)!=0u?1.f:-1.f);
    float2 along=float2(0.f,1.f),across=float2(1.f,0.f);
    float stretch=1.f;
    if(hot){
        const float3 velocity=g.velocity-float3(0.f,2.f*g.gravity*age,0.f);
        const float4 tangent=mul(float4(velocity,0.f),vp[view]);
        const float2 direction=(tangent.xy*clip.w-clip.xy*tangent.w)*float2(outputDimensions.xy);
        along=direction*rsqrt(max(dot(direction,direction),1.e-8f));
        across=float2(along.y,-along.x);
        stretch=1.4f+min(3.f,length(velocity)*.72f);
    }
    o.position=clip;
    o.position.xy+=(across*o.uv.x+along*o.uv.y*stretch)*halfSize*clip.w;
    o.tint=float4(saturate(tint),opacity);
    return o;
}

float4 DebrisPS(DebrisVertex i):SV_Target0{
    const float soft=saturate(1.f-dot(i.uv,i.uv));
    const float alpha=i.tint.a*soft*soft;
    return float4(i.tint.rgb*alpha,alpha);
}
