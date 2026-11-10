#pragma once
#include "Graphics.h"
#include "TerminalDomain.h"
#include "VrCapture.h"
#include "AtmosphereDomain.h"
#include "OpticalDomain.h"
#include "SnowDomain.h"
#include "Assets.h"
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
#include "SessionLog.h"
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

namespace lab {
using Microsoft::WRL::ComPtr;
namespace graphics_internal {
extern thread_local uint64_t traceFrame;
constexpr UINT64 MiB=1024ull*1024,UploadChunk=8*MiB;

static_assert(MaxViews<=D3D12_MAX_VIEW_INSTANCE_COUNT);
constexpr uint32_t ShadowSlots=4,ShadowFace=512,ClusterWords=(MaxLights+31)/32;
constexpr uint32_t DirectLightLimit=4,SmallSceneOcclusionThreshold=256;
enum GpuCacheKind:uint32_t {
    CacheStaticInstances=1,CacheVertices,CacheIndices,CacheMeshlets,CacheRemap,CacheTriangles,CacheRanges,
    CacheMaterials,CacheDeformations,CacheSkin,CacheMorphs,CacheTexture,CacheStaticBlas
};
static_assert(NoRayTracing==GpuRayNoTracingFlag);
constexpr uint32_t DescriptorCapacity=8192,ResourceDomainWords=32,ResourceAbiVersion=1;
// Domain IDs are append-only. New systems append a domain; existing domain ABIs never shift.
enum class ResourceDomain:uint32_t {Meshes,Frame,Lighting,History,Animation,Visibility,RayTracing,Count};
constexpr uint32_t ResourceDomainCount=uint32_t(ResourceDomain::Count);
enum MeshBinding:uint32_t {MeshVertices,MeshMeshlets,MeshRemap,MeshTriangles,MeshRanges,MeshMaterials,MeshIndices,MeshBindingCount};
enum FrameBinding:uint32_t {
    FrameInstances=0,FrameReservedArguments=1,FrameReservedCounter=2,FrameInstanceUAV=3,
    FrameReservedCullArguments=4,FrameReservedCullCounter=5,FrameReservedShadowArguments=6,FrameReservedShadowCounter=7,
    FrameCpuDepthArguments=8,FrameCpuDepthCounter=9,FramePublishedDepthArguments=10,FramePublishedDepthCounter=11,
    FrameCpuCullArguments=12,FrameCpuCullCounter=13,FramePublishedCullArguments=14,FramePublishedCullCounter=15,
    FrameCpuShadowArguments=16,FrameCpuShadowCounter=17,FrameBindingCount=18
};
enum LightingBinding:uint32_t {LightingLights,LightingClusters,LightingClusterUAV,LightingShadowAtlas,LightingBindingCount};
enum HistoryBinding:uint32_t {HistoryHierarchy,HistoryDepthMS,HistoryColorResolved,HistoryReflectionResolved,HistoryPrevious,HistoryDepthPrevious,HistoryOutput,HistoryDepthOutput,HistoryCurrent,HistoryDepthResolved,HistoryHiZMipBase};
enum AnimationBinding:uint32_t {AnimationDynamicVertices,AnimationDynamicVerticesUAV,AnimationDeformations,AnimationMotions,AnimationBones,AnimationWeights,AnimationSkin,AnimationMorphs,AnimationDeformedBounds,AnimationDeformedBoundsUAV,AnimationBindingCount};
enum VisibilityBinding:uint32_t {VisibilityHotBounds,VisibilityHotBoundsUAV,VisibilityHotMeshes,VisibilityHotMeshesUAV,VisibilityBindingCount};
enum RayBinding:uint32_t {RayTLAS,RayDescriptors,RayBlasAddresses,RayBindingCount};
using DescriptorHandle=GpuResourceHandle;
struct DomainDirectoryEntry {uint32_t descriptor{Invalid},generation{},count{},reserved{};};
struct DomainBindingEntry {uint32_t descriptor{Invalid},generation{};};
static_assert(sizeof(DomainDirectoryEntry)==16&&sizeof(DomainBindingEntry)==8);
using DescriptorAllocator=GpuDescriptorAllocator<ComPtr<ID3D12Resource>>;
inline UINT64 Align(UINT64 v,UINT64 a){return (v+a-1)&~(a-1);}
inline D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES p{};p.Type=type;p.CreationNodeMask=p.VisibleNodeMask=1;return p;}
inline D3D12_RESOURCE_DESC Buffer(UINT64 size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=Align(size,4);d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
inline D3D12_RESOURCE_DESC Texture(UINT w,UINT h,DXGI_FORMAT format,UINT samples=1,UINT layers=1,UINT mips=1,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=UINT16(layers);d.MipLevels=UINT16(mips);d.Format=format;d.SampleDesc.Count=samples;d.Flags=flags;return d;}
inline bool SameResourceDesc(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b){return a.Dimension==b.Dimension&&a.Alignment==b.Alignment&&a.Width==b.Width&&a.Height==b.Height&&a.DepthOrArraySize==b.DepthOrArraySize&&a.MipLevels==b.MipLevels&&a.Format==b.Format&&a.SampleDesc.Count==b.SampleDesc.Count&&a.SampleDesc.Quality==b.SampleDesc.Quality&&a.Layout==b.Layout&&a.Flags==b.Flags;}
// Only layouts survive ExecuteCommandLists. Access dependencies belong to one recording.
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

    LogEvent(
        traceFrame,
        "gpu.texture_barrier",
        "resource=%p layout=%u->%u sync=%llu->%llu access=%llu->%llu",
        static_cast<void*>(texture.resource.Get()),
        unsigned(b.LayoutBefore),
        unsigned(b.LayoutAfter),
        static_cast<unsigned long long>(b.SyncBefore),
        static_cast<unsigned long long>(b.SyncAfter),
        static_cast<unsigned long long>(b.AccessBefore),
        static_cast<unsigned long long>(b.AccessAfter));

    texture.layout=layout;
    prior.access=access;
    prior.sync=sync;
}
inline void BBarrier(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource,D3D12_BARRIER_SYNC before,D3D12_BARRIER_SYNC after,D3D12_BARRIER_ACCESS a,D3D12_BARRIER_ACCESS b){D3D12_BUFFER_BARRIER bar{};bar.SyncBefore=before;bar.SyncAfter=after;bar.AccessBefore=a;bar.AccessAfter=b;bar.pResource=resource;bar.Offset=0;bar.Size=UINT64_MAX;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_BUFFER;g.NumBarriers=1;g.pBufferBarriers=&bar;list->Barrier(1,&g);LogEvent(traceFrame,"gpu.buffer_barrier","resource=%p sync=%llu->%llu access=%llu->%llu",static_cast<void*>(resource),static_cast<unsigned long long>(before),static_cast<unsigned long long>(after),static_cast<unsigned long long>(a),static_cast<unsigned long long>(b));}
inline bool StaticUploadFormat(DXGI_FORMAT f){switch(f){
case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SINT:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SINT:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_UINT:case DXGI_FORMAT_R8G8B8A8_SINT:
case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:
case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:
case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:case DXGI_FORMAT_BC4_SNORM:
case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC5_SNORM:case DXGI_FORMAT_BC6H_UF16:case DXGI_FORMAT_BC6H_SF16:
case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:return true;default:return false;}}
inline void Uav(ID3D12GraphicsCommandList7* l){D3D12_GLOBAL_BARRIER b{};b.SyncBefore=D3D12_BARRIER_SYNC_ALL_SHADING;b.SyncAfter=D3D12_BARRIER_SYNC_ALL_SHADING;b.AccessBefore=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;b.AccessAfter=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_GLOBAL;g.NumBarriers=1;g.pGlobalBarriers=&b;l->Barrier(1,&g);}
struct Surface {XMFLOAT4 colorRoughness,emissionMetallic;uint32_t albedo{Invalid},normal{Invalid},metalRough{Invalid},emissive{Invalid};XMFLOAT4 parameters{1,0,0,0};XMUINT4 extra{Invalid,0,0,0};XMFLOAT4 opacity{1,.5f,1,0};std::array<TextureMapping,5> mapping{};XMFLOAT4 procedural{};};
static_assert(sizeof(Surface)==272);
struct Light {XMFLOAT4 positionRadius,colorPower,directionOuter,parameters;};
struct RenderPassPlan {
    bool occlusion{},depthHierarchy{},clusteredLights{},reflections{},shadowAtlas{},rayQueries{},rayWork{};
};
inline bool SurfaceMayReflect(const Surface& s,uint32_t quality){
    if(s.extra.w==1)return false;
    // If emissive is textured its sampled value may fall below the constant,
    // therefore only an untextured >=1 emission can prove reflections useless.
    if(s.emissive==Invalid&&std::max({s.emissionMetallic.x,s.emissionMetallic.y,s.emissionMetallic.z})>=1.f)return false;
    float minimumRoughness=s.metalRough==Invalid?s.colorRoughness.w:0.f;
    float maximumMetal=s.emissionMetallic.w;
    if(maximumMetal>.55f&&minimumRoughness<.32f)return true;
    if(!quality)return false;
    float threshold=quality==1?.18f:quality==2?.28f:.38f;
    return minimumRoughness<threshold;
}
struct alignas(256) Constants {XMFLOAT4X4 vp[MaxViews],inverseVP[MaxViews],previousVP[MaxViews],shadowVP[ShadowSlots*6];XMFLOAT4 eye[MaxViews];XMUINT4 resourceAbi,dimensions,outputDimensions,counts,topology;XMFLOAT4 timing;};
static_assert(sizeof(Constants)%256==0);
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
struct QueueState {const char* name{};ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;UINT64 serial{};UINT64 Signal(){auto v=++serial;Hr(queue->Signal(fence.Get(),v),"queue signal");LogEvent(traceFrame,"gpu.signal","queue=%s fence=%llu",name,static_cast<unsigned long long>(v));return v;}void Wait(const QueueState& q,UINT64 value){Hr(queue->Wait(q.fence.Get(),value),"queue dependency");LogEvent(traceFrame,"gpu.wait","queue=%s producer=%s fence=%llu",name,q.name,static_cast<unsigned long long>(value));}};
struct Commands {ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList7> list;void Reset(){Hr(allocator->Reset(),"command allocator");Hr(list->Reset(allocator.Get(),nullptr),"command list");textureDependencies.Begin(list.Get());}void Close(){Hr(list->Close(),"command close");}void Execute(QueueState& q){ID3D12CommandList* lists[]={list.Get()};q.queue->ExecuteCommandLists(1,lists);LogEvent(traceFrame,"gpu.execute","queue=%s command_list=%p",q.name,static_cast<void*>(list.Get()));}void Submit(QueueState& q){Close();Execute(q);}};

}
}
#include "GraphicsFrameGraph.h"
namespace lab {
namespace graphics_internal {

}
using namespace graphics_internal;
struct Graphics::Impl : GpuDomainServices {
    struct TextureReadback {
        ComPtr<ID3D12Resource> source,buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        uint64_t handle{},frameId{},fenceValue{},capacity{},bytes{};
        uint32_t subresource{},tightRowBytes{};
        DXGI_FORMAT pixelFormat{DXGI_FORMAT_UNKNOWN};
        void* mapped{};
        bool recorded{},released{},failed{};
        ~TextureReadback(){if(mapped&&buffer)buffer->Unmap(0,nullptr);}
    };
    std::vector<std::unique_ptr<TextureReadback>> textureReadbacks;
    uint64_t nextTextureReadback{1};
    VrCapture capture;
    VrCaptureTicket captureTicket{};
    struct PendingCapture {VrCaptureTicket ticket;uint64_t readback{};double submittedAt{};};
    std::vector<PendingCapture> captureReadbacks;
    Graphics* facade{};
    std::vector<ComPtr<ID3D12Resource>> xrColorTargets;

