#define MaxViews 4
#include "../src/core/GpuInstanceVisual.h"

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
static const uint MeshNativeBounds=7;
static const uint MeshTriangleSolids=14;
static const uint Mg2Routes=15,Mg2Ranges=16,Mg2Meshlets=17,Mg2Vertices=18,
    Mg2Triangles=19,Mg2Indices=20,Mg2Addresses=21,Mg2Remap=22,Mg2Bounds=23,
    Mg2CameraBoxes=24,MeshCameraBoxes=25;
static const uint Mg2MeshBit=0x80000000u;

static const uint FrameInstances=0;

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
static const uint FrameOpaqueOrder=1;
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
static const uint LightingRoomProbes=22,LightingRoomProbesUAV=23;
static const uint LightingSolarVisibility=24,LightingSolarVisibilityUAV=25,LightingOpticalStatsUAV=26;
static const uint LightingReflectionIncidents=27,LightingReflectionIncidentsUAV=28;
static const uint LightingSolarSpatialUAV=29,LightingSolarPrevious=30,LightingSolarReceivers=31;

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
static const uint VisibilityDepthStatsUAV=4;
static const uint VisibilityBuildingBoxes=5;

static const uint RayTLAS=0;
static const uint RayDescriptorsSlot=1;
static const uint RayBlasAddresses=2;
static const uint RayInstanceMapSlot=3;


static const uint ShuttleCrtFlag=OASIS_INSTANCE_COCKPIT_DISPLAY;
static const uint ShuttleSpeedDustFlag=OASIS_INSTANCE_SPEED_DUST;
struct GpuInstance {float4 positionRadius;float4 rotation;float3 scale;uint visualData;uint mesh;uint material;uint id;uint flags;};
struct Vertex {float3 position;float3 normal;float2 uv;float2 uv1;};
struct Meshlet {uint vertexOffset;uint triangleOffset;uint vertexCount;uint triangleCount;};
struct MeshRange {uint firstMeshlet;uint meshletCount;uint firstVertex;uint vertexCount;uint firstIndex;uint indexCount;float4 bounds;};
struct TextureMapping {float4 offsetScale;float4 rotationSet;};
struct Surface {float4 colorRoughness;float4 emissionMetallic;uint albedo;uint normal;uint metalRough;uint emissive;float4 parameters;uint4 extra;float4 opacity;TextureMapping mapping[5];float4 procedural;
    float4 tintGamma;uint roughnessOverride;float roughnessBias;float metallicBias;float overrideUv;
    float4 opticalPolicy;float4 opticalLayers;
};
struct Light {float4 positionRadius;float4 colorPower;float4 directionOuter;float4 parameters;};

bool IsRoomLight(Light light){return ((uint)light.parameters.z&OASIS_LIGHT_BUILDING)!=0;}
static const float RoomLightMargin=.002f;
bool RoomLightReaches(Light light,float3 position){
    const float3 offset=position-light.positionRadius.xyz;
    // Room walls and ceilings sit exactly on these planes. Give rasterized
    // positions a two millimetre margin in OASIS scene units.
    return abs(offset.x)<=light.directionOuter.x+RoomLightMargin&&
        abs(offset.z)<=light.directionOuter.z+RoomLightMargin&&
        offset.y>=-light.directionOuter.y-RoomLightMargin&&offset.y<=light.directionOuter.w+RoomLightMargin;
}

// One diffuse bounce from the floor and walls of the light's room volume.
float3 RoomBounceFallback(Light light,float3 position,float3 normal,float3 albedo,float metallic){
    const float3 delta=position-light.positionRadius.xyz;
    const float3 roomExtent=max(light.directionOuter.xyz,0.0291262136f);
    const float3 local=delta/roomExtent;
    const float transfer=rcp(1.f+dot(local,local));
    const float floorWeight=.5f-.25f*normal.y;
    const float3 roomReflectance=lerp(float3(.54f,.51f,.47f),float3(.34f,.31f,.28f),floorWeight);
    return albedo*(1.f-metallic)*light.colorPower.rgb*light.colorPower.w*
        roomReflectance*(transfer/max(16.9667263f,dot(roomExtent,roomExtent)));
}


