#include "impact_shared.hlsli"

struct Projectile {
    float3 position;float age;
    float3 velocity;float radius;
    float3 acceleration;float lifetime;
    uint id;uint kind;uint alive;uint _pad;
};
struct ProjectileImpact {
    float3 position;float radius;
    float3 direction;uint projectileId;
    uint instanceId;uint primitiveIndex;uint kind;uint _pad;
};
struct ProjectileVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct ProjectileRange {
    uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;
    float4 bounds;
};
struct CutTriangle {
    ProjectileVertex a,b,c;
    uint material,instanceId,pad0,pad1;
};
cbuffer ProjectileParameters : register(b0) {
    uint sourceProjectiles,destinationProjectiles,impactBuffer,controlBuffer;
    uint sceneDescriptor,projectileCount,impactCapacity,instanceMask;
    float deltaTime,maxSegmentLength,_reserved0,_reserved1;
    uint spawnMask,spawnSlotLeft,spawnSlotRight,resetSource;
    Projectile spawnLeft,spawnRight;
    uint rayMapDescriptor,instanceRoutesDescriptor,rangesDescriptor,verticesDescriptor;
    uint indicesDescriptor,rayInstanceCount,rangeCount,instanceCount;
    uint sourceTriangles,cutTrianglesA,cutTrianglesB,stateDescriptor;
    uint cutOriginalCount,_reserved4,_reserved5,_reserved6;
};

groupshared float groupDistance[64];
groupshared uint groupInstance[64];
groupshared uint groupPrimitive[64];

float3 CollisionRotate(float3 v,float4 q){
    return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);
}

bool SegmentBounds(float3 origin,float3 direction,float distance,float4 bounds){
    float3 offset=origin-bounds.xyz;
    float a=dot(direction,direction),b=dot(offset,direction);
    float c=dot(offset,offset)-bounds.w*bounds.w;
    float discriminant=b*b-a*c;
    if(a<=1e-20||discriminant<0)return false;
    float root=sqrt(max(0,discriminant));
    return (-b+root)>=0&&(-b-root)<=distance*a;
}

float TriangleDistance(float3 origin,float3 direction,float3 a,float3 b,float3 c){
    float3 e1=b-a,e2=c-a,h=cross(direction,e2);
    float determinant=dot(e1,h);
    if(abs(determinant)<1e-10)return -1;
    float inverse=1/determinant;
    float3 s=origin-a;
    float u=dot(s,h)*inverse;
    if(u<0||u>1)return -1;
    float3 q=cross(s,e1);
    float v=dot(direction,q)*inverse;
    if(v<0||u+v>1)return -1;
    return dot(e2,q)*inverse;
}

