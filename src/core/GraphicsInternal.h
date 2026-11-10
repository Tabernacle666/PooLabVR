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
enum FrameBinding:uint32_t {FrameInstances,FrameArguments,FrameCounter,FrameInstanceUAV,FrameCullArguments,FrameCullCounter,FrameShadowArguments,FrameShadowCounter,FrameBindingCount};
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

struct CompiledBarrier {
    D3D12_BARRIER_TYPE type{D3D12_BARRIER_TYPE_BUFFER};
    D3D12_BUFFER_BARRIER buffer{};
    D3D12_TEXTURE_BARRIER texture{};
};
inline void EmitBarriers(ID3D12GraphicsCommandList7* list,const std::vector<CompiledBarrier>& barriers){
    for(const auto& barrier:barriers){
        D3D12_BARRIER_GROUP group{};group.Type=barrier.type;group.NumBarriers=1;
        if(barrier.type==D3D12_BARRIER_TYPE_TEXTURE)group.pTextureBarriers=&barrier.texture;else group.pBufferBarriers=&barrier.buffer;
        list->Barrier(1,&group);
    }
}

struct GraphStamp {
    bool valid{};GraphQueue queue{};UINT64 fence{};std::array<UINT64,GraphQueueCount> frontier{};
};
struct GraphBarrierState {
    bool valid{},texture{};GraphQueue queue{};D3D12_BARRIER_SYNC sync{D3D12_BARRIER_SYNC_NONE};D3D12_BARRIER_ACCESS access{D3D12_BARRIER_ACCESS_NO_ACCESS};D3D12_BARRIER_LAYOUT layout{D3D12_BARRIER_LAYOUT_UNDEFINED};
};
struct GraphResourceState {
    ID3D12Resource* resource{};uint32_t subresource{UINT32_MAX};GraphStamp writer{};std::array<GraphStamp,GraphQueueCount> readers{};GraphBarrierState barrier{};GraphStamp pending{};GraphBarrierState pendingBarrier{};TextureResource* pendingTexture{};
};
inline GpuSubresourceShape TextureShape(ID3D12Resource* resource){
    auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE1D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE3D)Fail("Texture declaration requires a texture resource");
    ComPtr<ID3D12Device> device;Hr(resource->GetDevice(IID_PPV_ARGS(&device)),"Texture format device");D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format,sizeof(format)),"Texture plane count");
    return {desc.MipLevels,desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:uint32_t(desc.DepthOrArraySize),format.PlaneCount};
}
inline D3D12_BARRIER_SUBRESOURCE_RANGE SingleSubresource(uint32_t index){D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=index;return range;}
struct GraphExternalFence {ID3D12Fence* fence{};UINT64 value{};};
struct GraphPass {
    const char* name{};GraphQueue queue{};Commands* commands{};UINT64 signal{};std::array<UINT64,GraphQueueCount> required{},coverFence{};std::array<std::array<UINT64,GraphQueueCount>,GraphQueueCount> cover{};bool sealed{};
    std::vector<GraphExternalFence> externalWaits,externalSignals;
    std::array<UINT64,GraphQueueCount> queueWaits{};
};

