#pragma once
#include "Graphics.h"
#include "Assets.h"
#include "Mg2Pages.h"
#include "TerritorialPatrimony.h"
#include "ArchitecturePatrimony.h"
#include "SolarSystem.h"
#include "Animation.h"
#include "GpuMemory.h"
#include "GpuDescriptorAllocator.h"
#include "GpuMeshTypes.h"
#include "MeshSolidPublication.h"
#include "GpuFrameResources.h"
#include "GpuSceneCache.h"
#include "GpuRayScene.h"
#include "FrameGraphDependencies.h"
#include "BuildConfig.h"
#include "RenderWorkSelection.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>
#include <bit>
#include <type_traits>
#include <limits>
#include <shlobj.h>
#include <functional>
#include <string>
#include <map>
#include <future>
#include <atomic>
#include <chrono>

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace OASIS {
using Microsoft::WRL::ComPtr;
// Fail() exits the process. Collect the removed-device report BEFORE it.
void GraphicsCheckHr(ID3D12Device*,HRESULT,const char*);
namespace graphics_internal {
constexpr UINT64 MiB=1024ull*1024,UploadChunk=8*MiB;

static_assert(MaxViews<=D3D12_MAX_VIEW_INSTANCE_COUNT);
constexpr uint32_t ShadowSlots=4,ShadowFace=512;
constexpr uint32_t DirectLightLimit=4,SmallSceneOcclusionThreshold=256;
enum GpuCacheKind:uint32_t {
    CacheStaticInstances=1,CacheVertices,CacheIndices,CacheMeshlets,CacheRemap,CacheTriangles,CacheRanges,
    CacheMaterials,CacheDeformations,CacheSkin,CacheMorphs,CacheTexture,CacheStaticBlas,
    CacheWritableVertices,CacheWritableIndices,CacheWritableMeshlets,CacheWritableRemap,CacheWritableTriangles,CacheWritableRanges,CacheTriangleSolids,CacheWritableTriangleSolids
};
static_assert(NoRayTracing==GpuRayNoTracingFlag);
constexpr uint32_t DescriptorCapacity=8192,ResourceDomainWords=32,ResourceAbiVersion=OASIS_RESOURCE_ABI_VERSION;

enum class ResourceDomain:uint32_t {Meshes,Frame,Lighting,History,Animation,Visibility,RayTracing,Count};
constexpr uint32_t ResourceDomainCount=uint32_t(ResourceDomain::Count);
enum MeshBinding:uint32_t {MeshVertices,MeshMeshlets,MeshRemap,MeshTriangles,MeshRanges,MeshMaterials,MeshIndices,MeshNativeBounds,MeshVerticesWrite,MeshIndicesWrite,MeshMeshletsWrite,MeshRemapWrite,MeshTrianglesWrite,MeshRangesWrite,MeshTriangleSolids,MeshBindingCount};
enum Mg2Binding:uint32_t {Mg2Routes=MeshBindingCount,Mg2Ranges,Mg2Meshlets,Mg2Vertices,Mg2Triangles,Mg2Indices,Mg2Addresses,Mg2Remap,Mg2Bounds,Mg2CameraBoxes,MeshCameraBoxes,MeshTerritorialSuppression,MeshCameraDispatchRanges,MeshTerritorialRaySuppression,MeshShadowDispatchRanges};
static_assert(MeshShadowDispatchRanges<ResourceDomainWords);
enum FrameBinding:uint32_t {
    FrameInstances=0,FrameOpaqueOrder=1,FrameReservedCounter=2,FrameInstanceUAV=3,
    FrameReservedCullArguments=4,FrameReservedCullCounter=5,FrameReservedShadowArguments=6,FrameReservedShadowCounter=7,
    FrameCpuDepthArguments=8,FrameCpuDepthCounter=9,FramePublishedDepthArguments=10,FramePublishedDepthCounter=11,
    FrameCpuCullArguments=12,FrameCpuCullCounter=13,FramePublishedCullArguments=14,FramePublishedCullCounter=15,
    FrameCpuShadowArguments=16,FrameCpuShadowCounter=17,
    FramePublishedInstances=18,FrameInstanceRanges=19,
    FrameCpuBatchVisibilityUAV=20,FrameCpuBatchVisibilitySRV=21,
    FrameCpuBatchArgumentsUAV=22,FrameCpuBatchCountUAV=23,
    FrameTileMarksUAV=24,FrameTileMarksSRV=25,
    FrameTilePrefixesUAV=26,FrameTilePrefixesSRV=27,
    FrameTileTotalsUAV=28,FrameTileTotalsSRV=29,
    FrameTileOffsetsUAV=30,FrameTileOffsetsSRV=31,FrameBindingCount=32
};
enum LightingBinding:uint32_t {
    LightingLights,LightingClusters,LightingClusterUAV,LightingShadowAtlas,
    LightingClusterIndices,LightingClusterIndicesUAV,
    LightingClusterGroupCounts,LightingClusterGroupCountsUAV,
    LightingClusterGroupOffsets,LightingClusterGroupOffsetsUAV,
    LightingDirectionalIndices,LightingDirectionalIndicesUAV,
    LightingDirectionalCount,LightingDirectionalCountUAV,
    LightingClusterBounds,LightingClusterBoundsUAV,
    LightingBvhNodes,LightingBvhNodesUAV,
    LightingBvhPairs,LightingBvhPairsUAV,
    LightingBvhBounds,LightingBvhBoundsUAV,
    LightingRoomProbes,LightingRoomProbesUAV,
    LightingSolarVisibility,LightingSolarVisibilityUAV,LightingOpticalStatsUAV,
    LightingReflectionIncidents,LightingReflectionIncidentsUAV,LightingSolarSpatialUAV,LightingSolarPrevious,LightingSolarReceivers,
    LightingBindingCount
};
enum HistoryBinding:uint32_t {HistoryHierarchy,HistoryDepthMS,HistoryColorResolved,HistoryReflectionResolved,HistoryPrevious,HistoryDepthPrevious,HistoryOutput,HistoryDepthOutput,HistoryCurrent,HistoryDepthResolved,HistoryHiZMipBase};
enum AnimationBinding:uint32_t {AnimationDynamicVertices,AnimationDynamicVerticesUAV,AnimationDeformations,AnimationMotions,AnimationBones,AnimationWeights,AnimationSkin,AnimationMorphs,AnimationDeformedBounds,AnimationDeformedBoundsUAV,AnimationBindingCount};
enum VisibilityBinding:uint32_t {VisibilityHotBounds,VisibilityHotBoundsUAV,VisibilityHotMeshes,VisibilityHotMeshesUAV,VisibilityDepthStatsUAV,VisibilityBuildingBoxes,VisibilityBindingCount};
enum RayBinding:uint32_t {RayTLAS,RayDescriptors,RayBlasAddresses,RayInstanceMap,RayBindingCount};
using DescriptorHandle=GpuResourceHandle;
struct DomainDirectoryEntry {uint32_t descriptor{Invalid},generation{},count{},reserved{};};
struct DomainBindingEntry {uint32_t descriptor{Invalid},generation{};};

struct InstanceSourceRange {uint32_t first{},count{},descriptor{},reserved{};};
static_assert(sizeof(DomainDirectoryEntry)==16&&sizeof(DomainBindingEntry)==8&&sizeof(InstanceSourceRange)==16);
using DescriptorAllocator=GpuDescriptorAllocator<ComPtr<ID3D12Resource>>;
inline UINT64 Align(UINT64 v,UINT64 a){return (v+a-1)&~(a-1);}
inline D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES p{};p.Type=type;p.CreationNodeMask=p.VisibleNodeMask=1;return p;}
inline D3D12_RESOURCE_DESC Buffer(UINT64 size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=Align(size,4);d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
inline D3D12_RESOURCE_DESC Texture(UINT w,UINT h,DXGI_FORMAT format,UINT samples=1,UINT layers=1,UINT mips=1,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=UINT16(layers);d.MipLevels=UINT16(mips);d.Format=format;d.SampleDesc.Count=samples;d.Flags=flags;return d;}
inline bool SameResourceDesc(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b){return a.Dimension==b.Dimension&&a.Alignment==b.Alignment&&a.Width==b.Width&&a.Height==b.Height&&a.DepthOrArraySize==b.DepthOrArraySize&&a.MipLevels==b.MipLevels&&a.Format==b.Format&&a.SampleDesc.Count==b.SampleDesc.Count&&a.SampleDesc.Quality==b.SampleDesc.Quality&&a.Layout==b.Layout&&a.Flags==b.Flags;}

struct TextureResource {ComPtr<ID3D12Resource> resource;D3D12_BARRIER_LAYOUT layout{D3D12_BARRIER_LAYOUT_COMMON};};
struct TextureDependency {ID3D12Resource* resource{};D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};};
struct TextureDependencies {
    ID3D12GraphicsCommandList7* list{};std::vector<TextureDependency> entries;
    void Begin(ID3D12GraphicsCommandList7* l){list=l;entries.clear();}
    TextureDependency& Get(ID3D12GraphicsCommandList7* l,ID3D12Resource* r){
        if(list!=l)Fail("Texture dependency outside recording scope");
        for(auto& entry:entries)if(entry.resource==r)return entry;
        entries.push_back({});auto& entry=entries.back();entry.resource=r;return entry;
    }
};
extern thread_local TextureDependencies textureDependencies;
inline void StartupTransition(
    ID3D12GraphicsCommandList7* list,
    TextureResource& texture,
    D3D12_BARRIER_LAYOUT layout,
    D3D12_BARRIER_ACCESS access,
    D3D12_BARRIER_SYNC sync)
{
    auto& prior=textureDependencies.Get(list,texture.resource.Get());

    D3D12_TEXTURE_BARRIER b{};
    b.SyncBefore=prior.sync;
    b.SyncAfter=sync;
    b.AccessBefore=prior.access;
    b.AccessAfter=access;
    b.LayoutBefore=texture.layout;
    b.LayoutAfter=layout;
    b.pResource=texture.resource.Get();

    b.Subresources.IndexOrFirstMipLevel=0xffffffffu;
    b.Subresources.NumMipLevels=0;

    D3D12_BARRIER_GROUP g{};
    g.Type=D3D12_BARRIER_TYPE_TEXTURE;
    g.NumBarriers=1;
    g.pTextureBarriers=&b;

    list->Barrier(1,&g);


    texture.layout=layout;
    prior.access=access;
    prior.sync=sync;
}
inline void BBarrier(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource,D3D12_BARRIER_SYNC before,D3D12_BARRIER_SYNC after,D3D12_BARRIER_ACCESS a,D3D12_BARRIER_ACCESS b){D3D12_BUFFER_BARRIER bar{};bar.SyncBefore=before;bar.SyncAfter=after;bar.AccessBefore=a;bar.AccessAfter=b;bar.pResource=resource;bar.Offset=0;bar.Size=UINT64_MAX;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_BUFFER;g.NumBarriers=1;g.pBufferBarriers=&bar;list->Barrier(1,&g);}
inline bool StaticUploadFormat(DXGI_FORMAT f){switch(f){
case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SINT:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SINT:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_UINT:case DXGI_FORMAT_R8G8B8A8_SINT:
case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:
case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:
case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:case DXGI_FORMAT_BC4_SNORM:
case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC5_SNORM:case DXGI_FORMAT_BC6H_UF16:case DXGI_FORMAT_BC6H_SF16:
case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:return true;default:return false;}}
inline void Uav(ID3D12GraphicsCommandList7* l){D3D12_GLOBAL_BARRIER b{};b.SyncBefore=D3D12_BARRIER_SYNC_ALL_SHADING;b.SyncAfter=D3D12_BARRIER_SYNC_ALL_SHADING;b.AccessBefore=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;b.AccessAfter=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_GLOBAL;g.NumBarriers=1;g.pGlobalBarriers=&b;l->Barrier(1,&g);}
struct Surface {XMFLOAT4 colorRoughness,emissionMetallic;uint32_t albedo{Invalid},normal{Invalid},metalRough{Invalid},emissive{Invalid};XMFLOAT4 parameters{1,0,0,0};XMUINT4 extra{Invalid,0,0,0};XMFLOAT4 opacity{1,.5f,1,1};std::array<TextureMapping,5> mapping{};XMFLOAT4 procedural{};
    XMFLOAT4 tintGamma{1,1,1,1};
    uint32_t roughnessOverride{Invalid};
    float roughnessBias{},metallicBias{},overrideUv{1.f};
    XMFLOAT4 opticalPolicy{0.f,.04f,.04f,1.f};
    XMFLOAT4 opticalLayers{0.f,1.f,0.f,0.f};
};
static_assert(sizeof(Surface)==336&&offsetof(Surface,tintGamma)==272&&offsetof(Surface,roughnessOverride)==288&&offsetof(Surface,opticalPolicy)==304&&offsetof(Surface,opticalLayers)==320);


inline uint32_t MaterialTextureMask(const Surface& s){
    return (s.albedo!=Invalid?1u:0u)|
           (s.normal!=Invalid?2u:0u)|
           (s.metalRough!=Invalid?4u:0u)|
           (s.emissive!=Invalid?8u:0u)|
           (s.extra.x!=Invalid?16u:0u);
}
constexpr uint32_t ShuttleCrtVariant=32u; // Follows the 32 texture-mask variants in scene.hlsl.
// Coverage is evaluated at 1x1 in the camera depth pass. Keep masked color
// at the same rate; opaque surfaces still use peripheral VRS. This is a run
// key bit, not an extra shader variant or material descriptor.
constexpr uint32_t FullRateCoverageRun=64u;
inline uint32_t MaterialRunKey(const Surface& s){
    return MaterialTextureMask(s)|(s.extra.y==1u?FullRateCoverageRun:0u);
}
struct Light {XMFLOAT4 positionRadius,colorPower,directionOuter,parameters;};
struct LightBvhNode {
    XMFLOAT3 low{};uint32_t lightIndex{UINT32_MAX};
    XMFLOAT3 high{};uint32_t reserved{};
};
static_assert(sizeof(LightBvhNode)==32);
struct RenderPassPlan {
    bool occlusion{},depthHierarchy{},clusteredLights{},reflections{},shadowAtlas{},rayQueries{},rayWork{};
};
inline bool SurfaceMayReflect(const Surface& s,uint32_t quality){
    if(s.opticalPolicy.x<.5f||s.extra.w==1u)return false;
    const float threshold=quality<=1u?.18f:.38f;
    const float minimum=(s.metalRough==Invalid&&s.roughnessOverride==Invalid&&s.parameters.w<=0.f&&s.extra.w!=2u)
        ?std::max(s.opticalPolicy.z,s.colorRoughness.w+s.roughnessBias):s.opticalPolicy.z;
    return minimum<threshold;
}
struct alignas(256) Constants {XMFLOAT4X4 vp[MaxViews],inverseVP[MaxViews],previousVP[MaxViews],shadowVP[ShadowSlots*6];XMFLOAT4 eye[MaxViews];XMUINT4 resourceAbi,dimensions,outputDimensions,counts,topology;XMFLOAT4 timing,tileViewer;XMUINT4 lightBvhLayout;XMFLOAT4 hud,atmosphereSphere,atmosphereColor,atmosphereLight,atmosphereEmission,atmosphereParams;XMUINT4 cutGeometry,cutRaster,membraneField;XMFLOAT4 membraneAnimation,membraneOptics,membraneHandPulses[HandCount*2u],membraneStrokeStart[HandCount],membraneStrokeEnd[HandCount],membranePassagePulses[MembranePassagePulseCount];XMUINT4 debrisBindings;
    XMUINT4 projectileOverlay;XMFLOAT4 nativeSky;
    XMFLOAT4 membraneTint;XMUINT4 spherePair;
    XMUINT4 sphereScene; // Native static geometry and solar transformations.
    XMFLOAT4 solarSun,solarOrientation;
    std::array<XMFLOAT4,SolarPlanetCount> solarBodyShift{},solarBodyRotation{};
    std::array<XMUINT4,(SolarPlanetCount+4u)/4u> solarObjectOffsets{};
    Light globalSun;
    XMFLOAT4 reflectionSettings; // General roughness threshold and DXR range.
    XMUINT4 solarCacheInfo; // compact solar work UAV, SRV, reserved, Prism profile
    XMUINT4 reflectionCacheInfo; // global dependency signature (lo,hi), stamp, flags
    XMFLOAT4X4 solarPreviousVP[MaxViews],solarPreviousInverseVP[MaxViews];
    XMUINT4 solarHistoryInfo;
    XMFLOAT4 meshletClipPlanes[MaxViews][6];
    XMFLOAT4 targetOrigin,targetFov[MaxViews];
    XMFLOAT4 forestAtmosphere; // fog, dew, insects, terrain height
    XMFLOAT4 forestFogBounds,forestFogStyle; // world XZ mapping; tone, contrast, min Y, Y span
    XMUINT4 forestFogBinding; // immutable atlas descriptor, bark specular percent, reserved
    XMFLOAT4 forestFogExtension; // published center XZ, ground Y, radius
    XMFLOAT4X4 prevDlssVP[2];XMFLOAT4 dlssJitter;XMUINT4 dlssOutput;};
static_assert(sizeof(Constants)%256==0);
static_assert(sizeof(Constants)<=16u*D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class T>struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=Type;T data{};};
struct MeshPipeline {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS,D3D12_PIPELINE_STATE_FLAGS> flags;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS,D3D12_SHADER_BYTECODE> ms;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> formats;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depthFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
};
struct PresentPipeline {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> formats;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depthFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
};
struct ComputePipeline {Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,D3D12_SHADER_BYTECODE> cs;Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS,D3D12_PIPELINE_STATE_FLAGS> flags;};
struct QueueState {ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;UINT64 serial{};UINT64 Signal(){auto v=++serial;Hr(queue->Signal(fence.Get(),v),"queue signal");return v;}void Wait(const QueueState& q,UINT64 value){Hr(queue->Wait(q.fence.Get(),value),"queue dependency");}};
struct Commands {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList7> list;void Reset(){Hr(allocator->Reset(),"command allocator");Hr(list->Reset(allocator.Get(),nullptr),"command list");textureDependencies.Begin(list.Get());}void Close(){Hr(list->Close(),"command close");}void Execute(QueueState& q){ID3D12CommandList* lists[]={list.Get()};q.queue->ExecuteCommandLists(1,lists);}void Submit(QueueState& q){Close();Execute(q);}};

}
}
#include "GraphicsFrameGraph.h"
namespace OASIS {
namespace graphics_internal {

}
using namespace graphics_internal;
struct Graphics::Impl : GpuDomainServices {
    std::vector<ComPtr<ID3D12Resource>> xrColorTargets;
    OutputSurface outputSurface;
    bool outputConfigured{};
    bool developerMode{true};
    AtmosphereSettings atmosphere{},companion{};
    uint32_t renderSphere{},sphereCount{1u};
    std::vector<uint32_t> sphereObjectOffsets;
    bool solarSystem{};
    std::optional<std::array<Pose,SolarBodyCount>> solarBodies;
    bool DenseSphereField()const{return sphereCount>SpectaclePairCount;}
    XMFLOAT3 SphereWorldCenter(uint32_t owner)const{
        return solarBodies?(*solarBodies)[owner].p:SphereAtmosphere(owner).center;
    }
    AtmosphereSettings SphereAtmosphere(uint32_t owner)const{
        auto profile=owner==0u?atmosphere:companion;
        if(owner>=SpectaclePairCount)profile.center=SpectacleCenters[owner];
        if(solarSystem&&owner!=0u){
            profile.radius=SolarBodyRadius(owner);
            profile.color=SolarTerrainColor(owner);
            profile.film.tint=Add({.47f,.47f,.47f},Scale(profile.color,1.65f));
        }
        return profile;
    }
    static constexpr uint32_t MembraneEventCapacity=2048;
    static constexpr uint32_t MembraneFieldSize=128+2;
    static_assert((MembraneEventCapacity&(MembraneEventCapacity-1u))==0);
    struct MembraneResources {
        ComPtr<ID3D12Resource> events;
        TextureResource field;
        DescriptorHandle eventSRV,eventUAV,fieldSRV,fieldUAV;
        bool resetHistory{true};
        double lastTime{},patternTime{},waveTime{};
    };
    std::array<MembraneResources,SpectacleSphereCount> sphereMembranes;
    MembraneResources& RenderMembrane(){return sphereMembranes[renderSphere];}
    ComPtr<ID3D12PipelineState> membranePSO;
    void EnsureMembraneResources();
    struct DebrisResources {
        ComPtr<ID3D12Resource> events,floor,drawArgs;
        DescriptorHandle eventSRV,eventUAV,floorSRV,floorUAV,argsUAV;
        bool needsReset{true};
    };
    std::array<DebrisResources,SpectacleSphereCount> sphereDebris;
    DebrisResources& RenderDebris(){return sphereDebris[renderSphere];}
    ComPtr<ID3D12PipelineState> debrisResetPSO,debrisBuildPSO,debrisDrawPSO;
    ComPtr<ID3D12CommandSignature> debrisDrawSignature;
    void EnsureDebrisResources();
    struct Capture {
        std::wstring path;
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes{};
        UINT width{},height{};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
        bool requested{},recorded{},failed{};
    } capture;
    int64_t hudStart{};
    uint64_t hudFrames{};
    float hudFps{-1};
    float cpuPrepareMs{};
    enum class TimingPath { Sky, Relief, Local, Native, Count };
    struct FrameCost { uint64_t frame{}; float ms{-1.f}; };
    std::array<FrameCost,size_t(TimingPath::Count)> frameCosts{};
    bool dlssDiagnosticEnabled{};
    DlssDiagnosticState dlssDiagnostic;
    bool sphereBaselineTiming{};
    bool scenePassTiming{},depthDiagnosticEnabled{};
    bool depthBaselineRequested{},depthHybridRequested{},depthSplitActive{},depthHybridActive{};
    uint32_t visibilityDiagnosticMask{}; // H0: zero in normal play, bit 32 freezes MG2.
    std::array<SphereGpuTimings,12> sphereGpuTimings{};
    size_t sphereGpuTimingCount{};
    void CollectGpuTimings();
    struct TimerQueue {
        ComPtr<ID3D12QueryHeap> queries;
        ComPtr<ID3D12Resource> readback;
        UINT64 frequency{},calibrationGpu{},calibrationQpc{};
        bool calibrationValid{};
    };
    struct TimerSample {
        bool pending{};
        uint32_t slot{};
        std::array<uint32_t,GraphQueueCount> counts{};
        std::array<UINT64,GraphQueueCount> fences{};
        float cpuMs{};
        uint64_t renderFrame{};
        TimingPath path{};
        uint64_t diagnosticFrame{};
        uint32_t dlssBatch{Invalid};
        uint32_t directPassMask{},computePassMask{};
        float dlssScale{},dlssCpuMs{-1.f};
        std::array<std::array<std::array<char,64>,GpuBatchTimer::MaxBatches>,GraphQueueCount> batchNames;
    };
    std::array<TimerQueue,GraphQueueCount> timerQueues{};
    std::array<TimerSample,GpuBatchTimer::Slots> timerSamples{};
    bool timerInitialized{};


std::vector<GpuStream*> streams;RenderScene visualFrame{};RenderContributions contributions;GpuSceneCache sceneCache;GpuRayScene rayScene;ComPtr<ID3D12Resource> residentStaticInstances;
    struct ResidentMaterialRun {
        uint32_t first{},count{},mask{},reserved{},category{};
        bool operator==(const ResidentMaterialRun&)const=default;
    };
    struct ResidentTileGpu {
        uint32_t page{Invalid},firstElement{},objectCapacity{},objectCount{};
        uint64_t topologyGeneration{};
        std::vector<uint32_t> opaqueOrder;
        uint64_t opaqueOrderRevision{};
        std::vector<ResidentMaterialRun> runs;
        std::vector<uint32_t> coveredLocals,filteredOrder;
        std::vector<GpuSceneDirtyRange> uncoveredSpans;
        std::vector<ResidentMaterialRun> filteredRuns;
        XMFLOAT3 cameraBoundsMin{},cameraBoundsMax{};
        bool cameraBoundsValid{},cameraBoundsUnbounded{};
    };
    struct TileBatchDispatch {
        GpuTileKey key{};
        uint32_t first{},count{},runCount{};
    };
    struct OpaqueMaterialRun {uint32_t first{},count{},mask{};};
    struct ResidentOpaqueBucket {
        uint32_t first{},count{},mask{},ordered{};
        uint32_t blockFirst{},blockCount{},superFirst{},superCount{};
    };
    struct ResidentBatchData {
        std::vector<TileBatchDispatch> tileBatchDispatch;
        std::vector<OpaqueMaterialRun> opaqueRuns;
        std::vector<ResidentOpaqueBucket> residentBuckets;
        std::vector<GpuSceneDirtyRange> cameraRanges,shadowRanges;
        uint32_t tileBatchBlocks{},forestCounter{};
        uint32_t residentCpuRejected{},residentOrderBoundaries{};
    };
    struct RayTileAck {
        GpuTileKey key{};
        uint64_t topologyGeneration{};
        std::vector<std::pair<uint32_t,uint64_t>> updates;
    };
    struct RayFrameData {
        std::vector<RayTileAck> acks;
        std::vector<GpuRayDirtyRange> cpuDirty,blasDirty;
        uint32_t forestProofSlot{Invalid};
        ID3D12Resource* forestProofAddressBuffer{};
        UINT64 forestProofAddressOffset{};
        bool dirty{},forestAddressesChanged{};
    };
    // Suballocated resident instance storage. A page owns one descriptor;
    // retired spans are reusable only after all queues have completed.
    struct ResidentInstancePage {
        ComPtr<ID3D12Resource> buffer;
        DescriptorHandle srv;
        uint32_t capacity{};
        int32_t layer{};
        std::map<uint32_t,uint32_t> free;
    };
    struct RetiredResidentSpan {
        uint32_t page{},first{},count{};
        GpuFenceValues fences{};
    };
    std::vector<ResidentInstancePage> residentInstancePages;
    std::vector<RetiredResidentSpan> retiredResidentSpans;
    void CollectResidentSpans();
    void RetireResidentSpan(ResidentTileGpu&);
    void AllocateResidentSpan(ResidentTileGpu&,uint32_t,int32_t);
    std::map<GpuTileKey,ResidentTileGpu> residentTileGpu;
    std::vector<uint64_t> staticMaterialBits;std::vector<uint32_t> staticTransparent,staticDeformedMeshes,staticDeformedRtMeshes;std::array<bool,4> staticReflectionCandidates{};std::vector<SceneLight> baseLights;uint32_t baseLightCount{};
    ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter3> adapter;ComPtr<ID3D12Device10> device;QueueState direct,compute,copy;PassGraph passGraph;FrameGraphPlan frameGraph;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineLibrary1> library;ComPtr<ID3D12CommandSignature> indirect,dispatchIndirect,cutBlockIndirect;
    ComPtr<ID3D12DescriptorHeap> descriptors,samplers,rtvs,dsvs;UINT descriptorStride{},rtvStride{},dsvStride{},rtvCount{},dsvCount{};DescriptorAllocator descriptorAllocator{DescriptorCapacity,&Fail};
    struct Page {ComPtr<ID3D12Heap> heap;UINT64 size{},used{};uint32_t residency{};bool dedicated{};};std::vector<Page> pages;Residency residency;GpuVramCache vramCache;GpuManagedMemory managedMemory;uint32_t pageCount{};UINT64 committed{};
    struct UploadGeneration {
        ComPtr<ID3D12Resource> resource;std::byte* mapped{};UINT64 chunk{};uint64_t generation{};
        ~UploadGeneration(){if(resource&&mapped)resource->Unmap(0,nullptr);}
    };
    std::shared_ptr<UploadGeneration> uploadGeneration;std::atomic<uint64_t> uploadGenerationSerial{};Commands startup;std::vector<ComPtr<ID3D12Resource>> uploads;uint64_t arb0UploadBytes{},arb0ReuseBytes{};bool arb0Measuring{};char arb0Report[384]{};std::wstring cachePath;std::vector<std::byte> cacheBlob;
MeshData meshes;SceneAnimation animation;JobSystem* jobs{};bool sceneLoaded{},writableSceneMeshes{};uint64_t geometryGenerationSerial{};ComPtr<ID3D12Resource> vertices,indices,meshlets,remap,triangles,ranges,materials,deformations,skinVertices,morphVertices,triangleSolids;DescriptorHandle skinSRV,morphSRV,indicesSRV,verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,deformationsSRV,triangleSolidsSRV,triangleSolidsUAV;std::array<DescriptorHandle,6> geometryUAVs{};
    ComPtr<ID3D12Resource> nativeMeshletBounds;
    DescriptorHandle nativeMeshletBoundsSRV;
    ComPtr<ID3D12Resource> nativeCameraBoxes;
    DescriptorHandle nativeCameraBoxesSRV;
    ComPtr<ID3D12Resource> buildingCameraBoxes;
    DescriptorHandle buildingCameraBoxesSRV;
    std::vector<ComPtr<ID3D12Resource>> blas;std::vector<uint32_t> dynamicMeshes,meshDynamicIndex;
    struct ResidentTexture {ComPtr<ID3D12Resource> resource;DescriptorHandle descriptor;uint32_t residency{};};std::vector<ResidentTexture> textures;std::vector<Surface> surfaceData,baseSurfaceData;
    uint32_t forestFogTexture{Invalid};
    XMFLOAT4 forestFogBounds{};XMFLOAT2 forestFogHeight{};
    std::vector<SurfaceSettings> surfaceEdits;
    std::vector<uint32_t> materialSlots;
    std::vector<MaterialTuning> materialTuning;
    std::vector<GpuSceneDirtyRange> dirtySurfaces;
    std::vector<uint64_t> shadowSurfaceRevision;
    uint64_t roomProbeMaterialRevision{};
    std::map<uint32_t,std::vector<uint32_t>> materialSurfacesBySlot;