struct RoomProbe {
    float4 radianceDistance[6];
    float4 secondMoment[2];
    uint cutVersion;
    uint3 reserved;
};

float3 RoomProbeOrigin(Light light){
    return light.positionRadius.xyz-float3(0.f,.42f*light.directionOuter.y,0.f);
}

float3 RoomProbeAxis(uint lobe){
    const float sign=(lobe&1u)!=0u?-1.f:1.f;
    return lobe<2u?float3(sign,0,0):lobe<4u?float3(0,sign,0):float3(0,0,sign);
}

struct LightBvhNode {
    float3 low;uint lightIndex;
    float3 high;uint reserved;
};
struct Deformation {float4 axisAmplitude;float4 frequencyPhase;};
static const uint SolarPlanetCount=9u;
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
    float4 hud;
    float4 atmosphereSphere;
    float4 atmosphereColor;
    float4 atmosphereLight;
    float4 atmosphereEmission;
    float4 atmosphereParams;
    uint4 cutGeometry;
    uint4 cutRaster; // cacheState, blockTable0, blockTable1, trianglePool
    uint4 membraneField;
    float4 membraneAnimation;
    float4 membraneOptics;
    float4 membraneHandPulses[4];
    float4 membraneStrokeStart[2];
    float4 membraneStrokeEnd[2];
    float4 membranePassagePulses[4];
    uint4 debrisBindings;
    uint4 projectileOverlay;
    float4 nativeSky;
    float4 membraneTint;
    uint4 spherePair;
    uint4 sphereScene;
    float4 solarSun,solarOrientation;
    float4 solarBodyShift[SolarPlanetCount],solarBodyRotation[SolarPlanetCount];
    uint4 solarObjectOffsets[(SolarPlanetCount+4u)/4u];
    Light globalSun;
    float4 reflectionSettings;
    uint4 solarCacheInfo; // compact solar work UAV, SRV, Prism camera category mask, Prism profile
    uint4 reflectionCacheInfo; // global dependency signature (lo,hi), stamp, flags
    row_major float4x4 solarPreviousVP[MaxViews],solarPreviousInverseVP[MaxViews];
    uint4 solarHistoryInfo; // previous width, height, valid, frame stamp
    float4 meshletClipPlanes[MaxViews][6];
    float4 targetOrigin,targetFov[MaxViews];
    float4 forestAtmosphere; // fog, dew, insects, terrain height
    float4 forestFogBounds,forestFogStyle;
    uint4 forestFogBinding;
#ifdef OASIS_DLSS_FRAME
    row_major float4x4 prevDlssVP[2];
    float4 dlssJitter;
    uint4 dlssOutput;
#endif
};
cbuffer Draw:register(b1){uint objectIndex;uint passIndex;uint param0;uint param1;uint param2;};

uint DomainTableDescriptor(uint domain){StructuredBuffer<uint4> directory=ResourceDescriptorHeap[resourceAbi.x];return directory[domain].x;}
uint DomainResource(uint domain,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(DomainTableDescriptor(domain))];return table[slot].x;}


uint GpuDomainResource(uint tableDescriptor,uint slot){StructuredBuffer<uint2> table=ResourceDescriptorHeap[NonUniformResourceIndex(tableDescriptor)];return table[slot].x;}


uint GpuFrameResourceDescriptor(uint descriptorIndex){return descriptorIndex;}

#define WorldInstances DomainResource(ResourceDomainFrame,FrameInstances)
#define PublishedInstances DomainResource(ResourceDomainFrame,FramePublishedInstances)
#define InstanceRanges DomainResource(ResourceDomainFrame,FrameInstanceRanges)
#define Vertices DomainResource(ResourceDomainMeshes,MeshVertices)
#define TriangleSolids DomainResource(ResourceDomainMeshes,MeshTriangleSolids)
#define Meshlets DomainResource(ResourceDomainMeshes,MeshMeshlets)
#define Remap DomainResource(ResourceDomainMeshes,MeshRemap)
#define Triangles DomainResource(ResourceDomainMeshes,MeshTriangles)
#define Ranges DomainResource(ResourceDomainMeshes,MeshRanges)
#define Materials DomainResource(ResourceDomainMeshes,MeshMaterials)
#define Indices DomainResource(ResourceDomainMeshes,MeshIndices)

