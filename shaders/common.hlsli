#define MaxViews 4
#define ResourceAbiVersion 1
// Domain IDs are append-only ABI. New systems append without renumbering existing domains.
static const uint ResourceDomainMeshes=0;
static const uint ResourceDomainFrame=1;
static const uint ResourceDomainLighting=2;
static const uint ResourceDomainHistory=3;
static const uint ResourceDomainAnimation=4;
static const uint ResourceDomainVisibility=5;
static const uint ResourceDomainRayTracing=6;

static const uint MeshVertices=0;
static const uint MeshMeshlets=1;
static const uint MeshRemap=2;
static const uint MeshTriangles=3;
static const uint MeshRanges=4;
static const uint MeshMaterials=5;
static const uint MeshIndices=6;

static const uint FrameInstances=0;
// Frame table slots 1, 2 and 4-7 are reserved to preserve the existing ABI.
static const uint FrameInstanceUAV=3;
static const uint FramePublishedInstances=18;
static const uint FrameInstanceRanges=19;
static const uint FrameCpuDepthArguments=8;
static const uint FrameCpuDepthCounter=9;
static const uint FramePublishedDepthArguments=10;
static const uint FramePublishedDepthCounter=11;
static const uint FrameCpuCullArguments=12;
static const uint FrameCpuCullCounter=13;
static const uint FramePublishedCullArguments=14;
static const uint FramePublishedCullCounter=15;
static const uint FrameCpuShadowArguments=16;
static const uint FrameCpuShadowCounter=17;
static const uint FrameCpuBatchVisibilityUAV=20;
static const uint FrameCpuBatchVisibilitySRV=21;
static const uint FrameCpuBatchArgumentsUAV=22;
static const uint FrameCpuBatchCountUAV=23;
static const uint FrameTileMarksUAV=24,FrameTileMarksSRV=25;
static const uint FrameTilePrefixesUAV=26,FrameTilePrefixesSRV=27;
static const uint FrameTileTotalsUAV=28,FrameTileTotalsSRV=29;
static const uint FrameTileOffsetsUAV=30,FrameTileOffsetsSRV=31;

static const uint LightingLights=0;
static const uint LightingClusters=1;
static const uint LightingClusterUAV=2;
static const uint LightingShadowAtlas=3;
static const uint LightingClusterIndices=4;
static const uint LightingClusterIndicesUAV=5;
static const uint LightingClusterGroupCounts=6;
static const uint LightingClusterGroupCountsUAV=7;
static const uint LightingClusterGroupOffsets=8;
static const uint LightingClusterGroupOffsetsUAV=9;
static const uint LightingDirectionalIndices=10;
static const uint LightingDirectionalIndicesUAV=11;
static const uint LightingDirectionalCount=12;
static const uint LightingDirectionalCountUAV=13;
static const uint LightingClusterBounds=14;
static const uint LightingClusterBoundsUAV=15;
static const uint LightingBvhNodes=16,LightingBvhNodesUAV=17;
static const uint LightingBvhPairs=18,LightingBvhPairsUAV=19;
static const uint LightingBvhBounds=20,LightingBvhBoundsUAV=21;

static const uint HistoryHierarchy=0;
static const uint HistoryDepthMS=1;
static const uint HistoryColorResolved=2;
static const uint HistoryReflectionResolved=3;
static const uint HistoryPrevious=4;
static const uint HistoryDepthPrevious=5;
static const uint HistoryOutput=6;
static const uint HistoryDepthOutput=7;
static const uint HistoryCurrent=8;
static const uint HistoryDepthResolved=9;
static const uint HistoryHiZMipBase=10;

static const uint AnimationDynamicVertices=0;
static const uint AnimationDynamicVerticesUAV=1;
static const uint AnimationDeformations=2;
static const uint AnimationMotions=3;
static const uint AnimationBones=4;
static const uint AnimationWeights=5;
static const uint AnimationSkin=6;
static const uint AnimationMorphs=7;
static const uint AnimationDeformedBounds=8;
static const uint AnimationDeformedBoundsUAV=9;

