#pragma once
#include "Graphics.h"
#include "Assets.h"
#include "SolarSystem.h"
#include "LunarBattle.h"
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

struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

namespace OASIS {
using Microsoft::WRL::ComPtr;
namespace graphics_internal {
constexpr UINT64 MiB=1024ull*1024,UploadChunk=8*MiB;

static_assert(MaxViews<=D3D12_MAX_VIEW_INSTANCE_COUNT);
constexpr uint32_t ShadowSlots=4,ShadowFace=512;
constexpr uint32_t DirectLightLimit=4,SmallSceneOcclusionThreshold=256;
enum GpuCacheKind:uint32_t {
    CacheStaticInstances=1,CacheVertices,CacheIndices,CacheMeshlets,CacheRemap,CacheTriangles,CacheRanges,
    CacheMaterials,CacheDeformations,CacheSkin,CacheMorphs,CacheTexture,CacheStaticBlas,CacheMeshletRegions,
    CacheWritableVertices,CacheWritableIndices,CacheWritableMeshlets,CacheWritableRemap,CacheWritableTriangles,CacheWritableRanges,CacheTriangleSolids,CacheWritableTriangleSolids,CacheWritableMeshletRegions
};
static_assert(NoRayTracing==GpuRayNoTracingFlag);
constexpr uint32_t DescriptorCapacity=8192,ResourceDomainWords=32,ResourceAbiVersion=OASIS_RESOURCE_ABI_VERSION;

enum class ResourceDomain:uint32_t {Meshes,Frame,Lighting,History,Animation,Visibility,RayTracing,Count};
constexpr uint32_t ResourceDomainCount=uint32_t(ResourceDomain::Count);
enum MeshBinding:uint32_t {MeshVertices,MeshMeshlets,MeshRemap,MeshTriangles,MeshRanges,MeshMaterials,MeshIndices,MeshRegions,MeshVerticesWrite,MeshIndicesWrite,MeshMeshletsWrite,MeshRemapWrite,MeshTrianglesWrite,MeshRangesWrite,MeshTriangleSolids,MeshBindingCount};
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
    LightingBindingCount
};
enum HistoryBinding:uint32_t {HistoryHierarchy,HistoryDepthMS,HistoryColorResolved,HistoryReflectionResolved,HistoryPrevious,HistoryDepthPrevious,HistoryOutput,HistoryDepthOutput,HistoryCurrent,HistoryDepthResolved,HistoryHiZMipBase};
enum AnimationBinding:uint32_t {AnimationDynamicVertices,AnimationDynamicVerticesUAV,AnimationDeformations,AnimationMotions,AnimationBones,AnimationWeights,AnimationSkin,AnimationMorphs,AnimationDeformedBounds,AnimationDeformedBoundsUAV,AnimationBindingCount};
enum VisibilityBinding:uint32_t {VisibilityHotBounds,VisibilityHotBoundsUAV,VisibilityHotMeshes,VisibilityHotMeshesUAV,VisibilityBindingCount};
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
};
static_assert(sizeof(Surface)==304&&offsetof(Surface,tintGamma)==272&&offsetof(Surface,roughnessOverride)==288);


