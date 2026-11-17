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
    uint sourceTriangles,bvhBlocks1Descriptor,cutPoolDescriptor,stateDescriptor;
    uint cutOriginalCount,cutStaticCount,bvhNodesDescriptor,bvhBlocksDescriptor;
};

groupshared float groupDistance[64];
groupshared uint groupInstance[64];
groupshared uint groupPrimitive[64];
// Only lane 0 traverses. A balanced median BVH over 32-bit block IDs has
// depth <= 32; 64 pending nodes more than covers a depth-first walk.
groupshared uint bvhStack[64];
groupshared uint bvhStackSize,bvhCandidateFirst,bvhCandidateCount;
groupshared uint bvhCandidateInstance,bvhCandidateBase,bvhTraversalFailed;
groupshared uint bvhCandidateState,bvhCandidateSource;

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
    // cutState.y selects the metadata table, cutState.z is the generation.
    bool baked=currentCut.z!=0u;
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
            // Inspect DXR candidates before committing: otherwise a deleted
            // source face could hide a valid non-destructible object behind it.
            bool useSourceBvh=sourceTriangles!=0xffffffffu&&
                bvhNodesDescriptor!=0xffffffffu&&bvhBlocksDescriptor!=0xffffffffu&&
                bvhBlocks1Descriptor!=0xffffffffu&&cutPoolDescriptor!=0xffffffffu;
            if(lane==0u&&rayInstanceCount!=0u){
                RaytracingAccelerationStructure scene=ResourceDescriptorHeap[sceneDescriptor];
                RayDesc ray;ray.Origin=p.position;ray.Direction=direction;ray.TMin=0;ray.TMax=distance;
                RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
                query.TraceRayInline(scene,RAY_FLAG_FORCE_NON_OPAQUE,instanceMask,ray);
                StructuredBuffer<uint> rayMap=ResourceDescriptorHeap[rayMapDescriptor];
                while(query.Proceed()){
                    if(query.CandidateType()!=CANDIDATE_NON_OPAQUE_TRIANGLE)continue;
                    uint rayIndex=query.CandidateInstanceID();
                    if(rayIndex>=rayInstanceCount)continue;
                    uint renderIndex=rayMap[rayIndex];
                    CutInstance candidate;
                    if(renderIndex>=instanceCount||
                       !LoadCutInstance(instanceRoutesDescriptor,renderIndex,candidate)||
                       (candidate.flags&8u)!=0u)continue;
                    bool covered=useSourceBvh&&renderIndex<cutStaticCount&&
                        candidate.id==(0x80000000u+renderIndex)&&
                        (candidate.flags&(1u|2u|16u))==0u;
                    if(!covered)query.CommitNonOpaqueTriangleHit();
                }
                if(query.CommittedStatus()==COMMITTED_TRIANGLE_HIT){
                    uint rayIndex=query.CommittedInstanceID();
                    if(rayIndex<rayInstanceCount){
                        uint renderIndex=rayMap[rayIndex];
                        if(renderIndex<instanceCount){
                            closest=query.CommittedRayT();
                            closestInstance=renderIndex;
                            closestPrimitive=query.CommittedPrimitiveIndex();
                        }
                    }
                }
            }
            // Intact destructible surfaces: walk only spatially intersected
            // source blocks. The stack and candidate are group-shared; lane 0
            // publishes one leaf, then every lane tests one exact triangle.
            // No early return on the first candidate: closest is accumulated
            // across all leaves and later reduced with the other collision paths.
            if(useSourceBvh){
                StructuredBuffer<CutSpatialNode> nodes=ResourceDescriptorHeap[bvhNodesDescriptor];
                StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
                    NonUniformResourceIndex(currentCut.y==0u?
                        bvhBlocksDescriptor:bvhBlocks1Descriptor)];
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
                            if(block.geometryState==2u)continue; // truly Empty
                            if(block.geometryState>2u){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            uint first=block.geometryState==0u?
                                block.firstTriangle:block.activeFirst;
                            uint count=block.geometryState==0u?
                                block.triangleCount:block.activeCount;
                            uint bound=cutOriginalCount;
                            if(block.geometryState==1u){
                                StructuredBuffer<CutTriangle> pool=ResourceDescriptorHeap[cutPoolDescriptor];
                                uint stride;pool.GetDimensions(bound,stride);
                            }
                            if(first>bound||count>bound-first){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            if(count==0u)continue;
                            CutInstance object;
                            if(block.instanceId>=cutStaticCount||
                               block.instanceId>=instanceCount||
                               !LoadCutInstance(instanceRoutesDescriptor,block.instanceId,object)||
                               object.id!=(0x80000000u+block.instanceId)||
                               (object.flags&(1u|2u|8u|16u))!=0u||
                               any(abs(object.scale.xyz)<1e-8f))continue;
                            uint mask=(object.flags&4u)?4u:(object.flags&3u)?2u:1u;
                            if((mask&instanceMask)==0u)continue;
                            // Blocks are in source-instance order. Find the
                            // first block of this instance to restore the local
                            // primitive index used by the old raster collision.
                            uint low=0u,high=blockCount;
                            while(low<high){
                                uint mid=(low+high)>>1;
                                if(blocks[mid].instanceId<block.instanceId)low=mid+1u;
                                else high=mid;
                            }
                            if(low==blockCount||blocks[low].instanceId!=block.instanceId){
                                bvhTraversalFailed=1u;bvhStackSize=0u;break;
                            }
                            bvhCandidateFirst=first;
                            bvhCandidateCount=count;
                            bvhCandidateInstance=block.instanceId;
                            bvhCandidateBase=blocks[low].firstTriangle;
                            bvhCandidateState=block.geometryState;
                            bvhCandidateSource=block.geometryState==0u?
                                sourceTriangles:cutPoolDescriptor;
                            break;
                        }
                    }
                    // Group-uniform barrier. No lane exits the candidate loop
                    // until lane 0 has finished publishing the next leaf.
                    GroupMemoryBarrierWithGroupSync();
                    if(bvhCandidateCount==0u)break;
                    StructuredBuffer<CutTriangle> activeFaces=ResourceDescriptorHeap[
                        NonUniformResourceIndex(bvhCandidateSource)];
                    // Modified blocks are unbounded by the original 64
                    // triangles. The same group cooperates in 64-wide rounds.
                    for(uint i=lane;i<bvhCandidateCount;i+=64u){
                        uint triangleIndex=bvhCandidateFirst+i;
                        CutTriangle face=activeFaces[triangleIndex];
                        float hit=TriangleDistance(p.position,direction,
                            face.a.position,face.b.position,face.c.position);
                        if(hit>=0.0f&&hit<=distance&&hit<closest){
                            closest=hit;
                            closestInstance=bvhCandidateInstance;
                            closestPrimitive=bvhCandidateState==0u?
                                triangleIndex-bvhCandidateBase:triangleIndex;
                        }
                    }
                    // Every lane finishes this leaf before lane 0 reuses the
                    // shared candidate fields on the next traversal iteration.
                    GroupMemoryBarrierWithGroupSync();
                }
            }
            // A malformed/overflowing BVH must never make a surviving face
            // disappear, nor restore original triangles inside an old hole.
            // Exceptional fallback scans the live block table, not cutOriginal.
            if(useSourceBvh&&bvhTraversalFailed!=0u){
                StructuredBuffer<CutSpatialBlock> blocks=ResourceDescriptorHeap[
                    NonUniformResourceIndex(currentCut.y==0u?
                        bvhBlocksDescriptor:bvhBlocks1Descriptor)];
                uint blockCount,stride;blocks.GetDimensions(blockCount,stride);
                for(uint blockId=0u;blockId<blockCount;++blockId){
                    CutSpatialBlock block=blocks[blockId];
                    if(block.geometryState>=2u||
                       !SegmentCutAabb(p.position,direction,distance,block.lower,block.upper))
                        continue;
                    CutInstance object;
                    if(block.instanceId>=instanceCount||
                       !LoadCutInstance(instanceRoutesDescriptor,block.instanceId,object)||
                       object.id!=(0x80000000u+block.instanceId)||
                       (object.flags&(1u|2u|8u|16u))!=0u||
                       any(abs(object.scale.xyz)<1e-8f))continue;
                    uint mask=(object.flags&4u)?4u:(object.flags&3u)?2u:1u;
                    if((mask&instanceMask)==0u)continue;
                    uint first=block.geometryState==0u?
                        block.firstTriangle:block.activeFirst;
                    uint count=block.geometryState==0u?
                        block.triangleCount:block.activeCount;
                    StructuredBuffer<CutTriangle> faces=ResourceDescriptorHeap[
                        NonUniformResourceIndex(block.geometryState==0u?
                            sourceTriangles:cutPoolDescriptor)];
                    for(uint i=lane;i<count;i+=64u){
                        uint triIndex=first+i;
                        CutTriangle face=faces[triIndex];
                        float hit=TriangleDistance(p.position,direction,
                            face.a.position,face.b.position,face.c.position);
                        if(hit>=0.0f&&hit<=distance&&hit<closest){
                            closest=hit;closestInstance=block.instanceId;
                            closestPrimitive=triIndex;
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
                   object.mesh>=rangeCount||(object.flags&8u)||(object.flags&2u))continue;
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
                bool covered=renderIndex<cutStaticCount&&
                    object.id==(0x80000000u+renderIndex)&&
                    (object.flags&(1u|2u|16u))==0u&&range.indexCount!=0u;
                // The original raster mesh must never reclose a cut hole.
                if(covered&&(baked||(useSourceBvh&&bvhTraversalFailed==0u)))continue;
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