static const uint VisibilityHotBounds=0;
static const uint VisibilityHotBoundsUAV=1;
static const uint VisibilityHotMeshes=2;
static const uint VisibilityHotMeshesUAV=3;

static const uint RayTLAS=0;
static const uint RayDescriptorsSlot=1;
static const uint RayBlasAddresses=2;
static const uint RayInstanceMapSlot=3;

// Frozen MeshSolid ABI: transform + mesh + material + id + flags only.
struct GpuInstance {float4 positionRadius;float4 rotation;float4 scale;uint mesh;uint material;uint id;uint flags;};
struct Vertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct Meshlet {uint vertexOffset;uint triangleOffset;uint vertexCount;uint triangleCount;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
struct TextureMapping {float4 offsetScale;float4 rotationSet;};
struct Surface {float4 colorRoughness;float4 emissionMetallic;uint albedo;uint normal;uint metalRough;uint emissive;float4 parameters;uint4 extra;float4 opacity;TextureMapping mapping[5];float4 procedural;};
struct Light {float4 positionRadius;float4 colorPower;float4 directionOuter;float4 parameters;};
// Structured GPU nodes use the same 32-byte layout as LightBvhNode in C++.
// P leaves are at [P,2P); index zero is never traversed.
struct LightBvhNode {
    float3 low;uint lightIndex;
    float3 high;uint reserved;
};
struct Deformation {float4 axisAmplitude;float4 frequencyPhase;};
cbuffer Frame:register(b0){
    row_major float4x4 vp[MaxViews];
    row_major float4x4 inverseVP[MaxViews];
    row_major float4x4 previousVP[MaxViews];
    row_major float4x4 shadowVP[24];
    float4 eye[MaxViews];
    uint4 resourceAbi;
    uint4 dimensions;
    uint4 outputDimensions;
    uint4 counts;
    uint4 topology;
    float4 timing;
    float4 tileViewer;
    uint4 lightBvhLayout;
};
cbuffer Draw:register(b1){uint objectIndex;uint passIndex;uint param0;uint param1;uint param2;};

uint DomainTableDescriptor(uint domain){StructuredBuffer<uint4> directory=ResourceDescriptorHeap[resourceAbi.x];return directory[domain].x;}
uint DomainResource(uint domain,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(DomainTableDescriptor(domain))];return table[slot].x;}
// Domain-local bindless tables use the same stable {descriptor,generation} entry format
// without adding a position to the renderer's global resource directory.
uint GpuDomainResource(uint tableDescriptor,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(tableDescriptor)];return table[slot].x;}
// SRV/UAV handles for semantic frame resources are resolved by GpuFrameContext on the
// CPU and passed to a domain through its own root constants/table. They never consume
// slots in this renderer-global ABI.
uint GpuFrameResourceDescriptor(uint descriptorIndex){return descriptorIndex;}

#define WorldInstances DomainResource(ResourceDomainFrame,FrameInstances)
#define PublishedInstances DomainResource(ResourceDomainFrame,FramePublishedInstances)
#define InstanceRanges DomainResource(ResourceDomainFrame,FrameInstanceRanges)
#define Vertices DomainResource(ResourceDomainMeshes,MeshVertices)
#define Meshlets DomainResource(ResourceDomainMeshes,MeshMeshlets)
#define Remap DomainResource(ResourceDomainMeshes,MeshRemap)
#define Triangles DomainResource(ResourceDomainMeshes,MeshTriangles)
#define Ranges DomainResource(ResourceDomainMeshes,MeshRanges)
#define Materials DomainResource(ResourceDomainMeshes,MeshMaterials)
#define Indices DomainResource(ResourceDomainMeshes,MeshIndices)