inline uint32_t MaterialTextureMask(const Surface& s){
    return (s.albedo!=Invalid?1u:0u)|
           (s.normal!=Invalid?2u:0u)|
           (s.metalRough!=Invalid?4u:0u)|
           (s.emissive!=Invalid?8u:0u)|
           (s.extra.x!=Invalid?16u:0u);
}
constexpr uint32_t ShuttleCrtVariant=32u; // Follows the 32 texture-mask variants in scene.hlsl.
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
    if(s.extra.w==1u||s.extra.w>=3u)return false;


    if(s.emissive==Invalid&&std::max({s.emissionMetallic.x,s.emissionMetallic.y,s.emissionMetallic.z})>=1.f)return false;
    const float minimumRoughness=std::clamp((s.metalRough==Invalid&&s.roughnessOverride==Invalid?
        s.colorRoughness.w:0.f)+s.roughnessBias,.04f,1.f);
    const float maximumMetal=std::clamp(s.emissionMetallic.w+s.metallicBias,0.f,1.f);
    if(maximumMetal>.55f&&minimumRoughness<.32f)return true;
    if(!quality)return false;
    float threshold=quality==1?.18f:quality==2?.28f:.38f;
    return minimumRoughness<threshold;
}
struct SphereProjection {
    XMFLOAT4 sphere,fog,light,emission,optics,tint,parameters;
    XMUINT4 atlas;
    XMFLOAT4 bounds[MaxViews];
    XMFLOAT4X4 sourceVP[2];
    XMFLOAT4 sourceRect[2],sourceDepth[2];
};
static_assert(sizeof(SphereProjection)==(20u+MaxViews)*16u);
struct alignas(256) Constants {XMFLOAT4X4 vp[MaxViews],inverseVP[MaxViews],previousVP[MaxViews],shadowVP[ShadowSlots*6];XMFLOAT4 eye[MaxViews];XMUINT4 resourceAbi,dimensions,outputDimensions,counts,topology;XMFLOAT4 timing,tileViewer;XMUINT4 lightBvhLayout;XMFLOAT4 hud,atmosphereSphere,atmosphereColor,atmosphereLight,atmosphereEmission,atmosphereParams;XMUINT4 cutGeometry,cutRaster,membraneField;XMFLOAT4 membraneAnimation,membraneOptics,membraneHandPulses[HandCount*2u],membraneStrokeStart[HandCount],membraneStrokeEnd[HandCount],membranePassagePulses[MembranePassagePulseCount];XMUINT4 debrisBindings;
    XMUINT4 sphereAtlas;XMFLOAT4 sphereRect,sphereWave;XMFLOAT4 sphereBounds[MaxViews];
    XMFLOAT4X4 sphereSourceVP[2];XMFLOAT4 sphereSourceRect[2],sphereSourceDepth[2];
    XMFLOAT4 sphereGuideOrigin,sphereGuideDistances;XMUINT4 sphereGuide;
    XMFLOAT4 membraneTint;XMUINT4 spherePair;
    SphereProjection sphereProjections[SpectacleSphereCount];
    XMUINT4 sphereOrder[MaxViews*((SpectacleSphereCount+3u)/4u)];
    XMUINT4 sphereTiles[MaxViews*SphereTileAxis*SphereTileAxis];
    XMUINT4 sphereScene; // Native range end, solar flag, relief normals, first native lunar instance.
    XMFLOAT4 solarSun,solarOrientation,lunarOrigin; // w is 1 for companion poses, 2 for the native Moon.
    std::array<LunarDrawPose,LunarPoseCount> lunarPoses;
    std::array<XMFLOAT4,LunarRippleCount> lunarRipples;
    XMFLOAT4X4 prevDlssVP[2];XMFLOAT4 dlssJitter;XMUINT4 dlssOutput;};
