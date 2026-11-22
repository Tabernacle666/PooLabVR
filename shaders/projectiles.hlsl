#include "impact_shared.hlsli"
#include "cut_surface.hlsli"
#include "../src/core/GpuInstanceVisual.h"

struct Projectile {
    float3 position;float age;
    float3 velocity;float radius;
    float3 acceleration;float lifetime;
    uint id;uint kind;uint alive;float visualRadius;
};
static const uint BuildingHitBlock=0xfffffffeu;
struct ProjectileVertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct ProjectileRange {
    uint firstMeshlet,meshletCount,firstVertex,vertexCount,firstIndex,indexCount;
    float4 bounds;
};
cbuffer ProjectileParameters : register(b0) {
    uint sourceProjectiles,destinationProjectiles,impactBuffer,controlBuffer;
    uint sceneDescriptor,projectileCount,impactCapacity,instanceMask;
    float deltaTime,maxSegmentLength;
    uint collisionOwner,collisionTick;
    uint spawnMask,spawnSlotLeft,spawnSlotRight,resetSource;
    Projectile spawnLeft,spawnRight;
    uint rayMapDescriptor,instanceRoutesDescriptor,rangesDescriptor,verticesDescriptor;
    uint indicesDescriptor,rayInstanceCount,rangeCount,instanceCount;
    uint sourceTriangles,stateDescriptor,cutOriginalCount,cutStaticCount;
    uint bvhNodesDescriptor,bvhBlocksDescriptor,collisionCutRoot,buildingHitBuffer;
    // Step 4c: read-only cache publication. Collision uses the cache only if
    // its generation matches the proven global cut; otherwise full fallback.
    uint cachePoolDescriptor,cacheStateDescriptor,cacheBlocks0Descriptor,cacheBlocks1Descriptor;
    uint membraneEvents,membraneCapacity;
    float membraneTime;
    float membraneTravelTime;
    float4 membraneSphere;
    uint4 collisionScene; // cut-block and static-instance ranges in the active body
    uint4 collisionTree;  // SRV, UAV, leaf capacity, static instance count
};
cbuffer CollisionLevel : register(b1) { uint collisionLevel; };

float3 CollisionRotate(float3 v, float4 q);

struct CollisionBounds {
    float3 lower;
    float3 upper;
};

uint CollisionOrdinal(uint index) {
    const uint nativeCount=collisionScene.w-collisionScene.z;
    return index<nativeCount?collisionScene.z+index:collisionTree.w+(index-nativeCount);
}

bool CollisionBodyInstance(uint index) {
    return index >= collisionTree.w ||
        (index >= collisionScene.z && index < collisionScene.w);
}

bool InCutSource(uint index,CutInstance object) {
    return bvhNodesDescriptor!=0xffffffffu&&
        index<cutStaticCount&&object.id==0x80000000u+index&&
        (object.flags&(1u|2u|16u))==0u;
}

bool EarlierHit(float distance,uint object,uint primitive,float best,uint bestObject,uint bestPrimitive) {
    return distance<best||(distance==best&&
        (object<bestObject||(object==bestObject&&primitive<bestPrimitive)));
}

[numthreads(64,1,1)]
void BuildProjectileBounds(uint3 thread : SV_DispatchThreadID) {
    const uint slot = thread.x + thread.y * (65535u * 64u);
    if (slot >= collisionTree.z) return;
    const uint index=CollisionOrdinal(slot);
    CollisionBounds bounds;
    bounds.lower = 3.402823466e38f;
    bounds.upper = -3.402823466e38f;
    CutInstance object;
    if (slot < (collisionScene.w-collisionScene.z)+(instanceCount-collisionTree.w) &&
        index < instanceCount && CollisionBodyInstance(index) &&
        LoadCutInstance(instanceRoutesDescriptor, index, object) &&
        object.mesh < rangeCount && (object.flags & (2u | 8u)) == 0u &&
        !InCutSource(index,object) &&
        all(abs(object.scale.xyz) >= 1e-8f)) {
        StructuredBuffer<ProjectileRange> ranges = ResourceDescriptorHeap[rangesDescriptor];
        const ProjectileRange range = ranges[object.mesh];
        if (range.indexCount != 0u) {
            const float3 center = object.positionRadius.xyz +
                CollisionRotate(range.bounds.xyz * object.scale.xyz, object.rotation);
            const float radius = range.bounds.w * max(abs(object.scale.x),
                max(abs(object.scale.y), abs(object.scale.z)));
            const float padding = max(1.0f, max(abs(center.x), max(abs(center.y), abs(center.z))) +
                radius) * 2e-6f;
            bounds.lower = center - (radius + padding);
            bounds.upper = center + (radius + padding);
        }
    }
    RWStructuredBuffer<CollisionBounds> tree = ResourceDescriptorHeap[collisionTree.y];
    tree[collisionTree.z + slot] = bounds;
}