bool IsForestPage(uint mesh){return (mesh&Mg2MeshBit)!=0;}
uint MeshResource(uint mesh,uint ordinary,uint paged){
    return DomainResource(ResourceDomainMeshes,IsForestPage(mesh)?paged:ordinary);
}
MeshRange LoadMeshRange(uint mesh){
    StructuredBuffer<MeshRange> ranges=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(mesh,MeshRanges,Mg2Ranges))];
    return ranges[mesh&~Mg2MeshBit];
}
// Large meshlet ranges fill Z, leaving Y for instanced opaque draws.
static const uint MeshletDispatchPage=32768u;
uint4 MeshletDispatch(uint ordinal,uint count,uint instances){
    if(count<=65535u)return uint4(ordinal,count,instances,1u);
    return uint4(ordinal,MeshletDispatchPage,instances,
                 (count+MeshletDispatchPage-1u)/MeshletDispatchPage);
}
uint MeshletOrdinal(uint3 group){return group.x+group.z*MeshletDispatchPage;}
uint MeshletVertexIndex(uint mesh,uint offset){
    StructuredBuffer<uint> remap=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(mesh,MeshRemap,Mg2Remap))];
    return remap[offset];
}
uint ForestPageMesh(uint mesh,uint2 identity,bool resident){
    if(IsForestPage(mesh))return mesh;
    const uint descriptor=DomainResource(ResourceDomainMeshes,Mg2Routes);
    if(descriptor==0xffffffffu)return mesh;
    StructuredBuffer<uint4> routes=ResourceDescriptorHeap[descriptor];
    const uint2 header=routes[0].xy;
    const uint taggedMesh=mesh|(resident?Mg2MeshBit:0u);
    uint h=identity.x*0x9e3779b1u^identity.y*0x85ebca6bu^taggedMesh*0xc2b2ae35u;
    h^=h>>16u;h*=0x7feb352du;h^=h>>15u;
    uint index=h&header.x;
    [loop]for(uint probe=0u;probe<header.y;++probe){
        const uint4 route=routes[index+1u];
        if(route.z==0xffffffffu)return mesh;
        if(all(route.xyz==uint3(identity,taggedMesh)))return route.w;
        index=(index+1u)&header.x;
    }
    return mesh;
}
uint RigidPageMesh(uint mesh,uint flags,uint2 identity,bool resident){
    // Raster and DXR resolve the same immutable instance variant.
    return (flags&(2u|16u))!=0u?mesh:ForestPageMesh(mesh,identity,resident);
}
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
uint OpaqueRenderIndex(uint ordinal){
    StructuredBuffer<uint> order=ResourceDescriptorHeap[DomainResource(ResourceDomainFrame,FrameOpaqueOrder)];
    return order[ordinal];
}

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
#define HasGlobalSun (lightBvhLayout.w!=0u)

static const uint ClusterOverflow=0xffffffffu;

#define LightBvhLeafCount lightBvhLayout.x


