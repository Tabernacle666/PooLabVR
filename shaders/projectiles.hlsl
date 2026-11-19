#include "impact_shared.hlsli"

struct Projectile {
    float3 position;float age;
    float3 velocity;float radius;
    float3 acceleration;float lifetime;
    uint id;uint kind;uint alive;float visualRadius;
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
// Same 32-byte node / 48-byte block GPU ABI as GraphicsInternal.h.
// A leaf has rightChild = 0xffffffffu and leftOrBlock = stable block ID.
struct CutSpatialNode {
    float3 lower;uint leftOrBlock;
    float3 upper;uint rightChild;
};
struct CutSpatialBlock {
    float3 lower;uint firstTriangle;
    float3 upper;uint triangleCount;
    uint instanceId,geometryState,activeFirst,activeCount;
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
    uint cutOriginalCount,cutStaticCount,bvhNodesDescriptor,bvhBlocksDescriptor;
    // Step 4c: read-only cache publication. Collision uses the cache only if
    // its generation matches the proven global cut; otherwise full fallback.
    uint cachePoolDescriptor,cacheStateDescriptor,cacheBlocks0Descriptor,cacheBlocks1Descriptor;
    uint membraneEvents,membraneCapacity;
    float membraneTime;
    float membraneTravelTime;
    float4 membraneSphere;
};

groupshared float groupDistance[64];
groupshared uint groupInstance[64];
groupshared uint groupPrimitive[64];
// Only lane 0 traverses. A balanced median BVH over 32-bit block IDs has
// depth <= 32; 64 pending nodes more than covers a depth-first walk.
groupshared uint bvhStack[64];
groupshared uint bvhStackSize,bvhCandidateFirst,bvhCandidateCount;
groupshared uint bvhCandidateBase,bvhCandidatePool,bvhTraversalFailed;

float3 CollisionRotate(float3 v,float4 q){
    return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);
}

void EmitMembraneEvent(float3 normal){
    RWStructuredBuffer<uint4> events=ResourceDescriptorHeap[membraneEvents];
    uint ticket;
    InterlockedAdd(events[0].x,1u,ticket);
    float2 oct=normal.xy/(abs(normal.x)+abs(normal.y)+abs(normal.z));
    if(normal.z<0.f)oct=(1.f-abs(oct.yx))*float2(oct.x>=0.f?1.f:-1.f,oct.y>=0.f?1.f:-1.f);
    events[1u+(ticket&(membraneCapacity-1u))]=asuint(float4(oct,membraneTravelTime,membraneTime));
    if(ticket<membraneCapacity){
        uint previousCount;
        InterlockedMax(events[0].y,ticket+1u,previousCount);
    }
}