    void FinishTextureReadbacks();

    BurstGpuProfiler performanceProfiler;
    DXGI_ADAPTER_DESC1 adapterDescription{};
    bool copyQueueTimestamps{};
    GpuValidation validation;std::vector<GpuStream*> streams;RenderScene visualFrame{};RenderContributions contributions;GpuSceneCache sceneCache;GpuRayScene rayScene;ComPtr<ID3D12Resource> residentStaticInstances;
    std::vector<uint64_t> staticMaterialBits;std::vector<uint32_t> staticTransparent,staticDeformedMeshes,staticDeformedRtMeshes;std::array<bool,4> staticReflectionCandidates{};std::array<SceneLight,MaxLights> baseLights{};uint32_t baseLightCount{};uint64_t staticShadowKey{1469598103934665603ull};
    ComPtr<IDXGIFactory6> factory;ComPtr<IDXGIAdapter3> adapter;ComPtr<ID3D12Device10> device;QueueState direct,compute,copy;PassGraph passGraph;FrameGraphPlan frameGraph;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineLibrary1> library;ComPtr<ID3D12CommandSignature> indirect,dispatchIndirect;
    ComPtr<ID3D12DescriptorHeap> descriptors,samplers,rtvs,dsvs;UINT descriptorStride{},rtvStride{},dsvStride{},rtvCount{},dsvCount{};DescriptorAllocator descriptorAllocator{DescriptorCapacity,&Fail};
    struct Page {ComPtr<ID3D12Heap> heap;UINT64 size{},used{};uint32_t residency{};bool dedicated{};};std::vector<Page> pages;Residency residency;GpuVramCache vramCache;GpuManagedMemory managedMemory;uint32_t pageCount{};UINT64 committed{};
    struct UploadGeneration {
        ComPtr<ID3D12Resource> resource;std::byte* mapped{};UINT64 chunk{};uint64_t generation{};
        ~UploadGeneration(){if(resource&&mapped)resource->Unmap(0,nullptr);}
    };
    std::shared_ptr<UploadGeneration> uploadGeneration;std::atomic<uint64_t> uploadGenerationSerial{};Commands startup;std::vector<ComPtr<ID3D12Resource>> uploads;std::wstring cachePath;std::vector<std::byte> cacheBlob;
    MeshData meshes;SceneAnimation animation;JobSystem* jobs{};const XR* configuredXr{};bool sceneLoaded{};ComPtr<ID3D12Resource> vertices,indices,meshlets,remap,triangles,ranges,materials,deformations,skinVertices,morphVertices;DescriptorHandle skinSRV,morphSRV,indicesSRV,verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,deformationsSRV;
    std::vector<ComPtr<ID3D12Resource>> blas;std::vector<uint32_t> dynamicMeshes,meshDynamicIndex;
    struct ResidentTexture {ComPtr<ID3D12Resource> resource;DescriptorHandle descriptor;uint32_t residency{};};std::vector<ResidentTexture> textures;std::vector<Surface> surfaceData,baseSurfaceData;
    struct SurfaceEdit {uint64_t owner{};LabSurfaceSettingsV1 settings{};};std::vector<SurfaceEdit> surfaceEdits;bool surfacesDirty{};
    uint32_t MsaaSamples{LAB_MSAA_SAMPLES},requestedSamples{LAB_MSAA_SAMPLES};
    struct MsaaClient {uint64_t owner{};uint32_t samples{};};std::vector<MsaaClient> msaaClients;
    struct AaClient {uint64_t owner{};uint32_t mode{};};std::vector<AaClient> aaClients;uint32_t aaMode{};
    TextureResource aaSource;GpuResourceLease aaSourceLease;ComPtr<ID3D12PipelineState> fxaaPSO;
    void ApplySurfaceEdits();
    std::vector<uint64_t> visibleMaterialBits;std::vector<uint32_t> materialResidencyOffsets,materialResidencyPages,residencyPageSeen,requiredResidencyPages;uint32_t residencyPageGeneration{1};
    ComPtr<ID3D12PipelineState> scenePSO,sceneDirectPSO,transparentPSO,transparentDirectPSO,depthPSO,shadowPSO,presentPSO,cullPSO,depthResolvePSO,hizPSO,clusterPSO,denoisePSO,mergeReflectionPSO,deformPSO,deformBoundsPSO,hotDataPSO,rayBuildPSO;
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
    struct Frame {
        TransientArena transient;std::vector<ComPtr<ID3D12Resource>> dynamicBLAS;UINT64 dynamicScratchBytes{};uint32_t lightCount{},shadowDirty{};bool hasShadowedLights{},hasShadowAtlasLights{};DescriptorHandle lightsSRV;UINT64 lightsOffset{};ComPtr<ID3D12Resource> lights;std::array<Light,MaxLights> lightData{};std::array<SceneLight,ShadowSlots> shadowLights{};std::array<uint64_t,ShadowSlots> shadowKeys{};std::array<uint32_t,ShadowSlots> shadowIDs{};double simulationTime{};
        ComPtr<ID3D12Resource> motionBuffer,boneBuffer,weightBuffer,deformedBounds;DescriptorHandle motionSRV,boneSRV,weightSRV,deformedBoundsSRV,deformedBoundsUAV;UINT64 motionOffset{},boneOffset{},weightOffset{};std::vector<uint32_t> transparent;uint32_t transparentCount{},instanceCapacity{};
        uint64_t submittedFrame{},sourceInput{},sourceTick{};std::array<std::vector<std::unique_ptr<Commands>>,GraphQueueCount> batchCommands;std::array<UINT64,GraphQueueCount> queueFences{};UINT64 fenceValue{},base{},bump{};std::shared_ptr<UploadGeneration> uploadGeneration;Constants* constants{};std::vector<GpuInstance> cpuObjects;GpuInstance* objects{};UINT64 constantsOffset{},objectsOffset{};
        uint32_t cpuCount{},dynamicCount{};DescriptorHandle rayUAV,blasAddressesSRV;ComPtr<ID3D12Resource> blasAddresses;ComPtr<ID3D12Resource> rayDescriptors;ComPtr<ID3D12Resource> instances,streamControl,clusters,tlas,scratch,dynamicVertices,dynamicScratch,hotBounds,hotMeshes;
        DescriptorHandle instanceSRV,instanceUAV,clusterSRV,clusterUAV,tlasSRV,dynamicSRV,dynamicUAV,hotBoundsSRV,hotBoundsUAV,hotMeshesSRV,hotMeshesUAV;uint32_t instanceCount{},additionalCount{},lastTlasCount{},streamControlSlots{};bool tlasBuilt{};FrameWorkSet deformationWork,blasWork;std::vector<uint8_t> dynamicBlasBuilt;std::vector<uint64_t> dynamicGeometryRevision,dynamicBlasRevision,pendingDynamicRevision;
        struct VisibilitySet {
            ComPtr<ID3D12Resource> depthArguments,depthCount,cullArguments,cullCount,shadowArguments,shadowCount;
            DescriptorHandle depthArgumentsUAV,depthCountUAV,cullArgumentsUAV,cullCountUAV,shadowArgumentsUAV,shadowCountUAV;
            uint32_t capacity{};
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
        void BuildPasses(GpuPassBuilder& builder,const GpuFrameContext&)override;
    };