uint LightBvhAfter(uint node){
    while(node>1u&&(node&1u)!=0u)node>>=1u;
    return node==1u?0u:node+1u;
}
bool LightBvhTouches(LightBvhNode bounds,float3 center,float radius){

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
float4 SolarQuat(float4 a,float4 b){
    return float4(a.w*b.xyz+b.w*a.xyz+cross(a.xyz,b.xyz),
                  a.w*b.w-dot(a.xyz,b.xyz));
}
GpuInstance NativeSolarInstance(GpuInstance instance){
    if(sphereScene.y==0u||(instance.id&0x80000000u)==0u)return instance;
    const uint objectId=instance.id&0x7fffffffu;
    if(objectId>=solarObjectOffsets[SolarPlanetCount/4u][SolarPlanetCount%4u])
        return instance;
    uint owner=0u;
    [unroll]for(uint i=1u;i<SolarPlanetCount;++i)
        owner+=uint(objectId>=solarObjectOffsets[i/4u][i%4u]);
    const float4 q=solarBodyRotation[owner];
    instance.positionRadius.xyz=Rotate(instance.positionRadius.xyz,q)+solarBodyShift[owner].xyz;
    instance.rotation=SolarQuat(q,instance.rotation);
    return instance;
}

float3 Unproject(float2 uv,float depth,uint view){float4 p=mul(float4(uv*float2(2,-2)+float2(-1,1),depth,1),inverseVP[view]);return p.xyz/p.w;}
float DepthAt(float z){return (timing.x*timing.y/z-timing.x)/(timing.y-timing.x);}
float LinearDepth(float d){return timing.x*timing.y/(timing.x+d*(timing.y-timing.x));}
uint ClusterIndex(uint2 pixel,float depth,uint view){uint x=(Width+63)/64,y=(Height+63)/64;uint z=min(23u,(uint)(saturate(log(max(LinearDepth(depth),timing.x)/timing.x)/log(timing.y/timing.x))*24));return (((view*24+z)*y+min(pixel.y/64,y-1))*x+min(pixel.x/64,x-1));}

#define Quality ((uint)timing.w)


struct InstanceSourceRange {uint first;uint count;uint descriptor;uint reserved;};
struct ResidentTileInstance {
    GpuInstance instance;
    float4 sector;
    uint padding0,padding1,enabled,padding2;
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


GpuInstance LoadInstanceStable(uint globalIndex){
    InstanceSourceRange route=ResolveInstanceRoute(globalIndex);
    if(route.descriptor==0xffffffffu){
        GpuInstance inactive=(GpuInstance)0;
        inactive.flags=4u|32u|128u;
        return inactive;
    }
    uint local=globalIndex-route.first;
    if(route.reserved!=1){
        StructuredBuffer<GpuInstance> instances=
            ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
        GpuInstance instance=instances[local];
        instance.mesh=RigidPageMesh(instance.mesh,instance.flags,uint2(globalIndex,instance.id),false);
        return NativeSolarInstance(instance);
    }
    StructuredBuffer<ResidentTileInstance> instances=
        ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
    ResidentTileInstance tile=instances[local];
    GpuInstance instance=tile.instance;
    if(tile.enabled==0)instance.flags|=4u|32u|128u;
    else instance.mesh=RigidPageMesh(instance.mesh,instance.flags,uint2(tile.padding0,tile.padding1),true);
    return instance;
}

GpuInstance LoadInstanceImpl(uint globalIndex,bool selected,uint selectedMesh){
    InstanceSourceRange route=ResolveInstanceRoute(globalIndex);
    if(route.descriptor==0xffffffffu){
        GpuInstance inactive=(GpuInstance)0;
        inactive.flags=4u|32u|128u;
        return inactive;
    }
    uint local=globalIndex-route.first;
    if(route.reserved!=1){
        StructuredBuffer<GpuInstance> instances=
            ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
        GpuInstance instance=instances[local];
        [branch]if(selected)instance.mesh=selectedMesh;
        else instance.mesh=RigidPageMesh(instance.mesh,instance.flags,uint2(globalIndex,instance.id),false);
        return NativeSolarInstance(instance);
    }
    StructuredBuffer<ResidentTileInstance> instances=
        ResourceDescriptorHeap[NonUniformResourceIndex(route.descriptor)];
    ResidentTileInstance tile=instances[local];
    GpuInstance instance=tile.instance;
    // Visibility is determined by the real view frustum and hardware
    // occlusion, never a home-built distance cutoff.
    if(tile.enabled==0){
        instance.flags|=4u|32u|128u;return instance;
    }
    // All surviving instances use their original mesh unless MG2 has
    // published a compatible cluster cut. Never switch to a handmade
    // distance mesh or mask an asset because of an arbitrary radius.
    [branch]if(selected)instance.mesh=selectedMesh;
    else instance.mesh=RigidPageMesh(instance.mesh,instance.flags,uint2(tile.padding0,tile.padding1),true);
    return instance;
}

GpuInstance LoadInstance(uint globalIndex){
    return LoadInstanceImpl(globalIndex,false,0u);
}
// HotDataMain resolved the immutable MG2 selection for this frame already.
// Camera indirect commands must consume that same selected range.
GpuInstance LoadCameraInstance(uint globalIndex){
    StructuredBuffer<uint> selectedMeshes=ResourceDescriptorHeap[HotMeshes];
    return LoadInstanceImpl(globalIndex,true,selectedMeshes[globalIndex]);
}

struct SkinVertex {uint4 joints;uint4 joints1;float4 weights;float4 weights1;};
struct MorphVertex {float3 position;float3 normal;};
struct Motion {row_major float4x4 transform;row_major float4x4 normalTransform;uint4 binding;uint4 morph;};
struct Bone {row_major float4x4 transform;row_major float4x4 normalTransform;};
float3 AuthorColor(Surface material,float3 base){
    base*=material.tintGamma.rgb;
    if(material.tintGamma.w!=1.f)base=pow(max(base,0.f),rcp(material.tintGamma.w));
    return base;
}
float AuthorRoughness(Surface material,float roughness,float2 uv,bool explicitLod,float lod=0.f){
    if(material.roughnessOverride!=0xffffffffu){
        Texture2D<float4> map=ResourceDescriptorHeap[NonUniformResourceIndex(material.roughnessOverride)];
        SamplerState sampling=SamplerDescriptorHeap[0];
        const float2 coord=uv*material.overrideUv;
        roughness=explicitLod?map.SampleLevel(sampling,coord,lod).r:map.Sample(sampling,coord).r;
    }
    if(material.roughnessOverride!=0xffffffffu||material.roughnessBias!=0.f)
        return clamp(roughness+material.roughnessBias,.04f,1.f);
    return roughness;
}
float2 MaterialUV(Surface material,uint slot,float2 uv0,float2 uv1){TextureMapping m=material.mapping[slot];float2 uv=(m.rotationSet.z>.5?uv1:uv0)*m.offsetScale.zw;return float2(m.rotationSet.x*uv.x-m.rotationSet.y*uv.y,m.rotationSet.y*uv.x+m.rotationSet.x*uv.y)+m.offsetScale.xy;}
float4 SampleMaterial(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.Sample(sampleState,MaterialUV(material,slot,uv0,uv1));}
float4 SampleMaterialLevel(uint descriptor,Surface material,uint slot,float2 uv0,float2 uv1){Texture2D<float4> image=ResourceDescriptorHeap[NonUniformResourceIndex(descriptor)];SamplerState sampleState=SamplerDescriptorHeap[NonUniformResourceIndex((uint)material.mapping[slot].rotationSet.w)];return image.SampleLevel(sampleState,MaterialUV(material,slot,uv0,uv1),0);}
// Dedicated cut-source inclusion lanes preserve OMM on the remaining scene.
uint HardwareRayMask(uint logicalMask,bool cutActive){
    return logicalMask|(cutActive?0u:((logicalMask&1u)<<3u)|((logicalMask&4u)<<2u));
}
float2 SurfaceRayOpacity(Surface material,float2 uv0,float2 uv1){
    if(material.extra.y==0u)return float2(1,0);
    float alpha=material.opacity.x;
    if(material.albedo!=0xffffffffu)
        alpha*=SampleMaterialLevel(material.albedo,material,0,uv0,uv1).a;
    return float2(material.extra.y==1u?(alpha>=material.opacity.y?1:0):saturate(alpha),material.extra.y);
}
// Textured candidates only. The solar query can defer its triangle intrinsics
// until after material admission; other ray paths keep their existing entry.
float2 RayTexturedOpacity(uint mesh,Surface material,uint primitive,uint geometry,float2 barycentric){
    StructuredBuffer<uint> indices=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(mesh,MeshIndices,Mg2Indices))];
    StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(mesh,MeshVertices,Mg2Vertices))];
    const MeshRange range=LoadMeshRange(mesh);
    const uint index=range.firstIndex+(primitive+(IsForestPage(mesh)?geometry:0u))*3u;
    Vertex a=vertices[indices[index]],b=vertices[indices[index+1]],c=vertices[indices[index+2]];
    const float3 weights=float3(1-barycentric.x-barycentric.y,barycentric);
    // Opacity samples only the albedo mapping's chosen UV set.
    const bool second=material.mapping[0].rotationSet.z>.5f;
    const float2 uv=(second?a.uv1:a.uv)*weights.x+
        (second?b.uv1:b.uv)*weights.y+(second?c.uv1:c.uv)*weights.z;
    return SurfaceRayOpacity(material,uv,uv);
}
float2 RayOpacity(uint instanceIndex,uint primitive,uint geometry,float2 barycentric){
    instanceIndex=RenderInstanceForRay(instanceIndex);
    StructuredBuffer<Surface> materials=ResourceDescriptorHeap[Materials];GpuInstance instance=LoadInstanceStable(instanceIndex);Surface material=materials[instance.material];
    if(material.extra.y==0)return float2(1,0);
    // Constant-opacity candidates need no triangle fetch, vertex loads or UV
    // interpolation. SurfaceRayOpacity already defines the same exact result.
    if(material.albedo==0xffffffffu)return SurfaceRayOpacity(material,0.f.xx,0.f.xx);
    return RayTexturedOpacity(instance.mesh,material,primitive,geometry,barycentric);
}