    std::vector<uint32_t> staticOpaqueOrder;
    uint32_t MsaaSamples{OASIS_MSAA_SAMPLES},requestedSamples{OASIS_MSAA_SAMPLES};
    uint32_t aaMode{};
    SceneReflections reflectionMode{SceneReflections::Full};
    SceneSolarVisibility solarVisibilityMode{SceneSolarVisibility::Adaptive};
    uint32_t prismProfile{},prismSceneMask{}; // zero = normal rendering; tree/logo/land bits
    bool diagnosticDxrEnabled{true},diagnosticFreezeMg2{};
    PeripheralShading peripheralShading{PeripheralShading::Auto};
    bool msaa4Supported{},msaaSupportKnown{},msaa4AllocationFailed{},dlssEvaluationFailed{};
    RenderingState renderingState{};
    bool SupportsMsaa4();
    TextureResource aaSource;GpuResourceLease aaSourceLease;DescriptorHandle aaSourceArraySrv;ComPtr<ID3D12PipelineState> fxaaPSO;
    TextureResource vrsImage;
    DescriptorHandle vrsImageUAV;
    ComPtr<ID3D12PipelineState> vrsPSO;
    std::array<XrFovf,MaxViews> vrsFov{};
    uint32_t vrsTileSize{},vrsWidth{},vrsHeight{};
    bool vrsImageReady{};
    void ApplySurfaceEdits(std::span<const uint32_t> indices={});
    std::vector<uint64_t> visibleMaterialBits;std::vector<uint32_t> materialResidencyOffsets,materialResidencyPages,residencyPageSeen,requiredResidencyPages;uint32_t residencyPageGeneration{1};uint64_t residencyDiagnosticFence{};
    std::array<ComPtr<ID3D12PipelineState>,ShuttleCrtVariant+1u> sceneVariantPSO,sceneDirectVariantPSO,sceneHybridVariantPSO,sceneDirectHybridVariantPSO;
    // Sphere relief caches and distance-dependent representations removed.
    // All live geometry is rendered through the native MG2/mesh pipeline.
    uint32_t nativeSphere{0u};
    ComPtr<ID3D12PipelineState> scenePSO,sceneDirectPSO,cutScenePSO,cutSceneDirectPSO,cutDepthPSO,cutShadowPSO,transparentPSO,transparentDirectPSO,depthPSO,depthSeedPSO,rasterOcclusionPSO,shadowPSO,presentPSO,dlssPresentPSO,cullPSO,depthResolvePSO,hizPSO,clusterCountPSO,clusterOffsetsPSO,clusterFillPSO,
        lightBvhPartialPSO,lightBvhBoundsPSO,lightBvhMortonPSO,
        lightBvhSortPSO,lightBvhLeavesPSO,lightBvhParentsPSO,roomProbesPSO,
        denoisePSO,mergeReflectionPSO,deformPSO,deformBoundsPSO,hotDataPSO,rayBuildPSO,batchOpaquePSO,rayMapFillPSO,residentBatchPSO,tileBatchMarkPSO,tileBatchScanPSO,tileBatchOffsetScanPSO,tileBatchOffsetFinalizePSO,tileBatchEmitPSO;
    struct Frame;
    // CLAS storage is independent of raster generations. A BLAS retains
    // every immutable CLAS allocation that it references, even after eviction.
    // The address is a GPU-produced uint64 in immutable COPY_SOURCE storage.
    // Keep its buffer alive alongside the AS; no CPU address mirror is needed.
    struct ForestAddressSlot {
        ComPtr<ID3D12Resource> buffer;
        UINT64 offset{};
        explicit operator bool()const{return bool(buffer);}
    };
    struct ForestClasEntry {
        ForestAddressSlot slot;
        ComPtr<ID3D12Resource> storage;
    };
    struct ForestBlasEntry {
        ForestAddressSlot slot;
        ComPtr<ID3D12Resource> storage;
        std::vector<ComPtr<ID3D12Resource>> dependencies;
        std::vector<std::pair<std::pair<uint32_t,uint32_t>,ForestClasEntry>> clusterAddresses;
        ForestClasEntry FindCluster(const std::pair<uint32_t,uint32_t>& key)const{
            for(const auto& [identity,entry]:clusterAddresses)
                if(identity==key&&entry.slot&&entry.storage)return entry;
            return {};
        }
    };
    struct ForestAccelerationCache {
        Mg2RayBackend backend{};
        std::map<std::pair<uint32_t,uint32_t>,ForestClasEntry> clas;
        std::map<std::vector<uint32_t>,std::shared_ptr<ForestBlasEntry>> blas;
        std::map<ID3D12Resource*,UINT64> allocations;
        uint64_t bytes{},evictions{},clock{};
        std::map<ID3D12Resource*,uint64_t> lastUse;
        void Touch(ID3D12Resource* r){if(r)lastUse[r]=++clock;}
        void Remember(ID3D12Resource* r){
            Touch(r);
            if(r&&!allocations.contains(r)){
                const auto size=r->GetDesc().Width;
                allocations.emplace(r,size);bytes+=size;
            }
        }
        // Only the serial producer calls this, after a failed candidate is
        // released. Evict whole cold allocation groups, never active CBLAS.
        void TrimCold(uint64_t target);
        void Recount();
        void RetireCatalog(const Mg2Pages&);
        uint64_t retiredClusters{};
        void Clear(){
            evictions+=clas.size()+blas.size();
            blas.clear();clas.clear();allocations.clear();lastUse.clear();bytes=clock=retiredClusters=0;
        }
    } forestAcceleration;
    // Physical backing for a stable virtual geometry address range. A block
    // stays mapped while any published/retiring geometry owns a reference.
    using ForestSourceStream=Mg2Pages::SourceStream;
    using ForestSourceTile=std::pair<ForestSourceStream,uint32_t>;
    struct ForestSourceBlock {
        ComPtr<ID3D12Heap> heap;
        ForestSourceStream stream{};
        uint32_t arena{},first{},count{};
        uint64_t lastUse{};
        bool ready{};
    };
    struct ForestSourcePool {
        struct Arena {
            std::array<ComPtr<ID3D12Resource>,Mg2Pages::SourceStreamCount> resources;
            std::array<UINT64,Mg2Pages::SourceStreamCount> begin{},end{};
        };
        std::vector<Arena> arenas;
        size_t admittedSegments{};
        std::map<ForestSourceTile,std::shared_ptr<ForestSourceBlock>> blocks;
        uint32_t ArenaFor(ForestSourceStream stream,UINT64 first,UINT64 bytes)const{
            const auto k=uint32_t(stream);
            auto it=std::upper_bound(arenas.begin(),arenas.end(),first,
                [k](UINT64 value,const Arena& a){return value<a.begin[k];});
            if(it==arenas.begin())return Invalid;
            --it;
            if(first>=it->end[k]||bytes>it->end[k]-first)return Invalid;
            return uint32_t(it-arenas.begin());
        }
        auto& Resource(uint32_t arena,ForestSourceStream stream){return arenas.at(arena).resources[uint32_t(stream)];}
        UINT64 Base(uint32_t arena,ForestSourceStream stream)const{return arenas.at(arena).begin[uint32_t(stream)];}
        uint64_t collectedRetiredClusters{},collectedGenerations{},clock{};
        void Reset(){arenas.clear();admittedSegments=0;blocks.clear();collectedRetiredClusters=collectedGenerations=clock=0;}
    } forestSourcePool;
    using TerritorialCatalogKey=std::pair<TerritorialDomain,TerritorialPatrimony::Key>;
    struct TerritorialDrawRoute {
        TerritorialCatalogKey territory;
        std::shared_ptr<const TerritorialPatrimony::Node> node;
        Mg2InstanceRoute key{};
        GpuInstance instance{};
        XMFLOAT4 bounds{};
    };
    struct ForestGeneration {
        std::map<std::pair<uint32_t,uint32_t>,ForestClasEntry> clasCache;
        std::vector<std::shared_ptr<ForestBlasEntry>> blasEntries;
        std::vector<uint32_t> newBlasParts;
        uint32_t clasReferencesBuiltParts{};
        std::vector<std::pair<uint32_t,uint32_t>> newClasKeys;
        bool published{};
        enum View { Routes,Ranges,Meshlets,Vertices,Triangles,Indices,Addresses,Remap,Bounds,CameraBoxes,Count };
        std::array<DescriptorHandle,Count> views{};
        // Worker describes views without touching the render descriptor allocator.
        struct ViewRequest {ID3D12Resource* resource{};UINT64 first{};uint32_t count{},stride{};};
        std::array<ViewRequest,Count> viewRequests{};
        std::vector<ComPtr<ID3D12Resource>> resources,staging;
        std::vector<uint32_t> selection;
        // Immutable instance routing is published with the matching geometry.
        std::vector<Mg2InstanceRoute> instanceRoutes;
        std::vector<TerritorialDrawRoute> collectiveRoutes;
        // Same immutable hash table as the GPU; native route is Invalid.
        std::vector<Mg2InstanceRoute> cpuRoutes;
        struct RayChange {Mg2InstanceRoute key{};bool addressOnly{};};
        uint64_t serial{},rayChangeBase{};
        D3D12_GPU_VIRTUAL_ADDRESS routeGpuAddress{};
        std::vector<RayChange> rayChanges;
        static void DiffRayRoutes(const ForestGeneration*,const ForestGeneration*,std::vector<RayChange>&);
        std::vector<UINT64> blasAddresses;
        std::vector<uint32_t> previousShadowVariants;
        std::vector<Mg2InstanceRoute> shadowChanges,removedRoutes;
        uint32_t FindRoute(const Mg2InstanceRoute&)const;
        // Route-only generations borrow the original geometry descriptors.
        std::shared_ptr<ForestGeneration> geometryOwner;
        uint32_t routeBytes{},variants{};
        Commands commands;
        ComPtr<ID3D12Resource> buildReadback;
        // Independent reserved source streams and fence-owned backing.
        struct SourceArenaView {
            ComPtr<ID3D12Resource> vertices,indices;
            uint32_t vertexCount{},indexCount{};
            DescriptorHandle vertexView,indexView;
        };
        std::vector<SourceArenaView> sourceArenas;
        ComPtr<ID3D12Resource> sourceArenaTable;
        std::vector<std::shared_ptr<ForestSourceBlock>> sourceBlocks;
        UINT64 geometryUploadBytes{};
        UINT64 sourceResidentBytes{},sourceDemandBytes{},sourceEvictedBytes{};
        uint32_t sourceUniqueClusters{};
        UINT64 blasStart{},blasCapacity{},clasCapacity{};
        UINT64 fence{};
        uint32_t addressCount{},pages{},clusters{},triangles{},bytes{},rasterMeshlets{};
        Mg2RayBackend backend{};
    };
    std::shared_ptr<const Mg2Pages> forestHierarchy;
    // Cook RTXMG source pages independently of the first VR frame.
    // Native full-resolution mesh buffers stay resident during the bake.
    std::future<Mg2Pages> forestBake;
    using TerritorialCatalogChanges=std::map<TerritorialCatalogKey,std::shared_ptr<const TerritorialPatrimony::Node>>;
    struct TerritorialRepresentation {
        uint32_t mesh{},material{},kind{};
        XMFLOAT3 origin{};
        XMFLOAT4 bounds{};
        float geometricError{},opticalError{};
    };
    struct TerritorialCatalogNode {
        std::shared_ptr<const TerritorialPatrimony::Node> node;
        std::vector<TerritorialRepresentation> representations;
    };
    struct TerritorialCatalogResult {
        std::shared_ptr<const Mg2Pages> catalog;
        std::map<TerritorialCatalogKey,TerritorialCatalogNode> replacements;
        uint32_t nextId{};
        uint64_t triangles{},architectureBefore{},architectureAfter{},architectureContacts{},architectureCacheHits{},architectureCacheWrites{};
    };
    struct TerritorialCatalogWork {
        std::future<TerritorialCatalogResult> completion;
        TerritorialCatalogChanges pending;
        std::shared_ptr<const TerritorialCatalogChanges> inFlight;
        std::set<uint32_t> retired;
        std::shared_ptr<const std::vector<uint32_t>> inFlightRetired;
        std::map<TerritorialCatalogKey,TerritorialCatalogNode> active;
        // Identity-only bindings, never dereferenced by the worker.
        std::array<const TerritorialPatrimony*,2> sources{};
        const ArchitecturePatrimony* architectureSource{};
        std::map<TerritorialPatrimony::Key,std::shared_ptr<const ArchitectureRecipeNode>> architectureAuthored;
        uint64_t activeRevision{1};
        uint32_t nextId{Mg2ResidentKeyBit-1u};
        bool blocked{};
    } territorialCatalog;
    void PrepareTerritorialCatalog(const XRFrame&,const GpuSceneCache*);
    // A render-only cut owns its exact descendant identities. Frame slots
    // retain it independently from the latest CPU catalog and ray mapping.
    struct TerritorialFrameCut {
        std::vector<GpuInstance> instances;
        std::vector<uint64_t> suppressed;
        struct TileCoverage {uint64_t topology{},revision{};std::vector<uint32_t> indices;};
        std::map<GpuTileKey,TileCoverage> tiles;
    };
    struct TerritorialCoverageGroup {
        TerritorialCatalogKey key;
        std::vector<GpuInstance> instances;
        std::vector<uint64_t> descendants;
        XMFLOAT4 support{};
        XMFLOAT3 supportHalf{};
        float error{};
        bool rayEligible{true};
    };
    std::vector<TerritorialCoverageGroup> territorialCoverage;
    uint64_t territorialCoverageSerial{},territorialCoverageRevision{};
    const GpuSceneCache* territorialCoverageSource{};
    std::vector<size_t> territorialCoverageChosen,territorialRayChosen;
    std::shared_ptr<const TerritorialFrameCut> territorialFrameCut,territorialRayCut,territorialParkCut;
    static constexpr float TerritorialRasterPixels=1.1f,TerritorialRayPixels=.55f;
    void PrepareTerritorialFrame(Frame&,const XRFrame&,const GpuSceneCache*);
    void UpdateForestFrameShadows(Frame&,const GpuSceneCache*,const ForestGeneration*,bool);
    struct ForestPreparedGeneration;
    struct ForestSelectionWork {
        std::future<void> completion;
        std::shared_ptr<ForestPreparedGeneration> prepared;
        std::vector<XMFLOAT4> meshBounds;
        std::map<TerritorialCatalogKey,TerritorialCatalogNode> territorialNodes;
        uint64_t territorialRevision{};
        std::map<uint32_t,TerritorialDrawRoute> collectiveInputs;
        std::vector<std::shared_ptr<ForestGeneration>> generations;
        std::shared_ptr<ForestGeneration> resident;
        Mg2RayBackend backend{};
        // The selection and producer workers are serialized by UpdateForest.
        // Only these workers access the persistent acceleration cache in play.
        ForestAccelerationCache* acceleration{};
        float selectMs{},packMs{},gpuSelectMs{};
        uint32_t gpuDispatches{};
        uint64_t gpuReadbackBytes{},gpuCompactBytes{},gpuUniqueCuts{},gpuVisitedNodes{},gpuPrunedNodes{};
        std::shared_ptr<const Mg2Pages> pages;
        // Render owner updates only changed tiles between worker requests.
        // The worker reads this stable registry without touching live world data.
        std::map<GpuTileKey,std::vector<Mg2InstanceRoute>> tileInputs;
        std::vector<Mg2InstanceRoute> staticInputs,stagedInputs;
        uint64_t stagedRevision{};
        std::vector<Mg2GpuRegistryEntry> registry;
        std::map<Mg2InstanceRoute,size_t> registrySlots;
        std::map<uint64_t,Mg2InstanceRoute> residentKeys;
        std::map<Mg2InstanceRoute,Mg2GpuRegistryEntry> parkedRegistry;
        std::shared_ptr<const TerritorialFrameCut> parkedCut;
        uint64_t parkedGeneration{};
        std::set<size_t> registryDirty;
        uint64_t registryScratchWords{},registryUploadBytes{},registryGpuBytes{};
        uint64_t retiredGenerations{};
        bool registryUploadValid{};
        const GpuSceneCache* tileSource{}; // identity only; worker never dereferences
        uint64_t staticGeneration{};
        bool staticCaptured{};
        uint32_t lodSelectionMask{};
        std::array<XMFLOAT4X4,MaxViews> vp{};
        std::array<XMFLOAT3,MaxViews> eyes{};
        std::array<XMFLOAT3,MaxViews> eyeMotion{};
        std::array<float,MaxViews> focal{};
        uint32_t eyeCount{};
        float nearPlane{},candidateMs{};
        int64_t requestedAt{};
        Mg2LodCut cut;
        bool requested{};
    } forestSelection;
    struct ForestBuildWork {
        std::future<void> completion;
        std::shared_ptr<ForestGeneration> candidate;
        Mg2ClusterStatus result;
        std::vector<uint8_t> opaqueMaterials;
        Mg2RayBackend backend{};
        UINT64 headroom{};
        float buildMs{},allocationMs{},uploadMs{},submitMs{};
        float waitMs{},validateMs{},cleanupMs{};
        uint32_t builtClas{};
        bool requested{},succeeded{},validationFailed{};
    } forestBuild;
    struct TerritorialGpuSelection {
        struct Node {XMFLOAT4 sphere;float geometric{},optical{};uint32_t escape{},selectable{};XMFLOAT4 supportBox{};};
        static_assert(sizeof(Node)==48);
        uint64_t revision{};
        std::vector<TerritorialCatalogKey> keys;
        uint32_t rootCount{};
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12Resource> nodes,roots,camera,output,readback;
    } territorialGpu;
    void PrepareTerritorialSelection(ForestSelectionWork&);
    // Exclusive MG2 producer queue. Only the serialized workers submit here in play.
    QueueState forestBuildQueue;
    // Serial selection worker owns these resources; reset only after joining it.
    ComPtr<ID3D12RootSignature> forestSelectRoot;
    ComPtr<ID3D12PipelineState> forestSelectPSO,forestSelectClearPSO,forestSelectCompactPSO,forestPreparePSO;
    ComPtr<ID3D12Resource> forestSelectPages,forestSelectClusters,forestSelectStructural,forestSelectNodes;
    std::shared_ptr<const Mg2Pages> forestSelectCatalog;
    ComPtr<ID3D12Resource> forestSelectInput,forestSelectOutput,forestSelectReadback,forestSelectScratch,forestSelectHash;
    ComPtr<ID3D12Resource> forestSelectRegistry,forestSelectCamera,forestSelectPriorFirst;
    uint64_t forestSelectPriorSerial{};
    void DispatchForestSelection(ForestSelectionWork&,std::span<uint32_t>,float);
    std::vector<Mg2ForestPart> forestParts;
    std::vector<std::shared_ptr<ForestGeneration>> forestGenerations;
    std::shared_ptr<ForestGeneration> forestPending,forestResident;
    // Session owner only; workers see the captured registry.
    std::vector<Mg2GpuRegistryEntry> forestStagedEntries;
    // Pending individual mutations share the producer, not growth ownership.
    std::map<uint64_t,Mg2GpuRegistryEntry> forestMutationEntries;
    bool ForestRouteReady(const WorldObject&,uint64_t)const;
    bool StageForestMutation(const WorldObject&,uint64_t);
    void ReleaseForestMutation(uint64_t);
    uint64_t forestStageRevision{},forestStageCheckedSerial{},forestInitialCheckedSerial{};
    bool forestStageOpen{},forestStageReady{},forestInitialReady{};
    bool StageForestGrowth(std::span<const WorldObject>,std::span<const uint64_t>,
                           std::span<const WorldObject>,std::span<const uint64_t>);
    void CommitForestGrowth();
    // Render-owner admission state; no worker consumes this journal.
    const GpuSceneCache* forestAdmissionSource{};
    std::set<GpuTileKey> forestAdmissionMissingTiles;
    uint64_t forestAdmissionStaticGeneration{};
    bool forestAdmissionScanned{},forestAdmissionStaticMissing{};
    bool ForestRouteEligible(uint32_t mesh,uint32_t flags,float sx,float sy,float sz)const;
    bool ForestRouteRequired(uint32_t mesh,uint32_t flags,float sx,float sy,float sz)const;
    bool ForestRouteLive(const Mg2InstanceRoute&,const GpuSceneCache*)const;
    bool ForestPublicationReady(const GpuSceneCache*);
    struct ForestRetiredGeneration {
        std::shared_ptr<ForestGeneration> generation;
        GpuFenceValues fences{};
        bool captured{};
    };
    std::vector<ForestRetiredGeneration> forestRetired;
    struct ForestRetireWork {
        std::future<float> completion;
        std::vector<std::shared_ptr<ForestGeneration>> batch;
        uint32_t inFlight{};
    } forestRetire;
    bool forestShadowUsesMg2{};
    // Render-owner camera sampling and event-driven MG2 scheduling.
    std::array<XMFLOAT3,MaxViews> forestCameraSampleEyes{},architectureEyeMotion{};
    int64_t forestCameraSampleAt{};
    uint32_t forestCameraSampleCount{};
    uint64_t forestAdmissionEpoch{1}; // never reset by a scene reconfigure
    bool forestRefreshSelection{},forestRetryUsed{};
    UINT64 forestRetryAvailable{};
    uint32_t forestRetryRetired{};
    float forestRadius{};
    bool mg2Diagnostic{},forestRequestFailed{};
    Mg2RayBackend forestBuildBackend{};
    bool partitionedTlas{};
    ComPtr<ID3D12PipelineState> ptlasPackPSO;
    Mg2ClusterStatus forestStatus{};
    ComPtr<ID3D12QueryHeap> forestBuildQueries;
    UINT64 forestTimestampHz{};
    D3D12_PIPELINE_STATE_FLAGS ForestPipelineFlags()const;
    void ConfigureForest(const SceneData&);
    void ResetForest();
    void PrepareForestPages(Frame&,const XRFrame&,const GpuSceneCache*,bool);
    // Serialized producer accesses this instance's source arenas and retirement pool.
    std::shared_ptr<ForestPreparedGeneration> PackForestCut(ForestSelectionWork&);
    void CollectForestRetirements();
    bool EnsureForestSourceArenas(const Mg2Pages&);
    void PublishForestSourceViews(ForestGeneration&);
    void ReleaseForestSourceViews(ForestGeneration&);
    uint64_t TrimForestSourceBlocks(const Mg2Pages&,uint64_t pressure);
    bool UploadForestPages(const Mg2LodCut&,ForestPreparedGeneration&,ForestGeneration&,ForestBuildWork&);
    bool UploadForestSourceTiles(const Mg2Pages&,const std::set<ForestSourceTile>&,ForestGeneration&,ForestBuildWork&);
    void CacheForestAcceleration(const ForestGeneration&);
    bool PrewarmForest(std::atomic<uint32_t>&,const std::atomic<bool>&);
    bool PrepareLoading(const XRFrame&,std::atomic<uint32_t>&,const std::atomic<bool>&);
    bool forestLoading{},forestPrewarmDone{},forestPrewarmComplete{};
    uint64_t forestPrewarmRevision{};
    // Startup uses the real render graph, but never an OpenXR-owned image.
    bool forestWarmRender{},forestWarmStarted{};
    uint32_t forestWarmSlots{},forestWarmRTV{Invalid},forestWarmDSV{Invalid};
    ComPtr<ID3D12Resource> forestWarmColor,forestWarmDepth;
    bool LoadingGeometrySettled(const XRFrame&,const GpuSceneCache*)const;
    bool WarmLoadingFrame(XRFrame&,std::atomic<uint32_t>&,const std::atomic<bool>&);
    bool ValidateForestGeneration(ForestGeneration&,ForestBuildWork&);
    static void MatchForestShadowVariants(ForestGeneration&,const ForestGeneration*);
    void InvalidateForestShadows(Frame&,const GpuSceneCache*,const ForestGeneration*,const ForestGeneration*);
    struct RoomProbeSlot {uint64_t key{};bool ready{};};
    struct RoomProbeUpdate {
        uint32_t ready{},dispatchCount{};
        std::vector<std::pair<uint32_t,uint32_t>> runs;
    };
    ComPtr<ID3D12Resource> roomProbes;
    DescriptorHandle roomProbesSRV,roomProbesUAV;
    uint32_t roomProbeCapacity{},roomProbeCursor{};
    std::vector<RoomProbeSlot> roomProbeSlots;
    std::vector<uint64_t> roomProbeSurfaceRevision;
    std::map<std::pair<int64_t,int64_t>,uint64_t> roomProbeCells;
    uint64_t roomProbeWideRevision{},roomProbeAllCellsRevision{},roomProbeNonresidentRevision{};
    double roomProbeNextCellScan{-1.},roomProbeLastSchedule{-1.},roomProbeCredits{};
    RoomProbeUpdate PrepareRoomProbes(Frame&,XMFLOAT3 viewer,double now);
    static constexpr uint32_t OpticalStatWords=38u*64u;
    bool PrepareOpticalSample(Frame&);
    static constexpr uint32_t DepthStatWords=14u*64u;
    bool PrepareDepthSample(Frame&);
    void PublishFrameObjects(Frame&);
    void RebaseSolarFrame(Frame&,XRFrame&,FXMMATRIX bodyToCosmos,CXMMATRIX cosmosToBody);
    struct ViewModelRange {
        uint32_t firstIndex{},indexCount{};
    };
    struct VrViewModelDraws {
        D3D12_GPU_VIRTUAL_ADDRESS left{},right{},orbit{};
        RightHandTool rightTool{RightHandTool::Empty};
        explicit operator bool()const{return left||right||orbit;}
    };
    struct PhosphorOrbitGpu {
        ComPtr<ID3D12Resource> vertices,indices;
        TextureResource plate;
        DescriptorHandle vertexSrv,indexSrv,plateSrv,plateUav;
        ComPtr<ID3D12PipelineState> opaquePSO,glassPSO,platePSO;
        uint32_t opaqueIndexCount{},glassIndexCount{};
        ViewModelRange leftGlove,rightGlove,basalt,scoria;
    } phosphorOrbit;
    void ConfigurePhosphorOrbit();
    void BakePhosphorOrbitPlate();
    void ResetPhosphorOrbit();
    VrViewModelDraws PrepareVrViewModels(Frame&,const XRFrame&);
    void DrawVrViewModels(ID3D12GraphicsCommandList7*,const VrViewModelDraws&);
    struct DlssEye {
        NVSDK_NGX_Handle* feature{};
        std::array<ComPtr<ID3D12Resource>,3> inputs;
        std::array<DescriptorHandle,3> inputUavs;
        ComPtr<ID3D12Resource> output;
        DescriptorHandle outputSrv;
    };
    ComPtr<ID3D12PipelineState> dlssPreparePSO;
    struct DlssState {
        NVSDK_NGX_Parameter* parameters{};
        std::array<DlssEye,2> eyes;
        std::array<XMFLOAT4X4,2> previousVP{};
        XMUINT2 minimum{},maximum{},optimal{},lastRenderSize{};
        float requested{},minScale{},maxScale{};
        UINT64 lastUseFence{};
        bool initialized{},available{},historyReady{};
    } dlss;
    void ConfigureDlss(DlssPreset preset);
    void ResetDlss();
    void UpdateDlss(int direction);
    bool PrepareDlss(DlssPreset preset=DlssPreset::Default);
    bool SetDlssScale(float scale);
    void RecordDlssInputs(ID3D12GraphicsCommandList7*,Frame&);
    bool EvaluateDlss(ID3D12GraphicsCommandList7*,const Constants&,bool reset);
    void TransitionDlssOutputs(ID3D12GraphicsCommandList7*,D3D12_RESOURCE_STATES before,
                               D3D12_RESOURCE_STATES after);
    // Raster and collision read the committed local blocks. The clipping
    // scratch buffer is private to the late Direct transaction.
    enum class CutKernel:uint32_t {
        Projectile,ProjectileBounds,ProjectileRefit,Stamp,Composite,Laser,ClearTiles,BuildTileArgs,
        Begin,Clip,Commit,FinishImpact,CacheBegin,CacheClear,CacheCount,CacheReserve,CacheScatter,
        CacheValidate,CachePublish,CacheCommit,GatherLaserSections,BuildLaserWallArgs,EmitLaserWalls,Count
    };
    static constexpr uint32_t CutProjectileCapacity=128;
    static constexpr float ScoriaSpeed=22.f;
    float CutFlightDuration(uint32_t hand,const XRFrame& frame)const;
    struct CutProjectile {
        XMFLOAT3 position;float age;
        XMFLOAT3 velocity;float radius;
        XMFLOAT3 acceleration;float lifetime;
        uint32_t id,kind,alive;
        float visualRadius;
    };
    static_assert(sizeof(CutProjectile)==64);
    struct CutImpact {
        XMFLOAT3 position;float radius;
        XMFLOAT3 direction;uint32_t projectileId;
        uint32_t instanceId,primitiveIndex,tick,recess;
        float hitTime;
        uint32_t blockId,owner,pending;
    };
    static_assert(sizeof(CutImpact)==64 && offsetof(CutImpact,pending)==60);
    static_assert(CutProjectileCapacity==128u); // CutImpactSlots in cut_surface.hlsli
    struct CutTriangle {
        Vertex a,b,c;
        uint32_t material,instanceId,padding0,padding1;
    };
    static_assert(sizeof(CutTriangle)==136);
    // GPU ABI: 48 bytes; all fields remain fixed for the lifetime of a loaded scene.
    // Block ID = index in cutSpatialBlocks, assigned in cutOriginal order.
    // IDs survive subtraction, but are invalidated when the scene is replaced/unloaded.
    enum class CutGeometryState:uint32_t {Source=0,Modified=1,Empty=2};
    struct CutSpatialBlock {
        XMFLOAT3 lower;uint32_t firstTriangle;   // byte offsets 0, 12
        XMFLOAT3 upper;uint32_t triangleCount;  // byte offsets 16, 28
        uint32_t instanceId;                   // byte offset 32
        uint32_t geometryState;                // 36: CutGeometryState
        uint32_t activeFirst;                  // 40: future modified-triangle range
        uint32_t activeCount;                  // 44: only meaningful in Modified
    };
    static_assert(sizeof(CutSpatialBlock)==48);
    static_assert(offsetof(CutSpatialBlock,firstTriangle)==12 &&
                  offsetof(CutSpatialBlock,upper)==16 &&
                  offsetof(CutSpatialBlock,triangleCount)==28 &&
                  offsetof(CutSpatialBlock,instanceId)==32 &&
                  offsetof(CutSpatialBlock,geometryState)==36 &&
                  offsetof(CutSpatialBlock,activeFirst)==40 &&
                  offsetof(CutSpatialBlock,activeCount)==44);
    // GPU ABI: 32 bytes, packed world-space AABB.
    // Internal: leftOrBlock=left child index, rightChild=right child index.
    // Leaf: rightChild=Invalid (UINT32_MAX), leftOrBlock=stable block ID.
    // Root is node 0; child indices are node indices, never collision order.
    struct CutSpatialNode {
        XMFLOAT3 lower;uint32_t leftOrBlock; // offsets 0, 12
        XMFLOAT3 upper;uint32_t rightChild;  // offsets 16, 28
    };
    static_assert(sizeof(CutSpatialNode)==32);
    static_assert(offsetof(CutSpatialNode,leftOrBlock)==12 &&
                  offsetof(CutSpatialNode,upper)==16 &&
                  offsetof(CutSpatialNode,rightChild)==28);
    struct CutFrame {
        ComPtr<ID3D12Resource> control,collisionBounds;
        DescriptorHandle controlUAV,collisionBoundsSRV,collisionBoundsUAV;
        uint32_t collisionLeaves{};
        ComPtr<ID3D12Resource> buildingHits,buildingReadback;
        DescriptorHandle buildingHitsUAV;
        uint64_t buildingFence{};
        uint64_t buildingFrame{},buildingGeometryRevision{};
        bool buildingStepped{};
    };
    struct CutDispatch {
        D3D12_GPU_VIRTUAL_ADDRESS step{},overlay{},projectileCut{},laserCut{};
        uint32_t source{},destination{};
        bool buildingReadback{};
        uint32_t laserLines{};
        uint32_t overlayCurrent{},overlayPrevious{};
        bool overlayReset{};
    };
    std::array<CutFrame,GpuSceneCache::FrameSlots> cutFrames;
    std::array<ComPtr<ID3D12Resource>,2> cutProjectiles;
    std::array<DescriptorHandle,2> cutProjectileSRVs{},cutProjectileUAVs{};
    ComPtr<ID3D12Resource> cutOriginal,cutScratch,cutState,cutControl,
        cutDispatchArguments,cutDrawCount,cutPendingImpacts;
    DescriptorHandle cutOriginalSRV,cutScratchSRV,cutScratchUAV,
        cutStateSRV,cutStateUAV,cutControlUAV,cutDispatchArgumentsUAV,
        cutDrawCountUAV,cutPendingImpactsUAV;
    static constexpr uint32_t CutShadowPredicatesPerSlot=7;
    ComPtr<ID3D12Resource> cutShadowPredicates;
    DescriptorHandle cutShadowPredicatesUAV;
    uint32_t lastCutShadowSlots{};
    bool cutShadowsPending{},cutWorkSubmitted{};
    uint32_t cutInitialCount{},cutCapacity{},cutStaticCount{};
    ComPtr<ID3D12Resource> cutSpatialBlocks,cutSpatialNodes,cutCandidateSelection;
    DescriptorHandle cutSpatialBlocksSRV,cutSpatialBlocksUAV,cutSpatialNodesSRV,
        cutCandidateSelectionSRV,cutCandidateSelectionUAV;
    uint32_t cutSpatialBlockCount{},cutSpatialNodeCount{},cutSpatialRoot{Invalid};
    // One live block table and one sparse staging table. Old runs become free
    // only after the Direct queue's raster and collision readers have finished.
    ComPtr<ID3D12Resource> cutCachePool,cutCacheState,cutCacheBlocks[2],
        cutCacheAllocator,cutCacheResults,
        cutCacheWork,cutCacheDispatch,cutCacheDrawArguments;
    DescriptorHandle cutCachePoolSRV,cutCachePoolUAV,cutCacheStateSRV,
        cutCacheStateUAV,cutCacheBlockSRVs[2],cutCacheBlockUAVs[2],
        cutCacheAllocatorUAV,
        cutCacheResultsUAV,cutCacheWorkUAV,cutCacheDispatchUAV,
        cutCacheDrawArgumentsUAV;
    uint32_t cutCachePoolCapacity{};
    bool cutCacheNeedsGrowth{},cutScratchNeedsGrowth{};
    static constexpr uint32_t CutPoolFull=2u;
    static constexpr uint32_t CutScratchFull=8u;
    static constexpr uint32_t LaserSectionCapacity=768;
    static constexpr uint32_t InitialCutBrushCapacity=8192;
    static constexpr uint32_t CutBrushStride=(3u+LaserSweep::MaxPlanes)*sizeof(XMFLOAT4)+32u*sizeof(uint32_t);
    uint32_t cutBrushCapacity{},cutBrushBound{},cutBrushSampleBound{};
    UINT64 cutBrushSampleFence{};
    ComPtr<ID3D12Resource> cutBrushSample;
    static constexpr XMUINT3 LaserWallGrid{4,2,4};
    struct SphereCutRange {
        uint32_t first{},count{},firstWall{Invalid},root{Invalid};
        XMFLOAT3 wallOrigin{},wallCellSize{};
    };
    std::array<SphereCutRange,SpectacleSphereCount> sphereCutRanges{};
    std::array<uint32_t,2> cutWallMaterials{Invalid,Invalid};
    ComPtr<ID3D12Resource> cutWallSegments,cutWallCounts,cutWallDispatch,cutBrushes,cutBrushCandidates,cutBrushCells;
    DescriptorHandle cutWallSegmentsUAV,cutWallCountsUAV,cutWallDispatchUAV,
        cutBrushesUAV,cutBrushCandidatesUAV,cutBrushCellsUAV;
    TextureResource cutOverlayMask;
    DescriptorHandle cutOverlaySRV,cutOverlayUAV;
    ComPtr<ID3D12Resource> cutOverlayTileLastSeen,cutOverlayTileDispatchArgs;
    std::array<ComPtr<ID3D12Resource>,2> cutOverlayActiveTiles,cutOverlayActiveCounts;
    DescriptorHandle cutOverlayTileLastSeenUAV,cutOverlayTileDispatchArgsUAV;
    std::array<DescriptorHandle,2> cutOverlayActiveTileUAVs,cutOverlayActiveCountUAVs;
    uint32_t cutOverlayTileCountX{},cutOverlayTileCountY{},cutOverlayGeneration{};
    bool cutOverlayNeedsInitialization{true};
    uint32_t cutNextId{1},cutStepSerial{};
    std::array<float,SpectacleSphereCount> cutFlightRemaining{};
    std::array<bool,SpectacleSphereCount> cutDrainPending{};
    std::array<uint64_t,SpectacleSphereCount> cutFlightEpoch{};
    bool cutActive{};
    void InitializeCutSource(const SceneData&);
    CutDispatch PrepareCutDispatch(Frame&,const XRFrame&,bool runProjectiles,bool sphereProxy);
    void ResetCutResources();
    std::array<ComPtr<ID3D12PipelineState>,size_t(CutKernel::Count)> cutPipelines;
    void EnsureCutPipelines();
    void BindCutPipeline(ID3D12GraphicsCommandList7*,CutKernel,D3D12_GPU_VIRTUAL_ADDRESS);
    TextureResource hdr,reflections,depth,hdrResolved,reflectionsResolved,hierarchy,shadow;
    TextureResource solarReceivers;
    DescriptorHandle solarReceiversSRV;
    uint32_t solarReceiversRTV{Invalid};
    ComPtr<ID3D12PipelineState> solarDepthPSO,solarDepthSeedPSO;
    std::array<TextureResource,2> solarHistory;
    std::array<DescriptorHandle,2> solarHistorySRV{},solarHistoryUAV{};
    std::array<XMFLOAT4X4,MaxViews> solarLastVP{},solarLastInverseVP{};
    uint32_t solarLastWidth{},solarLastHeight{};
    uint64_t solarLastFrame{};
    bool solarHistoryValid{};
    static constexpr uint32_t SolarSpatialSlots=1u<<20;
    ComPtr<ID3D12Resource> solarSpatial;
    DescriptorHandle solarSpatialUAV;
    ComPtr<ID3D12Resource> solarTileWork;
    DescriptorHandle solarTileWorkSRV,solarTileWorkUAV;
    ComPtr<ID3D12PipelineState> solarTileResetPSO,solarTileCompactPSO,solarTileFinalizePSO;
    ComPtr<ID3D12PipelineState> solarSamplesPSO,solarSpatialClearPSO,solarSpatialBuildPSO;
    std::array<TextureResource,2> history,historyDepth;
    uint32_t width{},height{},viewCount{},renderWidth{},renderHeight{},renderMips{},mips{},clustersCount{};DXGI_FORMAT colorFormat{};
    uint32_t hdrRTV{},hdrResolvedRTV{Invalid},reflectionsRTV{},depthDSV{},depthReadDSV{},shadowDSV{};DescriptorHandle depthSRV,hierarchySRV,hdrResolvedSRV,hdrResolvedUAV,reflectionsResolvedSRV,shadowSRV;std::vector<DescriptorHandle> hierarchyUAV;std::array<DescriptorHandle,2> historySRV{},historyUAV{},historyDepthSRV{},historyDepthUAV{};
    struct SampleTargets {
        TextureResource hdr,reflections,depth,hdrResolved,reflectionsResolved;
        DescriptorHandle depthSRV,hdrResolvedSRV,hdrResolvedUAV,reflectionsResolvedSRV;
    };
    std::array<SampleTargets,2> parkedTargets;
    void SwapSampleTargets(SampleTargets& t);
    std::vector<uint32_t> xrRTV,xrDSV;
    bool cutDiagnosticEnabled{},opticalDiagnosticEnabled{};
    static constexpr uint32_t CutDiagnosticStride=80u;
    struct Frame {
        std::shared_ptr<ForestGeneration> forestRaster,forestTlas,forestPreviousTlas;
        bool forestRayReset{};
        std::shared_ptr<const TerritorialFrameCut> territorialCut,territorialRayCut,territorialTlasCut;
        DescriptorHandle territorialInstanceSRV,territorialSuppressionSRV,territorialCameraSRV;
        DescriptorHandle territorialRayInstanceSRV,territorialRaySuppressionSRV,territorialShadowSRV;
        uint32_t lodRayMask{15u};
        ComPtr<ID3D12Resource> forestDrawReadback,forestRayProofReadback;
        bool forestRayProofPending{};
        uint64_t forestDrawFrame{};
        ComPtr<ID3D12Resource> cutCacheReadback,cutDiagnosticReadback,cutImpactReadback;
        std::vector<ComPtr<ID3D12Resource>> retiredCutResources;
        UINT64 cutImpactReadbackFence{},cutImpactReadbackEpoch{};
        uint32_t cutImpactReadbackOwner{};
        UINT64 cutCacheReadbackFence{};
        uint32_t cutCacheReadbackCapacity{},cutCacheReadbackMask{},cutScratchReadbackCapacity{};
        UINT64 cutDiagnosticFence{};
        uint64_t cutDiagnosticFrame{};
        uint32_t cutDiagnosticMask{},cutDiagnosticPoolCapacity{},cutDiagnosticScratchCapacity{};
        struct UploadSlice {
            ID3D12Resource* resource{};
            std::byte* data{};
            UINT64 offset{},bytes{};
        };
        std::shared_ptr<UploadGeneration> transferUpload;
        std::vector<std::shared_ptr<UploadGeneration>> transferOverflow;
        UINT64 transferBump{};
        struct TileTransfer {
            GpuTileKey key{};
            uint64_t topologyGeneration{};
            ComPtr<ID3D12Resource> buffer;
            uint32_t firstElement{};
            UploadSlice staging;
            std::vector<GpuSceneDirtyRange> ranges;
            std::vector<std::pair<uint32_t,uint64_t>> updates;
        };
        std::vector<TileTransfer> tileTransfers;
        std::vector<ComPtr<ID3D12Resource>> heldTileSources;
        TransientArena transient;std::vector<ComPtr<ID3D12Resource>> dynamicBLAS;UINT64 dynamicScratchBytes{};std::vector<UINT64> dynamicScratchOffsets;uint32_t lightCount{},shadowDirty{};bool hasShadowedLights{},hasShadowAtlasLights{};DescriptorHandle lightsSRV;UINT64 lightsOffset{};ComPtr<ID3D12Resource> lights;std::vector<Light> lightData;std::vector<uint32_t> lightIds;uint32_t lightCapacity{},clusterIndexCapacity{},directionalCapacity{};
        Light globalSun{};bool hasGlobalSun{};
        ComPtr<ID3D12Resource> opticalStats,opticalStatsReadback;
        DescriptorHandle opticalStatsUAV;
        uint64_t opticalSampleFence{},opticalSampleFrame{};
        ComPtr<ID3D12Resource> depthStats,depthStatsReadback;
        DescriptorHandle depthStatsUAV;
        uint64_t depthSampleFence{},depthSampleFrame{};
        bool depthSampleDirect{};
        std::array<SceneLight,ShadowSlots> shadowLights{};std::array<uint64_t,ShadowSlots> shadowKeys{};std::array<uint32_t,ShadowSlots> shadowIDs{};double simulationTime{};