#define InstanceUAV DomainResource(ResourceDomainFrame,FrameInstanceUAV)
#define CpuDepthArguments DomainResource(ResourceDomainFrame,FrameCpuDepthArguments)
#define CpuDepthCounter DomainResource(ResourceDomainFrame,FrameCpuDepthCounter)
#define PublishedDepthArguments DomainResource(ResourceDomainFrame,FramePublishedDepthArguments)
#define PublishedDepthCounter DomainResource(ResourceDomainFrame,FramePublishedDepthCounter)
#define CpuCullArguments DomainResource(ResourceDomainFrame,FrameCpuCullArguments)
#define CpuCullCounter DomainResource(ResourceDomainFrame,FrameCpuCullCounter)
#define PublishedCullArguments DomainResource(ResourceDomainFrame,FramePublishedCullArguments)
#define PublishedCullCounter DomainResource(ResourceDomainFrame,FramePublishedCullCounter)
#define CpuShadowArguments DomainResource(ResourceDomainFrame,FrameCpuShadowArguments)
#define CpuShadowCounter DomainResource(ResourceDomainFrame,FrameCpuShadowCounter)
#define CpuBatchVisibilityUAV DomainResource(ResourceDomainFrame,FrameCpuBatchVisibilityUAV)
#define CpuBatchVisibilitySRV DomainResource(ResourceDomainFrame,FrameCpuBatchVisibilitySRV)
#define CpuBatchArgumentsUAV DomainResource(ResourceDomainFrame,FrameCpuBatchArgumentsUAV)
#define CpuBatchCountUAV DomainResource(ResourceDomainFrame,FrameCpuBatchCountUAV)
#define TileMarksUAV DomainResource(ResourceDomainFrame,FrameTileMarksUAV)
#define TileMarksSRV DomainResource(ResourceDomainFrame,FrameTileMarksSRV)
#define TilePrefixesUAV DomainResource(ResourceDomainFrame,FrameTilePrefixesUAV)
#define TilePrefixesSRV DomainResource(ResourceDomainFrame,FrameTilePrefixesSRV)
#define TileTotalsUAV DomainResource(ResourceDomainFrame,FrameTileTotalsUAV)
#define TileTotalsSRV DomainResource(ResourceDomainFrame,FrameTileTotalsSRV)
#define TileOffsetsUAV DomainResource(ResourceDomainFrame,FrameTileOffsetsUAV)
#define TileOffsetsSRV DomainResource(ResourceDomainFrame,FrameTileOffsetsSRV)

#define Lights DomainResource(ResourceDomainLighting,LightingLights)
#define Clusters DomainResource(ResourceDomainLighting,LightingClusters)
#define ClusterUAV DomainResource(ResourceDomainLighting,LightingClusterUAV)
#define ShadowAtlas DomainResource(ResourceDomainLighting,LightingShadowAtlas)
#define ClusterIndices DomainResource(ResourceDomainLighting,LightingClusterIndices)
#define ClusterIndicesUAV DomainResource(ResourceDomainLighting,LightingClusterIndicesUAV)
#define ClusterGroupCounts DomainResource(ResourceDomainLighting,LightingClusterGroupCounts)
#define ClusterGroupCountsUAV DomainResource(ResourceDomainLighting,LightingClusterGroupCountsUAV)
#define ClusterGroupOffsets DomainResource(ResourceDomainLighting,LightingClusterGroupOffsets)
#define ClusterGroupOffsetsUAV DomainResource(ResourceDomainLighting,LightingClusterGroupOffsetsUAV)
#define DirectionalIndices DomainResource(ResourceDomainLighting,LightingDirectionalIndices)
#define DirectionalIndicesUAV DomainResource(ResourceDomainLighting,LightingDirectionalIndicesUAV)
#define DirectionalCount DomainResource(ResourceDomainLighting,LightingDirectionalCount)
#define DirectionalCountUAV DomainResource(ResourceDomainLighting,LightingDirectionalCountUAV)
#define ClusterBounds DomainResource(ResourceDomainLighting,LightingClusterBounds)
#define ClusterBoundsUAV DomainResource(ResourceDomainLighting,LightingClusterBoundsUAV)
#define LightBvhNodes DomainResource(ResourceDomainLighting,LightingBvhNodes)
#define LightBvhNodesUAV DomainResource(ResourceDomainLighting,LightingBvhNodesUAV)
#define LightBvhPairs DomainResource(ResourceDomainLighting,LightingBvhPairs)
#define LightBvhPairsUAV DomainResource(ResourceDomainLighting,LightingBvhPairsUAV)
#define LightBvhBounds DomainResource(ResourceDomainLighting,LightingBvhBounds)
#define LightBvhBoundsUAV DomainResource(ResourceDomainLighting,LightingBvhBoundsUAV)

