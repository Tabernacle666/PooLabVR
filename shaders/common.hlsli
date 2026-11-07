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
static const uint FrameArgumentsSlot=1;
static const uint FrameCounterSlot=2;
static const uint FrameInstanceUAV=3;
static const uint FrameCullArguments=4;
static const uint FrameCullCounter=5;
static const uint FrameShadowArguments=6;
static const uint FrameShadowCounter=7;

static const uint LightingLights=0;
static const uint LightingClusters=1;
static const uint LightingClusterUAV=2;
static const uint LightingShadowAtlas=3;

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

// Frozen MeshSolid ABI: transform + mesh + material + id + flags only.
struct GpuInstance {float4 positionRadius;float4 rotation;float4 scale;uint mesh;uint material;uint id;uint flags;};
struct Vertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct Meshlet {uint vertexOffset;uint triangleOffset;uint vertexCount;uint triangleCount;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
struct TextureMapping {float4 offsetScale;float4 rotationSet;};
struct Surface {float4 colorRoughness;float4 emissionMetallic;uint albedo;uint normal;uint metalRough;uint emissive;float4 parameters;uint4 extra;float4 opacity;TextureMapping mapping[5];float4 procedural;};
struct Light {float4 positionRadius;float4 colorPower;float4 directionOuter;float4 parameters;};
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
};
cbuffer Draw:register(b1){uint objectIndex;uint passIndex;uint param0;uint param1;};

uint DomainTableDescriptor(uint domain){StructuredBuffer<uint4> directory=ResourceDescriptorHeap[resourceAbi.x];return directory[domain].x;}
uint DomainResource(uint domain,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(DomainTableDescriptor(domain))];return table[slot].x;}
// Domain-local bindless tables use the same stable {descriptor,generation} entry format
// without adding a position to the renderer's global resource directory.
uint GpuDomainResource(uint tableDescriptor,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(tableDescriptor)];return table[slot].x;}
// SRV/UAV handles for semantic frame resources are resolved by GpuFrameContext on the
// CPU and passed to a domain through its own root constants/table. They never consume
// slots in this renderer-global ABI.
uint GpuFrameResourceDescriptor(uint descriptorIndex){return descriptorIndex;}

#define Instances DomainResource(ResourceDomainFrame,FrameInstances)
#define Vertices DomainResource(ResourceDomainMeshes,MeshVertices)
#define Meshlets DomainResource(ResourceDomainMeshes,MeshMeshlets)
#define Remap DomainResource(ResourceDomainMeshes,MeshRemap)
#define Triangles DomainResource(ResourceDomainMeshes,MeshTriangles)
#define Ranges DomainResource(ResourceDomainMeshes,MeshRanges)
#define Materials DomainResource(ResourceDomainMeshes,MeshMaterials)
#define Indices DomainResource(ResourceDomainMeshes,MeshIndices)

#define Arguments DomainResource(ResourceDomainFrame,FrameArgumentsSlot)
#define Counter DomainResource(ResourceDomainFrame,FrameCounterSlot)
#define InstanceUAV DomainResource(ResourceDomainFrame,FrameInstanceUAV)
#define FrameArguments Arguments
#define FrameCounter Counter
#define CullArguments DomainResource(ResourceDomainFrame,FrameCullArguments)
#define CullCounter DomainResource(ResourceDomainFrame,FrameCullCounter)
#define ShadowArguments DomainResource(ResourceDomainFrame,FrameShadowArguments)
#define ShadowCounter DomainResource(ResourceDomainFrame,FrameShadowCounter)

#define Lights DomainResource(ResourceDomainLighting,LightingLights)
#define Clusters DomainResource(ResourceDomainLighting,LightingClusters)
#define ClusterUAV DomainResource(ResourceDomainLighting,LightingClusterUAV)
#define ShadowAtlas DomainResource(ResourceDomainLighting,LightingShadowAtlas)

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

#define HiZMips dimensions.z
#define Width dimensions.x
#define Height dimensions.y
#define OutputWidth outputDimensions.x
#define OutputHeight outputDimensions.y
#define InstanceCount counts.x
#define AdditionalCount counts.y
#define LightCount counts.z
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

struct SkinVertex {uint4 joints;uint4 joints1;float4 weights;float4 weights1;};
struct MorphVertex {float3 position;float3 normal;};
struct Motion {row_major float4x4 transform;row_major float4x4 normalTransform;uint4 binding;uint4 morph;};
struct Bone {row_major float4x4 transform;row_major float4x4 normalTransform;};
float2 MaterialUV(Surface material,uint slot,float2 uv0,float2 uv1){TextureMapping m=material.mapping[slot];float2 uv=(m.rotationSet.z>.5?uv1:uv0)*m.offsetScale.zw;return float2(m.rotationSet.x*uv.x-m.rotationSet.y*uv.y,m.rotationSet.y*uv.x+m.rotationSet.x*uv.y)+m.offsetScale.xy;}
float4 SampleMaterial(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.Sample(sampleState,MaterialUV(material,slot,uv0,uv1));}
float4 SampleMaterialLevel(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.SampleLevel(sampleState,MaterialUV(material,slot,uv0,uv1),0);}
float2 RayOpacity(uint instanceIndex,uint primitive,float2 barycentric){
    StructuredBuffer<GpuInstance> instances=ResourceDescriptorHeap[Instances];StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];GpuInstance instance=instances[instanceIndex];Surface material=materials[instance.material];
    if(material.extra.y==0)return float2(1,0);
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[Ranges];StructuredBuffer<uint> indices=ResourceDescriptorHeap[Indices];StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[Vertices];MeshRange range=ranges[instance.mesh];uint index=range.firstIndex+primitive*3;Vertex a=vertices[indices[index]],b=vertices[indices[index+1]],c=vertices[indices[index+2]];
    float3 weights=float3(1-barycentric.x-barycentric.y,barycentric);float2 uv0=a.uv*weights.x+b.uv*weights.y+c.uv*weights.z,uv1=a.uv1*weights.x+b.uv1*weights.y+c.uv1*weights.z;
    float alpha=material.opacity.x;if(material.albedo!=0xffffffff)alpha*=SampleMaterialLevel(material.albedo,material,0,uv0,uv1).a;
    return float2(material.extra.y==1?(alpha>=material.opacity.y?1:0):saturate(alpha),material.extra.y);
}

#include "optical_common.hlsli"