uint LoadRayTriangle(uint instanceIndex,uint primitive,uint geometry,out Vertex a,out Vertex b,out Vertex c){
    GpuInstance instance=LoadInstanceStable(RenderInstanceForRay(instanceIndex));
    StructuredBuffer<uint> indices=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(instance.mesh,MeshIndices,Mg2Indices))];
    StructuredBuffer<Vertex> vertices=ResourceDescriptorHeap[
        NonUniformResourceIndex(MeshResource(instance.mesh,MeshVertices,Mg2Vertices))];
    const MeshRange range=LoadMeshRange(instance.mesh);
    const uint first=range.firstIndex+(primitive+(IsForestPage(instance.mesh)?geometry:0u))*3u;
    uint ia=indices[first],ib=indices[first+1],ic=indices[first+2];
    if(instance.flags&2) {
        StructuredBuffer<Vertex> deformed=ResourceDescriptorHeap[DynamicVertices];
        a=deformed[ia];b=deformed[ib];c=deformed[ic];
    } else {
        a=vertices[ia];b=vertices[ib];c=vertices[ic];
    }
    float3 scaleSign=sign(instance.scale.xyz)*max(abs(instance.scale.xyz),.000001);
    a.position=World(a.position,instance);b.position=World(b.position,instance);c.position=World(c.position,instance);
    a.normal=Rotate(a.normal/scaleSign,instance.rotation);
    b.normal=Rotate(b.normal/scaleSign,instance.rotation);
    c.normal=Rotate(c.normal/scaleSign,instance.rotation);
    return instance.material;
}