class PassGraph {
    std::array<QueueState*,GraphQueueCount> queues_{};
    std::array<UINT64,GraphQueueCount> planned_{},completed_{};
    std::array<std::array<UINT64,GraphQueueCount>,GraphQueueCount> queueFrontier_{};
    std::vector<GraphResourceState> resources_;
    GraphResourceState& State(ID3D12Resource* resource,uint32_t subresource=UINT32_MAX){
        for(auto& state:resources_)if(state.resource==resource&&state.subresource==subresource)return state;
        resources_.push_back({});auto& state=resources_.back();state.resource=resource;state.subresource=subresource;return state;
    }
    static bool Self(const GraphStamp& stamp,const GraphPass& pass){return stamp.valid&&stamp.queue==pass.queue&&stamp.fence==pass.signal;}
    void Merge(GraphPass& pass,const GraphStamp& stamp){
        if(!stamp.valid||Self(stamp,pass)||stamp.fence<=completed_[QI(stamp.queue)])return;for(uint32_t i=0;i<GraphQueueCount;i++)pass.required[i]=std::max(pass.required[i],stamp.frontier[i]);uint32_t producer=QI(stamp.queue);if(stamp.fence>pass.coverFence[producer]){pass.coverFence[producer]=stamp.fence;pass.cover[producer]=stamp.frontier;}
    }
    GraphStamp PendingStamp(const GraphPass& pass)const{GraphStamp stamp{};stamp.valid=true;stamp.queue=pass.queue;stamp.fence=pass.signal;return stamp;}
    void AutoBufferBarrier(GraphPass& pass,GraphResourceState& state,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers){
        // A compiled batch is one ExecuteCommandLists scope. Queue/fence dependencies
        // establish completion of prior batches; accesses from earlier logical passes in
        // this batch retain their real sync/access state and use normal GPU barriers.
        if(sync==D3D12_BARRIER_SYNC_NONE&&access==D3D12_BARRIER_ACCESS_NO_ACCESS)return;
        const bool local=Self(state.pending,pass);
        CompiledBarrier compiled{};auto& b=compiled.buffer;b.pResource=state.resource;b.Offset=0;b.Size=UINT64_MAX;
        b.SyncBefore=local?state.pendingBarrier.sync:D3D12_BARRIER_SYNC_NONE;b.SyncAfter=sync;
        b.AccessBefore=local?state.pendingBarrier.access:D3D12_BARRIER_ACCESS_NO_ACCESS;b.AccessAfter=access;barriers.push_back(compiled);
    }
    void CompileTextureBarrier(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,const D3D12_BARRIER_SUBRESOURCE_RANGE* range=nullptr){
        CompiledBarrier compiled{};compiled.type=D3D12_BARRIER_TYPE_TEXTURE;auto& b=compiled.texture;b.SyncBefore=beforeSync;b.SyncAfter=afterSync;b.AccessBefore=beforeAccess;b.AccessAfter=afterAccess;b.LayoutBefore=beforeLayout;b.LayoutAfter=afterLayout;b.pResource=texture.resource.Get();if(range)b.Subresources=*range;else{b.Subresources.IndexOrFirstMipLevel=0xffffffffu;b.Subresources.NumMipLevels=0;}barriers.push_back(compiled);
        LogEvent(traceFrame,range?"gpu.graph_texture_subresource_barrier":"gpu.graph_texture_barrier","pass=%s resource=%p layout=%u->%u sync=%llu->%llu access=%llu->%llu",pass.name,static_cast<void*>(texture.resource.Get()),unsigned(beforeLayout),unsigned(afterLayout),static_cast<unsigned long long>(beforeSync),static_cast<unsigned long long>(afterSync),static_cast<unsigned long long>(beforeAccess),static_cast<unsigned long long>(afterAccess));
    }
    void RecordTextureState(GraphPass& pass,GraphResourceState& state,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){
        GraphBarrierState actual{};actual.valid=true;actual.texture=true;actual.queue=pass.queue;actual.sync=sync;actual.access=access;actual.layout=layout;Pending(pass,state,actual,&texture);
    }
    void Hazard(GraphPass& pass,GraphResourceState& state,GraphMode mode){
        if(mode==GraphMode::Read){Merge(pass,state.writer);state.readers[QI(pass.queue)]=PendingStamp(pass);return;}
        Merge(pass,state.writer);for(const auto& reader:state.readers)Merge(pass,reader);state.readers={};state.writer=PendingStamp(pass);
    }
    void Pending(GraphPass& pass,GraphResourceState& state,const GraphBarrierState& finalState,TextureResource* texture){state.pending=PendingStamp(pass);state.pendingBarrier=finalState;state.pendingTexture=texture;}
public:
    void Initialize(QueueState& copy,QueueState& compute,QueueState& direct){queues_={&copy,&compute,&direct};}
    void BeginFrame(){for(uint32_t i=0;i<GraphQueueCount;i++){planned_[i]=queues_[i]->serial;completed_[i]=queues_[i]->fence->GetCompletedValue();queueFrontier_[i][i]=std::max(queueFrontier_[i][i],queues_[i]->serial);}}
    void Forget(ID3D12Resource* resource){if(!resource)return;std::erase_if(resources_,[resource](const auto& state){return state.resource==resource;});}
    GraphPass BeginPass(const char* name,GraphQueue queue){GraphPass pass{};pass.name=name;pass.queue=queue;pass.signal=++planned_[QI(queue)];pass.required=queueFrontier_[QI(queue)];LogEvent(traceFrame,"gpu.graph_pass","name=%s queue=%s signal=%llu",name,queues_[QI(queue)]->name,static_cast<unsigned long long>(pass.signal));return pass;}
    void Buffer(GraphPass& pass,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,std::vector<CompiledBarrier>& barriers){
        if(!resource)return;auto& state=State(resource);Hazard(pass,state,mode);AutoBufferBarrier(pass,state,firstSync,firstAccess,barriers);GraphBarrierState final{};final.valid=true;final.queue=pass.queue;final.sync=finalSync;final.access=finalAccess;Pending(pass,state,final,nullptr);
    }
    void TextureRange(GraphPass& pass,ID3D12Resource* resource,TextureResource* canonical,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers,GpuTextureSubresources requested={}){
        if(!resource)return;auto shape=TextureShape(resource);GpuTextureSubresources range;if(!GpuNormalizeSubresources(shape,requested,range))Fail("Invalid domain texture subresource range");
        GpuForEachSubresource(shape,range,[&](uint32_t index,uint32_t,uint32_t,uint32_t){
            auto& state=State(resource,index);bool local=Self(state.pending,pass);
            auto prior=local?state.pendingBarrier:state.barrier;auto before=prior.valid?prior.layout:initialLayout;
            auto hazard=mode==GraphMode::Read&&before!=layout?GraphMode::ReadWrite:mode;Hazard(pass,state,hazard);
            D3D12_TEXTURE_BARRIER bar{};bar.pResource=resource;bar.Subresources=SingleSubresource(index);bar.LayoutBefore=before;bar.LayoutAfter=layout;
            bar.SyncBefore=local?prior.sync:D3D12_BARRIER_SYNC_NONE;bar.AccessBefore=local?prior.access:D3D12_BARRIER_ACCESS_NO_ACCESS;bar.SyncAfter=sync;bar.AccessAfter=access;
            CompiledBarrier compiled{};compiled.type=D3D12_BARRIER_TYPE_TEXTURE;compiled.texture=bar;barriers.push_back(compiled);
            GraphBarrierState final{};final.valid=final.texture=true;final.queue=pass.queue;final.sync=sync;final.access=access;final.layout=layout;Pending(pass,state,final,canonical);
            LogEvent(traceFrame,"gpu.graph_texture_subresource_barrier","pass=%s resource=%p subresource=%u layout=%u->%u",pass.name,resource,index,unsigned(before),unsigned(layout));
        });
        if(canonical&&range.firstMip==0&&range.mipCount==shape.mips&&range.firstArraySlice==0&&range.arraySliceCount==shape.arraySlices&&range.firstPlane==0&&range.planeCount==shape.planes)canonical->layout=layout;
    }
    void Dependency(GraphPass& pass,ID3D12Resource* resource,GraphMode mode){
        if(!resource)return;if(resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER){Hazard(pass,State(resource),mode);return;}
        auto shape=TextureShape(resource);GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){Hazard(pass,State(resource,index),mode);});
    }
    void Dependency(GraphPass& pass,const GraphStamp& producer){Merge(pass,producer);}
    void ExternalWait(GraphPass& pass,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;for(auto& wait:pass.externalWaits)if(wait.fence==fence){wait.value=std::max(wait.value,value);return;}pass.externalWaits.push_back({fence,value});}
    void ExternalSignal(GraphPass& pass,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;for(auto& signal:pass.externalSignals)if(signal.fence==fence){signal.value=std::max(signal.value,value);return;}pass.externalSignals.push_back({fence,value});}
    void TransitionTexture(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers){
        auto shape=TextureShape(texture.resource.Get());GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);
        GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingTexture!=&texture)Fail("Texture transition outside declared graph pass");auto prior=state.pendingBarrier;auto range=SingleSubresource(index);CompileTextureBarrier(pass,texture,prior.layout,prior.sync,prior.access,layout,sync,access,barriers,&range);RecordTextureState(pass,state,texture,layout,sync,access);});
        texture.layout=layout;
    }
    void TransitionTextureSubresource(GraphPass& pass,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& requested,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,bool completesWholeTexture=false){
        auto shape=TextureShape(texture.resource.Get());GpuTextureSubresources range;
        if(!requested.NumMipLevels){if(requested.IndexOrFirstMipLevel==UINT32_MAX){GpuNormalizeSubresources(shape,{},range);}else{uint32_t index=requested.IndexOrFirstMipLevel;uint32_t count=shape.mips*shape.arraySlices;if(index>=count*shape.planes)Fail("Texture subresource index");range={index%shape.mips,1,(index/shape.mips)%shape.arraySlices,1,index/count,1};}}
        else if(!GpuNormalizeSubresources(shape,{requested.IndexOrFirstMipLevel,requested.NumMipLevels,requested.FirstArraySlice,requested.NumArraySlices,requested.FirstPlane,requested.NumPlanes},range))Fail("Texture subresource transition range");
        GpuForEachSubresource(shape,range,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingTexture!=&texture)Fail("Texture subresource transition outside declared graph pass");if(state.pendingBarrier.layout!=beforeLayout)Fail("Texture subresource before-layout mismatch");auto native=SingleSubresource(index);CompileTextureBarrier(pass,texture,beforeLayout,beforeSync,beforeAccess,afterLayout,afterSync,afterAccess,barriers,&native);RecordTextureState(pass,state,texture,afterLayout,afterSync,afterAccess);});
        if(completesWholeTexture){GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingBarrier.layout!=afterLayout)Fail("Whole-texture completion with mixed mip layouts");});texture.layout=afterLayout;}
    }
    void Seal(GraphPass& pass){
        if(pass.sealed)return;pass.sealed=true;auto frontier=pass.required;frontier[QI(pass.queue)]=pass.signal;for(auto& state:resources_){
            auto finish=[&](GraphStamp& stamp){if(Self(stamp,pass))stamp.frontier=frontier;};finish(state.writer);for(auto& reader:state.readers)finish(reader);
            if(Self(state.pending,pass)){state.barrier=state.pendingBarrier;state.pending={};state.pendingTexture=nullptr;}
        }
    }
    UINT64 FenceValue(GraphQueue queue)const{return queues_[QI(queue)]->serial;}
    bool CrossQueueWait(const GraphPass& pass)const{for(auto value:pass.queueWaits)if(value)return true;return !pass.externalWaits.empty();}
    void CompileSubmission(GraphPass& pass){
        Seal(pass);std::array<bool,GraphQueueCount> wait{};for(uint32_t producer=0;producer<GraphQueueCount;producer++)wait[producer]=producer!=QI(pass.queue)&&pass.required[producer]>queueFrontier_[QI(pass.queue)][producer];for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(wait[producer])for(uint32_t covering=0;covering<GraphQueueCount;covering++)if(covering!=producer&&wait[covering]&&pass.coverFence[covering]>=pass.required[covering]&&pass.cover[covering][producer]>=pass.required[producer]){wait[producer]=false;break;}
        for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(wait[producer])pass.queueWaits[producer]=pass.required[producer];
        auto frontier=pass.required;frontier[QI(pass.queue)]=pass.signal;for(uint32_t i=0;i<GraphQueueCount;i++)queueFrontier_[QI(pass.queue)][i]=std::max(queueFrontier_[QI(pass.queue)][i],frontier[i]);
    }
    UINT64 Submit(const GraphPass& pass,bool signalRequired){
        if(!pass.sealed||!pass.commands)Fail("Uncompiled batch submission");auto& queue=*queues_[QI(pass.queue)];
        for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(pass.queueWaits[producer]){UINT64 needed=pass.queueWaits[producer];queue.Wait(*queues_[producer],needed);LogEvent(traceFrame,"gpu.graph_dependency","pass=%s consumer=%s producer=%s fence=%llu",pass.name,queue.name,queues_[producer]->name,static_cast<unsigned long long>(needed));}
        for(const auto& waitFence:pass.externalWaits){Hr(queue.queue->Wait(waitFence.fence,waitFence.value),"external GPU producer wait");LogEvent(traceFrame,"gpu.graph_external_wait","pass=%s queue=%s fence=%p value=%llu",pass.name,queue.name,static_cast<void*>(waitFence.fence),static_cast<unsigned long long>(waitFence.value));}
        pass.commands->Execute(queue);const UINT64 actual=pass.signal;
        if(signalRequired){if(actual<=queue.serial)Fail("Batch fence prediction");Hr(queue.queue->Signal(queue.fence.Get(),actual),"batch queue signal");queue.serial=actual;LogEvent(traceFrame,"gpu.signal","queue=%s fence=%llu",queue.name,static_cast<unsigned long long>(actual));}
        for(const auto& signalFence:pass.externalSignals){Hr(queue.queue->Signal(signalFence.fence,signalFence.value),"external GPU consumer signal");LogEvent(traceFrame,"gpu.graph_external_signal","pass=%s queue=%s fence=%p value=%llu",pass.name,queue.name,static_cast<void*>(signalFence.fence),static_cast<unsigned long long>(signalFence.value));}return actual;
    }

};