#define Hierarchy DomainResource(ResourceDomainHistory,HistoryHierarchy)
#define DepthMS DomainResource(ResourceDomainHistory,HistoryDepthMS)
#define ColorResolved DomainResource(ResourceDomainHistory,HistoryColorResolved)
#define ReflectionResolved DomainResource(ResourceDomainHistory,HistoryReflectionResolved)
#define HistoryPrevious DomainResource(ResourceDomainHistory,HistoryPrevious)
#define HistoryDepthPrevious DomainResource(ResourceDomainHistory,HistoryDepthPrevious)
#define HistoryOutput DomainResource(ResourceDomainHistory,HistoryOutput)
#define HistoryDepthOutput DomainResource(ResourceDomainHistory,HistoryDepthOutput)
#define HistoryCurrent DomainResource(ResourceDomainHistory,HistoryCurrent)
#define DepthResolved DomainResource(ResourceDomainHistory,HistoryDepthResolved)
uint HiZUAV(uint mip){return DomainResource(ResourceDomainHistory,HistoryHiZMipBase+mip);}

#define DynamicVertices DomainResource(ResourceDomainAnimation,AnimationDynamicVertices)
#define DynamicVerticesUAV DomainResource(ResourceDomainAnimation,AnimationDynamicVerticesUAV)
#define Deformations DomainResource(ResourceDomainAnimation,AnimationDeformations)
#define Motions DomainResource(ResourceDomainAnimation,AnimationMotions)
#define Bones DomainResource(ResourceDomainAnimation,AnimationBones)
#define Weights DomainResource(ResourceDomainAnimation,AnimationWeights)
#define Skin DomainResource(ResourceDomainAnimation,AnimationSkin)
#define Morphs DomainResource(ResourceDomainAnimation,AnimationMorphs)
#define DeformedBounds DomainResource(ResourceDomainAnimation,AnimationDeformedBounds)
#define DeformedBoundsUAV DomainResource(ResourceDomainAnimation,AnimationDeformedBoundsUAV)

#define HotBounds DomainResource(ResourceDomainVisibility,VisibilityHotBounds)
#define HotBoundsUAV DomainResource(ResourceDomainVisibility,VisibilityHotBoundsUAV)
#define HotMeshes DomainResource(ResourceDomainVisibility,VisibilityHotMeshes)
#define HotMeshesUAV DomainResource(ResourceDomainVisibility,VisibilityHotMeshesUAV)

#define TLAS DomainResource(ResourceDomainRayTracing,RayTLAS)
#define RayDescriptors DomainResource(ResourceDomainRayTracing,RayDescriptorsSlot)
#define BlasAddresses DomainResource(ResourceDomainRayTracing,RayBlasAddresses)
#define RayInstanceMapDescriptor DomainResource(ResourceDomainRayTracing,RayInstanceMapSlot)
uint RenderInstanceForRay(uint compactIndex){
    StructuredBuffer<uint> map=ResourceDescriptorHeap[RayInstanceMapDescriptor];
    return map[compactIndex];
}