uint RoomProbeCutVersion(){
    if(resourceAbi.w==0xffffffffu)return 0u;
    StructuredBuffer<uint4> state=ResourceDescriptorHeap[resourceAbi.w];
    return state[0].z;
}

float3 RoomDiffuseBounce(Light light,uint index,float3 position,float3 normal,float3 albedo,float metallic){
    const float3 fallback=RoomBounceFallback(light,position,normal,albedo,metallic);
    if(lightBvhLayout.y==0u||light.parameters.w!=1.f)return fallback;
    StructuredBuffer<RoomProbe> probes=ResourceDescriptorHeap[DomainResource(
        ResourceDomainLighting,LightingRoomProbes)];
    if(probes[index].cutVersion!=RoomProbeCutVersion())return fallback;
    const float3 offset=position-RoomProbeOrigin(light);
    const float probeDistance=length(offset);
    const float distance=probeDistance/max(light.positionRadius.w,0.0291262136f);
    const float3 towardSurface=offset/max(probeDistance,1.e-5f);
    float3 irradiance=0.f;
    float sum=0.f;
    [unroll]for(uint component=0;component<3u;++component){
        const uint lobe=2u*component+(normal[component]<0.f?1u:0u);
        const float3 axis=RoomProbeAxis(lobe);
        const float4 sample=probes[index].radianceDistance[lobe];
        const float moment=probes[index].secondMoment[lobe/4u][lobe%4u];
        const float excess=max(0.f,distance*max(0.f,dot(towardSurface,axis))-sample.w-.025f);
        const float variance=max(.002f,moment-sample.w*sample.w);
        const float visibility=variance/(variance+excess*excess);
        const float normalWeight=saturate(dot(normal,axis));
        irradiance+=sample.rgb*(normalWeight*visibility);
        sum+=normalWeight;
    }
    return albedo*(1.f-metallic)*irradiance/max(sum,1.e-4f);
}