    std::shared_ptr<TerminalDomain> terminalDomain;
    std::shared_ptr<AtmosphereDomain> atmosphereDomain;
    std::shared_ptr<OpticalDomain> opticalDomain;
    std::shared_ptr<SnowDomain> snowDomain;
    std::unique_ptr<MeshSolidDomain> meshDomain;std::vector<GpuRayPublicationState> rayPublicationScratch;std::vector<std::shared_ptr<GpuDomain>> externalDomains;std::vector<GpuDomain*> domainRegistry;bool domainServicesReady{};

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
    GpuMemoryStats MemoryStats()const override;
    void ReleaseDescriptor(DescriptorHandle& handle)override;
    ComPtr<ID3D12Resource> Placed(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial=D3D12_RESOURCE_STATE_COMMON,const D3D12_CLEAR_VALUE* clear=nullptr,bool dedicated=false) ;
    ComPtr<ID3D12Resource> Temporary(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state) ;
    ComPtr<ID3D12Resource> Host(UINT64 size,D3D12_HEAP_TYPE type);
    void CommandsInit(Commands& c,D3D12_COMMAND_LIST_TYPE type);
    void QueueInit(QueueState& q,D3D12_COMMAND_LIST_TYPE type);
    void WaitStartup(QueueState& q);
    DescriptorHandle Srv(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={});
    DescriptorHandle UavBuffer(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={});
    DescriptorHandle TextureSrv(ID3D12Resource* r,DXGI_FORMAT format,DescriptorHandle handle={});
    DescriptorHandle TextureUav(ID3D12Resource* r,DXGI_FORMAT format,uint32_t mip=0,DescriptorHandle handle={});
    uint32_t MakeRTV(ID3D12Resource* r,DXGI_FORMAT format,uint32_t reuse=Invalid);
    uint32_t MakeDSV(ID3D12Resource* r,uint32_t reuse=Invalid);
    GpuCacheKey CacheKey(uint32_t kind,const void* data,size_t bytes,uint64_t generation=0);
    ComPtr<ID3D12Resource> UploadBuffer(const void* data,UINT64 bytes);
    ComPtr<ID3D12Resource> CachedUploadBuffer(const void* data,UINT64 bytes,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr);
    template<class T>ComPtr<ID3D12Resource> UploadVector(const std::vector<T>& v){return UploadBuffer(v.data(),v.size()*sizeof(T));}
    template<class T>ComPtr<ID3D12Resource> CachedUploadVector(const std::vector<T>& v,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr){if(v.empty())Fail("cacheable empty vector upload");return CachedUploadBuffer(v.data(),v.size()*sizeof(T),kind,generation,name);}
    uint32_t UploadTexture(const TextureData& t);
    std::shared_ptr<UploadGeneration> NewUploadGeneration(UINT64 chunk);
    void Bind(ID3D12GraphicsCommandList7* l,Frame& f,bool graphics);
    ComPtr<ID3D12PipelineState> Pipeline(const wchar_t* name,const D3D12_PIPELINE_STATE_STREAM_DESC& desc,UINT64 hash);
    UINT64 Hash(const std::vector<std::byte>& a,UINT64 seed=1469598103934665603ull);
    std::vector<std::byte> Shader(const wchar_t* name);
    void Pipelines();
    void Initialize(const XrGraphicsRequirementsD3D12KHR& req,bool gpuValidation);
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
    void EnsureInstances(Frame& f,uint32_t required);
    void EnsureVisibilityBuffers(Frame& f,uint32_t cpuCount,uint32_t publishedCount);
    void EnsureStreamControl(Frame& f,uint32_t slots);
    void ResetSceneResources();
    void ConfigureSampleTargets(bool fresh);
    void ApplyMultisampling();
    void Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra);
    void ReplaceScene(const SceneData& assets,RenderContributions extra);
    void UnloadScene();
    UINT64 Reserve(Frame& f,UINT64 size,UINT64 alignment=256);
    void PrepareLights(Frame& f,const RenderScene& snapshot) ;
    const RenderScene& Visuals(const FrameInput& input);
    bool Prepare(const FrameInput& input) ;
    bool PrepareResidency(const XRFrame& xr,Frame& f,bool coreSceneWork) ;


    void ClearCount(ID3D12GraphicsCommandList7* l,ID3D12Resource* r,DescriptorHandle descriptor);
    void ArgumentsReady(ID3D12GraphicsCommandList7* l,ID3D12Resource* args,ID3D12Resource* count);
    void Viewport(ID3D12GraphicsCommandList7* l,UINT w,UINT h,UINT x=0,UINT y=0);
    void ReserveStreams();
    void CancelReservedStreams();
    void Render(XRFrame& xr,const std::function<void()>& waitForStreams);
    ~Impl();
};
}