[numthreads(64,1,1)]
void RefitProjectileBounds(uint3 thread : SV_DispatchThreadID) {
    const uint index = thread.x + thread.y * (65535u * 64u);
    if (index >= collisionLevel) return;
    const uint node = collisionLevel + index;
    RWStructuredBuffer<CollisionBounds> tree = ResourceDescriptorHeap[collisionTree.y];
    const CollisionBounds left = tree[node * 2u], right = tree[node * 2u + 1u];
    CollisionBounds result;
    result.lower = min(left.lower, right.lower);
    result.upper = max(left.upper, right.upper);
    tree[node] = result;
}

groupshared float groupDistance[64];
groupshared uint groupInstance[64];
groupshared uint groupPrimitive[64];
groupshared uint groupCutBlock[64],groupGroundHit[64];
// Only lane 0 traverses. A balanced median BVH over 32-bit block IDs has
// depth <= 32; 64 pending nodes more than covers a depth-first walk.
groupshared uint bvhStack[64];
groupshared uint bvhStackSize,bvhCandidateFirst,bvhCandidateCount;
groupshared uint bvhCandidateBase,bvhCandidatePool,bvhTraversalFailed;
groupshared uint rasterNode, rasterCandidate;

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
    RWByteAddressBuffer control=ResourceDescriptorHeap[controlBuffer];
    if(lane==0u&&buildingHitBuffer!=0xffffffffu){
        RWStructuredBuffer<BuildingImpact> output=ResourceDescriptorHeap[buildingHitBuffer];
        output[index]=(BuildingImpact)0;
        if(index==0u)output[CutImpactSlots]=(BuildingImpact)0;
    }
    Projectile p;
    if(resetSource!=0){
        p.position=float3(0,0,0);p.age=0;
        p.velocity=float3(0,0,0);p.radius=0;
        p.acceleration=float3(0,0,0);p.lifetime=0;
        p.id=0;p.kind=0;p.alive=0;p.visualRadius=0;
    }else p=source[index];
    const uint queueIndex=collisionOwner*CutImpactSlots+index;
    bool pending=false;
    if(impactBuffer!=0xffffffffu){
        RWStructuredBuffer<CutImpact> impacts=ResourceDescriptorHeap[impactBuffer];
        pending=impacts[queueIndex].pending!=0u;
    }
    if(!pending){
        if((spawnMask&1u)!=0&&index==spawnSlotLeft)p=spawnLeft;
        if((spawnMask&2u)!=0&&index==spawnSlotRight)p=spawnRight;
    }
    if(p.alive==0u||((p.kind>>8u)&255u)!=collisionOwner){
        if(lane==0u)destination[index]=p;
        return;
    }
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
    const float interval=max(0,min(deltaTime,p.lifetime-p.age));
    float remaining=interval;
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
            uint closestInstance=0xffffffffu,closestPrimitive=0xffffffffu,closestCutBlock=0xffffffffu;
            bool groundHit=false;
            // DXR covers the compact ray set. The resident raster world is
            // intersected below even when no TLAS instances exist.
            if(lane==0&&rayInstanceCount!=0){
                RaytracingAccelerationStructure scene=ResourceDescriptorHeap[sceneDescriptor];
                RayDesc ray;ray.Origin=p.position;ray.Direction=direction;ray.TMin=0;ray.TMax=distance;
                RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
                query.TraceRayInline(scene,RAY_FLAG_NONE,instanceMask,ray);
                StructuredBuffer<uint> rayMap=ResourceDescriptorHeap[rayMapDescriptor];
                while(query.Proceed()){
                    if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
                    const uint rayIndex=query.CandidateInstanceID();
                    if(rayIndex>=rayInstanceCount)continue;
                    const uint renderIndex=rayMap[rayIndex];
                    CutInstance candidate;
                    if(renderIndex<instanceCount&&CollisionBodyInstance(renderIndex)&&
                       LoadCutInstance(instanceRoutesDescriptor,renderIndex,candidate)&&
                       (candidate.flags&8u)==0u&&
                       (collisionOwner==0u||(candidate.flags&OASIS_INSTANCE_BUILDING)==0u)&&
                       !InCutSource(renderIndex,candidate)){
                        query.CommitNonOpaqueTriangleHit();
                    }
                }
                if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
                    closest=query.CommittedRayT();
                    const uint renderIndex=rayMap[query.CommittedInstanceID()];
                    CutInstance object;
                    LoadCutInstance(instanceRoutesDescriptor,renderIndex,object);
                    closestInstance=object.id;
                    closestPrimitive=query.CommittedPrimitiveIndex();
                    if((object.flags&OASIS_INSTANCE_BUILDING)!=0u)closestCutBlock=BuildingHitBlock;
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
                    bvhStackSize=1u;bvhStack[0]=collisionCutRoot;
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
                            if(blockId<collisionScene.x||blockId>=collisionScene.y)continue;
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
                        const uint objectId=0x80000000u+face.instanceId;
                        const uint primitive=bvhCandidatePool!=0u?triangleIndex:triangleIndex-bvhCandidateBase;
                        if(hit>=0.0f&&hit<=distance&&EarlierHit(hit,objectId,primitive,
                                closest,closestInstance,closestPrimitive)){
                            closest=hit;closestInstance=objectId;closestPrimitive=primitive;
                            closestCutBlock=face.pad1;
                            groundHit=((flags>>16u)&3u)==2u&&
                                face.a.normal.y+face.b.normal.y+face.c.normal.y>2.4f;
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
            // Walk the frame's conservative hierarchy without a candidate
            // cap. Only the group leader visits nodes; the group tests the mesh.
            StructuredBuffer<CollisionBounds> rasterTree=ResourceDescriptorHeap[collisionTree.x];
            if(lane==0u)rasterNode=1u;
            GroupMemoryBarrierWithGroupSync();
            for(;;){
                if(lane==0u){
                    rasterCandidate=0xffffffffu;
                    while(rasterNode!=0u){
                        const uint node=rasterNode;
                        const CollisionBounds bounds=rasterTree[node];
                        const bool overlaps=all(bounds.lower<=bounds.upper)&&
                            SegmentCutAabb(p.position,direction,distance,bounds.lower,bounds.upper);
                        if(overlaps&&node<collisionTree.z){
                            rasterNode=node*2u;
                            continue;
                        }
                        uint next=node;
                        while((next&1u)!=0u&&next>1u)next>>=1u;
                        rasterNode=next==1u?0u:next+1u;
                        if(overlaps){
                            rasterCandidate=CollisionOrdinal(node-collisionTree.z);
                            break;
                        }
                    }
                }
                GroupMemoryBarrierWithGroupSync();
                const uint renderIndex=rasterCandidate;
                if(renderIndex==0xffffffffu)break;
                // All lanes capture the candidate before the leader advances.
                GroupMemoryBarrierWithGroupSync();
                CutInstance object;
                if(!LoadCutInstance(instanceRoutesDescriptor,renderIndex,object)||
                   object.mesh>=rangeCount||(object.flags&8u)||(object.flags&2u)||
                   (collisionOwner!=0u&&(object.flags&OASIS_INSTANCE_BUILDING)!=0u))continue;
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
                    if(hit>=0&&hit<=distance&&EarlierHit(hit,object.id,triangleIndex,
                            closest,closestInstance,closestPrimitive)){
                        closest=hit;closestInstance=object.id;closestPrimitive=triangleIndex;
                        closestCutBlock=(object.flags&OASIS_INSTANCE_BUILDING)!=0u?BuildingHitBlock:0xffffffffu;
                        groundHit=false;
                    }
                }
            }
            // Scratch contains only the last transaction. If traversal fails,
            // walk the active body's live blocks for this same generation.
            if(useCutBvh&&bvhTraversalFailed!=0u){
                const uint blockDescriptor=cacheMatches?
                    (currentCache.x==0u?cacheBlocks0Descriptor:cacheBlocks1Descriptor):bvhBlocksDescriptor;
                StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
                    NonUniformResourceIndex(blockDescriptor)];
                StructuredBuffer<CutTriangle> original=ResourceDescriptorHeap[sourceTriangles];
                StructuredBuffer<CutTriangle> pool=ResourceDescriptorHeap[
                    NonUniformResourceIndex(cacheMatches?cachePoolDescriptor:sourceTriangles)];
                for(uint blockId=collisionScene.x;blockId<collisionScene.y;++blockId){
                    const CutSpatialBlock block=blocks[blockId];
                    if(block.geometryState==2u||!SegmentCutAabb(p.position,direction,distance,
                                                                block.lower,block.upper))continue;
                    const bool modified=block.geometryState==1u;
                    if(!modified){
                        CutInstance object;
                        if(!LoadCutInstance(instanceRoutesDescriptor,block.instanceId,object)||
                           !InCutSource(block.instanceId,object)||(object.flags&8u)!=0u)continue;
                    }
                    const uint first=modified?block.activeFirst:block.firstTriangle;
                    const uint count=modified?block.activeCount:block.triangleCount;
                    for(uint local=lane;local<count;local+=64u){
                        CutTriangle face;
                        if(modified)face=pool[first+local];
                        else face=original[first+local];
                        const uint flags=face.pad0;
                        const uint mask=(flags&4u)?4u:(flags&3u)?2u:1u;
                        if((flags&8u)!=0u||(mask&instanceMask)==0u)continue;
                        const float hit=TriangleDistance(p.position,direction,
                            face.a.position,face.b.position,face.c.position);
                        const uint objectId=0x80000000u+face.instanceId;
                        const uint primitive=first+local;
                        if(hit>=0.0f&&hit<=distance&&EarlierHit(hit,objectId,primitive,
                                closest,closestInstance,closestPrimitive)){
                            closest=hit;
                            closestInstance=objectId;
                            closestPrimitive=primitive;
                            closestCutBlock=face.pad1;
                            groundHit=((flags>>16u)&3u)==2u&&
                                face.a.normal.y+face.b.normal.y+face.c.normal.y>2.4f;
                        }
                    }
                }
            }
            groupDistance[lane]=closest;groupInstance[lane]=closestInstance;
            groupPrimitive[lane]=closestPrimitive;groupCutBlock[lane]=closestCutBlock;
            groupGroundHit[lane]=uint(groundHit);
            GroupMemoryBarrierWithGroupSync();
            for(uint stride=32;stride>0;stride>>=1){
                if(lane<stride){
                    const uint other=lane+stride;
                    const bool before=EarlierHit(groupDistance[other],groupInstance[other],
                        groupPrimitive[other],groupDistance[lane],groupInstance[lane],groupPrimitive[lane]);
                    if(before){
                        groupDistance[lane]=groupDistance[other];
                        groupInstance[lane]=groupInstance[other];
                        groupPrimitive[lane]=groupPrimitive[other];
                        groupCutBlock[lane]=groupCutBlock[other];
                        groupGroundHit[lane]=groupGroundHit[other];
                    }
                }
                GroupMemoryBarrierWithGroupSync();
            }
            // A reused frame slot can still own a pending CPU readback.
            // Retain this projectile at the boundary until an event slot is available.
            if(groupCutBlock[0]==BuildingHitBlock&&buildingHitBuffer==0xffffffffu){
                if(lane==0u)destination[index]=p;
                return;
            }
            if(lane==0u&&membraneEvents!=0xffffffffu)
                CrossMembrane(p.position,direction,min(distance,groupDistance[0]));
            if(groupInstance[0]!=0xffffffffu){
                if(lane==0){
                    const float3 position=p.position+direction*groupDistance[0];
                    if(groupCutBlock[0]==BuildingHitBlock&&buildingHitBuffer!=0xffffffffu){
                        RWStructuredBuffer<BuildingImpact> output=ResourceDescriptorHeap[buildingHitBuffer];
                        BuildingImpact hit;
                        hit.position=position;hit.renderId=groupInstance[0];
                        hit.direction=direction;hit.projectile=p.id;
                        hit.origin=p.position;
                        hit.cpuMetadata=0u;
                        hit.hitTime=interval-remaining+step*(groupDistance[0]/distance);
                        output[index]=hit;
                    }else if(impactBuffer!=0xffffffffu&&groupCutBlock[0]!=0xffffffffu&&
                       groupCutBlock[0]>=collisionScene.x&&groupCutBlock[0]<collisionScene.y){
                        RWStructuredBuffer<CutImpact> impacts=ResourceDescriptorHeap[impactBuffer];
                        CutImpact hit;
                        hit.position=position;hit.radius=p.radius;hit.direction=direction;
                        hit.projectileId=p.id;hit.instanceId=groupInstance[0];
                        hit.primitiveIndex=groupPrimitive[0];hit.tick=collisionTick;
                        hit.recess=p.kind>>31u;
                        if(hit.recess!=0u&&groupGroundHit[0]!=0u&&collisionOwner==0u&&
                           direction.y<-.07f){
                            hit.recess|=TerrainRecess;
                            hit.direction=float3(0,-1,0);
                            hit.radius=max(hit.radius,.025f);
                        }
                        hit.hitTime=interval-remaining+step*(groupDistance[0]/distance);
                        hit.blockId=groupCutBlock[0];hit.owner=collisionOwner;hit.pending=1u;
                        impacts[queueIndex]=hit;
                    }
                    p.position=position;p.alive=0;destination[index]=p;
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