#define HiZMips dimensions.z
#define Width dimensions.x
#define Height dimensions.y
#define OutputWidth outputDimensions.x
#define OutputHeight outputDimensions.y
#define InstanceCount counts.x
#define RayInstanceCount topology.w
#define AdditionalCount counts.y
#define LightCount counts.z
// Cluster cache: compact entries at cluster*32, 0xffffffff signals overflow.
static const uint ClusterLocalCapacity=32u;
static const uint ClusterOverflow=0xffffffffu;
// Reuse the same 16-byte frame-constant footprint as the retired reflection grid.
#define LightBvhLeafCount lightBvhLayout.x
// Stackless exact BVH traversal shared by later consumers. Returning UINT_MAX
// denotes exhaustion, not an index cap; every valid leaf remains addressable.
uint LightBvhAfter(uint node){
    while(node>1u&&(node&1u)!=0u)node>>=1u;
    return node==1u?0u:node+1u;
}
bool LightBvhTouches(LightBvhNode bounds,float3 center,float radius){
    // Rejection is conservative; boxes are inflated during GPU leaf refits.
    if(any(bounds.low>bounds.high))return false;
    float3 delta=max(max(bounds.low-center,center-bounds.high),0.0f);
    return dot(delta,delta)<=radius*radius;
}
uint LightBvhNextLight(inout uint cursor,uint leaves,float3 center,float radius){
    if(LightCount==0u||leaves==0u){
        cursor=0u;
        return 0xffffffffu;
    }
    StructuredBuffer<LightBvhNode> nodes=ResourceDescriptorHeap[LightBvhNodes];
    [loop]while(cursor!=0u){
        uint nodeIndex=cursor;
        LightBvhNode node=nodes[nodeIndex];
        if(!LightBvhTouches(node,center,radius)){
            cursor=LightBvhAfter(nodeIndex);
            continue;
        }
        if(nodeIndex<leaves){
            cursor=nodeIndex*2u;
            continue;
        }
        cursor=LightBvhAfter(nodeIndex);
        if(node.lightIndex<LightCount)return node.lightIndex;
    }
    return 0xffffffffu;
}


#define HistoryValid counts.w
#define ViewCount topology.x
static const float Pi=3.14159265358979323846;
float3 Rotate(float3 v,float4 q){return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
float3 World(float3 p,GpuInstance i){return i.positionRadius.xyz+Rotate(p*i.scale.xyz,i.rotation);}
float3 Unproject(float2 uv,float depth,uint view){float4 p=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),inverseVP[view]);return p.xyz/p.w;}
float DepthAt(float z){return (timing.x*timing.y/z-timing.x)/(timing.y-timing.x);}
float LinearDepth(float d){return timing.x*timing.y/(timing.x+d*(timing.y-timing.x));}
uint ClusterIndex(uint2 pixel,float depth,uint view){uint x=(Width+63)/64,y=(Height+63)/64;uint z=min(23u,(uint)(saturate(log(max(LinearDepth(depth),timing.x)/timing.x)/log(timing.y/timing.x))*24));return (((view*24+z)*y+min(pixel.y/64,y-1))*x+min(pixel.x/64,x-1));}

#define Quality ((uint)timing.w)