// Declaration layer above PassGraph. Every core/domain pass and every resource access
// is registered first. Reads bind the current logical version when declared; explicit
// versions support out-of-order publication consumers. After Resolve(), PassGraph
// compiles GPU state sequentially; recording consumes immutable barriers.
struct CompiledNode;
struct FrameGraphTransition {
    TextureResource* texture{};
    D3D12_BARRIER_LAYOUT layout{};
    D3D12_BARRIER_SYNC sync{};
    D3D12_BARRIER_ACCESS access{};
    bool during{},subresource{},completesWholeTexture{};
    D3D12_BARRIER_SUBRESOURCE_RANGE range{};
    D3D12_BARRIER_LAYOUT beforeLayout{};
    D3D12_BARRIER_SYNC beforeSync{};
    D3D12_BARRIER_ACCESS beforeAccess{};
};
struct FrameGraphUse {
    ID3D12Resource* resource{};TextureResource* canonicalTexture{};GraphMode mode{GraphMode::Read};bool texture{};
    D3D12_BARRIER_LAYOUT initialLayout{D3D12_BARRIER_LAYOUT_COMMON},layout{D3D12_BARRIER_LAYOUT_COMMON};
    D3D12_BARRIER_SYNC firstSync{D3D12_BARRIER_SYNC_NONE},finalSync{D3D12_BARRIER_SYNC_NONE};
    D3D12_BARRIER_ACCESS firstAccess{D3D12_BARRIER_ACCESS_NO_ACCESS},finalAccess{D3D12_BARRIER_ACCESS_NO_ACCESS};
    GpuTextureSubresources subresources{};
};
struct FrameGraphNode {
    std::string name;GraphQueue queue{GraphQueue::Direct};uint64_t stableKey{};UINT64 signal{};GraphStamp completion{};
    std::vector<FrameGraphUse> uses;std::vector<GraphExternalFence> waits,signals;std::vector<ID3D12Resource*> afterAllUsers;
    std::vector<FrameGraphTransition> transitions;
    std::function<void(const CompiledNode&,ID3D12GraphicsCommandList7*)> record;
};
struct BurstGpuProfiler {
    static constexpr uint32_t MaxFrames=512,MaxPassesPerFrame=256,QueriesPerPass=2;
    static constexpr uint32_t MaxQueries=MaxFrames*MaxPassesPerFrame*QueriesPerPass;
    struct QueueData {
        ComPtr<ID3D12QueryHeap> queries;
        ComPtr<ID3D12Resource> readback;
        uint64_t frequency{},calibrationGpu{},calibrationCpu{};
        uint32_t cursor{};
        bool supported{};
    };
    struct Record {
        uint64_t frameId{};
        GraphQueue queue{};
        uint32_t beginQuery{},endQuery{};
        uint32_t batchIndex{},nodeIndex{};
        bool crossQueueWait{};
        std::array<uint32_t,GraphQueueCount> batchesPerQueue{};
        std::array<char,96> name{};
    };
    struct Token {GraphQueue queue{};uint32_t begin{},end{},recordIndex{};bool valid{};};
    std::array<QueueData,GraphQueueCount> queues{};
    std::vector<Record> records;
    bool prepared{},active{};
    uint64_t qpcFrequency{},droppedPasses{};
    std::array<uint32_t,GraphQueueCount> batchesPerQueue{};
    uint32_t nextRecord{};