void CrossMembrane(float3 origin,float3 direction,float distance){
    const float3 offset=origin-membraneSphere.xyz;
    const float b=dot(offset,direction);
    const float3 closest=offset-b*direction;
    const float discriminant=membraneSphere.w*membraneSphere.w-dot(closest,closest);
    if(discriminant<=0.f)return;
    const float chord=sqrt(discriminant);
    const float entry=-b-chord,exit=-b+chord;
    if(entry>0.f&&entry<=distance)EmitMembraneEvent(normalize(offset+entry*direction));
    if(exit>0.f&&exit<=distance)EmitMembraneEvent(normalize(offset+exit*direction));
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

// World-space slab test, inclusive on face/tangent contact. A near-parallel
// axis is never divided by zero; the node bounds are already expanded outward.
bool SegmentCutAabb(float3 origin,float3 direction,float length,float3 lower,float3 upper){
    float tMin=0.0f,tMax=length;
    [unroll] for(uint axis=0u;axis<3u;++axis){
        float d=direction[axis];
        if(abs(d)<=1e-20f){
            if(origin[axis]<lower[axis]||origin[axis]>upper[axis])return false;
        }else{
            float nearT=(lower[axis]-origin[axis])/d;
            float farT=(upper[axis]-origin[axis])/d;
            tMin=max(tMin,min(nearT,farT));
            tMax=min(tMax,max(nearT,farT));
            if(tMin>tMax)return false;
        }
    }
    return true;
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
        p.id=0;p.kind=0;p.alive=0;p.visualRadius=0;
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
    // Both producers run on the Direct queue. The cache commit and the
    // authoritative cut commit occur at the end of the same frame, before
    // either can be read by the next Projectile.Step.
    uint4 currentCache=uint4(0u,0u,0u,0u);
    if(cacheStateDescriptor!=0xffffffffu){
        StructuredBuffer<uint4> generations=ResourceDescriptorHeap[cacheStateDescriptor];
        currentCache=generations[0];
    }
    const bool cacheMatches=baked&&currentCache.x<2u&&
        currentCache.z==1u&&currentCache.y==currentCut.z&&
        cachePoolDescriptor!=0xffffffffu&&
        cacheBlocks0Descriptor!=0xffffffffu&&cacheBlocks1Descriptor!=0xffffffffu;
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
                           !(baked&&renderIndex<cutStaticCount&&(candidate.id&0x80000000u)!=0u)){
                            closest=rayT;
                            closestInstance=renderIndex;
                            closestPrimitive=query.CommittedPrimitiveIndex();
                        }
                    }
                }
            }
            // 4c: same immutable BVH both before and after perforation.
            // Cache state is read-only here; if its generation does not match
            // the global soup, retain the proven global collision fallback.
            bool useCutBvh=(!baked||cacheMatches)&&
                sourceTriangles!=0xffffffffu&&
                bvhNodesDescriptor!=0xffffffffu&&bvhBlocksDescriptor!=0xffffffffu;
            if(useCutBvh){
                StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[bvhNodesDescriptor];
                uint blocksDescriptor=cacheMatches?
                    (currentCache.x==0u?cacheBlocks0Descriptor:cacheBlocks1Descriptor):
                    bvhBlocksDescriptor;
                StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
                    NonUniformResourceIndex(blocksDescriptor)];
                StructuredBuffer<CutTriangle> sourceFaces=ResourceDescriptorHeap[sourceTriangles];
                uint poolCount=0u,poolStride=0u;
                if(cacheMatches){
                    StructuredBuffer<CutTriangle> poolFaces=ResourceDescriptorHeap[cachePoolDescriptor];
                    poolFaces.GetDimensions(poolCount,poolStride);
                }
                uint nodeCount,nodeStride,blockCount,blockStride;
                nodes.GetDimensions(nodeCount,nodeStride);
                blocks.GetDimensions(blockCount,blockStride);
                if(lane==0u){
                    bvhStackSize=1u;bvhStack[0]=0u;
                    bvhTraversalFailed=(nodeCount==0u||blockCount==0u)?1u:0u;
                    if(bvhTraversalFailed!=0u)bvhStackSize=0u;
                }
                GroupMemoryBarrierWithGroupSync();
                for(;;){
                    if(lane==0u){
                        bvhCandidateCount=0u;
                        while(bvhStackSize!=0u){
                            uint nodeIndex=bvhStack[--bvhStackSize];
                            if(nodeIndex>=nodeCount){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            CutSpatialNode node=nodes[nodeIndex];
                            if(!SegmentCutAabb(p.position,direction,distance,node.lower,node.upper))
                                continue;
                            if(node.rightChild!=0xffffffffu){
                                // Push right first, then left. Ordering is for
                                // traversal only; hits compete by exact distance.
                                if(bvhStackSize>62u){
                                    // Never discard a candidate silently: the
                                    // raster path below will search all instances.
                                    bvhTraversalFailed=1u;bvhStackSize=0u;break;
                                }
                                bvhStack[bvhStackSize++]=node.rightChild;
                                bvhStack[bvhStackSize++]=node.leftOrBlock;
                                continue;
                            }
                            uint blockId=node.leftOrBlock;
                            if(blockId>=blockCount){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            CutSpatialBlock block=blocks[blockId];
                            if(block.firstTriangle>cutOriginalCount||
                               block.triangleCount>cutOriginalCount-block.firstTriangle||
                               block.triangleCount>64u||
                               (cacheMatches&&block.geometryState>2u)||
                               (cacheMatches&&block.geometryState==1u&&
                                (block.activeFirst>poolCount||
                                 block.activeCount>poolCount-block.activeFirst||
                                 block.activeCount==0u))){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            // Empty is an explicit state, never Source(0).
                            if(cacheMatches&&block.geometryState==2u)continue;
                            if(block.triangleCount==0u&&
                               (!cacheMatches||block.geometryState!=1u))continue;
                            const bool modified=cacheMatches&&block.geometryState==1u;
                            uint base=0u;
                            if(!modified){
                                CutInstance object;
                                if(block.instanceId>=cutStaticCount||
                                   block.instanceId>=instanceCount||
                                   !LoadCutInstance(instanceRoutesDescriptor,block.instanceId,object)||
                                   object.id!=(0x80000000u+block.instanceId)||
                                   (object.flags&(1u|2u|8u|16u))!=0u||
                                   any(abs(object.scale.xyz)<1e-8f))continue;
                                uint low=0u,high=blockCount;
                                while(low<high){
                                    uint mid=(low+high)>>1;
                                    if(blocks[mid].instanceId<block.instanceId)low=mid+1u;
                                    else high=mid;
                                }
                                if(low==blockCount||blocks[low].instanceId!=block.instanceId){
                                    bvhTraversalFailed=1u;bvhStackSize=0u;break;
                                }
                                base=blocks[low].firstTriangle;
                            }
                            // Published triangles already live in world space. A
                            // generated wall's storage owner is not its collision policy.
                            bvhCandidatePool=modified?1u:0u;
                            bvhCandidateFirst=modified?block.activeFirst:block.firstTriangle;
                            bvhCandidateCount=modified?block.activeCount:block.triangleCount;
                            bvhCandidateBase=base;
                            break;
                        }
                    }
                    // Group-uniform barrier. No lane exits the candidate loop
                    // until lane 0 has finished publishing the next leaf.
                    GroupMemoryBarrierWithGroupSync();
                    if(bvhCandidateCount==0u)break;
                    // A single modified block may contain more than 64
                    // triangles after repeated cuts. Cooperative rounds must
                    // cover the WHOLE activeCount without a lane limit.
                    for(uint local=lane;local<bvhCandidateCount;local+=64u){
                        uint triangleIndex=bvhCandidateFirst+local;
                        CutTriangle face;
                        if(bvhCandidatePool!=0u){
                            StructuredBuffer<CutTriangle> poolFaces=
                                ResourceDescriptorHeap[cachePoolDescriptor];
                            face=poolFaces[triangleIndex];
                        }else face=sourceFaces[triangleIndex];
                        const uint flags=face.pad0;
                        const uint mask=(flags&4u)?4u:(flags&3u)?2u:1u;
                        if((flags&8u)!=0u||(mask&instanceMask)==0u)continue;
                        float hit=TriangleDistance(p.position,direction,
                            face.a.position,face.b.position,face.c.position);
                        if(hit>=0.0f&&hit<=distance&&hit<closest){
                            closest=hit;
                            closestInstance=face.instanceId;
                            closestPrimitive=bvhCandidatePool!=0u?
                                triangleIndex:triangleIndex-bvhCandidateBase;
                        }
                    }
                    // Every lane finishes this leaf before lane 0 reuses the
                    // shared candidate fields on the next traversal iteration.
                    GroupMemoryBarrierWithGroupSync();
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
                   (baked&&renderIndex<cutStaticCount&&(object.id&0x80000000u)!=0u))continue;
                uint mask=(object.flags&4u)?4u:(object.flags&3u)?2u:1u;
                if((mask&instanceMask)==0||any(abs(object.scale.xyz)<1e-8))continue;
                float4 inverseRotation=float4(-object.rotation.xyz,object.rotation.w);
                float3 localOrigin=CollisionRotate(p.position-object.positionRadius.xyz,inverseRotation)/object.scale.xyz;
                float3 localDirection=CollisionRotate(direction,inverseRotation)/object.scale.xyz;
                ProjectileRange range=ranges[object.mesh];
                // Match InitializeCutSource's actual membership predicate:
                // static input instance, non-dynamic/non-deformed/non-alpha,
                // with source triangles. Excluded instances retain raster hits.
                // On a traversal failure the original raster search is restored.
                if(useCutBvh&&bvhTraversalFailed==0u&&
                   renderIndex<cutStaticCount&&
                   object.id==(0x80000000u+renderIndex)&&
                   (object.flags&(1u|2u|16u))==0u&&
                   range.indexCount!=0u)continue;
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
            // Only the current cutFace generation participates in collisions.
            // The original static scene is skipped after the first cut.
            if(baked&&(!cacheMatches||!useCutBvh||bvhTraversalFailed!=0u)){
                uint descriptor=currentCut.y==1u?cutTrianglesA:cutTrianglesB;
                StructuredBuffer<CutTriangle> cut=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];
                for(uint triangleIndex=lane;triangleIndex<currentCut.x;triangleIndex+=64u){
                    CutTriangle cutFace=cut[triangleIndex];
                    if((cutFace.pad0&8u)!=0)continue;
                    float hit=TriangleDistance(p.position,direction,
                        cutFace.a.position,cutFace.b.position,cutFace.c.position);
                    if(hit>=0&&hit<=distance&&hit<closest){
                        closest=hit;closestInstance=cutFace.instanceId;
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
            if(lane==0u&&membraneEvents!=0xffffffffu)
                CrossMembrane(p.position,direction,min(distance,groupDistance[0]));
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