static_assert(sizeof(Constants)%256==0);
static_assert(sizeof(Constants)<=16u*D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT);
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class T>struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=Type;T data{};};
struct MeshPipeline {
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
struct ComputePipeline {Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,D3D12_SHADER_BYTECODE> cs;};
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
            if(owner==SolarMoon){
                profile.density=.12f;
                profile.film.opacity=.038f;
            }
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
    float cpuPrepareMs{},potentialFrameMs{-1.f};
    bool dlssDiagnosticEnabled{};
    DlssDiagnosticState dlssDiagnostic;
    bool sphereBaselineTiming{};
    bool scenePassTiming{};
    std::array<SphereGpuTimings,12> sphereGpuTimings{};
    size_t sphereGpuTimingCount{};
    void CollectGpuTimings();
    struct TimerQueue {
        ComPtr<ID3D12QueryHeap> queries;
        ComPtr<ID3D12Resource> readback;
        UINT64 frequency{};
    };
    struct TimerSample {
        bool pending{};
        uint32_t slot{};
        std::array<uint32_t,GraphQueueCount> counts{};
        std::array<UINT64,GraphQueueCount> fences{};
        float cpuMs{};
        uint64_t diagnosticFrame{};
        uint32_t dlssBatch{Invalid};
        uint32_t directPassMask{};
        float dlssScale{},dlssCpuMs{-1.f};
        std::array<std::array<std::array<char,64>,GpuBatchTimer::MaxBatches>,GraphQueueCount> batchNames;
    };
    std::array<TimerQueue,GraphQueueCount> timerQueues{};
    std::array<TimerSample,GpuBatchTimer::Slots> timerSamples{};
    bool timerInitialized{};


std::vector<GpuStream*> streams;RenderScene visualFrame{};RenderContributions contributions;GpuSceneCache sceneCache;GpuRayScene rayScene;ComPtr<ID3D12Resource> residentStaticInstances;
    struct ResidentMaterialRun {
        uint32_t first{},count{},mask{},reserved{};
        bool operator==(const ResidentMaterialRun&)const=default;
    };
    static_assert(sizeof(ResidentMaterialRun)==16);
    struct ResidentTileGpu {
        ComPtr<ID3D12Resource> buffer,runBuffer;
        DescriptorHandle srv,runSrv;
        uint32_t objectCapacity{},objectCount{},runCapacity{};
        uint64_t topologyGeneration{},runRevision{};
        std::vector<uint32_t> opaqueOrder;
        uint64_t opaqueOrderRevision{};
        std::vector<ResidentMaterialRun> runs;
        bool runsDirty{};
    };
    struct TileBatchDispatch {
        GpuTileKey key{};
        uint32_t first{},count{},blockFirst{},blockCount{},
            superFirst{},superCount{},runFirst{},runCount{},runDescriptor{};
    };
    struct OpaqueMaterialRun {uint32_t first{},count{},mask{};};
    struct ResidentBatchData {
        std::vector<TileBatchDispatch> tileBatchDispatch;
        std::vector<OpaqueMaterialRun> opaqueRuns;
        uint32_t tileBatchBlocks{};
    };
    struct RayTileAck {
        GpuTileKey key{};
        uint64_t topologyGeneration{};
        std::vector<std::pair<uint32_t,uint64_t>> updates;
    };
    struct RayFrameData {
        std::vector<RayTileAck> acks;
        std::vector<GpuRayDirtyRange> cpuDirty;
        bool dirty{};
    };
    std::map<GpuTileKey,ResidentTileGpu> residentTileGpu;
    std::vector<uint64_t> staticMaterialBits;std::vector<uint32_t> staticTransparent,staticDeformedMeshes,staticDeformedRtMeshes;std::array<bool,4> staticReflectionCandidates{};std::vector<SceneLight> baseLights;uint32_t baseLightCount{};
    ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter3> adapter;ComPtr<ID3D12Device10> device;QueueState direct,compute,copy;PassGraph passGraph;FrameGraphPlan frameGraph;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineLibrary1> library;ComPtr<ID3D12CommandSignature> indirect,dispatchIndirect,cutBlockIndirect;
    ComPtr<ID3D12DescriptorHeap> descriptors,samplers,rtvs,dsvs;UINT descriptorStride{},rtvStride{},dsvStride{},rtvCount{},dsvCount{};DescriptorAllocator descriptorAllocator{DescriptorCapacity,&Fail};
    struct Page {ComPtr<ID3D12Heap> heap;UINT64 size{},used{};uint32_t residency{};bool dedicated{};};std::vector<Page> pages;Residency residency;GpuVramCache vramCache;GpuManagedMemory managedMemory;uint32_t pageCount{};UINT64 committed{};
    struct UploadGeneration {
        ComPtr<ID3D12Resource> resource;std::byte* mapped{};UINT64 chunk{};uint64_t generation{};
        ~UploadGeneration(){if(resource&&mapped)resource->Unmap(0,nullptr);}
    };
    std::shared_ptr<UploadGeneration> uploadGeneration;std::atomic<uint64_t> uploadGenerationSerial{};Commands startup;std::vector<ComPtr<ID3D12Resource>> uploads;std::wstring cachePath;std::vector<std::byte> cacheBlob;
MeshData meshes;SceneAnimation animation;JobSystem* jobs{};bool sceneLoaded{},writableSceneMeshes{};uint64_t geometryGenerationSerial{};ComPtr<ID3D12Resource> vertices,indices,meshlets,remap,triangles,ranges,materials,deformations,skinVertices,morphVertices,meshletRegions,triangleSolids;DescriptorHandle skinSRV,morphSRV,indicesSRV,verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,deformationsSRV,meshletRegionsSRV,triangleSolidsSRV,meshletRegionsUAV,triangleSolidsUAV;std::array<DescriptorHandle,6> geometryUAVs{};
    std::vector<ComPtr<ID3D12Resource>> blas;std::vector<uint32_t> dynamicMeshes,meshDynamicIndex;
    struct ResidentTexture {ComPtr<ID3D12Resource> resource;DescriptorHandle descriptor;uint32_t residency{};};std::vector<ResidentTexture> textures;std::vector<Surface> surfaceData,baseSurfaceData;
    std::vector<SurfaceSettings> surfaceEdits;
    std::vector<uint32_t> materialSlots;
    std::vector<MaterialTuning> materialTuning;
    std::vector<GpuSceneDirtyRange> dirtySurfaces;
    std::vector<uint64_t> shadowSurfaceRevision;
    uint64_t roomProbeMaterialRevision{};
    std::map<uint32_t,std::vector<uint32_t>> materialSurfacesBySlot;
    std::vector<std::vector<uint32_t>> materialCaptureOwners;
    std::vector<uint32_t> staticOpaqueOrder;
    uint32_t MsaaSamples{OASIS_MSAA_SAMPLES},requestedSamples{OASIS_MSAA_SAMPLES};
    uint32_t aaMode{};
    TextureResource aaSource;GpuResourceLease aaSourceLease;DescriptorHandle aaSourceArraySrv;ComPtr<ID3D12PipelineState> fxaaPSO;
    TextureResource vrsImage;
    DescriptorHandle vrsImageUAV;
    ComPtr<ID3D12PipelineState> vrsPSO;
    std::array<XrFovf,MaxViews> vrsFov{};
    uint32_t vrsTileSize{},vrsWidth{},vrsHeight{};
    bool vrsImageReady{};
    void ApplySurfaceEdits(std::span<const uint32_t> indices={});
    std::vector<uint64_t> visibleMaterialBits;std::vector<uint32_t> materialResidencyOffsets,materialResidencyPages,residencyPageSeen,requiredResidencyPages;uint32_t residencyPageGeneration{1};
    std::array<ComPtr<ID3D12PipelineState>,ShuttleCrtVariant+1u> sceneVariantPSO,sceneDirectVariantPSO;
    struct SphereLodCache {
        struct View {
            XMFLOAT4X4 vp{};
            XMFLOAT4 rect{},depth{};
            XMFLOAT3 direction{};
            uint64_t fence{},revision{};
        };
        bool enabled{};
        SphereLodSettings settings;
        TextureResource relief,reliefNormals;
        DescriptorHandle reliefSRV,reliefUAV,reliefNormalsSRV,reliefNormalsUAV;
        ComPtr<ID3D12Resource> mean;
        DescriptorHandle meanSRV,meanUAV;
        bool meanReady{};
        std::array<View,SphereCacheLayers> views{};
        SphereRepresentation current{SphereRepresentation::Geometry},candidate{SphereRepresentation::Geometry};
        double candidateSince{},refreshAfter{},waveStart{-1.};
        double liteSourceAfter{},liveCaptureAfter{};
        uint32_t liteSource{Invalid};
        std::array<uint32_t,2> transitionViews{Invalid,Invalid};
        SphereZone zone{SphereZone::Far};
        float warpRadians{};
        uint64_t revision{1};
    } sphereLod,companionLod,oasisRelief;
    std::array<SphereLodCache,SpectacleSphereCount-SpectaclePairCount> extraSphereLods;
    SphereLodCache& SphereCache(uint32_t owner){
        if(owner>=SpectaclePairCount)return extraSphereLods[owner-SpectaclePairCount];
        return owner==1u?companionLod:sphereLod;
    }
    bool HasOasisRelief()const{return bool(oasisRelief.relief.resource);}
    SphereLodCache& SphereAtlas(uint32_t owner){
        if(owner==0u&&HasOasisRelief())return oasisRelief;
        return DenseSphereField()?sphereLod:SphereCache(owner);
    }
    uint32_t SphereCaptureSize(uint32_t owner)const{
        return owner==0u&&HasOasisRelief()?OasisReliefSize:SphereCacheSize;
    }
    const SphereLodCache::View& SphereView(uint32_t owner,uint32_t slot);
    uint32_t SphereViewLayer(uint32_t owner,uint32_t slot)const;
    struct SphereSelection {
        std::array<uint32_t,2> views{Invalid,Invalid};
        std::array<XMFLOAT4,MaxViews> bounds{};
        SphereRepresentation level{SphereRepresentation::Hidden};
        bool fresh{},captureVisible{},nativeVisible{};
    };
    uint32_t nativeSphere{Invalid},pendingNativeSphere{Invalid};
    double pendingNativeSince{};
    ComPtr<ID3D12PipelineState> sphereCapturePSO,sphereMeanPSO,sphereProxyPSO,sphereInspectPSO;
    void SetupSphereCache(SphereLodCache&,uint32_t layers=SphereCacheLayers,uint32_t size=SphereCacheSize);
    void ResetSphereCache(SphereLodCache&);
    SphereRepresentation SelectSphereRepresentation(const XRFrame&,uint32_t owner,
        SphereSelection&,std::optional<SphereRepresentation> force,bool ellipse);
    uint32_t SelectInteractiveSphere(const XRFrame&)const;
    void SelectSpherePair(XRFrame&,std::array<SphereSelection,SpectacleSphereCount>&);
    void BinSphereField(Constants&,XRFrame&,const std::array<SphereSelection,SpectacleSphereCount>&);
    ComPtr<ID3D12PipelineState> spherePerceptionPSO;
    ComPtr<ID3D12PipelineState> scenePSO,sceneDirectPSO,cutScenePSO,cutSceneDirectPSO,cutDepthPSO,cutShadowPSO,transparentPSO,transparentDirectPSO,depthPSO,shadowPSO,presentPSO,dlssPresentPSO,cullPSO,depthResolvePSO,hizPSO,clusterCountPSO,clusterOffsetsPSO,clusterFillPSO,
        lightBvhPartialPSO,lightBvhBoundsPSO,lightBvhMortonPSO,
        lightBvhSortPSO,lightBvhLeavesPSO,lightBvhParentsPSO,roomProbesPSO,
        denoisePSO,mergeReflectionPSO,deformPSO,deformBoundsPSO,hotDataPSO,rayBuildPSO,batchOpaquePSO,rayMapFillPSO,tileBatchMarkPSO,tileBatchScanPSO,tileBatchOffsetScanPSO,tileBatchOffsetFinalizePSO,tileBatchEmitPSO;
    struct Frame;
    struct RoomProbeState {
        uint64_t signature{};
        uint32_t first{},ready{},next{};
    };
    struct RoomProbeUpdate {
        RoomProbeState state;
        uint32_t dispatchFirst{},dispatchCount{};
    };
    ComPtr<ID3D12Resource> roomProbes;
    DescriptorHandle roomProbesSRV,roomProbesUAV;
    uint32_t roomProbeCapacity{};
    RoomProbeState roomProbeState{};
    RoomProbeUpdate PrepareRoomProbes(Frame&,XMFLOAT3 viewer);
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
    std::array<TextureResource,2> history,historyDepth;
    uint32_t width{},height{},viewCount{},renderWidth{},renderHeight{},renderMips{},mips{},clustersCount{};DXGI_FORMAT colorFormat{};
    uint32_t hdrRTV{},hdrResolvedRTV{Invalid},reflectionsRTV{},depthDSV{},shadowDSV{};DescriptorHandle depthSRV,hierarchySRV,hdrResolvedSRV,hdrResolvedUAV,reflectionsResolvedSRV,shadowSRV;std::vector<DescriptorHandle> hierarchyUAV;std::array<DescriptorHandle,2> historySRV{},historyUAV{},historyDepthSRV{},historyDepthUAV{};
    struct SampleTargets {
        TextureResource hdr,reflections,depth,hdrResolved,reflectionsResolved;
        DescriptorHandle depthSRV,hdrResolvedSRV,hdrResolvedUAV,reflectionsResolvedSRV;
    };
    std::array<SampleTargets,2> parkedTargets;
    void SwapSampleTargets(SampleTargets& t);
    std::vector<uint32_t> xrRTV,xrDSV;
    bool cutDiagnosticEnabled{};
    static constexpr uint32_t CutDiagnosticStride=80u;
    struct Frame {
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
            UploadSlice staging;
            std::vector<GpuSceneDirtyRange> ranges;
            std::vector<std::pair<uint32_t,uint64_t>> updates;
        };
        struct TileRunTransfer {
            GpuTileKey key{};
            uint64_t revision{};
            ComPtr<ID3D12Resource> buffer;
            UploadSlice staging;
        };
        std::vector<TileTransfer> tileTransfers;
        std::vector<TileRunTransfer> tileRunTransfers;
        std::vector<ComPtr<ID3D12Resource>> heldTileSources;
        struct CachedTileDraw {
            Commands commands;
            bool initialized{};
            uint64_t signature{};
        };
        std::map<GpuTileKey,std::array<CachedTileDraw,2>> tileDrawCache;
        TransientArena transient;std::vector<ComPtr<ID3D12Resource>> dynamicBLAS;UINT64 dynamicScratchBytes{};uint32_t lightCount{},shadowDirty{};bool hasShadowedLights{},hasShadowAtlasLights{};DescriptorHandle lightsSRV;UINT64 lightsOffset{};ComPtr<ID3D12Resource> lights;std::vector<Light> lightData;uint32_t lightCapacity{},clusterIndexCapacity{},directionalCapacity{};
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
        Commands dlssEvaluation;
        std::array<std::vector<std::unique_ptr<Commands>>,GraphQueueCount> batchCommands;std::array<UINT64,GraphQueueCount> queueFences{};UINT64 fenceValue{},base{},bump{};std::shared_ptr<UploadGeneration> uploadGeneration;Constants* constants{};std::vector<GpuInstance> cpuObjects;GpuInstance* objects{};UINT64 constantsOffset{},objectsOffset{};
        uint32_t cpuCount{},dynamicCount{},snapshotCount{},residentCount{},rayInstanceCount{},rayMapCapacity{};
        struct RayMapSegment {
            uint32_t compactFirst{},count{},renderFirst{};
            bool operator==(const RayMapSegment&)const=default;
        };
        std::vector<RayMapSegment> rayMapSegments,committedRayMapSegments,dirtyRayMapSegments;
        bool rayMapBuild{};
        DescriptorHandle rayUAV,rayMapSRV,rayMapUAV,blasAddressesSRV,opaqueOrderSRV;
        ComPtr<ID3D12Resource> blasAddresses,rayDescriptors,rayInstanceMap;
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
    uint32_t MakeDSV(ID3D12Resource* r,uint32_t reuse=Invalid);
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
    uint64_t HashMemory(const void* data,size_t size,uint64_t hash=1469598103934665603ull);
    GpuInstance ConvertObject(const RenderObject& o,float alpha)const;
    const GpuInstance& CpuInstance(const Frame& f,uint32_t index)const;
    GpuCacheKey StaticBlasKey(uint32_t mesh) ;
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
    GpuInstance ConvertResidentObject(const GpuResidentTileObject& o,double x,double z)const;
    void EnsureRayInstanceMap(Frame& f,uint32_t required);
    void EnsureVisibilityBuffers(Frame& f,uint32_t cpuCount,uint32_t publishedCount);
    void EnsureBatchRunCounters(Frame& f,uint32_t runCount);
    void EnsureTileBatchBuffers(Frame& f,uint32_t objectCount,uint32_t blockCount);
    void EnsureStreamControl(Frame& f,uint32_t slots);
    void ResetSceneResources();
    void ConfigureSampleTargets(bool fresh);
    void ApplyMultisampling();
    void Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra);
    void ConfigureOutput(const OutputSurface&,const SceneData&,JobSystem&,RenderContributions);
    void ReleaseOutput();
    void ReplaceScene(const SceneData& assets,RenderContributions extra);
    void UnloadScene();
    UINT64 Reserve(Frame& f,UINT64 size,UINT64 alignment=256);
    ResidentBatchData PrepareResidentBatches(Frame& f,const GpuSceneCache* tileCatalog,bool coreSceneWork,bool campusVisible);
    uint64_t buildingRevision{};
    std::vector<SceneLight> buildingLights;
    RayFrameData PrepareRayScene(Frame& f,const GpuSceneCache* tileCatalog,uint32_t frameSlot,bool campusVisible);
    bool AddLightBvhPass(Frame& f,bool coreSceneWork);
    void PrepareLights(Frame& f,const RenderScene& snapshot) ;
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
        bool reflections{},useDlss{},proxyOnly{},dense{},runOverlay{},visibleSphere{};
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