    void Prepare(ID3D12Device* device,QueueState&,QueueState&,QueueState&){
        if(prepared)return;
        records.reserve(MaxFrames*MaxPassesPerFrame);
        LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)Fail("Performance QPC frequency");
        qpcFrequency=uint64_t(frequency.QuadPart);
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
        Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options3,sizeof(options3)),"Performance timestamp support");
        queues[QI(GraphQueue::Copy)].supported=options3.CopyQueueTimestampQueriesSupported!=FALSE;
        queues[QI(GraphQueue::Compute)].supported=true;
        queues[QI(GraphQueue::Direct)].supported=true;
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];if(!q.supported)continue;
            D3D12_QUERY_HEAP_DESC queryDesc{};
            // Copy command lists require their dedicated timestamp heap type.
            queryDesc.Type=i==QI(GraphQueue::Copy)?D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queryDesc.Count=MaxQueries;
            Hr(device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&q.queries)),"Performance query heap");
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=uint64_t(MaxQueries)*sizeof(uint64_t);desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;
            desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&q.readback)),"Performance readback");
        }
        prepared=true;
    }
    void Begin(QueueState& copy,QueueState& compute,QueueState& direct){
        if(!prepared||active)Fail("Performance profiler not prepared or already active");
        records.clear();droppedPasses=0;
        std::array<QueueState*,GraphQueueCount> sources{&copy,&compute,&direct};
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];q.cursor=0;if(!q.supported)continue;
            Hr(sources[i]->queue->GetTimestampFrequency(&q.frequency),"Performance timestamp frequency");
            Hr(sources[i]->queue->GetClockCalibration(&q.calibrationGpu,&q.calibrationCpu),"Performance clock calibration");
            if(!q.frequency)Fail("Performance zero GPU frequency");
        }
        active=true;
    }
    void End(){active=false;}
    void PrepareFrame(uint32_t activePasses){nextRecord=uint32_t(records.size());records.resize(std::min<size_t>(records.size()+activePasses,MaxFrames*MaxPassesPerFrame));}
    Token ReservePass(GraphQueue queue){
        auto& q=queues[QI(queue)];if(!active||!q.supported)return {};
        // Never grow storage or address queries beyond the preallocated burst.
        if(q.cursor>MaxQueries-QueriesPerPass||nextRecord>=records.size()){++droppedPasses;return {};}
        uint32_t begin=q.cursor++,end=q.cursor++;
        return {queue,begin,end,nextRecord++,true};
    }
    void FinishReservations(){records.resize(nextRecord);}
    void BeginPass(Token token,uint64_t frameId,const char* name,ID3D12GraphicsCommandList7* commands,uint32_t batchIndex,uint32_t nodeIndex,bool crossQueueWait){
        if(!token.valid)return;auto& q=queues[QI(token.queue)];
        commands->EndQuery(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.begin);
        GraphQueue queue=token.queue;uint32_t begin=token.begin,end=token.end;
        Record r{};r.frameId=frameId;r.queue=queue;r.beginQuery=begin;r.endQuery=end;
        r.batchIndex=batchIndex;r.nodeIndex=nodeIndex;r.batchesPerQueue=batchesPerQueue;r.crossQueueWait=crossQueueWait;
        strncpy_s(r.name.data(),r.name.size(),name,_TRUNCATE);records[token.recordIndex]=r;
    }
    void EndPass(Token token,ID3D12GraphicsCommandList7* commands){
        if(!token.valid)return;auto& q=queues[QI(token.queue)];
        commands->EndQuery(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.end);
        commands->ResolveQueryData(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.begin,QueriesPerPass,q.readback.Get(),uint64_t(token.begin)*sizeof(uint64_t));
    }
    void Collect(std::vector<GpuPerformancePassSample>& out){
        out.clear();out.reserve(records.size());
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];if(!q.supported||!q.cursor)continue;
            D3D12_RANGE read{0,size_t(q.cursor)*sizeof(uint64_t)};void* mapped{};
            Hr(q.readback->Map(0,&read,&mapped),"Performance readback map");
            auto* timestamps=static_cast<const uint64_t*>(mapped);
            for(const auto& r:records)if(QI(r.queue)==i){
                const uint64_t begin=timestamps[r.beginQuery],end=timestamps[r.endQuery];
                const double scale=double(qpcFrequency)/double(q.frequency);
                const double startQpc=double(q.calibrationCpu)+(double(begin)-double(q.calibrationGpu))*scale;
                const double endQpc=double(q.calibrationCpu)+(double(end)-double(q.calibrationGpu))*scale;
                out.push_back({r.frameId,static_cast<PerformanceQueue>(i),r.name.data(),startQpc,endQpc,(double(end)-double(begin))*1000.0/double(q.frequency),r.batchIndex,r.nodeIndex,r.crossQueueWait,r.batchesPerQueue});
            }
            D3D12_RANGE written{0,0};q.readback->Unmap(0,&written);
        }
    }
};
struct CompiledNode {
    uint32_t node{};
    std::vector<CompiledBarrier> barriersBefore;
    std::vector<CompiledBarrier> barriersAfter;
    std::vector<std::vector<CompiledBarrier>> barriersDuring;
    BurstGpuProfiler::Token timestamp{};
};
class FrameGraphPlan {
    struct Compiled {
        std::vector<uint64_t> structure;
        std::vector<uint32_t> order;
        std::vector<uint8_t> edges;
        uint64_t lastUse{};
    };
    static constexpr size_t MaxCompiledPlans=16;
    PassGraph* executor_{};bool diagnostics_{};std::vector<FrameGraphNode> nodes_;FrameGraphDependencies dependencies_;std::array<UINT64,GraphQueueCount> lastFences_{};
    std::vector<Compiled> compiled_;std::vector<uint64_t> structureScratch_;uint64_t compileUse_{};bool executionCompiled_{};
    Compiled& Compile(){
        dependencies_.StructureKey(structureScratch_);
        ++compileUse_;for(auto& cached:compiled_)if(cached.structure==structureScratch_){if(!dependencies_.RestoreResolved(cached.order,cached.edges))Fail("Compiled frame graph restore");cached.lastUse=compileUse_;return cached;}
        Resolve();Compiled built;built.structure=structureScratch_;built.order=dependencies_.ResolvedOrder();built.edges=dependencies_.DependencyMatrix();built.lastUse=compileUse_;
        if(compiled_.size()<MaxCompiledPlans){compiled_.push_back(std::move(built));return compiled_.back();}
        auto victim=std::min_element(compiled_.begin(),compiled_.end(),[](const auto& a,const auto& b){return a.lastUse<b.lastUse;});*victim=std::move(built);return *victim;
    }
public:
    explicit FrameGraphPlan(PassGraph& executor):executor_(&executor){nodes_.reserve(32);compiled_.reserve(MaxCompiledPlans);structureScratch_.reserve(512);}
    void SetDiagnostics(bool enabled){diagnostics_=enabled;}
    void BeginFrame(){nodes_.clear();dependencies_.BeginFrame();executionBatches.clear();lastFences_={};executionCompiled_=false;}
    uint32_t AddPass(const char* name,GraphQueue queue,uint64_t stableKey,std::function<void(const CompiledNode&,ID3D12GraphicsCommandList7*)> record={}){
        if(!name||!*name)Fail("Frame graph pass name");for(const auto& node:nodes_)if(node.stableKey==stableKey&&node.name==name)Fail("Duplicate frame graph pass");nodes_.push_back({});auto& node=nodes_.back();node.name=name;node.queue=queue;node.stableKey=stableKey;node.record=std::move(record);node.uses.reserve(16);auto index=dependencies_.AddPass(name,queue,stableKey);if(index!=nodes_.size()-1)Fail(dependencies_.ErrorMessage());return index;
    }
    FrameGraphNode& Node(uint32_t index){if(index>=nodes_.size())Fail("Frame graph node index");return nodes_[index];}
    void SetStableKey(uint32_t index,uint64_t key){Node(index).stableKey=key;if(!dependencies_.SetStableKey(index,key))Fail(dependencies_.ErrorMessage());}
    void Buffer(uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,uint32_t contentVersion=GpuContentVersion::Current){
        if(!resource)return;Node(node).uses.push_back({resource,nullptr,mode,false,D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON,firstSync,finalSync,firstAccess,finalAccess});
        if(!dependencies_.Buffer(node,resource,mode,contentVersion))Fail(dependencies_.ErrorMessage());
    }
    void Texture(uint32_t node,TextureResource* canonical,ID3D12Resource* resource,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,GpuTextureSubresources requested={},uint32_t contentVersion=GpuContentVersion::Current){
        if(!resource)return;auto shape=TextureShape(resource);GpuTextureSubresources normalized;if(!GpuNormalizeSubresources(shape,requested,normalized))Fail("Frame graph texture range");
        Node(node).uses.push_back({resource,canonical,mode,true,initialLayout,layout,sync,sync,access,access,normalized});
        if(!dependencies_.Texture(node,resource,mode,shape,normalized,contentVersion))Fail(dependencies_.ErrorMessage());
    }
    void ExternalWait(uint32_t node,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;auto& waits=Node(node).waits;for(auto& wait:waits)if(wait.fence==fence){wait.value=std::max(wait.value,value);return;}waits.push_back({fence,value});}
    void TransitionTexture(uint32_t node,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool during=false){
        FrameGraphTransition transition{};transition.texture=&texture;transition.layout=layout;transition.sync=sync;transition.access=access;transition.during=during;Node(node).transitions.push_back(transition);
    }
    void TransitionTextureSubresource(uint32_t node,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& range,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool completesWholeTexture=false){
        FrameGraphTransition transition{};transition.texture=&texture;transition.layout=layout;transition.sync=sync;transition.access=access;transition.during=transition.subresource=true;transition.completesWholeTexture=completesWholeTexture;transition.range=range;transition.beforeLayout=beforeLayout;transition.beforeSync=beforeSync;transition.beforeAccess=beforeAccess;Node(node).transitions.push_back(transition);
    }
    void ExternalSignal(uint32_t node,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;auto& signals=Node(node).signals;for(auto& signal:signals)if(signal.fence==fence){signal.value=std::max(signal.value,value);return;}signals.push_back({fence,value});}
    void AfterAllUsers(uint32_t node,ID3D12Resource* resource){if(resource){Node(node).afterAllUsers.push_back(resource);if(!dependencies_.AfterAllUsers(node,resource))Fail(dependencies_.ErrorMessage());}}
    void Dependency(uint32_t from,uint32_t to){if(!dependencies_.Dependency(from,to))Fail(dependencies_.ErrorMessage());}
    void Resolve(){if(!dependencies_.Resolve())Fail(dependencies_.ErrorMessage());}
    struct ExecutionBatch {
        GraphQueue queue{};
        std::vector<uint32_t> nodes;
        std::vector<CompiledNode> compiledNodes;
        GraphPass pass{};
        struct Wait {GraphQueue sourceQueue{};uint64_t fenceValue{};};
        std::vector<Wait> waits;
        bool signalRequired{};
        uint64_t signalValue{};
    };
    struct BatchRecording {
        JobSystem::Group group;
        Commands* commands{};
        GraphPass pass{};
        std::atomic<bool> ready{};
        const ExecutionBatch* batch{};
        const std::vector<FrameGraphNode>* nodes{};
        BurstGpuProfiler* profiler{};
        uint64_t frameId{};
        uint32_t batchIndex{};
        bool crossQueueWait{};
    };
    std::vector<ExecutionBatch> executionBatches;