        ComPtr<ID3D12Resource> lightBvhNodes,lightBvhPairs,lightBvhBounds;
        DescriptorHandle lightBvhNodesSRV,lightBvhNodesUAV,
            lightBvhPairsSRV,lightBvhPairsUAV,
            lightBvhBoundsSRV,lightBvhBoundsUAV;
        uint32_t lightBvhLeaves{},lightBvhNodeCapacity{},
            lightBvhPairCapacity{},lightBvhBoundsCapacity{},
            lightBvhSortedCount{};
        bool lightBvhSorted{},lightBvhPendingSort{};
        uint64_t lightBvhLastSortFrame{};
        std::vector<Light> lightBvhSortSnapshot;
        ComPtr<ID3D12Resource> motionBuffer,boneBuffer,weightBuffer,deformedBounds;DescriptorHandle motionSRV,boneSRV,weightSRV,deformedBoundsSRV,deformedBoundsUAV;UINT64 motionOffset{},boneOffset{},weightOffset{};struct TransparentDraw {uint32_t index{},mesh{};XMFLOAT3 center{};};
        std::vector<TransparentDraw> transparent;uint32_t transparentCount{},instanceCapacity{};
        // The frame slot is selected only after all its queue fences retire.
        // Keep captured D3D12 objects alive until this list is replaced.
        struct ResidentDrawCache {
            Commands commands;
            std::vector<uint64_t> signature;
            std::vector<ComPtr<ID3D12Object>> references;
            uint32_t psoSets{},indirectCalls{};
        } residentDrawCache;
        Commands dlssEvaluation;
        std::array<std::vector<std::unique_ptr<Commands>>,GraphQueueCount> batchCommands;std::array<UINT64,GraphQueueCount> queueFences{};UINT64 fenceValue{},base{},bump{};std::shared_ptr<UploadGeneration> uploadGeneration;Constants* constants{};std::vector<GpuInstance> cpuObjects;GpuInstance* objects{};UINT64 constantsOffset{},objectsOffset{};
        uint32_t cpuCount{},dynamicCount{},snapshotCount{},residentCount{},rayInstanceCount{},rayMapCapacity{};
        struct RayMapSegment {
            uint32_t compactFirst{},count{},renderFirst{};
            bool operator==(const RayMapSegment&)const=default;
        };
        std::vector<RayMapSegment> rayMapSegments,committedRayMapSegments,dirtyRayMapSegments;
        bool rayMapBuild{};
        DescriptorHandle rayUAV,rayMapSRV,rayMapUAV,blasAddressesSRV,opaqueOrderSRV,residentWorkSRV;
        ComPtr<ID3D12Resource> blasAddresses,rayDescriptors,rayInstanceMap;
        ComPtr<ID3D12Resource> ptlasWriteArgs;
        DescriptorHandle ptlasWriteUAV;
        uint32_t ptlasCapacity{},rayInstanceCapacity{};
        bool tlasPartitioned{};
        ComPtr<ID3D12Resource> worldInstances,publishedInstances,instanceRanges,streamControl,clusters,clusterIndices,clusterGroupCounts,clusterGroupOffsets,clusterDirectionalIndices,clusterDirectionalCount,clusterBounds,tlas,scratch,dynamicVertices,dynamicScratch,hotBounds,hotMeshes;
        DescriptorHandle worldInstanceSRV,worldInstanceUAV,publishedInstanceSRV,instanceRangesSRV,clusterSRV,clusterUAV,clusterIndicesSRV,clusterIndicesUAV,clusterGroupCountsSRV,clusterGroupCountsUAV,clusterGroupOffsetsSRV,clusterGroupOffsetsUAV,clusterDirectionalIndicesSRV,clusterDirectionalIndicesUAV,clusterDirectionalCountSRV,clusterDirectionalCountUAV,clusterBoundsSRV,clusterBoundsUAV,tlasSRV,dynamicSRV,dynamicUAV,hotBoundsSRV,hotBoundsUAV,hotMeshesSRV,hotMeshesUAV;
        InstanceSourceRange* instanceRangeData{};
        uint32_t instanceRangeCapacity{2};
        uint32_t worldInstanceCapacity{},publishedInstanceCapacity{},instanceCount{},additionalCount{},lastTlasCount{},streamControlSlots{};bool tlasBuilt{};FrameWorkSet deformationWork,blasWork;std::vector<uint8_t> dynamicBlasBuilt;std::vector<uint64_t> dynamicGeometryRevision,dynamicBlasRevision,pendingDynamicRevision;
        struct VisibilitySet {
            ComPtr<ID3D12Resource> depthArguments,depthCount,cullArguments,cullCount,shadowArguments,shadowCount;
            DescriptorHandle depthArgumentsUAV,depthCountUAV,cullArgumentsUAV,cullCountUAV,shadowArgumentsUAV,shadowCountUAV;