// Global IDs retain their normal CPU/simulator address space. Resident
// 120-m world tiles have their own persistent GPU storage and range entries.
struct InstanceSourceRange {uint first;uint count;uint descriptor;uint reserved;};
struct ResidentTileInstance {
    GpuInstance instance;
    float4 sector; // X,Z, conservative radius, unused.
    uint farMesh,hideFar,enabled,reserved;
};
InstanceSourceRange ResolveInstanceRoute(uint globalIndex){
    StructuredBuffer<InstanceSourceRange> routes=ResourceDescriptorHeap[InstanceRanges];
    uint routeCount=routes[0].reserved;
    InstanceSourceRange route=routes[0];
    if(globalIndex>=route.count){
        uint lo=1,hi=routeCount;
        while(lo<hi){
            uint mid=(lo+hi)>>1;
            if(routes[mid].first<=globalIndex)lo=mid+1;
            else hi=mid;
        }
        route=routes[lo-1];
    }
    return route;
}
// Shadow maps, TLAS and opacity queries read unchanged tile geometry.
// An inactive weather object is explicitly masked, not camera-culled.
GpuInstance LoadInstanceStable(uint globalIndex){
    InstanceSourceRange route=ResolveInstanceRoute(globalIndex);
    uint local=globalIndex-route.first;
    if(route.reserved!=1){
        StructuredBuffer<GpuInstance> instances=
            ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
        return instances[local];
    }
    StructuredBuffer<ResidentTileInstance> instances=
        ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
    ResidentTileInstance tile=instances[local];
    GpuInstance instance=tile.instance;
    if(tile.enabled==0)instance.flags|=4u|32u|128u;
    return instance;
}
// Only the raster-facing path uses the current camera and authored LOD.
GpuInstance LoadInstance(uint globalIndex){
    InstanceSourceRange route=ResolveInstanceRoute(globalIndex);
    uint local=globalIndex-route.first;
    if(route.reserved!=1){
        StructuredBuffer<GpuInstance> instances=
            ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
        return instances[local];
    }
    StructuredBuffer<ResidentTileInstance> instances=
        ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
    ResidentTileInstance tile=instances[local];
    GpuInstance instance=tile.instance;
    float2 eye=tileViewer.xy;
    float2 cellDelta=tile.sector.xy-eye;
    float envelope=timing.y+48.0+tile.sector.z;
    if(tile.enabled==0||dot(cellDelta,cellDelta)>envelope*envelope){
        instance.flags|=4u|32u|128u;return instance;
    }
    float2 delta=instance.positionRadius.xz-eye;
    if(dot(delta,delta)>900.0){
        if(tile.hideFar!=0){
            instance.flags|=4u|32u|128u;return instance;
        }
        if(tile.farMesh!=0xffffffffu){
            instance.mesh=tile.farMesh;
            StructuredBuffer<MeshRange> meshes=ResourceDescriptorHeap[Ranges];
            float3 scale=abs(instance.scale.xyz);
            instance.positionRadius.w=meshes[instance.mesh].bounds.w*
                max(scale.x,max(scale.y,scale.z));
        }
    }
    return instance;
}

struct SkinVertex {uint4 joints;uint4 joints1;float4 weights;float4 weights1;};
struct MorphVertex {float3 position;float3 normal;};
struct Motion {row_major float4x4 transform;row_major float4x4 normalTransform;uint4 binding;uint4 morph;};
struct Bone {row_major float4x4 transform;row_major float4x4 normalTransform;};
float2 MaterialUV(Surface material,uint slot,float2 uv0,float2 uv1){TextureMapping m=material.mapping[slot];float2 uv=(m.rotationSet.z>.5?uv1:uv0)*m.offsetScale.zw;return float2(m.rotationSet.x*uv.x-m.rotationSet.y*uv.y,m.rotationSet.y*uv.x+m.rotationSet.x*uv.y)+m.offsetScale.xy;}
float4 SampleMaterial(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.Sample(sampleState,MaterialUV(material,slot,uv0,uv1));}
float4 SampleMaterialLevel(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.SampleLevel(sampleState,MaterialUV(material,slot,uv0,uv1),0);}
float2 RayOpacity(uint instanceIndex,uint primitive,float2 barycentric){
    instanceIndex=RenderInstanceForRay(instanceIndex);
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];GpuInstance instance=LoadInstanceStable(instanceIndex);Surface material=materials[instance.material];
    if(material.extra.y==0)return float2(1,0);
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];StructuredBuffer<uint> indices=ResourceDescriptorHeap[Indices];StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];MeshRange range=ranges[instance.mesh];uint index=range.firstIndex+primitive*3;Vertex a=vertices[indices[index]],b=vertices[indices[index+1]],c=vertices[indices[index+2]];
    float3 weights=float3(1-barycentric.x-barycentric.y,barycentric);float2 uv0=a.uv*weights.x+b.uv*weights.y+c.uv*weights.z,uv1=a.uv1*weights.x+b.uv1*weights.y+c.uv1*weights.z;
    float alpha=material.opacity.x;if(material.albedo!=0xffffffff)alpha*=SampleMaterialLevel(material.albedo,material,0,uv0,uv1).a;
    return float2(material.extra.y==1?(alpha>=material.opacity.y?1:0):saturate(alpha),material.extra.y);
}

#include "optical_common.hlsli"