    void PublicationComplete(const GpuPublication& publication,uint32_t fallback){
        if(!publication.consumerFence||!publication.consumerValue)return;
        // Attach release to an existing last user, joining the other queues there.
        // No empty Direct command list is needed to return a publication slot.
        Compile();std::vector<uint32_t> users;users.reserve(nodes_.size());
        for(uint32_t node:dependencies_.ResolvedOrder()){
            bool uses=false;for(const auto& use:dependencies_.Uses())if(use.pass==node)
                for(const auto& resource:publication.resources)uses|=use.resource==resource.resource.Get();
            if(uses)users.push_back(node);
        }
        uint32_t completion=users.empty()?fallback:users.back();
        for(uint32_t user:users)if(user!=completion)Dependency(user,completion);
        ExternalWait(completion,publication.producerFence.Get(),publication.producerValue);
        ExternalSignal(completion,publication.consumerFence.Get(),publication.consumerValue);
    }
    void CompileExecutionBatches(){
        Compile();
        // Content reads may be independent while their physical layouts are not.
        // Order layout changes along the resolved DAG, including future-version users.
        const auto layoutOrder=dependencies_.ResolvedOrder();bool layoutEdges=false;
        auto overlap=[](uint32_t a,uint32_t countA,uint32_t b,uint32_t countB){return a<b+countB&&b<a+countA;};
        for(size_t i=0;i<layoutOrder.size();++i)for(size_t j=i+1;j<layoutOrder.size();++j){
            uint32_t previous=layoutOrder[i],next=layoutOrder[j];bool ordering=false;
            for(const auto& before:nodes_[previous].uses)if(before.texture&&before.mode==GraphMode::Read)
                for(const auto& after:nodes_[next].uses)if(after.texture&&after.mode==GraphMode::Read&&before.resource==after.resource&&before.layout!=after.layout&&
                    overlap(before.subresources.firstMip,before.subresources.mipCount,after.subresources.firstMip,after.subresources.mipCount)&&
                    overlap(before.subresources.firstArraySlice,before.subresources.arraySliceCount,after.subresources.firstArraySlice,after.subresources.arraySliceCount)&&
                    overlap(before.subresources.firstPlane,before.subresources.planeCount,after.subresources.firstPlane,after.subresources.planeCount))ordering=true;
            if(ordering){Dependency(previous,next);layoutEdges=true;}
        }
        if(layoutEdges)Compile();
        executionBatches.clear();executionBatches.reserve(nodes_.size());
        std::vector<uint32_t> nodeBatches(nodes_.size(),UINT32_MAX);
        std::array<uint64_t,GraphQueueCount> planned{};
        std::array<std::array<uint64_t,GraphQueueCount>,GraphQueueCount> covered{};
        std::array<std::vector<GraphExternalFence>,GraphQueueCount> externalCovered;
        std::array<uint32_t,GraphQueueCount> lastBatch;lastBatch.fill(UINT32_MAX);
        for(uint32_t q=0;q<GraphQueueCount;++q)planned[q]=executor_->FenceValue(GraphQueue(q));
        for(uint32_t index:dependencies_.ResolvedOrder()){
            const auto& node=nodes_[index];const uint32_t q=QI(node.queue);
            std::array<uint64_t,GraphQueueCount> needed{};
            for(uint32_t producer=0;producer<nodes_.size();++producer)if(nodes_[producer].queue!=node.queue&&HasDependency(producer,index)){
                uint32_t b=nodeBatches[producer];if(b==UINT32_MAX)Fail("Batch dependency order");
                const auto& source=executionBatches[b];needed[QI(source.queue)]=std::max(needed[QI(source.queue)],source.signalValue);
            }
            bool newWait=false;for(uint32_t source=0;source<GraphQueueCount;++source)newWait|=needed[source]>covered[q][source];
            for(const auto& wait:node.waits){uint64_t value=0;for(const auto& prior:externalCovered[q])if(prior.fence==wait.fence)value=prior.value;newWait|=wait.value>value;}
            bool split=executionBatches.empty()||executionBatches.back().queue!=node.queue||executionBatches.back().signalRequired||newWait;
            if(split){ExecutionBatch batch{};batch.queue=node.queue;batch.signalValue=++planned[q];batch.nodes.reserve(8);batch.waits.reserve(GraphQueueCount);executionBatches.push_back(std::move(batch));}
            auto& batch=executionBatches.back();const uint32_t batchIndex=uint32_t(executionBatches.size()-1);
            for(uint32_t source=0;source<GraphQueueCount;++source)if(needed[source]>covered[q][source]){
                batch.waits.push_back({GraphQueue(source),needed[source]});covered[q][source]=needed[source];
            }
            for(const auto& wait:node.waits){auto& prior=externalCovered[q];auto found=std::find_if(prior.begin(),prior.end(),[&](const auto& item){return item.fence==wait.fence;});if(found==prior.end())prior.push_back(wait);else found->value=std::max(found->value,wait.value);}
            batch.nodes.push_back(index);nodeBatches[index]=batchIndex;lastBatch[q]=batchIndex;
            batch.signalRequired=dependencies_.HasCrossQueueOutput(index)||!node.signals.empty();
        }
        // Each queue's last batch retires its frame-slot allocators and resources.
        for(uint32_t b:lastBatch)if(b!=UINT32_MAX)executionBatches[b].signalRequired=true;
        executor_->BeginFrame();
        for(auto& batch:executionBatches){
            if(batch.nodes.empty())Fail("Empty execution batch");
            auto pass=executor_->BeginPass(nodes_[batch.nodes.front()].name.c_str(),batch.queue);
            if(pass.signal!=batch.signalValue)Fail("Execution batch fence prediction");
            for(const auto& wait:batch.waits){GraphStamp source{true,wait.sourceQueue,wait.fenceValue,{}};source.frontier[QI(wait.sourceQueue)]=wait.fenceValue;executor_->Dependency(pass,source);}
            batch.compiledNodes.reserve(batch.nodes.size());
            for(uint32_t index:batch.nodes){
                const auto& node=nodes_[index];pass.name=node.name.c_str();CompiledNode compiled{};compiled.node=index;
                for(const auto& wait:node.waits)executor_->ExternalWait(pass,wait.fence,wait.value);
                for(uint32_t producer=0;producer<nodes_.size();++producer)if(HasDependency(producer,index)&&nodes_[producer].completion.valid)executor_->Dependency(pass,nodes_[producer].completion);
                for(const auto& use:node.uses){
                    if(use.texture)executor_->TextureRange(pass,use.resource,use.canonicalTexture,use.initialLayout,use.mode,use.layout,use.firstSync,use.firstAccess,compiled.barriersBefore,use.subresources);
                    else executor_->Buffer(pass,use.resource,use.mode,use.firstSync,use.firstAccess,use.finalSync,use.finalAccess,compiled.barriersBefore);
                }
                for(auto* resource:node.afterAllUsers)executor_->Dependency(pass,resource,GraphMode::ReadWrite);
                // Intra-node barriers retain their original command positions. Final
                // transitions are emitted after record(), inside that node's timestamps.
                for(bool during:{true,false})for(const auto& transition:node.transitions)if(transition.during==during){
                    if(during)compiled.barriersDuring.emplace_back();
                    auto& barriers=during?compiled.barriersDuring.back():compiled.barriersAfter;
                    if(transition.subresource)executor_->TransitionTextureSubresource(pass,*transition.texture,transition.range,transition.beforeLayout,transition.beforeSync,transition.beforeAccess,transition.layout,transition.sync,transition.access,barriers,transition.completesWholeTexture);
                    else executor_->TransitionTexture(pass,*transition.texture,transition.layout,transition.sync,transition.access,barriers);
                }
                for(const auto& signal:node.signals)executor_->ExternalSignal(pass,signal.fence,signal.value);
                batch.compiledNodes.push_back(std::move(compiled));
            }
            executor_->CompileSubmission(pass);batch.pass=std::move(pass);
            GraphStamp completion{true,batch.queue,batch.signalValue,batch.pass.required};completion.frontier[QI(batch.queue)]=batch.signalValue;
            for(uint32_t index:batch.nodes)nodes_[index].completion=completion;
        }
        executionCompiled_=true;
    }
    static void RecordExecutionBatch(void* context,uint32_t,uint32_t){
        auto& recording=*static_cast<BatchRecording*>(context);traceFrame=recording.frameId;
        auto& commands=*recording.commands;commands.Reset();auto* list=commands.list.Get();
        for(const auto& compiled:recording.batch->compiledNodes){
            const auto& node=(*recording.nodes)[compiled.node];
            if(recording.profiler)recording.profiler->BeginPass(compiled.timestamp,recording.frameId,node.name.c_str(),list,recording.batchIndex,compiled.node,recording.crossQueueWait);
            EmitBarriers(list,compiled.barriersBefore);
            if(node.record)node.record(compiled,list);
            EmitBarriers(list,compiled.barriersAfter);
            if(recording.profiler)recording.profiler->EndPass(compiled.timestamp,list);
        }
        commands.Close();recording.ready.store(true,std::memory_order_release);
    }
    void Execute(JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){ExecuteImpl(nullptr,jobs,frameId,commands,beforeSubmit);}
    void ExecuteProfiled(BurstGpuProfiler& profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){ExecuteImpl(&profiler,jobs,frameId,commands,beforeSubmit);}
    void ExecuteImpl(BurstGpuProfiler* profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){
        if(!executor_||!executionCompiled_)Fail("Frame graph batches not compiled");const uint32_t n=uint32_t(nodes_.size());
        if(profiler){
            profiler->batchesPerQueue={};uint32_t activePasses=0;
            for(const auto& batch:executionBatches){++profiler->batchesPerQueue[QI(batch.queue)];if(profiler->queues[QI(batch.queue)].supported)activePasses+=uint32_t(batch.compiledNodes.size());}
            profiler->PrepareFrame(activePasses);
            for(auto& batch:executionBatches)for(auto& compiled:batch.compiledNodes)compiled.timestamp=profiler->ReservePass(batch.queue);
            profiler->FinishReservations();
        }
        if(diagnostics_)for(uint32_t from=0;from<n;++from)for(uint32_t to=0;to<n;++to)if(HasDependency(from,to))LogEvent(traceFrame,"gpu.frame_graph_edge","from=%s to=%s",nodes_[from].name.c_str(),nodes_[to].name.c_str());
        if(diagnostics_)for(const auto& use:dependencies_.Uses())if(dependencies_.LatestVersion(use.resource,use.subresource)>1)
            LogEvent(traceFrame,"gpu.frame_graph_version","pass=%s resource=%p subresource=%u mode=%u input=%u output=%u",nodes_[use.pass].name.c_str(),const_cast<void*>(use.resource),use.subresource,unsigned(use.mode),use.inputVersion,use.outputVersion);
        // Allocate every context and command list before publishing any job. Each
        // frame slot owns a distinct allocator/list for every queue batch ordinal.
        auto recording=std::make_unique<BatchRecording[]>(executionBatches.size());
        std::array<uint32_t,GraphQueueCount> queueBatch{};
        for(uint32_t index=0;index<executionBatches.size();++index){
            const auto& batch=executionBatches[index];auto& item=recording[index];
            item.commands=&commands(batch.queue,queueBatch[QI(batch.queue)]++);item.pass=batch.pass;item.pass.commands=item.commands;
            item.batch=&batch;item.nodes=&nodes_;item.profiler=profiler;item.frameId=frameId;item.batchIndex=index;
            item.crossQueueWait=executor_->CrossQueueWait(item.pass);
        }
        for(uint32_t index=0;index<executionBatches.size();++index)
            jobs.Submit(RecordExecutionBatch,&recording[index],0,1,&recording[index].group);
        for(uint32_t index=0;index<executionBatches.size();++index){
            const auto& batch=executionBatches[index];auto& item=recording[index];
            jobs.Wait(item.group);
            if(!item.group.Complete()||!item.ready.load(std::memory_order_acquire))Fail("Batch recording incomplete");
            if(beforeSubmit)beforeSubmit(item.pass);
            UINT64 signal=executor_->Submit(item.pass,batch.signalRequired);if(batch.signalRequired)lastFences_[QI(batch.queue)]=signal;
            for(uint32_t nodeIndex:batch.nodes){auto& node=nodes_[nodeIndex];node.signal=signal;LogEvent(traceFrame,"gpu.frame_graph_execute","pass=%s queue=%u batch=%u node=%u signal=%llu",node.name.c_str(),unsigned(node.queue),index,nodeIndex,static_cast<unsigned long long>(signal));}
        }
    }
    const std::vector<uint32_t>& ResolvedOrder()const{return dependencies_.ResolvedOrder();}
    bool HasDependency(uint32_t from,uint32_t to)const{return dependencies_.HasDependency(from,to);}
    UINT64 Signal(uint32_t node)const{return node<nodes_.size()?nodes_[node].signal:0;}
    const std::array<UINT64,GraphQueueCount>& LastFences()const{return lastFences_;}
};
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
        uint32_t cpuCount{},dynamicCount{};DescriptorHandle rayUAV,blasAddressesSRV;ComPtr<ID3D12Resource> blasAddresses;ComPtr<ID3D12Resource> rayDescriptors;ComPtr<ID3D12Resource> instances,streamControl,arguments,count,shadowArguments,shadowCount,cullArguments,cullCount,clusters,tlas,scratch,dynamicVertices,dynamicScratch,hotBounds,hotMeshes;
        DescriptorHandle instanceSRV,instanceUAV,argumentsUAV,countUAV,shadowArgumentsUAV,shadowCountUAV,cullArgumentsUAV,cullCountUAV,clusterSRV,clusterUAV,tlasSRV,dynamicSRV,dynamicUAV,hotBoundsSRV,hotBoundsUAV,hotMeshesSRV,hotMeshesUAV;uint32_t instanceCount{},additionalCount{},lastTlasCount{},streamControlSlots{};bool tlasBuilt{};FrameWorkSet deformationWork,blasWork;std::vector<uint8_t> dynamicBlasBuilt;std::vector<uint64_t> dynamicGeometryRevision,dynamicBlasRevision,pendingDynamicRevision;
        ComPtr<ID3D12Resource> resourceDirectory;std::array<ComPtr<ID3D12Resource>,ResourceDomainCount> resourceTables;DescriptorHandle resourceDirectorySRV;std::array<DescriptorHandle,ResourceDomainCount> resourceTableSRV{};DomainDirectoryEntry* resourceDirectoryData{};std::array<DomainBindingEntry*,ResourceDomainCount> resourceTableData{};
    };static_assert(GpuSceneCache::FrameSlots==3);std::array<Frame,GpuSceneCache::FrameSlots> frames;Frame* active{};GpuFrameResources frameResources;uint64_t frameNumber{};bool historyReady{};std::array<XMFLOAT4X4,MaxViews> lastVP{};std::array<XMFLOAT4X4,ShadowSlots*6> shadowMatrices{};
    struct BoundPublication {GpuStream* stream{};GpuDomain* domain{};GpuPublication publication{};bool deferred{};};std::vector<BoundPublication> publications;GpuPublicationCatalog publicationCatalog;
    struct ReservedStream {GpuStream* stream{};GpuPublication publication;};
    std::vector<ReservedStream> reservedStreams;

    struct MeshSolidDomain final : GpuDomain {
        struct Binding {GpuPublication publication{};uint32_t base{},capacity{},controlSlot{};};
        Impl& owner;std::vector<Binding> bindings;uint64_t total{};
        explicit MeshSolidDomain(Impl& value):owner(value){bindings.reserve(16);}
        GpuWorldDomain Identity()const noexcept override{return GpuDomainId::MeshSolid;}
        const char* Name()const noexcept override{return "MeshSolid";}
        void BeginFrame(uint64_t)override{bindings.clear();total=owner.active?owner.active->cpuCount:0;}
        bool ValidatePublication(const GpuPublication& p)const override{
            if(!GpuDomain::ValidatePublication(p)||!p.consumerFence||!p.consumerValue)return false;
            auto* instances=p.Find(GpuMeshPublicationAbi::Instances);auto* metadata=p.Find(GpuMeshPublicationAbi::Metadata);auto* indirect=p.Find(GpuMeshPublicationAbi::IndirectArgs);
            for(const auto& resource:p.resources)if((resource.binding>=GpuMeshPublicationAbi::LightBase&&resource.binding<GpuMeshPublicationAbi::LightBase+MaxLights)){uint32_t index=resource.binding-GpuMeshPublicationAbi::LightBase;if(index>=owner.active->lightCount||resource.bytes!=sizeof(Light)||!(owner.active->lightData[index].parameters.z==5.f))return false;}
            return instances&&metadata&&indirect&&instances->type==GpuPublishedResourceType::Buffer&&metadata->type==GpuPublishedResourceType::Buffer&&indirect->type==GpuPublishedResourceType::Buffer&&instances->bytes%sizeof(GpuInstance)==0&&metadata->bytes==GpuMeshPublicationAbi::MetadataBytes&&indirect->bytes==GpuMeshPublicationAbi::IndirectBytes;
        }
        void BindPublication(const GpuPublication& p)override{
            auto* instances=p.Find(GpuMeshPublicationAbi::Instances);uint64_t capacity=instances->bytes/sizeof(GpuInstance);if(capacity>UINT32_MAX||total+capacity>=Invalid)Fail("MeshSolid publication address space exhausted");bindings.push_back({p,uint32_t(total),uint32_t(capacity),uint32_t(bindings.size())});total+=capacity;
        }
        uint32_t TotalInstances()const{if(total>=Invalid)Fail("MeshSolid instance total overflow");return uint32_t(total);}
        const std::vector<Binding>& Bindings()const{return bindings;}
        static void RecordCopies(void* context,ID3D12GraphicsCommandList7* list){
            auto& self=*static_cast<MeshSolidDomain*>(context);auto& f=*self.owner.active;
            for(const auto& bound:self.bindings){auto& p=bound.publication;auto* instances=p.Find(GpuMeshPublicationAbi::Instances);auto* metadata=p.Find(GpuMeshPublicationAbi::Metadata);auto* indirect=p.Find(GpuMeshPublicationAbi::IndirectArgs);list->CopyBufferRegion(f.instances.Get(),uint64_t(bound.base)*sizeof(GpuInstance),instances->resource.Get(),instances->offset,instances->bytes);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes;list->CopyBufferRegion(f.streamControl.Get(),controlBase,metadata->resource.Get(),metadata->offset,metadata->bytes);list->CopyBufferRegion(f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::MetadataBytes,indirect->resource.Get(),indirect->offset,indirect->bytes);for(const auto& r:p.resources)if((r.binding>=GpuMeshPublicationAbi::LightBase&&r.binding<GpuMeshPublicationAbi::LightBase+MaxLights))list->CopyBufferRegion(f.lights.Get(),uint64_t(r.binding-GpuMeshPublicationAbi::LightBase)*sizeof(Light),r.resource.Get(),r.offset,r.bytes);}
        }
        void BuildPasses(GpuPassBuilder& builder,const GpuFrameContext&)override{
            if(bindings.empty())return;auto& f=*owner.active;
            builder.BeginPass("MeshSolid.Publications",GpuPassQueue::Copy,{this,&RecordCopies});
            builder.Buffer(f.instances.Get(),GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            builder.Buffer(f.streamControl.Get(),GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            bool lightWrite=false;for(const auto& b:bindings)for(const auto& r:b.publication.resources)lightWrite|=(r.binding>=GpuMeshPublicationAbi::LightBase&&r.binding<GpuMeshPublicationAbi::LightBase+MaxLights);
            // Upload establishes the CPU light slots; this dependent copy replaces
            // the reserved dynamic slots with the same GPU pose used by meshes.
            if(lightWrite)builder.Buffer(f.lights.Get(),GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            std::vector<ID3D12Resource*> declared;
            for(const auto& b:bindings)for(const auto& r:b.publication.resources){
                if(std::find(declared.begin(),declared.end(),r.resource.Get())!=declared.end())continue;
                declared.push_back(r.resource.Get());builder.Buffer(r.resource.Get(),GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            }
            builder.EndPass();
        }
    };

    std::shared_ptr<TerminalDomain> terminalDomain;
    std::shared_ptr<AtmosphereDomain> atmosphereDomain;
    std::shared_ptr<OpticalDomain> opticalDomain;
    std::shared_ptr<SnowDomain> snowDomain;
    std::unique_ptr<MeshSolidDomain> meshDomain;std::vector<GpuRayPublicationState> rayPublicationScratch;std::vector<std::shared_ptr<GpuDomain>> externalDomains;std::vector<GpuDomain*> domainRegistry;bool domainServicesReady{};

    TextureResource* CanonicalTexture(ID3D12Resource* resource);
    struct DomainPassBuilder final : GpuPassBuilder {
        Impl& owner;uint32_t node{UINT32_MAX};GpuWorldDomain domainId{GpuDomainId::Invalid};bool open{};
        explicit DomainPassBuilder(Impl& o):owner(o){}
        void SetDomain(GpuWorldDomain id){if(open)Fail("GPU domain changed with pass open");domainId=id;}
        static GraphQueue Queue(GpuPassQueue q){return q==GpuPassQueue::Copy?GraphQueue::Copy:q==GpuPassQueue::Compute?GraphQueue::Compute:GraphQueue::Direct;}
        static GraphMode Mode(GpuPassResourceAccess a){return a==GpuPassResourceAccess::Read?GraphMode::Read:a==GpuPassResourceAccess::Write?GraphMode::Write:GraphMode::ReadWrite;}
        static uint32_t NameHash(const char* name){uint32_t h=2166136261u;for(;*name;name++){h^=uint8_t(*name);h*=16777619u;}return h;}

        void ProducerWaits(ID3D12Resource* resource){for(const auto& bound:owner.publications)for(const auto& published:bound.publication.resources)if(published.resource.Get()==resource){owner.frameGraph.ExternalWait(node,bound.publication.producerFence.Get(),bound.publication.producerValue);break;}}
        void BeginPass(const char* name,GpuPassQueue queue,GpuPassCallback cb={})override{if(open)Fail("GPU domain pass already open");if(!name||!*name||strlen(name)>=64)Fail("Invalid GPU domain pass name");auto q=Queue(queue);
            // EndPass raises the priority of compute that does not directly
            // wait on an external producer. Resource dependencies still hold
            // depth, lighting and other consumers until their inputs are ready.
            constexpr uint64_t AsyncComputeKey=55;
            uint64_t key=q==GraphQueue::Compute?AsyncComputeKey:
                (1ull<<63)|(uint64_t(domainId)<<32)|NameHash(name);
            node=owner.frameGraph.AddPass(name,q,key,[cb](const CompiledNode&,ID3D12GraphicsCommandList7* list){if(cb)cb.record(cb.context,list);});open=true;}
        void Buffer(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,uint32_t contentVersion=GpuContentVersion::Current)override{if(!open)Fail("GPU domain buffer outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Buffer(node,resource,Mode(access),sync,barrierAccess,sync,barrierAccess,contentVersion);}
        void Texture(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_LAYOUT initialLayout,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,GpuTextureSubresources range={},uint32_t contentVersion=GpuContentVersion::Current)override{if(!open)Fail("GPU domain texture outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Texture(node,owner.CanonicalTexture(resource),resource,initialLayout,Mode(access),layout,sync,barrierAccess,range,contentVersion);}
        void EndPass()override{if(!open)Fail("GPU domain pass not open");auto& pass=owner.frameGraph.Node(node);if(pass.queue==GraphQueue::Compute)owner.frameGraph.SetStableKey(node,DomainComputePriority(!pass.waits.empty()));open=false;node=UINT32_MAX;}
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