            ComPtr<ID3D12Resource> batchArguments,batchCount,batchVisibility;
            ComPtr<ID3D12Resource> tileBatchMarks,tileBatchPrefixes,tileBatchTotals,tileBatchOffsets;
            DescriptorHandle batchArgumentsUAV,batchCountUAV,batchVisibilityUAV,batchVisibilitySRV;
            DescriptorHandle tileMarksUAV,tileMarksSRV,tilePrefixesUAV,tilePrefixesSRV,
                tileTotalsUAV,tileTotalsSRV,tileOffsetsUAV,tileOffsetsSRV;
            uint32_t capacity{},batchRunCapacity{},tileCapacity{},tileBlockCapacity{};
        };
        VisibilitySet cpuVisibility,publishedVisibility;
        ComPtr<ID3D12Resource> resourceDirectory;std::array<ComPtr<ID3D12Resource>,ResourceDomainCount> resourceTables;DescriptorHandle resourceDirectorySRV;std::array<DescriptorHandle,ResourceDomainCount> resourceTableSRV{};DomainDirectoryEntry* resourceDirectoryData{};std::array<DomainBindingEntry*,ResourceDomainCount> resourceTableData{};
    };static_assert(GpuSceneCache::FrameSlots==3);std::array<Frame,GpuSceneCache::FrameSlots> frames;Frame* active{};GpuFrameResources frameResources;uint64_t frameNumber{};bool historyReady{};std::array<XMFLOAT4X4,MaxViews> lastVP{};std::array<XMFLOAT4X4,ShadowSlots*6> shadowMatrices{};
    struct BoundPublication {GpuStream* stream{};GpuDomain* domain{};GpuPublication publication{};bool deferred{};};std::vector<BoundPublication> publications;GpuPublicationCatalog publicationCatalog;
    struct ReservedStream {GpuStream* stream{};GpuPublication publication;};
    std::vector<ReservedStream> reservedStreams;