// One group cooperates on each projectile. The TLAS still handles deformed
// geometry; static meshes are also intersected directly using their actual
// resident instance and indexed triangles.
[numthreads(64,1,1)]
void StepProjectiles(uint3 group:SV_GroupID,uint lane:SV_GroupIndex){
    uint index=group.x;if(index>=projectileCount)return;
    StructuredBuffer<Projectile> source=ResourceDescriptorHeap[sourceProjectiles];
    RWStructuredBuffer<Projectile> destination=ResourceDescriptorHeap[destinationProjectiles];
    RWStructuredBuffer<ProjectileImpact> impacts=ResourceDescriptorHeap[impactBuffer];
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlBuffer];
    Projectile p;
    if(resetSource!=0){
        p.position=float3(0,0,0);p.age=0;
        p.velocity=float3(0,0,0);p.radius=0;
        p.acceleration=float3(0,0,0);p.lifetime=0;
        p.id=0;p.kind=0;p.alive=0;p._pad=0;
    }else p=source[index];
    if((spawnMask&1u)!=0&&index==spawnSlotLeft)p=spawnLeft;
    if((spawnMask&2u)!=0&&index==spawnSlotRight)p=spawnRight;
    if(p.alive==0){if(lane==0)destination[index]=p;return;}
    uint4 currentCut=uint4(cutOriginalCount,0,0,0);
    if(stateDescriptor!=0xffffffffu){
        StructuredBuffer<uint4> generation=ResourceDescriptorHeap[stateDescriptor];
        currentCut=generation[0];
    }
    bool baked=currentCut.y!=0u;
    float remaining=max(0,min(deltaTime,p.lifetime-p.age));
    if(maxSegmentLength<=0){if(lane==0){uint ignored;control.InterlockedOr(8,1,ignored);}return;}
    while(remaining>0){
        float speed=length(p.velocity),accel=length(p.acceleration),step=remaining;
        if(accel>1e-6){
            float distanceStep=2*maxSegmentLength/(sqrt(speed*speed+2*accel*maxSegmentLength)+speed);
            step=min(step,distanceStep);
        }else if(speed>1e-6)step=min(step,maxSegmentLength/speed);
        if(step<=0){if(lane==0){uint ignored;control.InterlockedOr(8,1,ignored);}return;}
        float3 delta=p.velocity*step+0.5*p.acceleration*step*step;
        float distance=length(delta);
        if(distance>1e-8){
            float3 direction=delta/distance;
            float closest=distance+1;
            uint closestInstance=0xffffffffu,closestPrimitive=0xffffffffu;
            // DXR covers the compact ray set. The resident raster world is
            // intersected below even when no TLAS instances exist.
            if(lane==0&&rayInstanceCount!=0){
                RaytracingAccelerationStructure scene=ResourceDescriptorHeap[sceneDescriptor];
                RayDesc ray;ray.Origin=p.position;ray.Direction=direction;ray.TMin=0;ray.TMax=distance;
                RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
                query.TraceRayInline(scene,RAY_FLAG_NONE,instanceMask,ray);
                while(query.Proceed()){}
                if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
                    uint rayIndex=query.CommittedInstanceID();
                    if(rayIndex<rayInstanceCount){
                        StructuredBuffer<uint> rayMap=ResourceDescriptorHeap[rayMapDescriptor];
                        uint renderIndex=rayMap[rayIndex];
                        CutInstance candidate;
                        float rayT=query.CommittedRayT();
                        if(renderIndex<instanceCount&&
                           LoadCutInstance(instanceRoutesDescriptor,renderIndex,candidate)&&
                           (candidate.flags&8u)==0&&
                           !(baked&&(candidate.id&0x80000000u)!=0u)){
                            closest=rayT;
                            closestInstance=renderIndex;
                            closestPrimitive=query.CommittedPrimitiveIndex();
                        }
                    }
                }
            }
            StructuredBuffer<ProjectileRange> ranges=ResourceDescriptorHeap[rangesDescriptor];
            StructuredBuffer<ProjectileVertex> vertices=ResourceDescriptorHeap[verticesDescriptor];
            StructuredBuffer<uint> indices=ResourceDescriptorHeap[indicesDescriptor];
            // Ray publications omit ordinary resident tiles. Iterate raster
            // instances, but test their mesh bounds before visiting triangles.
            for(uint renderIndex=0;renderIndex<instanceCount;++renderIndex){
                CutInstance object;
                if(!LoadCutInstance(instanceRoutesDescriptor,renderIndex,object)||
                   object.mesh>=rangeCount||(object.flags&8u)||(object.flags&2u)||
                   (baked&&(object.id&0x80000000u)!=0u))continue;
                uint mask=(object.flags&4u)?4u:(object.flags&3u)?2u:1u;
                if((mask&instanceMask)==0||any(abs(object.scale.xyz)<1e-8))continue;
                float4 inverseRotation=float4(-object.rotation.xyz,object.rotation.w);
                float3 localOrigin=CollisionRotate(p.position-object.positionRadius.xyz,inverseRotation)/object.scale.xyz;
                float3 localDirection=CollisionRotate(direction,inverseRotation)/object.scale.xyz;
                ProjectileRange range=ranges[object.mesh];
                if(!SegmentBounds(localOrigin,localDirection,distance,range.bounds))continue;
                for(uint triangleIndex=lane;triangleIndex<range.indexCount/3u;triangleIndex+=64u){
                    uint first=range.firstIndex+triangleIndex*3u;
                    float3 a=vertices[indices[first]].position;
                    float3 b=vertices[indices[first+1]].position;
                    float3 c=vertices[indices[first+2]].position;
                    float hit=TriangleDistance(localOrigin,localDirection,a,b,c);
                    if(hit>=0&&hit<=distance&&hit<closest){
                        closest=hit;closestInstance=renderIndex;closestPrimitive=triangleIndex;
                    }
                }
            }
            // Only the current triangle generation participates in collisions.
            // The original static scene is skipped after the first cut.
            if(baked){
                uint descriptor=currentCut.y==1u?cutTrianglesA:cutTrianglesB;
                StructuredBuffer<CutTriangle> cut=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
                for(uint triangleIndex=lane;triangleIndex<currentCut.x;triangleIndex+=64u){
                    CutTriangle triangle=cut[triangleIndex];
                    if((triangle.pad0&8u)!=0)continue;
                    float hit=TriangleDistance(p.position,direction,
                        triangle.a.position,triangle.b.position,triangle.c.position);
                    if(hit>=0&&hit<=distance&&hit<closest){
                        closest=hit;closestInstance=triangle.instanceId;
                        closestPrimitive=triangleIndex;
                    }
                }
            }
            groupDistance[lane]=closest;groupInstance[lane]=closestInstance;groupPrimitive[lane]=closestPrimitive;
            GroupMemoryBarrierWithGroupSync();
            for(uint stride=32;stride>0;stride>>=1){
                if(lane<stride&&groupDistance[lane+stride]<groupDistance[lane]){
                    groupDistance[lane]=groupDistance[lane+stride];
                    groupInstance[lane]=groupInstance[lane+stride];
                    groupPrimitive[lane]=groupPrimitive[lane+stride];
                }
                GroupMemoryBarrierWithGroupSync();
            }
            if(groupInstance[0]!=0xffffffffu){
                if(lane==0){
                    uint slot=control.Load(0);
                    for(;;){
                        if(slot>=impactCapacity){uint ignored;control.InterlockedOr(4,1,ignored);return;}
                        uint observed;control.InterlockedCompareExchange(0,slot,slot+1,observed);
                        if(observed==slot)break;slot=observed;
                    }
                    ProjectileImpact hit;hit.position=p.position+direction*groupDistance[0];
                    hit.radius=p.radius;hit.direction=direction;hit.projectileId=p.id;
                    hit.instanceId=groupInstance[0];hit.primitiveIndex=groupPrimitive[0];
                    hit.kind=p.kind;hit._pad=0;impacts[slot]=hit;
                    p.position=hit.position;p.alive=0;destination[index]=p;
                }
                return;
            }
            GroupMemoryBarrierWithGroupSync();
        }
        p.position+=delta;p.velocity+=p.acceleration*step;p.age+=step;remaining-=step;
    }
    if(p.age>=p.lifetime)p.alive=0;
    if(lane==0)destination[index]=p;
}