    struct MeshSolidDomain final : GpuDomain {
        struct Binding {GpuPublication publication{};uint32_t base{},capacity{},controlSlot{};};
        Impl& owner;std::vector<Binding> bindings;uint64_t total{};
        explicit MeshSolidDomain(Impl& value);
        GpuWorldDomain Identity()const noexcept override;
        const char* Name()const noexcept override;
        void BeginFrame(uint64_t)override;
        bool ValidatePublication(const GpuPublication& p)const override;
        void BindPublication(const GpuPublication& p)override;
        uint32_t TotalInstances()const;
        const std::vector<Binding>& Bindings()const;
        static void RecordCopies(void* context,ID3D12GraphicsCommandList7* list);
        static void RecordPhysicsLightCopies(void* context,ID3D12GraphicsCommandList7* list);
        void BuildPasses(GpuPassBuilder& builder,const GpuFrameContext&)override;
    };

    std::unique_ptr<MeshSolidDomain> meshDomain;std::vector<GpuRayPublicationState> rayPublicationScratch;
    struct RayPublicationRange {uint32_t compactFirst{},count{},renderFirst{};};
    std::vector<RayPublicationRange> rayPublicationRanges;
    struct RaySlotLease {uint32_t first{},capacity{};};
    using RaySlotOwner=std::array<uint64_t,4>;
    std::map<RaySlotOwner,RaySlotLease> raySlotLeases;
    uint32_t raySlotCount{};
    uint32_t ReserveRaySlots(const RaySlotOwner&,uint32_t count);
    std::vector<std::shared_ptr<GpuDomain>> externalDomains;std::vector<GpuDomain*> domainRegistry;bool domainServicesReady{};

    TextureResource* CanonicalTexture(ID3D12Resource* resource);
    struct DomainPassBuilder final : GpuPassBuilder {
        Impl& owner;uint32_t node{UINT32_MAX};GpuWorldDomain domainId{GpuDomainId::Invalid};bool open{};
        explicit DomainPassBuilder(Impl& o);
        void SetDomain(GpuWorldDomain id);
        static GraphQueue Queue(GpuPassQueue q);
        static GraphMode Mode(GpuPassResourceAccess a);
        static uint32_t NameHash(const char* name);

        void ProducerWaits(ID3D12Resource* resource);
        void BeginPass(const char* name,GpuPassQueue queue,GpuPassCallback cb={})override;
        void Buffer(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,uint32_t contentVersion=GpuContentVersion::Current)override;
        void Texture(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_LAYOUT initialLayout,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,GpuTextureSubresources range={},uint32_t contentVersion=GpuContentVersion::Current)override;
        void EndPass()override;
    };
    struct CachedShadow {uint32_t id{Invalid};uint64_t key{};bool valid{};};std::array<CachedShadow,ShadowSlots> shadowCache;
    Impl();
    GpuDomain* FindDomain(GpuWorldDomain id)const;
    void RegisterDomain(std::shared_ptr<GpuDomain> domain);
    bool UnregisterDomain(GpuWorldDomain id);
    void DetachStream(GpuStream& stream);
    double sceneTimeOrigin{};bool sceneTimeOriginValid{};XrTime lastDisplayTime{};uint64_t lastTrackingRevision{},rootHash{},abiHash{};
    struct CacheHeader {uint64_t magic{0x32314f535052564cull};uint32_t schema{2},vendor{},device{},subsystem{},revision{};uint32_t driverHigh{},driverLow{},reserved{};uint64_t root{},abi{};};CacheHeader cacheIdentity{};bool persistentCache{};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index);
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index);
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index);
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT index);
    uint32_t Physical(DescriptorHandle handle)const;
    uint32_t Generation(DescriptorHandle handle)const;
    ID3D12Device* Device()const noexcept override;
    ID3D12RootSignature* BindlessRootSignature()const noexcept override;
    ID3D12DescriptorHeap* BindlessResourceHeap()const noexcept override;
    ID3D12DescriptorHeap* BindlessSamplerHeap()const noexcept override;
    GpuResourceHandle CreateBufferSrv(ID3D12Resource* resource,uint32_t count,uint32_t stride)override;
    GpuResourceHandle CreateBufferUav(ID3D12Resource* resource,uint32_t count,uint32_t stride)override;
    GpuResourceHandle CreateTextureSrv(ID3D12Resource* resource,DXGI_FORMAT format)override;
    GpuResourceHandle CreateTextureUav(ID3D12Resource* resource,DXGI_FORMAT format,uint32_t mip=0)override;
    uint32_t DescriptorIndex(GpuResourceHandle handle)const override;
    uint32_t DescriptorGeneration(GpuResourceHandle handle)const override;
    GpuFenceValues SubmittedFences()const;
    GpuFenceValues CompletedFences()const;
    void CollectManaged();
    GpuResourceLease CreateManagedBuffer(const GpuBufferDesc& d)override;
    GpuResourceLease CreateManagedTexture2D(const GpuTextureDesc& d)override;
    GpuResourceLease CreateManagedTexture3D(const GpuTextureDesc& d)override;
    GpuResourceLease UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name)override;
    GpuResourceLease UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources)override;
    void ReleaseDescriptor(DescriptorHandle& handle)override;
    ComPtr<ID3D12Resource> Placed(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial=D3D12_RESOURCE_STATE_COMMON,const D3D12_CLEAR_VALUE* clear=nullptr,bool dedicated=false) ;
    ComPtr<ID3D12Resource> Temporary(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state,
                                           bool required=true);
    ComPtr<ID3D12Resource> Host(UINT64 size,D3D12_HEAP_TYPE type);
    void CommandsInit(Commands& c,D3D12_COMMAND_LIST_TYPE type);
    void QueueInit(QueueState& q,D3D12_COMMAND_LIST_TYPE type);
    void WaitStartup(QueueState& q);
    DescriptorHandle Srv(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={},UINT64 firstElement=0);
    DescriptorHandle UavBuffer(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={});
    DescriptorHandle TextureSrv(ID3D12Resource* r,DXGI_FORMAT format,DescriptorHandle handle={},bool arrayView=false);
    DescriptorHandle TextureUav(ID3D12Resource* r,DXGI_FORMAT format,uint32_t mip=0,DescriptorHandle handle={},bool arrayView=false);
    uint32_t MakeRTV(ID3D12Resource* r,DXGI_FORMAT format,uint32_t reuse=Invalid);
    uint32_t MakeDSV(ID3D12Resource* r,uint32_t reuse=Invalid,bool readOnly=false);
    GpuCacheKey CacheKey(uint32_t kind,const void* data,size_t bytes,uint64_t generation=0);
    ComPtr<ID3D12Resource> UploadBuffer(const void* data,UINT64 bytes);
    ComPtr<ID3D12Resource> CachedUploadBuffer(const void* data,UINT64 bytes,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE);
    template<class T>ComPtr<ID3D12Resource> UploadVector(const std::vector<T>& v){return v.empty()?ComPtr<ID3D12Resource>{}:UploadBuffer(v.data(),v.size()*sizeof(T));}
    template<class T>ComPtr<ID3D12Resource> CachedUploadVector(const std::vector<T>& v,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){return v.empty()?ComPtr<ID3D12Resource>{}:CachedUploadBuffer(v.data(),v.size()*sizeof(T),kind,generation,name,flags);}
    uint32_t UploadTexture(const TextureData& t);
    std::shared_ptr<UploadGeneration> NewUploadGeneration(UINT64 chunk,uint32_t slots=3);
    Frame::UploadSlice StageFrameTransfer(Frame&,UINT64 bytes);
    void Bind(ID3D12GraphicsCommandList7* l,Frame& f,bool graphics);
    ComPtr<ID3D12PipelineState> Pipeline(const wchar_t* name,const D3D12_PIPELINE_STATE_STREAM_DESC& desc,UINT64 hash);
    UINT64 Hash(const std::vector<std::byte>& a,UINT64 seed=1469598103934665603ull);
    std::vector<std::byte> Shader(const wchar_t* name);
    void Pipelines();
    void Initialize(const XrGraphicsRequirementsD3D12KHR& req);
    void InitializeDesktop();
    void InitializeDevice();
    void EnableClusterPreview();
    bool clusterPreview{};
    uint64_t HashMemory(const void* data,size_t size,uint64_t hash=1469598103934665603ull);
    GpuInstance ConvertObject(const RenderObject& o,float alpha)const;
    const GpuInstance& CpuInstance(const Frame& f,uint32_t index)const;
    GpuCacheKey StaticBlasKey(uint32_t mesh) ;
    // Scene-local arrays outlive all dependent native BLAS and resident CLAS.
    // Only the source atlas and ordered UV triangle determine the micromap.
    struct FoliageOmm {
        uint32_t mesh{},material{};
        std::map<std::array<uint32_t,6>,uint32_t> indices;
        ComPtr<ID3D12Resource> nativeArray,nvapiArray;
        uint32_t Index(const Vertex& a,const Vertex& b,const Vertex& c) const;
        bool TryIndex(const Vertex& a,const Vertex& b,const Vertex& c,uint32_t& index) const;
    };
    std::vector<FoliageOmm> foliageOmms;
    const FoliageOmm* FindFoliageOmm(uint32_t mesh) const;
    void PrepareFoliageOmm(const SceneData&);
    void BuildBLAS();
    D3D12_RAYTRACING_GEOMETRY_DESC DynamicGeometry(Frame& f,uint32_t mesh) ;
    void DynamicResources(Frame& f) ;
    void InitializeResourceAbi(Frame& f);
    void SetDomain(Frame& f,ResourceDomain domain,std::initializer_list<DescriptorHandle> descriptors);
    void BuildDynamic(ID3D12GraphicsCommandList7* list,Frame& f) ;
    void EnsureInstances(Frame& f,uint32_t worldRequired,uint32_t cpuRequired,uint32_t totalRequired);
    void EnsureFrameInstanceCapacity(Frame&,uint32_t required);
    void EnsureInstanceRoutes(Frame& f,uint32_t required);
    void EnsureLightBuffers(Frame& f);
    void EnsureLightBvhBuffers(Frame& f);
    GpuInstance ConvertResidentBase(const GpuResidentTileObject& o)const;
    void EnsureRayInstanceMap(Frame& f,uint32_t required);
    void EnsureVisibilityBuffers(Frame& f,uint32_t cpuCount,uint32_t publishedCount);
    void EnsureBatchRunCounters(Frame& f,uint32_t runCount);
    void EnsureTileBatchBuffers(Frame& f,uint32_t objectCount,uint32_t blockCount);
    void EnsureStreamControl(Frame& f,uint32_t slots);
    void ResetSceneResources();
    bool ConfigureSampleTargets(bool fresh);
    void ApplyMultisampling();
    void Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra);
    void ConfigureOutput(const OutputSurface&,const SceneData&,JobSystem&,RenderContributions);
    void ReleaseOutput();
    void ReplaceScene(const SceneData& assets,RenderContributions extra);
    void UnloadScene();
    UINT64 Reserve(Frame& f,UINT64 size,UINT64 alignment=256);
    XMFLOAT4 InstanceCameraSphere(const GpuInstance&,uint32_t mesh)const;
    ResidentBatchData PrepareResidentBatches(Frame& f,const GpuSceneCache* tileCatalog,
        const XRFrame& xr,bool coreSceneWork,bool campusVisible);
    uint64_t buildingRevision{};
    std::vector<SceneLight> buildingLights;
    struct PtlasDispatch { D3D12_GPU_VIRTUAL_ADDRESS count{},ops{}; };
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO PtlasPrebuild(uint32_t capacity);
    PtlasDispatch PreparePtlasDispatch(Frame&,uint32_t writes,uint32_t updates);
    void BuildPtlas(Frame&,ID3D12GraphicsCommandList7*,bool update,const PtlasDispatch&);
    RayFrameData PrepareRayScene(Frame& f,const GpuSceneCache* tileCatalog,uint32_t frameSlot,bool campusVisible);
    bool AddLightBvhPass(Frame& f,bool coreSceneWork);
    void PrepareLights(Frame&,const RenderScene&,const std::optional<SceneLight>& sun={});
    const RenderScene& Visuals(const FrameInput& input);
    bool Prepare(const FrameInput& input) ;
    bool PrepareResidency(const XRFrame& xr,Frame& f,bool coreSceneWork,bool campusVisible);


    void ClearCount(ID3D12GraphicsCommandList7* l,ID3D12Resource* r,DescriptorHandle descriptor);
    void ArgumentsReady(ID3D12GraphicsCommandList7* l,ID3D12Resource* args,ID3D12Resource* count);
    void Viewport(ID3D12GraphicsCommandList7* l,UINT w,UINT h,UINT x=0,UINT y=0);
    void ReserveStreams();
    void CancelReservedStreams();
    void ReturnUnusedPublication(const GpuPublication& publication);
    void RequestCapture(const std::wstring& path);
    struct PresentComposite {
        uint32_t dlssStatus{};
        bool blendReflection{},useDlss{};
    };
    void DrawPresentComposite(ID3D12GraphicsCommandList7*,Frame&,const XRFrame&,
                              const uint32_t (&params)[4],const PresentComposite&);
    void RecordCapture(ID3D12GraphicsCommandList7* list,ID3D12Resource* target);
    bool SaveCapture();
    static bool EncodeCapture(Capture&,bool replaceExisting);
    std::future<bool> FinishCaptureAsync();
    void Render(XRFrame& xr,const GpuProducerCoordinator& coordinator);
    ~Impl();
};
}
