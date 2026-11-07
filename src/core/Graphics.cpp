#include "Graphics.h"
#include "TerminalDomain.h"
#include "VrCapture.h"
#include "AtmosphereDomain.h"
#include "OpticalDomain.h"
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
namespace {
thread_local uint64_t traceFrame{};
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
UINT64 Align(UINT64 v,UINT64 a){return (v+a-1)&~(a-1);}
D3D12_HEAP_PROPERTIES Heap(D3D12_HEAP_TYPE type){D3D12_HEAP_PROPERTIES p{};p.Type=type;p.CreationNodeMask=p.VisibleNodeMask=1;return p;}
D3D12_RESOURCE_DESC Buffer(UINT64 size,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=Align(size,4);d.Height=1;d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;return d;}
D3D12_RESOURCE_DESC Texture(UINT w,UINT h,DXGI_FORMAT format,UINT samples=1,UINT layers=1,UINT mips=1,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=UINT16(layers);d.MipLevels=UINT16(mips);d.Format=format;d.SampleDesc.Count=samples;d.Flags=flags;return d;}
bool SameResourceDesc(const D3D12_RESOURCE_DESC& a,const D3D12_RESOURCE_DESC& b){return a.Dimension==b.Dimension&&a.Alignment==b.Alignment&&a.Width==b.Width&&a.Height==b.Height&&a.DepthOrArraySize==b.DepthOrArraySize&&a.MipLevels==b.MipLevels&&a.Format==b.Format&&a.SampleDesc.Count==b.SampleDesc.Count&&a.SampleDesc.Quality==b.SampleDesc.Quality&&a.Layout==b.Layout&&a.Flags==b.Flags;}
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
thread_local TextureDependencies textureDependencies;
void StartupTransition(
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
void BBarrier(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource,D3D12_BARRIER_SYNC before,D3D12_BARRIER_SYNC after,D3D12_BARRIER_ACCESS a,D3D12_BARRIER_ACCESS b){D3D12_BUFFER_BARRIER bar{};bar.SyncBefore=before;bar.SyncAfter=after;bar.AccessBefore=a;bar.AccessAfter=b;bar.pResource=resource;bar.Offset=0;bar.Size=UINT64_MAX;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_BUFFER;g.NumBarriers=1;g.pBufferBarriers=&bar;list->Barrier(1,&g);LogEvent(traceFrame,"gpu.buffer_barrier","resource=%p sync=%llu->%llu access=%llu->%llu",static_cast<void*>(resource),static_cast<unsigned long long>(before),static_cast<unsigned long long>(after),static_cast<unsigned long long>(a),static_cast<unsigned long long>(b));}
bool StaticUploadFormat(DXGI_FORMAT f){switch(f){
case DXGI_FORMAT_R8_UNORM:case DXGI_FORMAT_R8_SNORM:case DXGI_FORMAT_R8_UINT:case DXGI_FORMAT_R8_SINT:case DXGI_FORMAT_R8G8_UNORM:case DXGI_FORMAT_R8G8_SNORM:case DXGI_FORMAT_R8G8_UINT:case DXGI_FORMAT_R8G8_SINT:case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:case DXGI_FORMAT_R8G8B8A8_SNORM:case DXGI_FORMAT_R8G8B8A8_UINT:case DXGI_FORMAT_R8G8B8A8_SINT:
case DXGI_FORMAT_R16_UNORM:case DXGI_FORMAT_R16_FLOAT:case DXGI_FORMAT_R16G16B16A16_FLOAT:
case DXGI_FORMAT_BC1_UNORM:case DXGI_FORMAT_BC1_UNORM_SRGB:case DXGI_FORMAT_BC2_UNORM:case DXGI_FORMAT_BC2_UNORM_SRGB:
case DXGI_FORMAT_BC3_UNORM:case DXGI_FORMAT_BC3_UNORM_SRGB:case DXGI_FORMAT_BC4_UNORM:case DXGI_FORMAT_BC4_SNORM:
case DXGI_FORMAT_BC5_UNORM:case DXGI_FORMAT_BC5_SNORM:case DXGI_FORMAT_BC6H_UF16:case DXGI_FORMAT_BC6H_SF16:
case DXGI_FORMAT_BC7_UNORM:case DXGI_FORMAT_BC7_UNORM_SRGB:return true;default:return false;}}
void Uav(ID3D12GraphicsCommandList7* l){D3D12_GLOBAL_BARRIER b{};b.SyncBefore=D3D12_BARRIER_SYNC_ALL_SHADING;b.SyncAfter=D3D12_BARRIER_SYNC_ALL_SHADING;b.AccessBefore=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;b.AccessAfter=D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE;D3D12_BARRIER_GROUP g{};g.Type=D3D12_BARRIER_TYPE_GLOBAL;g.NumBarriers=1;g.pGlobalBarriers=&b;l->Barrier(1,&g);}
struct Surface {XMFLOAT4 colorRoughness,emissionMetallic;uint32_t albedo{Invalid},normal{Invalid},metalRough{Invalid},emissive{Invalid};XMFLOAT4 parameters{1,0,0,0};XMUINT4 extra{Invalid,0,0,0};XMFLOAT4 opacity{1,.5f,1,0};std::array<TextureMapping,5> mapping{};XMFLOAT4 procedural{};};
static_assert(sizeof(Surface)==272);
struct Light {XMFLOAT4 positionRadius,colorPower,directionOuter,parameters;};
struct RenderPassPlan {
    bool occlusion{},depthHierarchy{},clusteredLights{},reflections{},shadowAtlas{},rayQueries{},rayWork{};
};
bool SurfaceMayReflect(const Surface& s,uint32_t quality){
    if(s.extra.w)return false;
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
void EmitBarriers(ID3D12GraphicsCommandList7* list,const std::vector<CompiledBarrier>& barriers){
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
GpuSubresourceShape TextureShape(ID3D12Resource* resource){
    auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE1D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE3D)Fail("Texture declaration requires a texture resource");
    ComPtr<ID3D12Device> device;Hr(resource->GetDevice(IID_PPV_ARGS(&device)),"Texture format device");D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format,sizeof(format)),"Texture plane count");
    return {desc.MipLevels,desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:uint32_t(desc.DepthOrArraySize),format.PlaneCount};
}
D3D12_BARRIER_SUBRESOURCE_RANGE SingleSubresource(uint32_t index){D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=index;return range;}
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
    void Execute(JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands){ExecuteImpl(nullptr,jobs,frameId,commands);}
    void ExecuteProfiled(BurstGpuProfiler& profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands){ExecuteImpl(&profiler,jobs,frameId,commands);}
    void ExecuteImpl(BurstGpuProfiler* profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands){
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

    void FinishTextureReadbacks(){for(auto& slot:textureReadbacks)if(slot->recorded&&!slot->fenceValue){slot->fenceValue=direct.serial;slot->recorded=false;}}

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
    void ApplySurfaceEdits(){
        if(baseSurfaceData.empty())return;surfaceData=baseSurfaceData;
        for(const auto& edit:surfaceEdits){auto s=edit.settings;if(s.material>=surfaceData.size())continue;auto& d=surfaceData[s.material];d.colorRoughness={s.color.x,s.color.y,s.color.z,s.roughness};d.procedural={s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};}
        staticReflectionCandidates={};for(const auto& instance:sceneCache.StaticInstances())if(instance.material<surfaceData.size())for(uint32_t q=0;q<staticReflectionCandidates.size();q++)staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[instance.material],q);
        surfacesDirty=true;
    }
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
    void SwapSampleTargets(SampleTargets& t){
        std::swap(hdr,t.hdr);std::swap(reflections,t.reflections);std::swap(depth,t.depth);
        std::swap(hdrResolved,t.hdrResolved);std::swap(reflectionsResolved,t.reflectionsResolved);
        std::swap(depthSRV,t.depthSRV);std::swap(hdrResolvedSRV,t.hdrResolvedSRV);
        std::swap(hdrResolvedUAV,t.hdrResolvedUAV);std::swap(reflectionsResolvedSRV,t.reflectionsResolvedSRV);
    }
    std::vector<uint32_t> xrRTV,xrDSV;
    struct Frame {
        TransientArena transient;std::vector<ComPtr<ID3D12Resource>> dynamicBLAS;UINT64 dynamicScratchBytes{};uint32_t lightCount{},shadowDirty{};bool hasShadowedLights{},hasShadowAtlasLights{};DescriptorHandle lightsSRV;UINT64 lightsOffset{};ComPtr<ID3D12Resource> lights;std::array<Light,MaxLights> lightData{};std::array<SceneLight,ShadowSlots> shadowLights{};std::array<uint64_t,ShadowSlots> shadowKeys{};std::array<uint32_t,ShadowSlots> shadowIDs{};double simulationTime{};
        ComPtr<ID3D12Resource> motionBuffer,boneBuffer,weightBuffer,deformedBounds;DescriptorHandle motionSRV,boneSRV,weightSRV,deformedBoundsSRV,deformedBoundsUAV;UINT64 motionOffset{},boneOffset{},weightOffset{};std::vector<uint32_t> transparent;uint32_t transparentCount{},instanceCapacity{};
        uint64_t submittedFrame{},sourceInput{},sourceTick{};std::array<std::vector<std::unique_ptr<Commands>>,GraphQueueCount> batchCommands;std::array<UINT64,GraphQueueCount> queueFences{};UINT64 fenceValue{},base{},bump{};std::shared_ptr<UploadGeneration> uploadGeneration;Constants* constants{};GpuInstance* objects{};UINT64 constantsOffset{},objectsOffset{};
        uint32_t cpuCount{},dynamicCount{};DescriptorHandle rayUAV,blasAddressesSRV;ComPtr<ID3D12Resource> blasAddresses;ComPtr<ID3D12Resource> rayDescriptors;ComPtr<ID3D12Resource> instances,streamControl,arguments,count,shadowArguments,shadowCount,cullArguments,cullCount,clusters,tlas,scratch,dynamicVertices,dynamicScratch,hotBounds,hotMeshes;
        DescriptorHandle instanceSRV,instanceUAV,argumentsUAV,countUAV,shadowArgumentsUAV,shadowCountUAV,cullArgumentsUAV,cullCountUAV,clusterSRV,clusterUAV,tlasSRV,dynamicSRV,dynamicUAV,hotBoundsSRV,hotBoundsUAV,hotMeshesSRV,hotMeshesUAV;uint32_t instanceCount{},additionalCount{},lastTlasCount{},streamControlSlots{};bool tlasBuilt{};FrameWorkSet deformationWork,blasWork;std::vector<uint8_t> dynamicBlasBuilt;std::vector<uint64_t> dynamicGeometryRevision,dynamicBlasRevision,pendingDynamicRevision;
        ComPtr<ID3D12Resource> resourceDirectory;std::array<ComPtr<ID3D12Resource>,ResourceDomainCount> resourceTables;DescriptorHandle resourceDirectorySRV;std::array<DescriptorHandle,ResourceDomainCount> resourceTableSRV{};DomainDirectoryEntry* resourceDirectoryData{};std::array<DomainBindingEntry*,ResourceDomainCount> resourceTableData{};
    };static_assert(GpuSceneCache::FrameSlots==3);std::array<Frame,GpuSceneCache::FrameSlots> frames;Frame* active{};GpuFrameResources frameResources;uint64_t frameNumber{};bool historyReady{};std::array<XMFLOAT4X4,MaxViews> lastVP{};std::array<XMFLOAT4X4,ShadowSlots*6> shadowMatrices{};
    struct BoundPublication {GpuStream* stream{};GpuDomain* domain{};GpuPublication publication{};};std::vector<BoundPublication> publications;GpuPublicationCatalog publicationCatalog;

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
    std::unique_ptr<MeshSolidDomain> meshDomain;std::vector<GpuRayPublicationState> rayPublicationScratch;std::vector<std::shared_ptr<GpuDomain>> externalDomains;std::vector<GpuDomain*> domainRegistry;bool domainServicesReady{};

    TextureResource* CanonicalTexture(ID3D12Resource* resource){
        if(!resource)return nullptr;for(auto* texture:{&hdr,&reflections,&depth,&hdrResolved,&reflectionsResolved,&hierarchy,&shadow})if(texture->resource.Get()==resource)return texture;for(auto& texture:history)if(texture.resource.Get()==resource)return &texture;for(auto& texture:historyDepth)if(texture.resource.Get()==resource)return &texture;return nullptr;
    }
    struct DomainPassBuilder final : GpuPassBuilder {
        Impl& owner;uint32_t node{UINT32_MAX};GpuWorldDomain domainId{GpuDomainId::Invalid};bool open{};
        explicit DomainPassBuilder(Impl& o):owner(o){}
        void SetDomain(GpuWorldDomain id){if(open)Fail("GPU domain changed with pass open");domainId=id;}
        static GraphQueue Queue(GpuPassQueue q){return q==GpuPassQueue::Copy?GraphQueue::Copy:q==GpuPassQueue::Compute?GraphQueue::Compute:GraphQueue::Direct;}
        static GraphMode Mode(GpuPassResourceAccess a){return a==GpuPassResourceAccess::Read?GraphMode::Read:a==GpuPassResourceAccess::Write?GraphMode::Write:GraphMode::ReadWrite;}
        static uint32_t NameHash(const char* name){uint32_t h=2166136261u;for(;*name;name++){h^=uint8_t(*name);h*=16777619u;}return h;}

        void ProducerWaits(ID3D12Resource* resource){for(const auto& bound:owner.publications)for(const auto& published:bound.publication.resources)if(published.resource.Get()==resource){owner.frameGraph.ExternalWait(node,bound.publication.producerFence.Get(),bound.publication.producerValue);break;}}
        void BeginPass(const char* name,GpuPassQueue queue,GpuPassCallback cb={})override{if(open)Fail("GPU domain pass already open");if(!name||!*name||strlen(name)>=64)Fail("Invalid GPU domain pass name");auto q=Queue(queue);uint64_t key=(1ull<<63)|(uint64_t(domainId)<<32)|NameHash(name);node=owner.frameGraph.AddPass(name,q,key,[cb](const CompiledNode&,ID3D12GraphicsCommandList7* list){if(cb)cb.record(cb.context,list);});open=true;}
        void Buffer(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,uint32_t contentVersion=GpuContentVersion::Current)override{if(!open)Fail("GPU domain buffer outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Buffer(node,resource,Mode(access),sync,barrierAccess,sync,barrierAccess,contentVersion);}
        void Texture(ID3D12Resource* resource,GpuPassResourceAccess access,D3D12_BARRIER_LAYOUT initialLayout,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barrierAccess,GpuTextureSubresources range={},uint32_t contentVersion=GpuContentVersion::Current)override{if(!open)Fail("GPU domain texture outside pass");owner.managedMemory.Pin(resource);ProducerWaits(resource);owner.frameGraph.Texture(node,owner.CanonicalTexture(resource),resource,initialLayout,Mode(access),layout,sync,barrierAccess,range,contentVersion);}
        void EndPass()override{if(!open)Fail("GPU domain pass not open");open=false;node=UINT32_MAX;}
    };
    struct CachedShadow {uint32_t id{Invalid};uint64_t key{};bool valid{};};std::array<CachedShadow,ShadowSlots> shadowCache;
    Impl():frameGraph(passGraph){meshDomain=std::make_unique<MeshSolidDomain>(*this);domainRegistry.push_back(meshDomain.get());atmosphereDomain=std::make_shared<AtmosphereDomain>();RegisterDomain(atmosphereDomain);opticalDomain=std::make_shared<OpticalDomain>();RegisterDomain(opticalDomain);publications.reserve(32);externalDomains.reserve(8);domainRegistry.reserve(9);}
    GpuDomain* FindDomain(GpuWorldDomain id)const{for(auto* domain:domainRegistry)if(domain->Identity()==id)return domain;return nullptr;}
    void RegisterDomain(std::shared_ptr<GpuDomain> domain){if(!domain||domain->Identity()==GpuDomainId::Invalid)Fail("Invalid GPU domain registration");if(FindDomain(domain->Identity()))Fail("Duplicate GPU domain registration");auto* raw=domain.get();externalDomains.push_back(std::move(domain));std::stable_sort(externalDomains.begin(),externalDomains.end(),[](const auto& a,const auto& b){return a->PresentationOrder()<b->PresentationOrder();});domainRegistry.push_back(raw);if(domainServicesReady)raw->Initialize(*this);LogEvent(0,"gpu.domain_registered","domain=%u name=%s",uint32_t(raw->Identity()),raw->Name());}
    bool UnregisterDomain(GpuWorldDomain id){
        if(id==GpuDomainId::Invalid||id==meshDomain->Identity()||active)return false;
        auto it=std::find_if(externalDomains.begin(),externalDomains.end(),[&](const std::shared_ptr<GpuDomain>& d){return d&&d->Identity()==id;});
        if(it==externalDomains.end())return false;auto* raw=it->get();
        if(std::find_if(streams.begin(),streams.end(),[&](GpuStream* stream){return stream&&stream->Domain()==id;})!=streams.end())return false;
        publications.clear();publicationCatalog.Clear();std::erase(domainRegistry,raw);externalDomains.erase(it);
        LogEvent(0,"gpu.domain_unregistered","domain=%u",uint32_t(id));return true;
    }
    void DetachStream(GpuStream& stream){if(active)Fail("GPU stream cannot detach while a frame is being prepared");WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);std::erase(streams,&stream);publications.clear();}
    double sceneTimeOrigin{};bool sceneTimeOriginValid{};XrTime lastDisplayTime{};uint64_t lastTrackingRevision{},rootHash{},abiHash{};
    struct CacheHeader {uint64_t magic{0x32314f535052564cull};uint32_t schema{2},vendor{},device{},subsystem{},revision{};uint32_t driverHigh{},driverLow{},reserved{};uint64_t root{},abi{};};CacheHeader cacheIdentity{};bool persistentCache{};
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index){auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*descriptorStride;return h;}
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index){auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(index)*descriptorStride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT index){auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*rtvStride;return h;}
    D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT index){auto h=dsvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*dsvStride;return h;}
    uint32_t Physical(DescriptorHandle handle)const{return descriptorAllocator.Resolve(handle);}
    uint32_t Generation(DescriptorHandle handle)const{return descriptorAllocator.Generation(handle);}
    ID3D12Device* Device()const noexcept override{return device.Get();}
    ID3D12RootSignature* BindlessRootSignature()const noexcept override{return root.Get();}
    ID3D12DescriptorHeap* BindlessResourceHeap()const noexcept override{return descriptors.Get();}
    ID3D12DescriptorHeap* BindlessSamplerHeap()const noexcept override{return samplers.Get();}
    GpuResourceHandle CreateBufferSrv(ID3D12Resource* resource,uint32_t count,uint32_t stride)override{return Srv(resource,count,stride);}
    GpuResourceHandle CreateBufferUav(ID3D12Resource* resource,uint32_t count,uint32_t stride)override{return UavBuffer(resource,count,stride);}
    GpuResourceHandle CreateTextureSrv(ID3D12Resource* resource,DXGI_FORMAT format)override{return TextureSrv(resource,format);}
    GpuResourceHandle CreateTextureUav(ID3D12Resource* resource,DXGI_FORMAT format,uint32_t mip=0)override{return TextureUav(resource,format,mip);}
    uint32_t DescriptorIndex(GpuResourceHandle handle)const override{return Physical(handle);}
    uint32_t DescriptorGeneration(GpuResourceHandle handle)const override{return Generation(handle);}
    GpuFenceValues SubmittedFences()const{return {copy.serial,compute.serial,direct.serial};}
    GpuFenceValues CompletedFences()const{return {copy.fence?copy.fence->GetCompletedValue():0,compute.fence?compute.fence->GetCompletedValue():0,direct.fence?direct.fence->GetCompletedValue():0};}
    void CollectManaged(){auto completed=CompletedFences();descriptorAllocator.Collect(completed);vramCache.Collect(completed);managedMemory.Collect(SubmittedFences(),completed);}
    GpuResourceLease CreateManagedBuffer(const GpuBufferDesc& d)override{CollectManaged();return managedMemory.Buffer(d);}
    GpuResourceLease CreateManagedTexture2D(const GpuTextureDesc& d)override{CollectManaged();return managedMemory.Texture(d,false);}
    GpuResourceLease CreateManagedTexture3D(const GpuTextureDesc& d)override{CollectManaged();return managedMemory.Texture(d,true);}
    GpuResourceLease UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name)override{
        if(bytes.empty()||!stride||bytes.size()%stride)return {};
        GpuBufferDesc desc{};desc.bytes=bytes.size();desc.stride=stride;desc.heap=D3D12_HEAP_TYPE_DEFAULT;desc.srv=true;desc.uav=false;desc.name=name;
        auto lease=CreateManagedBuffer(desc);if(!lease)return {};
        residency.Begin();managedMemory.Pin(lease->resource);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))return {};residency.Wait(direct.queue.Get());
        auto upload=Host(bytes.size(),D3D12_HEAP_TYPE_UPLOAD);std::byte* mapped{};Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"managed static buffer map");memcpy(mapped,bytes.data(),bytes.size());upload->Unmap(0,nullptr);
        startup.Reset();BBarrier(startup.list.Get(),lease->resource,D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_NO_ACCESS,D3D12_BARRIER_ACCESS_COPY_DEST);startup.list->CopyBufferRegion(lease->resource,0,upload.Get(),0,bytes.size());BBarrier(startup.list.Get(),lease->resource,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);return lease;
    }
    GpuResourceLease UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources)override{
        if(!StaticUploadFormat(d.format)||!d.width||!d.height||!d.depthOrArraySize||!d.mips)return {};
        const uint64_t expected=uint64_t(d.depthOrArraySize)*d.mips;if(subresources.size()!=expected||expected>UINT32_MAX)return {};
        auto lease=CreateManagedTexture2D(d);if(!lease)return {};auto native=lease->resource;auto nativeDesc=native->GetDesc();
        residency.Begin();managedMemory.Pin(native);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))return {};residency.Wait(direct.queue.Get());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(static_cast<size_t>(expected));std::vector<UINT> rows(static_cast<size_t>(expected));std::vector<UINT64> rowSizes(static_cast<size_t>(expected));UINT64 uploadBytes{};device->GetCopyableFootprints(&nativeDesc,0,UINT(expected),0,footprints.data(),rows.data(),rowSizes.data(),&uploadBytes);if(!uploadBytes)return {};
        auto upload=Host(uploadBytes,D3D12_HEAP_TYPE_UPLOAD);std::byte* mapped{};Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"managed static texture map");
        for(uint32_t i=0;i<uint32_t(expected);++i){const auto& src=subresources[i];if(!src.data||src.rowPitch<rowSizes[i]||src.slicePitch<src.rowPitch*rows[i]){upload->Unmap(0,nullptr);return {};}auto* source=static_cast<const std::byte*>(src.data);auto* target=mapped+footprints[i].Offset;for(uint32_t row=0;row<rows[i];++row)memcpy(target+uint64_t(row)*footprints[i].Footprint.RowPitch,source+uint64_t(row)*src.rowPitch,size_t(rowSizes[i]));}
        upload->Unmap(0,nullptr);startup.Reset();TextureResource tracked{native};StartupTransition(startup.list.Get(),tracked,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY);
        for(uint32_t i=0;i<uint32_t(expected);++i){D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=native;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=i;D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[i];startup.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}StartupTransition(startup.list.Get(),tracked,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING);
        startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);return lease;
    }
    GpuMemoryStats MemoryStats()const override{auto s=managedMemory.Stats();s.descriptorsLive=descriptorAllocator.LiveCount();s.descriptorsRetired=descriptorAllocator.RetiredCount();s.descriptorsRecycled=descriptorAllocator.RecycledCount();return s;}
    void ReleaseDescriptor(DescriptorHandle& handle)override{descriptorAllocator.Release(handle,SubmittedFences());}
    ComPtr<ID3D12Resource> Placed(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial=D3D12_RESOURCE_STATE_COMMON,const D3D12_CLEAR_VALUE* clear=nullptr,bool dedicated=false) {
        auto info=device->GetResourceAllocationInfo(0,1,&desc);if(info.SizeInBytes==UINT64_MAX)Fail("GPU allocation description");Page* page=nullptr;
        if(!dedicated)for(uint32_t i=0;i<pageCount;i++)if(!pages[i].dedicated&&Align(pages[i].used,info.Alignment)+info.SizeInBytes<=pages[i].size){page=&pages[i];break;}
        if(!page){DXGI_QUERY_VIDEO_MEMORY_INFO budget{};Hr(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&budget),"VRAM budget");UINT64 heapAlignment=dedicated?info.Alignment:4*MiB;UINT64 size=Align(dedicated?info.SizeInBytes:std::max(32*MiB,info.SizeInBytes),heapAlignment);auto completed=CompletedFences();auto headroom=managedMemory.Stats().headroom;pages.emplace_back();page=&pages[pageCount++];page->size=size;page->dedicated=dedicated;D3D12_HEAP_DESC h{};h.SizeInBytes=size;h.Alignment=heapAlignment;h.Properties=Heap(D3D12_HEAP_TYPE_DEFAULT);if(dedicated)h.Flags=D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT;else if(!residency.Reserve(size,completed[2],headroom)){vramCache.MakeRoom(size,completed,headroom);if(!residency.Reserve(size,completed[2],headroom))Fail("Resident GPU geometry exceeds available VRAM budget");}Hr(device->CreateHeap(&h,IID_PPV_ARGS(&page->heap)),"placed resource heap");page->residency=residency.Add(page->heap.Get(),size,dedicated?GpuResidencyClass::Required:GpuResidencyClass::Pinned,!dedicated);committed+=size;}
        auto offset=Align(page->used,info.Alignment);ComPtr<ID3D12Resource> r;Hr(device->CreatePlacedResource(page->heap.Get(),offset,&desc,initial,clear,IID_PPV_ARGS(&r)),"placed GPU resource");page->used=offset+info.SizeInBytes;return r;
    }
    ComPtr<ID3D12Resource> Temporary(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state) {
        auto info=device->GetResourceAllocationInfo(0,1,&desc);if(info.SizeInBytes==UINT64_MAX||!info.SizeInBytes)Fail("temporary GPU allocation description");auto completed=CompletedFences();auto headroom=managedMemory.Stats().headroom;if(!residency.Reserve(info.SizeInBytes,completed[2],headroom)){vramCache.MakeRoom(info.SizeInBytes,completed,headroom);if(!residency.Reserve(info.SizeInBytes,completed[2],headroom))Fail("Temporary GPU resource exceeds available VRAM budget");}auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> resource;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"temporary GPU resource");return resource;
    }
    ComPtr<ID3D12Resource> Host(UINT64 size,D3D12_HEAP_TYPE type){auto heap=Heap(type);auto desc=Buffer(size);ComPtr<ID3D12Resource> r;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"host buffer");return r;}
    void CommandsInit(Commands& c,D3D12_COMMAND_LIST_TYPE type){Hr(device->CreateCommandAllocator(type,IID_PPV_ARGS(&c.allocator)),"command allocator create");ComPtr<ID3D12GraphicsCommandList> base;Hr(device->CreateCommandList(0,type,c.allocator.Get(),nullptr,IID_PPV_ARGS(&base)),"command list create");Hr(base.As(&c.list),"enhanced command list");Hr(c.list->Close(),"command list close");}
    void QueueInit(QueueState& q,D3D12_COMMAND_LIST_TYPE type){q.name=type==D3D12_COMMAND_LIST_TYPE_DIRECT?"Direct":type==D3D12_COMMAND_LIST_TYPE_COMPUTE?"Compute":"Copy";D3D12_COMMAND_QUEUE_DESC d{};d.Type=type;Hr(device->CreateCommandQueue(&d,IID_PPV_ARGS(&q.queue)),"GPU queue");Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q.fence)),"queue fence");}
    void WaitStartup(QueueState& q){UINT64 v=q.Signal();HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)Fail("GPU event");Hr(q.fence->SetEventOnCompletion(v,event),"GPU completion");WaitForSingleObject(event,INFINITE);CloseHandle(event);}
    DescriptorHandle Srv(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={}){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=count;d.Buffer.StructureByteStride=stride;device->CreateShaderResourceView(r,&d,Cpu(index));return handle;}
    DescriptorHandle UavBuffer(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle={}){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=stride?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=count;d.Buffer.StructureByteStride=stride;d.Buffer.Flags=stride?D3D12_BUFFER_UAV_FLAG_NONE:D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(index));return handle;}
    DescriptorHandle TextureSrv(ID3D12Resource* r,DXGI_FORMAT format,DescriptorHandle handle={}){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);auto desc=r->GetDesc();D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;d.Texture3D.MipLevels=desc.MipLevels;}else if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else if(desc.DepthOrArraySize>1){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipLevels=desc.MipLevels;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=desc.MipLevels;}device->CreateShaderResourceView(r,&d,Cpu(index));return handle;}
    DescriptorHandle TextureUav(ID3D12Resource* r,DXGI_FORMAT format,uint32_t mip=0,DescriptorHandle handle={}){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);auto desc=r->GetDesc();D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;d.Texture3D.MipSlice=mip;d.Texture3D.WSize=UINT(-1);}else if(desc.DepthOrArraySize>1){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;d.Texture2DArray.MipSlice=mip;}else{d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=mip;}device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(index));return handle;}
    uint32_t MakeRTV(ID3D12Resource* r,DXGI_FORMAT format,uint32_t reuse=Invalid){uint32_t index=reuse==Invalid?rtvCount++:reuse;D3D12_RENDER_TARGET_VIEW_DESC d{};d.Format=format;auto desc=r->GetDesc();if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}device->CreateRenderTargetView(r,&d,Rtv(index));return index;}
    uint32_t MakeDSV(ID3D12Resource* r,uint32_t reuse=Invalid){uint32_t index=reuse==Invalid?dsvCount++:reuse;D3D12_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D32_FLOAT;auto desc=r->GetDesc();if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}device->CreateDepthStencilView(r,&d,Dsv(index));return index;}
    GpuCacheKey CacheKey(uint32_t kind,const void* data,size_t bytes,uint64_t generation=0){
        uint64_t content=HashMemory(data,bytes);if(!content)content=1;uint64_t revision=generation?generation:content;if(!revision)revision=1;return {content,revision,kind};
    }
    ComPtr<ID3D12Resource> UploadBuffer(const void* data,UINT64 bytes){auto r=Placed(Buffer(bytes));auto h=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);void* p;D3D12_RANGE empty{0,0};Hr(h->Map(0,&empty,&p),"asset upload map");memcpy(p,data,size_t(bytes));h->Unmap(0,nullptr);startup.list->CopyBufferRegion(r.Get(),0,h.Get(),0,bytes);BBarrier(startup.list.Get(),r.Get(),D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);uploads.push_back(h);return r;}
    ComPtr<ID3D12Resource> CachedUploadBuffer(const void* data,UINT64 bytes,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr){
        if(!bytes)Fail("cacheable zero-byte upload");auto key=CacheKey(kind,data,size_t(bytes),generation);auto cached=vramCache.Acquire(key,Buffer(bytes),D3D12_RESOURCE_STATE_COMMON,true,CompletedFences(),managedMemory.Stats().headroom,name);if(!cached)Fail("Cacheable GPU buffer allocation");if(cached.reused)return cached.resource;
        auto host=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);void* p;D3D12_RANGE empty{0,0};Hr(host->Map(0,&empty,&p),"cacheable asset upload map");memcpy(p,data,size_t(bytes));host->Unmap(0,nullptr);startup.list->CopyBufferRegion(cached.resource.Get(),0,host.Get(),0,bytes);BBarrier(startup.list.Get(),cached.resource.Get(),D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);uploads.push_back(host);vramCache.Commit(cached.resource.Get());return cached.resource;
    }
    template<class T>ComPtr<ID3D12Resource> UploadVector(const std::vector<T>& v){return UploadBuffer(v.data(),v.size()*sizeof(T));}
    template<class T>ComPtr<ID3D12Resource> CachedUploadVector(const std::vector<T>& v,uint32_t kind,uint64_t generation=0,const wchar_t* name=nullptr){if(v.empty())Fail("cacheable empty vector upload");return CachedUploadBuffer(v.data(),v.size()*sizeof(T),kind,generation,name);}
    uint32_t UploadTexture(const TextureData& t){
        auto format=t.format;auto desc=Texture(t.width,t.height,format,1,1,t.mips);uint64_t hash=1469598103934665603ull;hash=HashMemory(&t.width,sizeof(t.width),hash);hash=HashMemory(&t.height,sizeof(t.height),hash);hash=HashMemory(&t.mips,sizeof(t.mips),hash);hash=HashMemory(&format,sizeof(format),hash);if(!t.bytes.empty())hash=HashMemory(t.bytes.data(),t.bytes.size(),hash);if(!hash)hash=1;GpuCacheKey key{hash,hash,CacheTexture};auto cached=vramCache.Acquire(key,desc,D3D12_RESOURCE_STATE_COMMON,false,CompletedFences(),managedMemory.Stats().headroom,L"Cached immutable texture");if(!cached)Fail("Texture cache allocation");auto resource=cached.resource;uint32_t resident=cached.residency;
        if(!cached.reused){TextureResource view{resource};StartupTransition(startup.list.Get(),view,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY);residency.Begin();vramCache.RequireActive();residency.Require(resident);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))Fail("Texture upload exceeds available VRAM budget");residency.Wait(direct.queue.Get());std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(t.mips);std::vector<UINT> rows(t.mips);std::vector<UINT64> rowSizes(t.mips);UINT64 size;device->GetCopyableFootprints(&desc,0,t.mips,0,footprints.data(),rows.data(),rowSizes.data(),&size);auto host=Host(size,D3D12_HEAP_TYPE_UPLOAD);std::byte* ptr;Hr(host->Map(0,nullptr,reinterpret_cast<void**>(&ptr)),"texture upload");for(uint32_t mip=0;mip<t.mips;mip++){for(uint32_t row=0;row<rows[mip];row++)memcpy(ptr+footprints[mip].Offset+UINT64(row)*footprints[mip].Footprint.RowPitch,t.bytes.data()+t.offsets[mip]+UINT64(row)*rowSizes[mip],size_t(rowSizes[mip]));D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=mip;D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=host.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[mip];startup.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}host->Unmap(0,nullptr);StartupTransition(startup.list.Get(),view,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING);uploads.push_back(host);startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);uploads.clear();startup.Reset();vramCache.Commit(resource.Get());}
        auto descriptor=TextureSrv(resource.Get(),format);textures.push_back({resource,descriptor,resident});return Physical(descriptor);
    }
    std::shared_ptr<UploadGeneration> NewUploadGeneration(UINT64 chunk){
        auto generation=std::make_shared<UploadGeneration>();generation->chunk=chunk;generation->generation=uploadGenerationSerial.fetch_add(1,std::memory_order_acq_rel)+1;generation->resource=Host(3*chunk,D3D12_HEAP_TYPE_UPLOAD);D3D12_RANGE empty{0,0};Hr(generation->resource->Map(0,&empty,reinterpret_cast<void**>(&generation->mapped)),"upload generation map");return generation;
    }
    void Bind(ID3D12GraphicsCommandList7* l,Frame& f,bool graphics){if(!f.uploadGeneration)Fail("frame upload generation missing");ID3D12DescriptorHeap* heaps[]={descriptors.Get(),samplers.Get()};l->SetDescriptorHeaps(2,heaps);auto address=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;if(graphics){l->SetGraphicsRootSignature(root.Get());l->SetGraphicsRootConstantBufferView(0,address);}else{l->SetComputeRootSignature(root.Get());l->SetComputeRootConstantBufferView(0,address);}}
    ComPtr<ID3D12PipelineState> Pipeline(const wchar_t* name,const D3D12_PIPELINE_STATE_STREAM_DESC& desc,UINT64 hash){wchar_t key[160];swprintf_s(key,L"%s_%016llx_%u_v%u_s%u",name,hash,uint32_t(colorFormat),viewCount,MsaaSamples);ComPtr<ID3D12PipelineState> p;if(!library||FAILED(library->LoadPipeline(key,&desc,IID_PPV_ARGS(&p)))){Hr(device->CreatePipelineState(&desc,IID_PPV_ARGS(&p)),"pipeline state");if(library)library->StorePipeline(key,p.Get());}p->SetName(name);return p;}
    UINT64 Hash(const std::vector<std::byte>& a,UINT64 seed=1469598103934665603ull){for(auto b:a){seed^=std::to_integer<uint8_t>(b);seed*=1099511628211ull;}return seed;}
    std::vector<std::byte> Shader(const wchar_t* name){std::wstring path=L"shaders/";path+=name;path+=L".cso";return ReadBytes(AssetPath(path.c_str()));}
    void Pipelines(){
        auto ms=Shader(L"MeshMain"),ps=Shader(L"PixelMain"),shadowMS=Shader(L"ShadowMain"),vs=Shader(L"Fullscreen"),tone=Shader(L"Tonemap"),depthPixel=Shader(L"DepthPixel");std::array<D3D12_VIEW_INSTANCE_LOCATION,MaxViews> locations{};for(uint32_t view=0;view<viewCount;view++)locations[view]={0,view};D3D12_VIEW_INSTANCING_DESC views{viewCount,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};MeshPipeline p{};p.root.data=root.Get();p.ms.data={ms.data(),ms.size()};p.mask.data=UINT_MAX;p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;p.raster.data.MultisampleEnable=MsaaSamples>1;p.depth.data.DepthEnable=TRUE;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.depthFormat.data=DXGI_FORMAT_D32_FLOAT;p.samples.data={MsaaSamples,0};p.views.data=views;p.blend.data.IndependentBlendEnable=TRUE;for(auto& r:p.blend.data.RenderTarget){r.SrcBlend=D3D12_BLEND_ONE;r.DestBlend=D3D12_BLEND_ZERO;r.BlendOp=D3D12_BLEND_OP_ADD;r.SrcBlendAlpha=D3D12_BLEND_ONE;r.DestBlendAlpha=D3D12_BLEND_ZERO;r.BlendOpAlpha=D3D12_BLEND_OP_ADD;r.LogicOp=D3D12_LOGIC_OP_NOOP;r.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(p),&p};p.ps.data={depthPixel.data(),depthPixel.size()};depthPSO=Pipeline(L"depth",desc,Hash(depthPixel,Hash(ms)));p.ps.data={ps.data(),ps.size()};p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[0]=p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;scenePSO=Pipeline(L"scene",desc,Hash(ps,Hash(ms)));p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[1]=DXGI_FORMAT_UNKNOWN;sceneDirectPSO=Pipeline(L"scene_direct",desc,Hash(ps,Hash(ms)));for(auto& blend:p.blend.data.RenderTarget){blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;}transparentDirectPSO=Pipeline(L"transparent_direct",desc,Hash(ps,Hash(ms)));p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;transparentPSO=Pipeline(L"transparent",desc,Hash(ps,Hash(ms)));for(auto& blend:p.blend.data.RenderTarget)blend.BlendEnable=FALSE;p.ps.data={depthPixel.data(),depthPixel.size()};p.ms.data={shadowMS.data(),shadowMS.size()};p.formats.data={};p.samples.data.Count=1;p.views.data={};p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;shadowPSO=Pipeline(L"shadow",desc,Hash(depthPixel,Hash(shadowMS)));
        PresentPipeline f{};f.root.data=root.Get();f.vs.data={vs.data(),vs.size()};f.ps.data={tone.data(),tone.size()};f.blend.data=p.blend.data;f.mask.data=UINT_MAX;f.raster.data=p.raster.data;f.raster.data.MultisampleEnable=FALSE;f.depth.data=p.depth.data;f.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;f.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;f.formats.data.NumRenderTargets=1;f.formats.data.RTFormats[0]=colorFormat;f.depthFormat.data=DXGI_FORMAT_D32_FLOAT;f.samples.data={1,0};f.views.data=views;D3D12_PIPELINE_STATE_STREAM_DESC fd{sizeof(f),&f};presentPSO=Pipeline(L"present",fd,Hash(tone,Hash(vs)));
        auto cs=[&](const wchar_t* entry,ComPtr<ID3D12PipelineState>& out){auto bytes=Shader(entry);ComputePipeline s;s.root.data=root.Get();s.cs.data={bytes.data(),bytes.size()};D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof(s),&s};out=Pipeline(entry,d,Hash(bytes));};cs(L"CullMain",cullPSO);cs(L"DepthMain",depthResolvePSO);cs(L"HiZMain",hizPSO);cs(L"ClusterMain",clusterPSO);cs(L"DenoiseMain",denoisePSO);cs(L"MergeReflection",mergeReflectionPSO);cs(L"FxaaMain",fxaaPSO);cs(L"DeformMain",deformPSO);cs(L"DeformBoundsMain",deformBoundsPSO);cs(L"HotDataMain",hotDataPSO);cs(L"RayBuildMain",rayBuildPSO);
        if(library&&persistentCache){SIZE_T size=library->GetSerializedSize();std::vector<std::byte> data(size);if(SUCCEEDED(library->Serialize(data.data(),size))){wchar_t suffix[40];swprintf_s(suffix,L".%lu.tmp",GetCurrentProcessId());auto temporary=cachePath+suffix;FILE* file=nullptr;_wfopen_s(&file,temporary.c_str(),L"wb");if(file){bool ok=fwrite(&cacheIdentity,1,sizeof(cacheIdentity),file)==sizeof(cacheIdentity)&&fwrite(data.data(),1,size,file)==size;ok=fclose(file)==0&&ok;if(ok)MoveFileExW(temporary.c_str(),cachePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);else DeleteFileW(temporary.c_str());}}}

    }
    void Initialize(const XrGraphicsRequirementsD3D12KHR& req,bool gpuValidation){if(gpuValidation)validation.Enable();frameGraph.SetDiagnostics(gpuValidation);Hr(CreateDXGIFactory2(gpuValidation?DXGI_CREATE_FACTORY_DEBUG:0,IID_PPV_ARGS(&factory)),"DXGI factory");for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d;a->GetDesc1(&d);if(d.AdapterLuid.HighPart==req.adapterLuid.HighPart&&d.AdapterLuid.LowPart==req.adapterLuid.LowPart){Hr(a.As(&adapter),"DXGI VRAM adapter");break;}}if(!adapter)Fail("OpenXR GPU adapter");DXGI_ADAPTER_DESC1 adapterDesc{};Hr(adapter->GetDesc1(&adapterDesc),"DXGI adapter description");adapterDescription=adapterDesc;if(adapterDesc.DedicatedVideoMemory<ConsoleSpec::MinDedicatedVram)Fail("LaboVR console requires 15 GiB dedicated VRAM");Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_2,IID_PPV_ARGS(&device)),"D3D12 feature level 12_2");validation.Attach(device.Get());D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};Hr(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"Shader Model 6.6");if(sm.HighestShaderModel<D3D_SHADER_MODEL_6_6)Fail("Shader Model 6.6 required");D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&o3,sizeof(o3)),"view instancing");copyQueueTimestamps=o3.CopyQueueTimestampQueriesSupported!=FALSE;Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&o5,sizeof(o5)),"DXR");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,&o7,sizeof(o7)),"mesh shaders");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&o12,sizeof(o12)),"enhanced barriers");if(o3.ViewInstancingTier<D3D12_VIEW_INSTANCING_TIER_1||o5.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||o7.MeshShaderTier==D3D12_MESH_SHADER_TIER_NOT_SUPPORTED||!o12.EnhancedBarriersSupported)Fail("DXR 1.1, mesh shaders, enhanced barriers required");LogEvent(0,"gpu.capabilities","view_instancing=%u dxr=%u mesh=%u enhanced_barriers=%u shader_model=%u",unsigned(o3.ViewInstancingTier),unsigned(o5.RaytracingTier),unsigned(o7.MeshShaderTier),unsigned(o12.EnhancedBarriersSupported),unsigned(sm.HighestShaderModel));residency.Initialize(device.Get(),adapter.Get());vramCache.Initialize(device.Get(),residency);QueueInit(direct,D3D12_COMMAND_LIST_TYPE_DIRECT);QueueInit(compute,D3D12_COMMAND_LIST_TYPE_COMPUTE);QueueInit(copy,D3D12_COMMAND_LIST_TYPE_COPY);passGraph.Initialize(copy,compute,direct);CommandsInit(startup,D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible,ComPtr<ID3D12DescriptorHeap>& out){D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=type;h.NumDescriptors=count;h.Flags=visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE;Hr(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&out)),"descriptor heap");};heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,DescriptorCapacity,true,descriptors);heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,1024,true,samplers);heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,64,false,rtvs);heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,64,false,dsvs);descriptorStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);rtvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);dsvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);D3D12_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_ANISOTROPIC;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;s.MaxAnisotropy=16;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;auto sh=samplers->GetCPUDescriptorHandleForHeapStart();device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&s,sh);
        D3D12_ROOT_PARAMETER1 params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;params[0].Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={1,0,5};D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};rs.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;rs.Desc_1_1.NumParameters=2;rs.Desc_1_1.pParameters=params;rs.Desc_1_1.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED|D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;ComPtr<ID3DBlob> blob;Hr(D3D12SerializeVersionedRootSignature(&rs,&blob,nullptr),"Root Signature 1.1");Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root signature");rootHash=HashMemory(blob->GetBufferPointer(),blob->GetBufferSize());abiHash=HashMemory(GetCoreIdentity().pipelineCacheIdentity,strlen(GetCoreIdentity().pipelineCacheIdentity));D3D12_INDIRECT_ARGUMENT_DESC args[2]{};args[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;args[0].Constant.RootParameterIndex=1;args[0].Constant.Num32BitValuesToSet=1;args[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;D3D12_COMMAND_SIGNATURE_DESC cs{};cs.ByteStride=16;cs.NumArgumentDescs=2;cs.pArgumentDescs=args;Hr(device->CreateCommandSignature(&cs,root.Get(),IID_PPV_ARGS(&indirect)),"mesh indirect signature");D3D12_INDIRECT_ARGUMENT_DESC dispatchArg{};dispatchArg.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC dispatchDesc{};dispatchDesc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);dispatchDesc.NumArgumentDescs=1;dispatchDesc.pArgumentDescs=&dispatchArg;Hr(device->CreateCommandSignature(&dispatchDesc,nullptr,IID_PPV_ARGS(&dispatchIndirect)),"compute dispatch indirect signature");
        DXGI_ADAPTER_DESC desc{};Hr(adapter->GetDesc(&desc),"cache GPU identity");LARGE_INTEGER driver{};persistentCache=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));cacheIdentity.vendor=desc.VendorId;cacheIdentity.device=desc.DeviceId;cacheIdentity.subsystem=desc.SubSysId;cacheIdentity.revision=desc.Revision;cacheIdentity.driverHigh=uint32_t(driver.HighPart);cacheIdentity.driverLow=driver.LowPart;cacheIdentity.root=rootHash;cacheIdentity.abi=abiHash;LogEvent(0,"gpu.identity","vendor=%04x device=%04x revision=%u driver=%u.%u vram_bytes=%llu engine=%s cuda_expected=sm_%u pipeline_abi=%s",desc.VendorId,desc.DeviceId,desc.Revision,cacheIdentity.driverHigh,cacheIdentity.driverLow,static_cast<unsigned long long>(desc.DedicatedVideoMemory),GetCoreIdentity().version,LAB_CUDA_ARCHITECTURE,GetCoreIdentity().pipelineCacheIdentity);
        wchar_t folder[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,folder)))Fail("cache directory");std::wstring dir=folder;dir+=L"\\";dir+=GetCoreIdentity().cacheNamespaceW;dir+=L"_";dir+=GetCoreIdentity().versionW;CreateDirectoryW(dir.c_str(),nullptr);wchar_t name[80];swprintf_s(name,L"\\pipelines_%016llx.bin",HashMemory(&cacheIdentity,sizeof(cacheIdentity)));cachePath=dir+name;
        FILE* cache=nullptr;if(persistentCache)_wfopen_s(&cache,cachePath.c_str(),L"rb");if(cache){CacheHeader header{};bool valid=fread(&header,1,sizeof(header),cache)==sizeof(header)&&memcmp(&header,&cacheIdentity,sizeof(header))==0;if(valid){_fseeki64(cache,0,SEEK_END);auto n=_ftelli64(cache)-int64_t(sizeof(header));_fseeki64(cache,sizeof(header),SEEK_SET);if(n>0&&n<128*1024*1024){cacheBlob.resize(size_t(n));if(fread(cacheBlob.data(),1,cacheBlob.size(),cache)!=cacheBlob.size())cacheBlob.clear();}}fclose(cache);}
        if(FAILED(device->CreatePipelineLibrary(cacheBlob.empty()?nullptr:cacheBlob.data(),cacheBlob.size(),IID_PPV_ARGS(&library)))){cacheBlob.clear();device->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(&library));}
        std::atomic_store_explicit(&uploadGeneration,NewUploadGeneration(UploadChunk),std::memory_order_release);managedMemory.Initialize(device.Get(),residency,*this,desc.DedicatedVideoMemory,this,[](void* context,ID3D12Resource* resource){static_cast<Impl*>(context)->passGraph.Forget(resource);});CollectManaged();domainServicesReady=true;for(auto* domain:domainRegistry)domain->Initialize(*this);
    }
    uint64_t HashMemory(const void* data,size_t size,uint64_t hash=1469598103934665603ull){auto* bytes=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;i++){hash^=bytes[i];hash*=1099511628211ull;}return hash;}
    GpuInstance ConvertObject(const RenderObject& o,float alpha)const{
        if(o.mesh>=meshes.ranges.size()||o.material>=surfaceData.size())Fail("render asset handle");Pose p=Blend(o.previous,o.current,alpha);GpuInstance out{};auto bounds=meshes.ranges[o.mesh].bounds;float expansion=(o.flags&DeformedObject)?1+std::abs(meshes.deformations[o.mesh].axisAmplitude.w):1;
        out.positionRadius={p.p.x,p.p.y,p.p.z,bounds.w*std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)})*expansion};out.rotation={p.q.x,p.q.y,p.q.z,p.q.w};out.scale={o.scale.x,o.scale.y,o.scale.z,0};out.mesh=o.mesh;out.material=o.material;out.id=o.id;out.flags=o.flags;if(surfaceData[o.material].extra.y==2)out.flags|=DynamicObject|AlphaBlendObject;return out;
    }
    const GpuInstance& CpuInstance(const Frame& f,uint32_t index)const{uint32_t staticCount=sceneCache.StaticCount();if(index<staticCount)return sceneCache.StaticInstances()[index];index-=staticCount;if(index>=f.dynamicCount)Fail("CPU instance index");return f.objects[index];}
    GpuCacheKey StaticBlasKey(uint32_t mesh) {
        if(mesh>=meshes.ranges.size())Fail("BLAS cache mesh index");const auto& range=meshes.ranges[mesh];uint64_t hash=HashMemory(meshes.vertices.data(),meshes.vertices.size()*sizeof(Vertex));hash=HashMemory(&range.firstIndex,sizeof(range.firstIndex),hash);hash=HashMemory(&range.indexCount,sizeof(range.indexCount),hash);if(range.indexCount)hash=HashMemory(meshes.indices.data()+range.firstIndex,size_t(range.indexCount)*sizeof(uint32_t),hash);uint64_t generation=HashMemory(&abiHash,sizeof(abiHash),hash);if(!hash)hash=1;if(!generation)generation=1;return {hash,generation,CacheStaticBlas};
    }
    void BuildBLAS(){
        uint32_t meshCount=uint32_t(meshes.ranges.size());blas.resize(meshCount);if(!meshCount)return;std::vector<GpuCacheKey> keys(meshCount);std::vector<uint32_t> missing;missing.reserve(meshCount);
        for(uint32_t i=0;i<meshCount;i++){keys[i]=StaticBlasKey(i);auto hit=vramCache.AcquireExisting(keys[i],true);if(hit)blas[i]=hit.resource;else missing.push_back(i);}
        if(missing.empty())return;
        vramCache.RequireActive();if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))Fail("Static BLAS build exceeds available VRAM budget");residency.Wait(direct.queue.Get());
        startup.Reset();uint32_t missCount=uint32_t(missing.size());std::vector<ComPtr<ID3D12Resource>> scratch(missCount),uncompacted(missCount);auto sizes=Temporary(Buffer(UINT64(missCount)*8,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto readback=Host(UINT64(missCount)*8,D3D12_HEAP_TYPE_READBACK);
        for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];auto& range=meshes.ranges[i];D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.VertexBuffer.StartAddress=vertices->GetGPUVirtualAddress();geometry.Triangles.VertexBuffer.StrideInBytes=sizeof(Vertex);geometry.Triangles.VertexCount=uint32_t(meshes.vertices.size());geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(range.firstIndex)*4;geometry.Triangles.IndexCount=range.indexCount;geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;input.NumDescs=1;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=&geometry;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);uncompacted[ordinal]=Temporary(Buffer(info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);scratch[ordinal]=Temporary(Buffer(info.ScratchDataSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;build.DestAccelerationStructureData=uncompacted[ordinal]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch[ordinal]->GetGPUVirtualAddress();D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compact{};compact.DestBuffer=sizes->GetGPUVirtualAddress()+UINT64(ordinal)*8;compact.InfoType=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;if(ordinal)BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);startup.list->BuildRaytracingAccelerationStructure(&build,1,&compact);}
        BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_COPY_SOURCE);startup.list->CopyBufferRegion(readback.Get(),0,sizes.Get(),0,UINT64(missCount)*8);startup.Submit(direct);WaitStartup(direct);
        UINT64* compactedSizes=nullptr;D3D12_RANGE range{0,SIZE_T(missCount)*8};Hr(readback->Map(0,&range,reinterpret_cast<void**>(&compactedSizes)),"BLAS compact sizes");startup.Reset();for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];if(!compactedSizes[ordinal])Fail("BLAS compact size");auto cached=vramCache.Acquire(keys[i],Buffer(compactedSizes[ordinal],D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true,CompletedFences(),managedMemory.Stats().headroom,L"Cached static BLAS");if(!cached||cached.reused)Fail("Static BLAS cache race");blas[i]=cached.resource;startup.list->CopyRaytracingAccelerationStructure(blas[i]->GetGPUVirtualAddress(),uncompacted[ordinal]->GetGPUVirtualAddress(),D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);BBarrier(startup.list.Get(),blas[i].Get(),D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_RAYTRACING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);}
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);startup.Submit(direct);WaitStartup(direct);for(uint32_t i:missing)vramCache.Commit(blas[i].Get());residency.Submitted(direct.serial);
    }
    D3D12_RAYTRACING_GEOMETRY_DESC DynamicGeometry(Frame& f,uint32_t mesh) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;g.Triangles.VertexBuffer={f.dynamicVertices->GetGPUVirtualAddress(),sizeof(Vertex)};g.Triangles.VertexCount=uint32_t(meshes.vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;g.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(meshes.ranges[mesh].firstIndex)*4;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;g.Triangles.IndexCount=meshes.ranges[mesh].indexCount;return g;
    }
    void DynamicResources(Frame& f) {
        f.motionBuffer=Placed(Buffer(std::max(size_t(1),animation.Motions().size())*sizeof(GpuMotion)));f.boneBuffer=Placed(Buffer(std::max(size_t(1),animation.Bones().size())*sizeof(GpuBone)));f.weightBuffer=Placed(Buffer(std::max(size_t(1),animation.Weights().size())*sizeof(float)));
        f.motionSRV=Srv(f.motionBuffer.Get(),uint32_t(std::max(size_t(1),animation.Motions().size())),sizeof(GpuMotion),f.motionSRV);f.boneSRV=Srv(f.boneBuffer.Get(),uint32_t(std::max(size_t(1),animation.Bones().size())),sizeof(GpuBone),f.boneSRV);f.weightSRV=Srv(f.weightBuffer.Get(),uint32_t(std::max(size_t(1),animation.Weights().size())),sizeof(float),f.weightSRV);
        f.deformedBounds=Placed(Buffer(meshes.ranges.size()*sizeof(XMFLOAT4),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));f.deformedBoundsSRV=Srv(f.deformedBounds.Get(),uint32_t(meshes.ranges.size()),sizeof(XMFLOAT4),f.deformedBoundsSRV);f.deformedBoundsUAV=UavBuffer(f.deformedBounds.Get(),uint32_t(meshes.ranges.size()),sizeof(XMFLOAT4),f.deformedBoundsUAV);

        f.dynamicVertices=Placed(Buffer(meshes.vertices.size()*sizeof(Vertex),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));f.dynamicSRV=Srv(f.dynamicVertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),f.dynamicSRV);f.dynamicUAV=UavBuffer(f.dynamicVertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),f.dynamicUAV);f.dynamicBLAS.resize(dynamicMeshes.size());f.dynamicBlasBuilt.assign(dynamicMeshes.size(),0);f.dynamicGeometryRevision.assign(dynamicMeshes.size(),0);f.dynamicBlasRevision.assign(dynamicMeshes.size(),0);f.pendingDynamicRevision.assign(dynamicMeshes.size(),0);f.deformationWork.Configure(uint32_t(dynamicMeshes.size()));f.blasWork.Configure(uint32_t(dynamicMeshes.size()));
        for(uint32_t i=0;i<dynamicMeshes.size();i++){auto geometry=DynamicGeometry(f,dynamicMeshes[i]);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;input.NumDescs=1;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=&geometry;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);f.dynamicBLAS[i]=Placed(Buffer(info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);f.dynamicScratchBytes=std::max(f.dynamicScratchBytes,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes));}
    }
    void InitializeResourceAbi(Frame& f){
        if(HistoryHiZMipBase+mips>ResourceDomainWords)Fail("resource domain table capacity");
        f.resourceDirectory=Host(UINT64(ResourceDomainCount)*sizeof(DomainDirectoryEntry),D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE empty{0,0};void* directory{};Hr(f.resourceDirectory->Map(0,&empty,&directory),"resource directory map");f.resourceDirectoryData=reinterpret_cast<DomainDirectoryEntry*>(directory);
        f.resourceDirectorySRV=Srv(f.resourceDirectory.Get(),ResourceDomainCount,sizeof(DomainDirectoryEntry),f.resourceDirectorySRV);
        for(uint32_t domain=0;domain<ResourceDomainCount;domain++){
            f.resourceTables[domain]=Host(UINT64(ResourceDomainWords)*sizeof(DomainBindingEntry),D3D12_HEAP_TYPE_UPLOAD);void* table{};Hr(f.resourceTables[domain]->Map(0,&empty,&table),"resource domain table map");f.resourceTableData[domain]=reinterpret_cast<DomainBindingEntry*>(table);std::fill_n(f.resourceTableData[domain],ResourceDomainWords,DomainBindingEntry{});
            f.resourceTableSRV[domain]=Srv(f.resourceTables[domain].Get(),ResourceDomainWords,sizeof(DomainBindingEntry),f.resourceTableSRV[domain]);f.resourceDirectoryData[domain]={Physical(f.resourceTableSRV[domain]),Generation(f.resourceTableSRV[domain]),ResourceDomainWords,0};
        }
    }
    void SetDomain(Frame& f,ResourceDomain domain,std::initializer_list<DescriptorHandle> descriptors){uint32_t d=uint32_t(domain);if(descriptors.size()>ResourceDomainWords)Fail("resource domain overflow");auto* table=f.resourceTableData[d];if(!table)Fail("resource domain not initialized");std::fill_n(table,ResourceDomainWords,DomainBindingEntry{});uint32_t i=0;for(DescriptorHandle handle:descriptors)table[i++]=handle.Valid()?DomainBindingEntry{Physical(handle),Generation(handle)}:DomainBindingEntry{};}
    void BuildDynamic(ID3D12GraphicsCommandList7* list,Frame& f) {
        
        uint32_t ordinal=0;
        for(uint32_t dynamicIndex:f.blasWork.Active()){
            if(dynamicIndex>=dynamicMeshes.size()||dynamicIndex>=f.dynamicBLAS.size()||dynamicIndex>=f.dynamicBlasBuilt.size())Fail("dynamic work index");
            if(ordinal++)BBarrier(list,f.dynamicScratch.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            uint32_t mesh=dynamicMeshes[dynamicIndex];auto geometry=DynamicGeometry(f,mesh);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.pGeometryDescs=&geometry;build.DestAccelerationStructureData=f.dynamicBLAS[dynamicIndex]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.dynamicScratch->GetGPUVirtualAddress();bool built=f.dynamicBlasBuilt[dynamicIndex]!=0;if(built){build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;build.SourceAccelerationStructureData=build.DestAccelerationStructureData;}BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),built?D3D12_BARRIER_SYNC_ALL_SHADING:D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,built?D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ:D3D12_BARRIER_ACCESS_NO_ACCESS,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE);list->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(list,f.dynamicBLAS[dynamicIndex].Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.dynamicBlasBuilt[dynamicIndex]=1;
        }
    }
    void EnsureInstances(Frame& f,uint32_t required){
        if(required<=f.instanceCapacity)return;
        uint32_t capacity=GrowCapacity(f.instanceCapacity,required);bool existing=f.instanceCapacity!=0;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;input.NumDescs=capacity;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);if(!info.ResultDataMaxSizeInBytes)Fail("DXR instance resource capacity exhausted");
        auto buffer=[&](ComPtr<ID3D12Resource>& destination,UINT64 bytes,D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON){passGraph.Forget(destination.Get());destination.Reset();destination=Temporary(Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),state);};
        buffer(f.instances,UINT64(capacity)*sizeof(GpuInstance));buffer(f.rayDescriptors,UINT64(capacity)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        buffer(f.hotBounds,UINT64(capacity)*16);buffer(f.hotMeshes,UINT64(capacity)*4);
        buffer(f.arguments,UINT64(capacity)*16);buffer(f.shadowArguments,UINT64(capacity)*16);buffer(f.cullArguments,UINT64(capacity)*16);
        buffer(f.tlas,info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        buffer(f.scratch,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto srv=[&](ID3D12Resource* resource,uint32_t count,uint32_t stride,DescriptorHandle& descriptor){descriptor=Srv(resource,count,stride,existing?descriptor:DescriptorHandle{});};
        auto uav=[&](ID3D12Resource* resource,uint32_t count,uint32_t stride,DescriptorHandle& descriptor){descriptor=UavBuffer(resource,count,stride,existing?descriptor:DescriptorHandle{});};
        srv(f.instances.Get(),capacity,sizeof(GpuInstance),f.instanceSRV);uav(f.instances.Get(),capacity,sizeof(GpuInstance),f.instanceUAV);
        uav(f.rayDescriptors.Get(),capacity*16,0,f.rayUAV);
        srv(f.hotBounds.Get(),capacity,16,f.hotBoundsSRV);uav(f.hotBounds.Get(),capacity,16,f.hotBoundsUAV);
        srv(f.hotMeshes.Get(),capacity,4,f.hotMeshesSRV);uav(f.hotMeshes.Get(),capacity,4,f.hotMeshesUAV);
        uav(f.arguments.Get(),capacity*4,0,f.argumentsUAV);uav(f.shadowArguments.Get(),capacity*4,0,f.shadowArgumentsUAV);uav(f.cullArguments.Get(),capacity*4,0,f.cullArgumentsUAV);
        if(!existing){buffer(f.count,4);buffer(f.shadowCount,4);buffer(f.cullCount,4);f.countUAV=UavBuffer(f.count.Get(),1,0);f.shadowCountUAV=UavBuffer(f.shadowCount.Get(),1,0);f.cullCountUAV=UavBuffer(f.cullCount.Get(),1,0);}
        if(f.tlasSRV.Valid())ReleaseDescriptor(f.tlasSRV);descriptorAllocator.Collect(CompletedFences());f.tlasSRV=descriptorAllocator.Allocate(f.tlas);D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.RaytracingAccelerationStructure.Location=f.tlas->GetGPUVirtualAddress();device->CreateShaderResourceView(nullptr,&view,Cpu(Physical(f.tlasSRV)));
        f.lastTlasCount=0;f.tlasBuilt=false;f.instanceCapacity=capacity;f.transparent.resize(capacity);uint32_t frameSlot=uint32_t(&f-frames.data());sceneCache.InvalidateFrame(frameSlot);rayScene.InvalidateFrame(frameSlot);
    }
    void EnsureStreamControl(Frame& f,uint32_t slots){
        if(slots<=f.streamControlSlots)return;uint32_t capacity=GrowCapacity(f.streamControlSlots,slots);passGraph.Forget(f.streamControl.Get());f.streamControl.Reset();f.streamControl=Temporary(Buffer(uint64_t(capacity)*GpuMeshPublicationAbi::ControlBytes),D3D12_RESOURCE_STATE_COMMON);f.streamControlSlots=capacity;
    }
    void ResetSceneResources(){
        active=nullptr;sceneLoaded=false;historyReady=false;sceneTimeOrigin=0;sceneTimeOriginValid=false;shadowCache={};
        auto release=[&](DescriptorHandle& handle){if(handle.Valid())ReleaseDescriptor(handle);};
        auto resetResource=[&](auto& resource){passGraph.Forget(resource.Get());resource.Reset();};
        auto releaseCached=[&](auto& resource){if(!resource)return;passGraph.Forget(resource.Get());vramCache.Release(resource.Get(),SubmittedFences());resource.Reset();};
        for(auto& texture:textures){release(texture.descriptor);releaseCached(texture.resource);}
        textures.clear();surfaceData.clear();baseSurfaceData.clear();surfacesDirty=false;aaSource={};aaSourceLease.reset();visibleMaterialBits.clear();materialResidencyOffsets.clear();materialResidencyPages.clear();residencyPageSeen.clear();requiredResidencyPages.clear();residencyPageGeneration=1;
        for(auto* handle:{&skinSRV,&morphSRV,&indicesSRV,&verticesSRV,&meshletsSRV,&remapSRV,&trianglesSRV,&rangesSRV,&materialsSRV,&deformationsSRV})release(*handle);
        releaseCached(vertices);releaseCached(indices);releaseCached(meshlets);releaseCached(remap);releaseCached(triangles);releaseCached(ranges);releaseCached(materials);releaseCached(deformations);releaseCached(skinVertices);releaseCached(morphVertices);
        releaseCached(residentStaticInstances);sceneCache.Reset();rayScene.Reset();staticMaterialBits.clear();staticTransparent.clear();staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};baseLights={};baseLightCount=0;staticShadowKey=1469598103934665603ull;
        for(auto& resource:blas)releaseCached(resource);blas.clear();dynamicMeshes.clear();meshDynamicIndex.clear();meshes={};animation=SceneAnimation{};visualFrame={};contributions={};
        for(auto& f:frames){
            for(auto* handle:{&f.motionSRV,&f.boneSRV,&f.weightSRV,&f.deformedBoundsSRV,&f.deformedBoundsUAV,&f.dynamicSRV,&f.dynamicUAV,&f.blasAddressesSRV,&f.lightsSRV,&f.clusterSRV,&f.clusterUAV})release(*handle);
            resetResource(f.motionBuffer);resetResource(f.boneBuffer);resetResource(f.weightBuffer);resetResource(f.deformedBounds);resetResource(f.dynamicVertices);resetResource(f.dynamicScratch);resetResource(f.blasAddresses);resetResource(f.lights);resetResource(f.clusters);
            f.dynamicBLAS.clear();f.dynamicBlasBuilt.clear();f.dynamicGeometryRevision.clear();f.dynamicBlasRevision.clear();f.pendingDynamicRevision.clear();f.deformationWork.Reset();f.blasWork.Reset();f.dynamicScratchBytes=0;f.lastTlasCount=0;f.tlasBuilt=false;f.transparent.clear();f.transparentCount=0;f.cpuCount=0;f.dynamicCount=0;f.additionalCount=0;
        }
        descriptorAllocator.Collect(CompletedFences());vramCache.Collect(CompletedFences());
    }
    void ConfigureSampleTargets(bool fresh){
        auto texture=[&](TextureResource& r,DXGI_FORMAT format,UINT samples,UINT layers,UINT levels,D3D12_RESOURCE_FLAGS flags){
            auto desc=Texture(width,height,format,samples,layers,levels,flags);
            if(r.resource&&SameResourceDesc(r.resource->GetDesc(),desc))return;
            passGraph.Forget(r.resource.Get());r.resource.Reset();r.layout=D3D12_BARRIER_LAYOUT_COMMON;
            D3D12_CLEAR_VALUE clear{};clear.Format=format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_D32_FLOAT:format;clear.DepthStencil.Depth=0;
            r.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON,(flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?&clear:nullptr);
        };
        auto hdrFlags=D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|(MsaaSamples==1?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE));texture(hdr,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,hdrFlags);texture(reflections,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);texture(depth,DXGI_FORMAT_R32_TYPELESS,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);hdrRTV=MakeRTV(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:hdrRTV);reflectionsRTV=MakeRTV(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:reflectionsRTV);depthDSV=MakeDSV(depth.resource.Get(),fresh?Invalid:depthDSV);depthSRV=TextureSrv(depth.resource.Get(),DXGI_FORMAT_R32_FLOAT,depthSRV);
        if(MsaaSamples>1){texture(hdrResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));hdrResolvedRTV=MakeRTV(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh||hdrResolvedRTV==Invalid?Invalid:hdrResolvedRTV);texture(reflectionsResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_NONE);hdrResolvedSRV=TextureSrv(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV);hdrResolvedUAV=TextureUav(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV);reflectionsResolvedSRV=TextureSrv(reflectionsResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV);}else{hdrResolvedSRV=TextureSrv(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV);hdrResolvedUAV=TextureUav(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV);reflectionsResolvedSRV=TextureSrv(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV);}
    }
    void ApplyMultisampling(){
        if(MsaaSamples==requestedSamples)return;
        // Rare option edit: retire all consumers before rebinding cached targets.
        WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);
        SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        MsaaSamples=requestedSamples;SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        ConfigureSampleTargets(false);Pipelines();historyReady=false;
    }
    void Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra){
        if(configuredXr){if(configuredXr!=&xr||jobs!=&workers)Fail("Graphics runtime services cannot change during scene replacement");WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();xrRTV.clear();xrDSV.clear();rtvCount=dsvCount=0;}
        else {configuredXr=&xr;jobs=&workers;}
        contributions=extra;if(assets.lights.size()>MaxLights)Fail("Scene GPU capacity");baseLightCount=uint32_t(assets.lights.size());for(uint32_t i=0;i<baseLightCount;i++){baseLights[i]=assets.lights[i];baseLights[i].id=0x80010000u+i;}visualFrame={};visualFrame.objects.reserve(256);width=xr.Width();height=xr.Height();viewCount=xr.ViewCount();if(!viewCount||viewCount>MaxViews)Fail("Unsupported XR view count");renderWidth=width;renderHeight=height;colorFormat=xr.Format();mips=std::bit_width(std::max(width,height));renderMips=mips;clustersCount=((width+63)/64)*((height+63)/64)*24*viewCount;if(MsaaSamples>1)for(DXGI_FORMAT format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,MsaaSamples,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)),"MSAA 4x");if(!q.NumQualityLevels)Fail("MSAA 4x required");}
        // OpenXR hands off legacy RENDER_TARGET / DEPTH_WRITE states. Keep these
        // images outside enhanced-layout tracking for their entire acquired lifetime.
        xrColorTargets.clear();for(const auto& image:xr.Colors()){xrRTV.push_back(MakeRTV(image.texture,colorFormat));xrColorTargets.emplace_back(image.texture);}
        for(const auto& image:xr.Depths())xrDSV.push_back(MakeDSV(image.texture));
        auto texture=[&](TextureResource& r,DXGI_FORMAT format,UINT samples,UINT layers,UINT levels,D3D12_RESOURCE_FLAGS flags,UINT w=0,UINT h=0){auto desc=Texture(w?w:width,h?h:height,format,samples,layers,levels,flags);if(r.resource&&SameResourceDesc(r.resource->GetDesc(),desc)){passGraph.Forget(r.resource.Get());return;}D3D12_CLEAR_VALUE clear{};clear.Format=format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_D32_FLOAT:format;clear.DepthStencil.Depth=0;passGraph.Forget(r.resource.Get());r.resource.Reset();r.layout=D3D12_BARRIER_LAYOUT_COMMON;r.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON,(flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?&clear:nullptr);};
        hdrResolvedRTV=Invalid;ConfigureSampleTargets(true);
        texture(hierarchy,DXGI_FORMAT_R32_FLOAT,1,viewCount,mips,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);hierarchySRV=TextureSrv(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,hierarchySRV);if(hierarchyUAV.size()>mips){for(uint32_t i=mips;i<hierarchyUAV.size();i++)ReleaseDescriptor(hierarchyUAV[i]);hierarchyUAV.resize(mips);}uint32_t oldHiZMips=uint32_t(hierarchyUAV.size());hierarchyUAV.resize(mips);for(uint32_t i=0;i<mips;i++)hierarchyUAV[i]=TextureUav(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,i,i<oldHiZMips?hierarchyUAV[i]:DescriptorHandle{});
        texture(shadow,DXGI_FORMAT_R32_TYPELESS,1,1,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,3072,2048);shadowDSV=MakeDSV(shadow.resource.Get());shadowSRV=TextureSrv(shadow.resource.Get(),DXGI_FORMAT_R32_FLOAT,shadowSRV);
        for(uint32_t i=0;i<history.size();i++){texture(history[i],DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);texture(historyDepth[i],DXGI_FORMAT_R32_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);historySRV[i]=TextureSrv(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,historySRV[i]);historyUAV[i]=TextureUav(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,historyUAV[i]);historyDepthSRV[i]=TextureSrv(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,historyDepthSRV[i]);historyDepthUAV[i]=TextureUav(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,0,historyDepthUAV[i]);}
        meshes=assets.meshes;animation.Initialize(assets);meshDynamicIndex.resize(meshes.ranges.size(),Invalid);for(uint32_t i=0;i<meshes.deformations.size();i++)if(meshes.deformations[i].axisAmplitude.w!=0||meshes.motions[i].node!=Invalid){meshDynamicIndex[i]=uint32_t(dynamicMeshes.size());dynamicMeshes.push_back(i);}startup.Reset();vertices=CachedUploadVector(meshes.vertices,CacheVertices);indices=CachedUploadVector(meshes.indices,CacheIndices);meshlets=CachedUploadVector(meshes.meshlets,CacheMeshlets);remap=CachedUploadVector(meshes.remap,CacheRemap);triangles=CachedUploadVector(meshes.triangles,CacheTriangles);ranges=CachedUploadVector(meshes.ranges,CacheRanges);indicesSRV=Srv(indices.Get(),uint32_t(meshes.indices.size()),4,indicesSRV);verticesSRV=Srv(vertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),verticesSRV);meshletsSRV=Srv(meshlets.Get(),uint32_t(meshes.meshlets.size()),sizeof(Meshlet),meshletsSRV);remapSRV=Srv(remap.Get(),uint32_t(meshes.remap.size()),4,remapSRV);trianglesSRV=Srv(triangles.Get(),uint32_t(meshes.triangles.size()),12,trianglesSRV);rangesSRV=Srv(ranges.Get(),uint32_t(meshes.ranges.size()),sizeof(MeshRange),rangesSRV);
        if(assets.samplers.size()>1021)Fail("material sampler capacity");
        for(uint32_t i=0;i<assets.samplers.size();i++){const auto& source=assets.samplers[i];D3D12_SAMPLER_DESC sampler{};auto address=[](uint32_t mode){return mode==33071?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:mode==33648?D3D12_TEXTURE_ADDRESS_MODE_MIRROR:D3D12_TEXTURE_ADDRESS_MODE_WRAP;};sampler.AddressU=address(source.wrapS);sampler.AddressV=address(source.wrapT);sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;bool minLinear=source.minFilter==9729||source.minFilter==9985||source.minFilter==9987,magLinear=source.magFilter!=9728,mipLinear=source.minFilter==9986||source.minFilter==9987;sampler.Filter=D3D12_ENCODE_BASIC_FILTER(minLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,magLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,mipLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,D3D12_FILTER_REDUCTION_TYPE_STANDARD);sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;sampler.MaxLOD=source.minFilter==9728||source.minFilter==9729?0:D3D12_FLOAT32_MAX;auto handle=samplers->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(i+3)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);device->CreateSampler(&sampler,handle);}
        std::vector<uint32_t> textureDescriptors;textureDescriptors.reserve(assets.textures.size());for(const auto& texture:assets.textures)textureDescriptors.push_back(UploadTexture(texture));surfaceData.reserve(assets.materials.size());for(const auto& material:assets.materials){Surface s{};s.colorRoughness=material.colorRoughness;s.emissionMetallic=material.emissionMetallic;s.albedo=material.albedo==Invalid?Invalid:textureDescriptors[material.albedo];s.normal=material.normal==Invalid?Invalid:textureDescriptors[material.normal];s.metalRough=material.metalRough==Invalid?Invalid:textureDescriptors[material.metalRough];s.emissive=material.emissive==Invalid?Invalid:textureDescriptors[material.emissive];s.parameters.x=material.normalScale;s.extra={material.occlusion==Invalid?Invalid:textureDescriptors[material.occlusion],material.alphaMode,material.doubleSided,material.unlit};s.opacity={material.alpha,material.cutoff,material.occlusionStrength,0};s.mapping=material.mapping;for(uint32_t slot=0;slot<5;slot++)s.mapping[slot].rotationSet.w=material.samplers[slot]==Invalid?0.f:float(3+material.samplers[slot]);surfaceData.push_back(s);}
        visibleMaterialBits.assign((surfaceData.size()+63)/64,0);materialResidencyOffsets.resize(surfaceData.size()+1);std::vector<uint32_t> textureResidencyByDescriptor(DescriptorCapacity,Invalid);uint32_t maxTextureResidency=0;for(const auto& texture:textures){uint32_t descriptor=Physical(texture.descriptor);textureResidencyByDescriptor[descriptor]=texture.residency;maxTextureResidency=std::max(maxTextureResidency,texture.residency);}
        baseSurfaceData=surfaceData;ApplySurfaceEdits();
        materialResidencyPages.clear();materialResidencyPages.reserve(surfaceData.size()*2);for(uint32_t material=0;material<surfaceData.size();material++){materialResidencyOffsets[material]=uint32_t(materialResidencyPages.size());const auto& surface=surfaceData[material];const uint32_t descriptors[]={surface.albedo,surface.normal,surface.metalRough,surface.emissive,surface.extra.x};for(uint32_t descriptor:descriptors){if(descriptor==Invalid)continue;if(descriptor>=textureResidencyByDescriptor.size()||textureResidencyByDescriptor[descriptor]==Invalid)Fail("material texture residency dependency");uint32_t page=textureResidencyByDescriptor[descriptor];bool duplicate=false;for(uint32_t i=materialResidencyOffsets[material];i<materialResidencyPages.size();i++)if(materialResidencyPages[i]==page){duplicate=true;break;}if(!duplicate)materialResidencyPages.push_back(page);}}materialResidencyOffsets[surfaceData.size()]=uint32_t(materialResidencyPages.size());residencyPageSeen.assign(textures.empty()?0:maxTextureResidency+1,0);requiredResidencyPages.clear();requiredResidencyPages.reserve(textures.size());std::vector<GpuInstance> staticInstances;staticInstances.resize(assets.objects.size());staticMaterialBits.assign(visibleMaterialBits.size(),0);staticTransparent.clear();staticTransparent.reserve(assets.objects.size());staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};std::vector<uint8_t> staticDeformedSeen(dynamicMeshes.size(),0),staticDeformedRtSeen(dynamicMeshes.size(),0);staticShadowKey=1469598103934665603ull;for(uint32_t i=0;i<assets.objects.size();i++){auto object=assets.objects[i];object.id=0x80000000u+i;auto gpu=ConvertObject(object,1.f);staticInstances[i]=gpu;if(gpu.material<surfaceData.size()){staticMaterialBits[gpu.material>>6]|=uint64_t(1)<<(gpu.material&63);for(uint32_t q=0;q<staticReflectionCandidates.size();q++)staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[gpu.material],q);}if(gpu.flags&AlphaBlendObject)staticTransparent.push_back(i);if(gpu.flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[gpu.mesh];if(dynamicIndex==Invalid)Fail("DeformedObject references mesh without dynamic deformation capability");if(!staticDeformedSeen[dynamicIndex]){staticDeformedSeen[dynamicIndex]=1;staticDeformedMeshes.push_back(dynamicIndex);}if(!(gpu.flags&NoRayTracing)&&!staticDeformedRtSeen[dynamicIndex]){staticDeformedRtSeen[dynamicIndex]=1;staticDeformedRtMeshes.push_back(dynamicIndex);}}if(!(gpu.flags&(DynamicObject|DeformedObject|NoShadow)))staticShadowKey=HashMemory(&gpu,sizeof(gpu),staticShadowKey);}sceneCache.Replace(std::move(staticInstances));rayScene.ReplaceStatic(sceneCache.StaticInstances());if(sceneCache.StaticCount())residentStaticInstances=CachedUploadBuffer(sceneCache.StaticInstances().data(),uint64_t(sceneCache.StaticCount())*sizeof(GpuInstance),CacheStaticInstances,sceneCache.ContentGeneration());materials=UploadVector(surfaceData);surfacesDirty=false;materialsSRV=Srv(materials.Get(),uint32_t(surfaceData.size()),sizeof(Surface),materialsSRV);deformations=CachedUploadVector(meshes.deformations,CacheDeformations);deformationsSRV=Srv(deformations.Get(),uint32_t(meshes.deformations.size()),sizeof(Deformation),deformationsSRV);skinVertices=CachedUploadVector(meshes.skin,CacheSkin);skinSRV=Srv(skinVertices.Get(),uint32_t(meshes.skin.size()),sizeof(SkinVertex),skinSRV);if(meshes.morphs.empty()){MorphVertex zero;morphVertices=CachedUploadBuffer(&zero,sizeof(zero),CacheMorphs);}else morphVertices=CachedUploadVector(meshes.morphs,CacheMorphs);morphSRV=Srv(morphVertices.Get(),uint32_t(std::max(size_t(1),meshes.morphs.size())),sizeof(MorphVertex),morphSRV);startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);uploads.clear();BuildBLAS();
        for(auto& f:frames){
            DynamicResources(f);EnsureInstances(f,sceneCache.StaticCount()+128);
            if(f.dynamicScratchBytes)f.dynamicScratch=Temporary(Buffer(f.dynamicScratchBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            f.lights=Placed(Buffer(MaxLights*sizeof(Light)));f.lightsSRV=Srv(f.lights.Get(),MaxLights,sizeof(Light),f.lightsSRV);
            f.clusters=Placed(Buffer(UINT64(clustersCount)*ClusterWords*4,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));f.clusterSRV=Srv(f.clusters.Get(),clustersCount,ClusterWords*4,f.clusterSRV);f.clusterUAV=UavBuffer(f.clusters.Get(),clustersCount,ClusterWords*4,f.clusterUAV);
        }
        auto name=[&](ID3D12Resource* resource,const wchar_t* wide,const char* label){if(resource){resource->SetName(wide);LogEvent(0,"gpu.resource","name=%s resource=%p width=%llu height=%u layers=%u",label,static_cast<void*>(resource),static_cast<unsigned long long>(resource->GetDesc().Width),resource->GetDesc().Height,unsigned(resource->GetDesc().DepthOrArraySize));}};
        name(residentStaticInstances.Get(),L"Persistent static instances","Persistent static instances");name(hdr.resource.Get(),L"HDR views","HDR views");name(reflections.resource.Get(),L"Reflections views","Reflections views");name(depth.resource.Get(),L"Depth reverse Z","Depth reverse Z");name(hierarchy.resource.Get(),L"HiZ views","HiZ views");name(shadow.resource.Get(),L"Static point shadow atlas","Static point shadow atlas");name(hdrResolved.resource.Get(),L"HDR resolved","HDR resolved");name(reflectionsResolved.resource.Get(),L"Reflections resolved","Reflections resolved");
        for(uint32_t i=0;i<history.size();i++){name(history[i].resource.Get(),L"Reflection history","Reflection history");name(historyDepth[i].resource.Get(),L"History depth","History depth");}
        for(auto& f:frames){name(f.tlas.Get(),L"Frame TLAS","Frame TLAS");name(f.instances.Get(),L"Frame instances","Frame instances");name(f.streamControl.Get(),L"GPU publication control","GPU publication control");name(f.rayDescriptors.Get(),L"TLAS instances","TLAS instances");name(f.dynamicVertices.Get(),L"Deformed vertices","Deformed vertices");name(f.clusters.Get(),L"Light clusters","Light clusters");name(f.arguments.Get(),L"Depth indirect","Depth indirect");name(f.shadowArguments.Get(),L"Shadow indirect","Shadow indirect");name(f.cullArguments.Get(),L"Lighting indirect","Lighting indirect");}
        startup.Reset();for(auto& f:frames){std::vector<XMUINT4> addresses(meshes.ranges.size());for(uint32_t i=0;i<addresses.size();i++){auto a=blas[i]->GetGPUVirtualAddress();uint32_t dynamic=meshDynamicIndex[i];auto b=dynamic==Invalid?a:f.dynamicBLAS[dynamic]->GetGPUVirtualAddress();addresses[i]={uint32_t(a),uint32_t(a>>32),uint32_t(b),uint32_t(b>>32)};}f.blasAddresses=UploadVector(addresses);f.blasAddressesSRV=Srv(f.blasAddresses.Get(),uint32_t(addresses.size()),sizeof(XMUINT4),f.blasAddressesSRV);}startup.Submit(direct);WaitStartup(direct);uploads.clear();
        for(auto& f:frames)InitializeResourceAbi(f);
        Pipelines();sceneLoaded=true;
    }
    void ReplaceScene(const SceneData& assets,RenderContributions extra){if(!configuredXr||!jobs)Fail("Graphics runtime is not configured");Configure(*configuredXr,assets,*jobs,extra);}
    void UnloadScene(){if(!configuredXr||!sceneLoaded)return;WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();}
    UINT64 Reserve(Frame& f,UINT64 size,UINT64 alignment=256){if(!f.uploadGeneration)Fail("frame upload generation missing");auto offset=Align(f.bump,alignment);if(offset+size>f.uploadGeneration->chunk)Fail("upload generation capacity");f.bump=offset+size;return f.base+offset;}
    void PrepareLights(Frame& f,const RenderScene& snapshot) {
        f.lightCount=std::min(snapshot.lightCount,MaxLights);f.shadowDirty=0;f.hasShadowedLights=false;f.hasShadowAtlasLights=false;f.shadowIDs.fill(Invalid);f.shadowKeys.fill(0);
        uint64_t staticKey=staticShadowKey;
        for(uint32_t i=0;i<f.dynamicCount;i++){const auto& o=f.objects[i];if(o.flags&(DynamicObject|DeformedObject|NoShadow))continue;staticKey=HashMemory(&o,sizeof(o),staticKey);}
        std::array<bool,ShadowSlots> assigned{};
        for(uint32_t i=0;i<f.lightCount;i++) {
            auto light=snapshot.lights[i];if(light.animationNode!=Invalid){auto world=animation.NodeMatrix(light.animationNode)*XMMatrixScaling(1,1,-1);XMStoreFloat3(&light.position,XMVector3TransformCoord(XMVectorZero(),world));XMStoreFloat3(&light.direction,XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0,0,-1,0),world)));}if(contributions.lightContext)contributions.lightContext(contributions.context,light,f.simulationTime);int slot=-1;
            if((light.flags&1)&&!(light.flags&(2|4))) {
                for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]&&shadowCache[j].id==light.id){slot=int(j);break;}
                if(slot<0)for(uint32_t j=0;j<ShadowSlots;j++)if(!assigned[j]){bool reserved=false;for(uint32_t k=i+1;k<f.lightCount;k++)if(snapshot.lights[k].id==shadowCache[j].id)reserved=true;if(!reserved){slot=int(j);break;}}
            }
            if(slot>=0) {
                uint32_t s=uint32_t(slot);assigned[s]=true;f.shadowIDs[s]=light.id;f.shadowLights[s]=light;
                uint64_t key=HashMemory(&light.position,sizeof(light.position),staticKey);key=HashMemory(&light.radius,sizeof(light.radius),key);key=HashMemory(&light.revision,sizeof(light.revision),key);f.shadowKeys[s]=key;
                if(!shadowCache[s].valid||shadowCache[s].id!=light.id||shadowCache[s].key!=key)f.shadowDirty|=1u<<s;
                auto at=XMLoadFloat3(&light.position);const XMVECTOR directions[]={XMVectorSet(1,0,0,0),XMVectorSet(-1,0,0,0),XMVectorSet(0,1,0,0),XMVectorSet(0,-1,0,0),XMVectorSet(0,0,1,0),XMVectorSet(0,0,-1,0)};
                for(uint32_t face=0;face<6;face++){auto up=face==2?XMVectorSet(0,0,-1,0):face==3?XMVectorSet(0,0,1,0):XMVectorSet(0,1,0,0);XMStoreFloat4x4(&shadowMatrices[s*6+face],XMMatrixLookToLH(at,directions[face],up)*XMMatrixPerspectiveFovLH(XM_PIDIV2,1,std::max(.1f,light.radius),.02f));}
            }
            f.hasShadowedLights|=(light.flags&1)!=0;f.hasShadowAtlasLights|=slot>=0;
            f.lightData[i]={{light.position.x,light.position.y,light.position.z,std::max(.1f,light.radius)},{light.color.x,light.color.y,light.color.z,light.power},{light.direction.x,light.direction.y,light.direction.z,light.outerCos},{light.innerCos,float(slot),float(light.flags),0}};
        }
        f.lightsOffset=Reserve(f,sizeof(Light)*MaxLights);if(f.lightCount)memcpy(f.uploadGeneration->mapped+f.lightsOffset,f.lightData.data(),f.lightCount*sizeof(Light));
    }
    const RenderScene& Visuals(const FrameInput& input){
        auto& r=visualFrame;r.objects.clear();r.count=0;r.lightCount=baseLightCount;for(uint32_t i=0;i<baseLightCount;i++)r.lights[i]=baseLights[i];double displayTime=input.targetDisplayQpc?QpcSeconds(input.targetDisplayQpc):input.time;if(!sceneTimeOriginValid&&displayTime>0){sceneTimeOrigin=displayTime;sceneTimeOriginValid=true;}double sceneTime=sceneTimeOriginValid?displayTime-sceneTimeOrigin:0;r.time=displayTime;r.inputFrameId=input.frameId;r.simulationTick=0;
        if(contributions.appendContext)contributions.appendContext(contributions.context,r,input,sceneTime);
        if(r.count!=r.objects.size())Fail("Render contribution count mismatch");
        return r;
    }
    bool Prepare(const FrameInput& input) {
        if(!sceneLoaded)return false;
        ApplyMultisampling();
        const auto& snapshot=Visuals(input);
        active=nullptr;std::array<UINT64,GraphQueueCount> completed={copy.fence->GetCompletedValue(),compute.fence->GetCompletedValue(),direct.fence->GetCompletedValue()};for(auto value:completed)if(value==UINT64_MAX){Hr(device->GetDeviceRemovedReason(),"D3D12 device removed");Fail("Invalid GPU completion fence");}
        for(uint32_t i=0;i<3;i++){auto& candidate=frames[(frameNumber+i)%3];bool ready=true;for(uint32_t q=0;q<GraphQueueCount;q++)ready&=completed[q]>=candidate.queueFences[q];if(ready){active=&candidate;break;}}
        uint64_t totalCpu=uint64_t(sceneCache.StaticCount())+snapshot.count;if(!active){LogEvent(snapshot.inputFrameId,"render.skip","reason=%s","frame_context_busy");return false;}if(totalCpu>=Invalid)Fail("CPU scene instance address space exhausted");
        auto& f=*active;
        UINT64 uploadRequired=UINT64(snapshot.count)*sizeof(GpuInstance)+animation.Motions().size()*sizeof(GpuMotion)+animation.Bones().size()*sizeof(GpuBone)+animation.Weights().size()*sizeof(float)+sizeof(Constants)+MaxLights*sizeof(Light)+8192+surfaceData.size()*sizeof(Surface);
        auto upload=std::atomic_load_explicit(&uploadGeneration,std::memory_order_acquire);if(!upload)Fail("upload generation unavailable");
        if(uploadRequired>upload->chunk){
            UINT64 capacity=upload->chunk;while(capacity<uploadRequired)capacity*=2;auto next=NewUploadGeneration(capacity);std::atomic_store_explicit(&uploadGeneration,next,std::memory_order_release);LogEvent(snapshot.inputFrameId,"gpu.capacity_generation","domain=graphics.upload generation=%llu old_bytes=%llu new_bytes=%llu",static_cast<unsigned long long>(next->generation),static_cast<unsigned long long>(upload->chunk*3),static_cast<unsigned long long>(next->chunk*3));upload=std::move(next);
        }
        f.uploadGeneration=upload;f.base=UINT64(&f-frames.data())*upload->chunk;auto* mapped=upload->mapped;
        f.sourceInput=snapshot.inputFrameId;f.sourceTick=snapshot.simulationTick;f.bump=0;f.simulationTime=sceneTimeOriginValid?snapshot.time-sceneTimeOrigin:0;animation.Evaluate(f.simulationTime);auto uploadAnimation=[&](const auto& values){using Value=typename std::decay_t<decltype(values)>::value_type;auto offset=Reserve(f,std::max(size_t(1),values.size())*sizeof(Value));if(!values.empty())memcpy(mapped+offset,values.data(),values.size()*sizeof(Value));else memset(mapped+offset,0,sizeof(Value));return offset;};f.motionOffset=uploadAnimation(animation.Motions());f.boneOffset=uploadAnimation(animation.Bones());f.weightOffset=uploadAnimation(animation.Weights());f.constantsOffset=Reserve(f,sizeof(Constants));f.constants=reinterpret_cast<Constants*>(mapped+f.constantsOffset);f.dynamicCount=snapshot.count;f.cpuCount=uint32_t(totalCpu);f.instanceCount=f.cpuCount;f.additionalCount=0;f.objectsOffset=Reserve(f,std::max(1u,f.dynamicCount)*sizeof(GpuInstance));f.objects=reinterpret_cast<GpuInstance*>(mapped+f.objectsOffset);
        struct Work {Impl* self;Frame* frame;const RenderScene* snapshot;float alpha;};double tickPhase=f.simulationTime/PhysicsDt;Work work{this,&f,&snapshot,std::clamp(float(tickPhase-std::floor(tickPhase)),0.f,1.f)};
        jobs->ParallelFor(f.dynamicCount,128,[](void* context,uint32_t begin,uint32_t end){auto& work=*static_cast<Work*>(context);auto& self=*work.self;auto& f=*work.frame;for(uint32_t i=begin;i<end;i++)f.objects[i]=self.ConvertObject(work.snapshot->objects[i],work.alpha);},&work);
        rayScene.UpdateDynamic(sceneCache.StaticCount(),std::span<const GpuInstance>(f.objects,f.dynamicCount));
        f.deformationWork.BeginFrame();f.blasWork.BeginFrame();
        auto deformationRevision=[&](uint32_t dynamicIndex){uint32_t mesh=dynamicMeshes[dynamicIndex];uint64_t revision=1469598103934665603ull;const auto& motion=animation.Motions()[mesh];if(motion.binding.x!=Invalid){revision=HashMemory(&motion,sizeof(motion),revision);if(motion.binding.z){uint32_t first=motion.binding.y,count=motion.binding.z;if(uint64_t(first)+count>animation.Bones().size())Fail("animation bone revision range");if(count)revision=HashMemory(animation.Bones().data()+first,size_t(count)*sizeof(GpuBone),revision);}if(motion.morph.x){uint32_t first=motion.morph.y,count=motion.morph.x;if(uint64_t(first)+count>animation.Weights().size())Fail("animation weight revision range");if(count)revision=HashMemory(animation.Weights().data()+first,size_t(count)*sizeof(float),revision);}}else{const auto& d=meshes.deformations[mesh];revision=HashMemory(&d,sizeof(d),revision);if(d.axisAmplitude.w!=0&&d.frequencyPhase.x!=0){float stretch=1+d.axisAmplitude.w*std::sin(float(f.simulationTime)*d.frequencyPhase.x+d.frequencyPhase.y);revision=HashMemory(&stretch,sizeof(stretch),revision);}}return revision?revision:1ull;};
        auto requestDeformation=[&](uint32_t dynamicIndex,bool rayVisible){if(dynamicIndex==Invalid||dynamicIndex>=dynamicMeshes.size())Fail("DeformedObject references mesh without dynamic deformation capability");uint64_t revision=deformationRevision(dynamicIndex);f.pendingDynamicRevision[dynamicIndex]=revision;if(f.dynamicGeometryRevision[dynamicIndex]!=revision&&!f.deformationWork.Activate(dynamicIndex))Fail("deformation work index");if(rayVisible&&f.dynamicBlasRevision[dynamicIndex]!=revision&&!f.blasWork.Activate(dynamicIndex))Fail("dynamic BLAS work index");};
        for(uint32_t dynamicIndex:staticDeformedMeshes){bool rayVisible=std::find(staticDeformedRtMeshes.begin(),staticDeformedRtMeshes.end(),dynamicIndex)!=staticDeformedRtMeshes.end();requestDeformation(dynamicIndex,rayVisible);}
        for(uint32_t i=0;i<f.dynamicCount;i++)if(f.objects[i].flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[f.objects[i].mesh];requestDeformation(dynamicIndex,(f.objects[i].flags&NoRayTracing)==0);}
        PrepareLights(f,snapshot);return true;
    }
    bool PrepareResidency(const XRFrame& xr,Frame& f,bool coreSceneWork) {
        residency.Begin();vramCache.RequireActive();if(visibleMaterialBits.size()!=staticMaterialBits.size())Fail("static material residency shape");
        std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);
        if(coreSceneWork){
            std::copy(staticMaterialBits.begin(),staticMaterialBits.end(),visibleMaterialBits.begin());
            auto markMaterial=[&](uint32_t material){if(material>=surfaceData.size())Fail("material residency index");visibleMaterialBits[material>>6]|=uint64_t(1)<<(material&63);};
            for(uint32_t i=0;i<f.dynamicCount;i++) {
                const auto& object=f.objects[i];const auto& range=meshes.ranges[object.mesh];
                XMVECTOR local=XMVectorMultiply(XMLoadFloat4(&range.bounds),XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.scale)));
                XMVECTOR center=XMVectorAdd(XMVector3Rotate(local,XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.rotation))),XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&object.positionRadius)));
                center=XMVectorSetW(center,1);bool visible=(object.flags&DeformedObject)!=0;
                for(uint32_t eye=0;eye<xr.viewCount&&!visible;eye++) {
                    auto matrix=XMLoadFloat4x4(&xr.vp[eye]);auto columns=XMMatrixTranspose(matrix);bool outside=false;
                    XMVECTOR planes[]={columns.r[3]+columns.r[0],columns.r[3]-columns.r[0],columns.r[3]+columns.r[1],columns.r[3]-columns.r[1],columns.r[2],columns.r[3]-columns.r[2]};
                    for(auto plane:planes)if(XMVectorGetX(XMVector4Dot(center,plane)) < -object.positionRadius.w*XMVectorGetX(XMVector3Length(plane))){outside=true;break;}
                    visible=!outside;
                }
                if(visible||surfaceData[object.material].extra.y)markMaterial(object.material);
            }
            for(uint32_t material:contributions.streamMaterials)markMaterial(material);
        }
        if(++residencyPageGeneration==0){std::fill(residencyPageSeen.begin(),residencyPageSeen.end(),0);residencyPageGeneration=1;}requiredResidencyPages.clear();
        for(uint32_t word=0;word<visibleMaterialBits.size();word++){uint64_t bits=visibleMaterialBits[word];while(bits){uint32_t bit=uint32_t(std::countr_zero(bits));uint32_t material=word*64+bit;bits&=bits-1;if(material>=surfaceData.size())continue;for(uint32_t i=materialResidencyOffsets[material];i<materialResidencyOffsets[material+1];i++){uint32_t page=materialResidencyPages[i];if(page>=residencyPageSeen.size())Fail("material residency page index");if(residencyPageSeen[page]!=residencyPageGeneration){residencyPageSeen[page]=residencyPageGeneration;requiredResidencyPages.push_back(page);}}}}
        for(uint32_t page:requiredResidencyPages)residency.Require(page);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom)){LogEvent(xr.input.frameId,"render.skip","reason=residency_budget");active=nullptr;historyReady=false;return false;}
        
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());return true;
    }


    void ClearCount(ID3D12GraphicsCommandList7* l,ID3D12Resource* r,DescriptorHandle descriptor){UINT zero[4]{};uint32_t index=Physical(descriptor);BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);l->ClearUnorderedAccessViewUint(Gpu(index),Cpu(index),r,zero,0,nullptr);BBarrier(l,r,D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
    void ArgumentsReady(ID3D12GraphicsCommandList7* l,ID3D12Resource* args,ID3D12Resource* count){for(auto r:{args,count})BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}
    void Viewport(ID3D12GraphicsCommandList7* l,UINT w,UINT h,UINT x=0,UINT y=0){D3D12_VIEWPORT v{float(x),float(y),float(w),float(h),0,1};D3D12_RECT r{LONG(x),LONG(y),LONG(x+w),LONG(y+h)};l->RSSetViewports(1,&v);l->RSSetScissorRects(1,&r);}
    void Render(XRFrame& xr){
        traceFrame=xr.input.frameId;if(!active)return;if(xr.viewCount!=viewCount||!xr.viewCount||xr.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(xr.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    // Even an unused publication must finish production before
                    // retirement; enqueue this dependency without a CPU wait.
                    if(publication.producerFence&&publication.producerValue)Hr(copy.queue->Wait(publication.producerFence.Get(),publication.producerValue),"GPU publication early producer wait");
                    Hr(copy.queue->Signal(publication.consumerFence.Get(),publication.consumerValue),"GPU publication early consumer fence");
                }
                bound.stream->Release(publication);bound.stream=nullptr;bound.publication={};
            }
        };
        for(auto* stream:streams){
            auto* domain=FindDomain(stream->Domain());if(!domain)Fail("GPU stream domain is not registered");auto publication=stream->Acquire();
            // A provider with no data contributes nothing to this frame. Other
            // streams, the room/menu and OpenXR presentation remain operative.
            // Physics publishes through queued consumer waits when its ring is
            // occupied, so saturation does not reach this no-data branch.
            if(!publication){LogEvent(traceFrame,"render.publication","state=unavailable domain=%u frame_continues=1",uint32_t(stream->Domain()));continue;}
            if(publication.domain!=stream->Domain())Fail("GPU publication changed stream domain");if(!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)Fail("GPU stream publication missing producer/consumer fence");if(!domain->ValidatePublication(publication))Fail("GPU publication rejected by domain contract");
            DescribePublication(publication);publications.push_back({stream,domain,std::move(publication)});domain->BindPublication(publications.back().publication);
        }
        auto& f=*active;
        const bool coreSceneWork=f.cpuCount!=0||!meshDomain->Bindings().empty();
        f.instanceCount=meshDomain->TotalInstances();EnsureInstances(f,f.instanceCount);EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);bool copyStatic=!pendingStatic.empty();
        rayPublicationScratch.clear();if(rayPublicationScratch.capacity()<meshDomain->Bindings().size())rayPublicationScratch.reserve(meshDomain->Bindings().size());for(const auto& bound:meshDomain->Bindings()){const auto* instances=bound.publication.Find(GpuMeshPublicationAbi::Instances);uint64_t generation=bound.publication.generation;generation=HashMemory(&instances->generation,sizeof(instances->generation),generation?generation:1469598103934665603ull);rayPublicationScratch.push_back({generation,bound.base,bound.capacity});}rayScene.StagePublications(frameSlot,rayPublicationScratch);if(!f.blasWork.Empty())rayScene.MarkGeometryDirty(frameSlot);auto rayCpuDirty=rayScene.PendingCpu(frameSlot);auto rayPublicationDirty=rayScene.PendingPublications(frameSlot);bool rayDirty=rayScene.NeedsTlas(frameSlot);
        if(coreSceneWork||!publications.empty()){
            if(!PrepareResidency(xr,f,coreSceneWork)){releasePublications(true);publications.clear();endDomains();return;}
        }else{
            std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);requiredResidencyPages.clear();
        }
        if(xr.input.trackingRevision!=lastTrackingRevision||(lastDisplayTime&&xr.displayTime-lastDisplayTime>xr.displayPeriod*2))historyReady=false;lastTrackingRevision=xr.input.trackingRevision;lastDisplayTime=xr.displayTime;
        f.submittedFrame=xr.input.frameId;LogEvent(traceFrame,"render.scene","source_input=%llu simulation_tick=%llu instance_capacity=%u streams=%u tlas_dirty=%u ray_cpu_ranges=%u ray_streams=%u",static_cast<unsigned long long>(f.sourceInput),static_cast<unsigned long long>(f.sourceTick),f.instanceCount,uint32_t(publications.size()),unsigned(rayDirty),uint32_t(rayCpuDirty.size()),uint32_t(rayPublicationDirty.size()));
        constexpr float resolutionScale=1.0f;renderWidth=width;renderHeight=height;renderMips=std::bit_width(std::max(renderWidth,renderHeight));constexpr uint32_t quality=3u;uint32_t output=uint32_t(frameNumber&1),previous=1-output;
        uint32_t staticCount=sceneCache.StaticCount();f.transparentCount=0;bool hasReflections=false;
        if(coreSceneWork){
            for(uint32_t index:staticTransparent)f.transparent[f.transparentCount++]=index;hasReflections=staticReflectionCandidates[quality];
            for(uint32_t i=0;i<f.dynamicCount;i++){const auto& object=f.objects[i];if(object.flags&AlphaBlendObject)f.transparent[f.transparentCount++]=staticCount+i;if(!hasReflections&&object.material<surfaceData.size())hasReflections=SurfaceMayReflect(surfaceData[object.material],quality);}
            if(!hasReflections)for(uint32_t material:contributions.streamMaterials){if(material>=surfaceData.size()){hasReflections=true;break;}if(SurfaceMayReflect(surfaceData[material],quality)){hasReflections=true;break;}}
            // MeshSolid publications do not expose a per-instance material summary. If a
            // producer has not supplied the existing streamMaterials contract, keep the
            // reflection path rather than guessing that GPU-only instances are matte.
            if(!hasReflections&&!meshDomain->Bindings().empty()&&contributions.streamMaterials.empty())hasReflections=true;
        }else{
            hasReflections=false;historyReady=false;
        }
        RenderPassPlan passPlan{};
        if(coreSceneWork){
            passPlan.occlusion=f.instanceCount>SmallSceneOcclusionThreshold;
            passPlan.depthHierarchy=passPlan.occlusion||hasReflections;
            passPlan.clusteredLights=f.lightCount>DirectLightLimit;
            passPlan.reflections=hasReflections;
            passPlan.shadowAtlas=f.hasShadowAtlasLights;
            passPlan.rayQueries=passPlan.reflections||f.hasShadowedLights;
            passPlan.rayWork=rayDirty&&passPlan.rayQueries;
        }
        auto& c=*f.constants;c={};bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        for(uint32_t view=0;view<xr.viewCount;view++){c.vp[view]=xr.vp[view];c.inverseVP[view]=xr.inverseVP[view];c.previousVP[view]=reflectionHistoryReady?lastVP[view]:xr.vp[view];c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};lastVP[view]=xr.vp[view];}
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV});
        SetDomain(f,ResourceDomain::Frame,{f.instanceSRV,f.argumentsUAV,f.countUAV,f.instanceUAV,f.cullArgumentsUAV,f.cullCountUAV,f.shadowArgumentsUAV,f.shadowCountUAV});
        SetDomain(f,ResourceDomain::Lighting,{f.lightsSRV,f.clusterSRV,f.clusterUAV,shadowSRV});
        SetDomain(f,ResourceDomain::History,{hierarchySRV,depthSRV,hdrResolvedSRV,reflectionsResolvedSRV,historySRV[previous],historyDepthSRV[previous],historyUAV[output],historyDepthUAV[output],historySRV[output],hierarchySRV});
        for(uint32_t mip=0;mip<hierarchyUAV.size();mip++)f.resourceTableData[uint32_t(ResourceDomain::History)][HistoryHiZMipBase+mip]={Physical(hierarchyUAV[mip]),Generation(hierarchyUAV[mip])};
        SetDomain(f,ResourceDomain::Animation,{f.dynamicSRV,f.dynamicUAV,deformationsSRV,f.motionSRV,f.boneSRV,f.weightSRV,skinSRV,morphSRV,f.deformedBoundsSRV,f.deformedBoundsUAV});
        SetDomain(f,ResourceDomain::Visibility,{f.hotBoundsSRV,f.hotBoundsUAV,f.hotMeshesSRV,f.hotMeshesUAV});
        SetDomain(f,ResourceDomain::RayTracing,{f.tlasSRV,f.rayUAV,f.blasAddressesSRV});
        c.resourceAbi={DescriptorIndex(f.resourceDirectorySRV),ResourceAbiVersion,0,0};c.dimensions={renderWidth,renderHeight,renderMips,0};c.outputDimensions={width,height,renderWidth,renderHeight};c.counts={coreSceneWork?f.instanceCount:0u,coreSceneWork?f.additionalCount:0u,coreSceneWork?f.lightCount:0u,coreSceneWork&&reflectionHistoryReady?1u:0u};c.topology={xr.viewCount,MsaaSamples,passPlan.reflections?1u:0u,0};c.timing={NearZ,FarZ,float(f.simulationTime),float(quality)};
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](uint32_t a,uint32_t b){auto distance=[&](uint32_t i){auto p=CpuInstance(f,i).positionRadius;auto delta=Sub({p.x,p.y,p.z},center);return Dot(delta,delta);};return distance(a)>distance(b);});
        }

        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useTexture=[&](uint32_t node,TextureResource& resource,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){frameGraph.Texture(node,&resource,resource.resource.Get(),resource.layout,mode,layout,sync,access);};

        if(surfacesDirty&&materials&&!surfaceData.empty()){
            UINT64 bytes=surfaceData.size()*sizeof(Surface),offset=Reserve(f,bytes);memcpy(f.uploadGeneration->mapped+offset,surfaceData.data(),size_t(bytes));
            auto node=frameGraph.AddPass("Materials.Update",GraphQueue::Direct,9,[&,offset,bytes](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyBufferRegion(materials.Get(),0,f.uploadGeneration->resource.Get(),offset,bytes);surfacesDirty=false;});
            useBuffer(node,materials.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        uint32_t coreColorNode=UINT32_MAX;
        if(coreSceneWork){
            auto uploadNode=frameGraph.AddPass("Upload",GraphQueue::Copy,10,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                if(!f.uploadGeneration)Fail("frame upload generation missing");auto uploadResource=f.uploadGeneration->resource.Get();auto copyAnimation=[&](ID3D12Resource* destination,UINT64 offset){l->CopyBufferRegion(destination,0,uploadResource,offset,destination->GetDesc().Width);};copyAnimation(f.motionBuffer.Get(),f.motionOffset);copyAnimation(f.boneBuffer.Get(),f.boneOffset);copyAnimation(f.weightBuffer.Get(),f.weightOffset);if(f.lightCount)l->CopyBufferRegion(f.lights.Get(),0,uploadResource,f.lightsOffset,UINT64(f.lightCount)*sizeof(Light));if(copyStatic){if(!residentStaticInstances)Fail("persistent static scene source missing");for(const auto& range:pendingStatic){UINT64 offset=UINT64(range.first)*sizeof(GpuInstance),bytes=UINT64(range.count)*sizeof(GpuInstance);l->CopyBufferRegion(f.instances.Get(),offset,residentStaticInstances.Get(),offset,bytes);}}if(f.dynamicCount)l->CopyBufferRegion(f.instances.Get(),UINT64(staticCount)*sizeof(GpuInstance),uploadResource,f.objectsOffset,UINT64(f.dynamicCount)*sizeof(GpuInstance));
            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})if(resource)useBuffer(uploadNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic||f.dynamicCount)useBuffer(uploadNode,f.instances.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic)useBuffer(uploadNode,residentStaticInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);if(f.lightCount)useBuffer(uploadNode,f.lights.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }

        frameResources.Clear();auto frameAddress=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;
        frameResources.Set({GpuFrameResourceId::SceneDepth,depth.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,depth.layout,depthSRV,{}, {},Dsv(depthDSV),0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::DepthResolved,hierarchy.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,hierarchy.layout,hierarchySRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::HdrColor,hdr.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,hdr.layout,MsaaSamples==1?hdrResolvedSRV:GpuResourceHandle{},MsaaSamples==1?hdrResolvedUAV:GpuResourceHandle{},Rtv(hdrRTV),{},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedColor=MsaaSamples>1?hdrResolved:hdr;frameResources.Set({GpuFrameResourceId::ResolvedColor,resolvedColor.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedColor.layout,hdrResolvedSRV,hdrResolvedUAV,MsaaSamples==1?Rtv(hdrRTV):Rtv(hdrResolvedRTV), {},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedReflection=MsaaSamples>1?reflectionsResolved:reflections;frameResources.Set({GpuFrameResourceId::Reflections,resolvedReflection.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedReflection.layout,reflectionsResolvedSRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::ShadowAtlas,shadow.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,shadow.layout,shadowSRV,{}, {},Dsv(shadowDSV),0,0,0,3072,2048,1});
        frameResources.Set({GpuFrameResourceId::Lighting,f.lights.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightsSRV,{}, {},{},0,f.lights?f.lights->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::Clusters,f.clusters.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterSRV,f.clusterUAV,{}, {},0,f.clusters?f.clusters->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::TLAS,f.tlas.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.tlasSRV,{}, {},{},0,f.tlas?f.tlas->GetDesc().Width:0,f.tlas?f.tlas->GetGPUVirtualAddress():0,0,0,0});
        GpuFrameResource constants{GpuFrameResourceId::FrameConstants,f.uploadGeneration->resource.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,{}, {}, {},{},f.constantsOffset,sizeof(Constants),frameAddress,0,0,0};frameResources.Set(constants);constants.id=GpuFrameResourceId::ViewConstants;frameResources.Set(constants);
        for(const auto& bound:publications)publicationCatalog.Add(bound.publication);
        GpuFrameContext domainContext{&frameResources,xr.input.frameId,renderWidth,renderHeight,xr.viewCount,frameAddress,&publicationCatalog,frameSlot,uint32_t(frames.size()),f.simulationTime};

        // Stream copies logically precede the renderer consumers of instances/control.
        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);
        domainBuilder.SetDomain(opticalDomain->Identity());opticalDomain->BuildFields(domainBuilder,domainContext,frameResources);domainBuilder.SetDomain(GpuDomainId::Invalid);
        c.resourceAbi.z=opticalDomain->FieldDescriptor();c.resourceAbi.w=opticalDomain->FieldCount();



        if(coreSceneWork){
            auto computeNode=frameGraph.AddPass("DeformCull",GraphQueue::Compute,30,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);auto dispatchPublished=[&](ID3D12GraphicsCommandList7* command,uint32_t pass,bool fullCapacity){command->SetComputeRoot32BitConstant(1,pass,1);command->SetComputeRoot32BitConstant(1,0,0);command->SetComputeRoot32BitConstant(1,f.cpuCount,2);if(f.cpuCount)command->Dispatch((f.cpuCount+63)/64,1,1);for(const auto& bound:meshDomain->Bindings()){command->SetComputeRoot32BitConstant(1,bound.base,0);command->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;uint64_t indirectOffset=controlBase+(fullCapacity?GpuMeshPublicationAbi::CapacityDispatchOffset:GpuMeshPublicationAbi::ActiveDispatchOffset);command->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),indirectOffset,nullptr,0);}command->SetComputeRoot32BitConstants(1,4,params,0);};if(passPlan.rayWork){l->SetPipelineState(rayBuildPSO.Get());for(const auto& range:rayCpuDirty){l->SetComputeRoot32BitConstant(1,range.first,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch((range.count+63)/64,1,1);}for(uint32_t bindingIndex:rayPublicationDirty){if(bindingIndex>=meshDomain->Bindings().size())Fail("ray publication dirty index");const auto& bound=meshDomain->Bindings()[bindingIndex];l->SetComputeRoot32BitConstant(1,bound.base,0);l->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::CapacityDispatchOffset,nullptr,0);}l->SetComputeRoot32BitConstants(1,4,params,0);}if(!f.deformationWork.Empty()){l->SetPipelineState(deformPSO.Get());for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch((meshes.ranges[mesh].vertexCount+63)/64,1,1);}BBarrier(l,f.dynamicVertices.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);l->SetPipelineState(deformBoundsPSO.Get());for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch(1,1,1);}BBarrier(l,f.deformedBounds.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(hotDataPSO.Get());dispatchPublished(l,0,false);for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);ClearCount(l,f.count.Get(),f.countUAV);l->SetPipelineState(cullPSO.Get());dispatchPublished(l,0,false);ArgumentsReady(l,f.arguments.Get(),f.count.Get());if(f.shadowDirty){ClearCount(l,f.shadowCount.Get(),f.shadowCountUAV);uint32_t shadowParams[4]={0,2,f.cpuCount,0};l->SetComputeRoot32BitConstants(1,4,shadowParams,0);if(f.cpuCount)l->Dispatch((f.cpuCount+63)/64,1,1);ArgumentsReady(l,f.shadowArguments.Get(),f.shadowCount.Get());}});
            if(f.streamControl)useBuffer(computeNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);useBuffer(computeNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})useBuffer(computeNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(computeNode,f.rayDescriptors.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);if(!f.deformationWork.Empty()){useBuffer(computeNode,f.dynamicVertices.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(computeNode,f.deformedBounds.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.arguments.Get(),f.count.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty)for(auto* resource:{f.shadowArguments.Get(),f.shadowCount.Get()})useBuffer(computeNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);

            auto depthNode=frameGraph.AddPass("Depth",GraphQueue::Direct,40,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);if(passPlan.rayQueries&&!f.blasWork.Empty())BuildDynamic(l,f);if(passPlan.rayWork){AliasMemory(l);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=f.instanceCount+f.additionalCount;build.Inputs.InstanceDescs=f.rayDescriptors->GetGPUVirtualAddress();build.DestAccelerationStructureData=f.tlas->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=f.scratch->GetGPUVirtualAddress();if(f.tlasBuilt&&f.lastTlasCount==f.instanceCount+f.additionalCount){build.SourceAccelerationStructureData=build.DestAccelerationStructureData;build.Inputs.Flags|=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;}l->BuildRaytracingAccelerationStructure(&build,0,nullptr);BBarrier(l,f.tlas.Get(),D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);f.lastTlasCount=f.instanceCount+f.additionalCount;f.tlasBuilt=true;}if(f.shadowDirty){auto dsv=Dsv(shadowDSV);l->OMSetRenderTargets(0,nullptr,FALSE,&dsv);l->SetPipelineState(shadowPSO.Get());for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot)){uint32_t x=(slot%2)*1536,y=(slot/2)*1024;D3D12_RECT rectangle{LONG(x),LONG(y),LONG(x+1536),LONG(y+1024)};l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,1,&rectangle);for(uint32_t face=0;face<6;face++){Viewport(l,512,512,x+(face%3)*512,y+(face/3)*512);l->SetGraphicsRoot32BitConstant(1,slot*6+face,1);l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.shadowArguments.Get(),0,f.shadowCount.Get(),0);}}EmitBarriers(l,compiled.barriersDuring[0]);}Viewport(l,renderWidth,renderHeight);auto depthHandle=Dsv(depthDSV);l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);l->SetPipelineState(depthPSO.Get());l->SetGraphicsRoot32BitConstants(1,4,params,0);l->ExecuteIndirect(indirect.Get(),f.instanceCount,f.arguments.Get(),0,f.count.Get(),0);});
            useBuffer(depthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(depthNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(depthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayWork)useBuffer(depthNode,f.rayDescriptors.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);for(auto* resource:{f.arguments.Get(),f.count.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.shadowDirty)for(auto* resource:{f.shadowArguments.Get(),f.shadowCount.Get()})useBuffer(depthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(passPlan.rayWork)useBuffer(depthNode,f.tlas.Get(),GraphMode::ReadWrite,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ|D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);useTexture(depthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);if(f.shadowDirty)useTexture(depthNode,shadow,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);

            if(f.shadowDirty)frameGraph.TransitionTexture(depthNode,shadow,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
            frameGraph.TransitionTexture(depthNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            uint32_t visibilityNode=UINT32_MAX;
            if(passPlan.depthHierarchy||passPlan.clusteredLights){
                visibilityNode=frameGraph.AddPass("Visibility",GraphQueue::Compute,50,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                    if(passPlan.depthHierarchy){
                        l->SetPipelineState(depthResolvePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);
                        if(passPlan.occlusion){
                            auto mipReady=[&](uint32_t mip){EmitBarriers(l,compiled.barriersDuring[mip]);};
                            mipReady(0);l->SetPipelineState(hizPSO.Get());for(uint32_t mip=1;mip<renderMips;mip++){l->SetComputeRoot32BitConstant(1,mip,1);l->Dispatch((std::max(1u,renderWidth>>mip)+7)/8,(std::max(1u,renderHeight>>mip)+7)/8,viewCount);mipReady(mip);}for(uint32_t mip=renderMips;mip<mips;mip++)mipReady(mip);
                            ClearCount(l,f.cullCount.Get(),f.cullCountUAV);l->SetPipelineState(cullPSO.Get());auto dispatchPublished=[&](ID3D12GraphicsCommandList7* command,uint32_t passIndex){command->SetComputeRoot32BitConstant(1,passIndex,1);command->SetComputeRoot32BitConstant(1,0,0);command->SetComputeRoot32BitConstant(1,f.cpuCount,2);if(f.cpuCount)command->Dispatch((f.cpuCount+63)/64,1,1);for(const auto& bound:meshDomain->Bindings()){command->SetComputeRoot32BitConstant(1,bound.base,0);command->SetComputeRoot32BitConstant(1,Invalid,2);uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;command->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);}command->SetComputeRoot32BitConstants(1,4,params,0);};dispatchPublished(l,1);ArgumentsReady(l,f.cullArguments.Get(),f.cullCount.Get());
                        }else EmitBarriers(l,compiled.barriersDuring[0]);
                    }
                    if(passPlan.clusteredLights){l->SetPipelineState(clusterPSO.Get());l->Dispatch((((renderWidth+63)/64)*((renderHeight+63)/64)*24*viewCount+63)/64,1,1);}
                
                });
                if(passPlan.depthHierarchy){useTexture(visibilityNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(visibilityNode,hierarchy,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.occlusion){useBuffer(visibilityNode,f.instances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(f.streamControl)useBuffer(visibilityNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);for(auto* resource:{f.cullArguments.Get(),f.cullCount.Get()})useBuffer(visibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);}
                if(passPlan.clusteredLights){useBuffer(visibilityNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useBuffer(visibilityNode,f.clusters.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);}
                if(passPlan.depthHierarchy){
                    if(passPlan.occlusion)for(uint32_t mip=0;mip<mips;++mip){
                        D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=mip;range.NumMipLevels=1;range.NumArraySlices=viewCount;range.NumPlanes=1;
                        frameGraph.TransitionTextureSubresource(visibilityNode,hierarchy,range,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,mip+1==mips);
                    }
                    else frameGraph.TransitionTexture(visibilityNode,hierarchy,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,true);
                }
            }
            ID3D12Resource* opaqueArguments=passPlan.occlusion?f.cullArguments.Get():f.arguments.Get();ID3D12Resource* opaqueCount=passPlan.occlusion?f.cullCount.Get():f.count.Get();

            auto opaqueNode=frameGraph.AddPass("Opaque",GraphQueue::Direct,60,[&,opaqueArguments,opaqueCount](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);Viewport(l,renderWidth,renderHeight);auto depthHandle=Dsv(depthDSV);D3D12_CPU_DESCRIPTOR_HANDLE targets[]={Rtv(hdrRTV),Rtv(reflectionsRTV)};float clear[]={.008f,.013f,.021f,1},black[]={0,0,0,0};l->ClearRenderTargetView(targets[0],clear,0,nullptr);if(passPlan.reflections)l->ClearRenderTargetView(targets[1],black,0,nullptr);l->OMSetRenderTargets(passPlan.reflections?2:1,targets,FALSE,&depthHandle);D3D12_SHADING_RATE_COMBINER combiners[]={D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());l->ExecuteIndirect(indirect.Get(),f.instanceCount,opaqueArguments,0,opaqueCount,0);if(f.transparentCount){l->SetPipelineState((passPlan.reflections?transparentPSO:transparentDirectPSO).Get());for(uint32_t index=0;index<f.transparentCount;index++){uint32_t object=f.transparent[index];l->SetGraphicsRoot32BitConstant(1,object,0);l->DispatchMesh(meshes.ranges[CpuInstance(f,object).mesh].meshletCount,1,1);}}});
            if(const auto* fields=frameResources.Find(GpuFrameResourceId::PopulationOpticalFields))useBuffer(opaqueNode,fields->resource,GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:domainContext.lightOcclusionTextures)frameGraph.Texture(opaqueNode,nullptr,field->resource,D3D12_BARRIER_LAYOUT_COMMON,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueNode;
            useBuffer(opaqueNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{opaqueArguments,opaqueCount})useBuffer(opaqueNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)useBuffer(opaqueNode,f.clusters.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.rayQueries)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(MsaaSamples>1){frameGraph.TransitionTexture(opaqueNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueNode,reflections,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);}else{frameGraph.TransitionTexture(opaqueNode,hdr,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueNode,reflections,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}frameGraph.TransitionTexture(opaqueNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(MsaaSamples>1){
                auto resolveNode=frameGraph.AddPass("ResolveColor",GraphQueue::Direct,70,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){for(uint32_t view=0;view<viewCount;view++){l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);if(passPlan.reflections)l->ResolveSubresource(reflectionsResolved.resource.Get(),view,reflections.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);}});
                coreColorNode=resolveNode;
                useTexture(resolveNode,hdr,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);if(passPlan.reflections){useTexture(resolveNode,reflections,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,reflectionsResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);}
                frameGraph.TransitionTexture(resolveNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(resolveNode,reflectionsResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }

            if(passPlan.reflections){
                auto postNode=frameGraph.AddPass("Post",GraphQueue::Direct,90,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(denoisePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(postNode,resolvedReflection,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,hierarchy,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(reflectionHistoryReady){useTexture(postNode,history[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,historyDepth[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}useTexture(postNode,history[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);useTexture(postNode,historyDepth[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                frameGraph.TransitionTexture(postNode,history[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);frameGraph.TransitionTexture(postNode,historyDepth[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                auto mergeNode=frameGraph.AddPass("Reflection.Composite",GraphQueue::Direct,91,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t mergeParams[]{0,0,Physical(hdrResolvedUAV),0};l->SetComputeRoot32BitConstants(1,4,mergeParams,0);l->SetPipelineState(mergeReflectionPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(mergeNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(mergeNode,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);coreColorNode=mergeNode;
            }

        }else{
            auto clearNode=frameGraph.AddPass("Core.ClearVisualTarget",GraphQueue::Direct,60,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Viewport(l,renderWidth,renderHeight);
                const float color[]={.008f,.013f,.021f,1.0f};auto rtv=Rtv(hdrRTV);auto dsv=Dsv(depthDSV);
                l->ClearRenderTargetView(rtv,color,0,nullptr);
                l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);
                EmitBarriers(l,compiled.barriersDuring[0]);
                EmitBarriers(l,compiled.barriersDuring[1]);
                if(MsaaSamples>1){
                    EmitBarriers(l,compiled.barriersDuring[2]);
                    for(uint32_t view=0;view<viewCount;++view)l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
            });
            useTexture(clearNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
            useTexture(clearNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
            if(MsaaSamples>1)useTexture(clearNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);
            frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET,true);
            frameGraph.TransitionTexture(clearNode,depth,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,true);
            if(MsaaSamples>1){
                frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,true);
                frameGraph.TransitionTexture(clearNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
            coreColorNode=clearNode;
        }

        if(!captureTicket.request)capture.Take(captureTicket);
        if(captureTicket.request&&captureTicket.options.eye>=viewCount){capture.Failed(captureTicket,"SELECTED EYE UNAVAILABLE");captureTicket={};}
        if(terminalDomain)terminalDomain->SetSuppressed(captureTicket.request&&!captureTicket.options.includeInterface);
        // Scene products have now been declared. External domains consume the
        // current versions and publish subsequent mutations before Present.
        // Cross-domain consumers registered first name their content version.
        if(aaMode){
            if(!aaSourceLease){GpuTextureDesc d{};d.width=renderWidth;d.height=renderHeight;d.depthOrArraySize=viewCount;d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.uav=false;d.name=L"FXAA.SpatialSource";aaSourceLease=CreateManagedTexture2D(d);if(!aaSourceLease)Fail("FXAA source allocation");aaSource.resource=aaSourceLease->resource;aaSource.layout=D3D12_BARRIER_LAYOUT_COMMON;}
            auto copy=frameGraph.AddPass("FXAA.Snapshot",GraphQueue::Direct,92,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyResource(aaSource.resource.Get(),resolvedColor.resource.Get());});
            useTexture(copy,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);useTexture(copy,aaSource,GraphMode::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            auto filter=frameGraph.AddPass("FXAA.Spatial",GraphQueue::Direct,93,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t p[]{DescriptorIndex(aaSourceLease->srv),Physical(hdrResolvedUAV),aaMode,0};l->SetComputeRoot32BitConstants(1,4,p,0);l->SetPipelineState(fxaaPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
            useTexture(filter,aaSource,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(filter,resolvedColor,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
        for(auto& domain:externalDomains){domainBuilder.SetDomain(domain->Identity());domain->BuildPasses(domainBuilder,domainContext);}domainBuilder.SetDomain(GpuDomainId::Invalid);

        void* captureRecord{};
        auto presentNode=frameGraph.AddPass("Present",GraphQueue::Direct,100,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Viewport(l,width,height);auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);l->OMSetRenderTargets(1,&target,FALSE,&ds);Bind(l,f,true);l->SetGraphicsRoot32BitConstants(1,4,params,0);l->SetGraphicsRoot32BitConstant(1,passPlan.reflections?1u:0u,3);l->SetPipelineState(presentPSO.Get());l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->DrawInstanced(3,1,0,0);
            if(captureRecord){
                auto* slot=static_cast<TextureReadback*>(captureRecord);
                D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource=slot->source.Get();barrier.Transition.Subresource=slot->subresource;
                barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
                l->ResourceBarrier(1,&barrier);Graphics::RecordTextureReadback(captureRecord,l);
                std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);l->ResourceBarrier(1,&barrier);
            }
        });useTexture(presentNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)useTexture(presentNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(presentNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

        frameGraph.Dependency(coreColorNode,presentNode);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom)){LogEvent(traceFrame,"render.skip","reason=managed_domain_residency_budget");releasePublications(true);publications.clear();managedMemory.EndFrame(SubmittedFences());endDomains();active=nullptr;historyReady=false;return;}
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());
        if(captureTicket.request){
            // The copy is part of Present itself. READBACK heaps stay COPY_DEST;
            // they require no enhanced barrier or separate frame-graph pass.
            auto handle=facade->RequestTextureReadback(xrColorTargets[xr.color].Get(),captureTicket.options.eye,xr.input.frameId,&captureRecord);
            if(handle)captureReadbacks.push_back({captureTicket,handle,QpcNowSeconds()});
            else capture.Failed(captureTicket,"GPU CAPTURE FORMAT / ALLOCATION FAILED");
            captureTicket={};
        }
        for(const auto& bound:publications)frameGraph.PublicationComplete(bound.publication,presentNode);
        frameGraph.CompileExecutionBatches();
        auto batchCommands=[&](GraphQueue queue,uint32_t index)->Commands&{
            auto& slots=f.batchCommands[QI(queue)];while(slots.size()<=index){auto commands=std::make_unique<Commands>();CommandsInit(*commands,queue==GraphQueue::Copy?D3D12_COMMAND_LIST_TYPE_COPY:queue==GraphQueue::Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT);slots.push_back(std::move(commands));}return *slots[index];
        };
        if(performanceProfiler.active)frameGraph.ExecuteProfiled(performanceProfiler,*jobs,xr.input.frameId,batchCommands);
        else frameGraph.Execute(*jobs,xr.input.frameId,batchCommands);
        if(captureRecord){
            auto* slot=static_cast<TextureReadback*>(captureRecord);auto fence=frameGraph.Signal(presentNode);
            if(!slot->recorded||!fence){slot->failed=true;LogEvent(traceFrame,"capture.error","stage=copy_not_submitted");}
            else {slot->fenceValue=fence;slot->recorded=false;LogEvent(traceFrame,"capture.gpu","frame=%llu eye=%u fence=%llu",static_cast<unsigned long long>(slot->frameId),slot->subresource,static_cast<unsigned long long>(fence));}
        }
        FinishTextureReadbacks();if(copyStatic)sceneCache.Commit(frameSlot);if(coreSceneWork&&(!rayDirty||passPlan.rayWork))rayScene.Commit(frameSlot);if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;xr.submitted=true;LogEvent(traceFrame,"gpu.ray_scene","tlas_update=%u cpu_dirty_ranges=%u renewed_streams=%u",unsigned(passPlan.rayWork),uint32_t(rayCpuDirty.size()),uint32_t(rayPublicationDirty.size()));
    }
    ~Impl(){if(device){residency.Wait(direct.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(copy.queue.Get());WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);publications.clear();externalDomains.clear();terminalDomain.reset();atmosphereDomain.reset();opticalDomain.reset();aaSource={};aaSourceLease.reset();managedMemory.EndFrame(SubmittedFences());CollectManaged();managedMemory.Shutdown();vramCache.Shutdown(CompletedFences());descriptorAllocator.EndRecording(SubmittedFences());descriptorAllocator.Collect(CompletedFences());}library.Reset();}
};
uint32_t Graphics::SetFogSettings(uint64_t owner,const LabFogSettingsV1& settings){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetSettings(owner,settings);
}
uint32_t Graphics::SetFogWind(uint64_t owner,const LabFogWindV1& wind){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetWind(owner,wind):0;}
uint32_t Graphics::SetFogInput(uint64_t owner,const LabFogInputV1& input){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetInput(owner,input):0;}
void Graphics::ClearAtmosphere(uint64_t owner){if(impl_->atmosphereDomain)impl_->atmosphereDomain->Clear(owner);if(impl_->opticalDomain)impl_->opticalDomain->Clear(owner);}
uint32_t Graphics::SetFogTransport(uint64_t owner,const LabFogTransportV1& s){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetTransport(owner,s);
}
uint32_t Graphics::SetAntialiasing(uint64_t owner,const LabAntialiasingV1& s){
    if(!owner||s.structSize<sizeof(s)||s.mode>2)return 0;for(auto& c:impl_->aaClients)if(c.owner==owner){c.mode=s.mode;impl_->aaMode=s.mode;return 1;}impl_->aaClients.push_back({owner,s.mode});impl_->aaMode=s.mode;return 1;
}
uint32_t Graphics::SetMultisampling(uint64_t owner,const LabMultisamplingV1& s){
    if(!owner||s.structSize<sizeof(s)||(s.samples!=1&&s.samples!=4))return 0;
    if(s.samples==4)for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        if(FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))||!q.NumQualityLevels)return 0;
    }
    auto& clients=impl_->msaaClients;auto it=std::find_if(clients.begin(),clients.end(),[&](const auto& c){return c.owner==owner;});
    if(it==clients.end())clients.push_back({owner,s.samples});else it->samples=s.samples;
    impl_->requestedSamples=clients.back().samples;return 1;
}
uint32_t Graphics::SetSurfaceSettings(uint64_t owner,const LabSurfaceSettingsV1& s){
    if(!owner||s.structSize<sizeof(s)||s.material>65535||(!impl_->surfaceData.empty()&&s.material>=impl_->surfaceData.size()))return 0;
    const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(!std::isfinite(v[i])||v[i]<lo[i]||v[i]>hi[i])return 0;
    for(auto& e:impl_->surfaceEdits)if(e.owner==owner&&e.settings.material==s.material){if(memcmp(&e.settings,&s,sizeof(s))){e.settings=s;impl_->ApplySurfaceEdits();}return 1;}impl_->surfaceEdits.push_back({owner,s});impl_->ApplySurfaceEdits();return 1;
}
void Graphics::ClearRendering(uint64_t owner){
    std::erase_if(impl_->msaaClients,[&](const auto& c){return c.owner==owner;});impl_->requestedSamples=impl_->msaaClients.empty()?LAB_MSAA_SAMPLES:impl_->msaaClients.back().samples;
    std::erase_if(impl_->aaClients,[&](const auto& c){return c.owner==owner;});impl_->aaMode=impl_->aaClients.empty()?0:impl_->aaClients.back().mode;
    if(std::erase_if(impl_->surfaceEdits,[&](const auto& c){return c.owner==owner;}))impl_->ApplySurfaceEdits();
}
uint32_t Graphics::SetPopulationOpticalSettings(uint64_t owner,const LabPopulationOpticalSettingsV1& s){return impl_->opticalDomain?impl_->opticalDomain->SetSettings(owner,s):0;}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV1& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV2& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::HitTestTerminal(uint64_t owner,const LabTerminalHitQueryV2& q,LabTerminalHitV2& hit)const{return impl_->terminalDomain?impl_->terminalDomain->HitTest(owner,q,hit):0;}
uint32_t Graphics::RequestCapture(uint64_t owner,const LabCaptureRequestV1& r,LabU64& out){return impl_->capture.Request(owner,r,out)?1u:0u;}
uint32_t Graphics::CaptureStatus(uint64_t owner,LabCaptureStatusV1& s)const{return impl_->capture.Status(owner,s)?1u:0u;}
void Graphics::ClearCapture(uint64_t owner){impl_->capture.Clear(owner);if(impl_->captureTicket.owner==owner)impl_->captureTicket={};}
void Graphics::PumpCaptures(){
    impl_->capture.Pump();
    if(impl_->captureTicket.request&&QpcNowSeconds()-impl_->captureTicket.queuedAt>30){impl_->capture.Failed(impl_->captureTicket,"NO RENDERED EYE AVAILABLE");impl_->captureTicket={};}
    for(auto it=impl_->captureReadbacks.begin();it!=impl_->captureReadbacks.end();){TextureReadbackResult r;auto state=PollTextureReadback(it->readback,r);if(!state){
            if(QpcNowSeconds()-it->submittedAt>30){impl_->capture.Failed(it->ticket,"GPU READBACK TIMEOUT");ReleaseTextureReadback(it->readback);it=impl_->captureReadbacks.erase(it);}
            else ++it;
            continue;
        }
        if(state==1&&r.tightRowBytes==r.width*4){std::vector<unsigned char> pixels(size_t(r.tightRowBytes)*r.height);for(uint32_t y=0;y<r.height;y++)std::memcpy(pixels.data()+size_t(y)*r.tightRowBytes,static_cast<const unsigned char*>(r.bytes)+size_t(y)*r.rowPitch,r.tightRowBytes);
            bool bgra=r.format==DXGI_FORMAT_B8G8R8A8_UNORM||r.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;impl_->capture.Encode(it->ticket,r.frameId,r.width,r.height,bgra,std::move(pixels));
        }else impl_->capture.Failed(it->ticket,"GPU CAPTURE READBACK FAILED");ReleaseTextureReadback(it->readback);it=impl_->captureReadbacks.erase(it);
    }
}
void Graphics::ClearTerminal(uint64_t owner){if(impl_->terminalDomain)impl_->terminalDomain->Clear(owner);}
Graphics::Graphics():impl_(std::make_unique<Impl>()){impl_->facade=this;}Graphics::~Graphics()=default;void Graphics::Initialize(const XrGraphicsRequirementsD3D12KHR& r,bool gpuValidation){impl_->Initialize(r,gpuValidation);}void Graphics::Configure(const XR& x,const SceneData& assets,JobSystem& jobs,RenderContributions extra){impl_->Configure(x,assets,jobs,extra);}void Graphics::ReplaceScene(const SceneData& assets,RenderContributions extra){impl_->ReplaceScene(assets,extra);}void Graphics::UnloadScene(){impl_->UnloadScene();}bool Graphics::SceneLoaded()const{return impl_->sceneLoaded;}void Graphics::RegisterDomain(std::unique_ptr<GpuDomain> domain){impl_->RegisterDomain(std::shared_ptr<GpuDomain>(std::move(domain)));}void Graphics::RegisterSharedDomain(std::shared_ptr<GpuDomain> domain){impl_->RegisterDomain(std::move(domain));}void Graphics::InitializeDomain(GpuDomain& domain){if(!impl_->domainServicesReady)Fail("GPU domain services not initialized");domain.Initialize(*impl_);}bool Graphics::UnregisterDomain(GpuWorldDomain id){return impl_->UnregisterDomain(id);}void Graphics::AttachStream(GpuStream& stream){if(stream.Domain()==GpuDomainId::Invalid||!impl_->FindDomain(stream.Domain()))Fail("GPU stream domain is not registered");impl_->streams.push_back(&stream);impl_->publications.reserve(impl_->streams.size());}void Graphics::DetachStream(GpuStream& stream){impl_->DetachStream(stream);}bool Graphics::Prepare(const FrameInput& s){return impl_->Prepare(s);}GpuMemoryStats Graphics::MemoryStats(){impl_->CollectManaged();return impl_->MemoryStats();}GpuValidationState Graphics::ValidationState()const{return impl_->validation.State(impl_->device.Get());}void Graphics::Render(XRFrame& f){impl_->Render(f);}void Graphics::WaitIdle(){impl_->WaitStartup(impl_->direct);impl_->WaitStartup(impl_->compute);impl_->WaitStartup(impl_->copy);}ID3D12Device* Graphics::Device()const{return impl_->device.Get();}ID3D12RootSignature* Graphics::BindlessRootSignature()const{return impl_->root.Get();}ID3D12DescriptorHeap* Graphics::BindlessResourceHeap()const{return impl_->descriptors.Get();}ID3D12DescriptorHeap* Graphics::BindlessSamplerHeap()const{return impl_->samplers.Get();}GpuResourceLease Graphics::CreateManagedBuffer(const GpuBufferDesc& d){return impl_->CreateManagedBuffer(d);}GpuResourceLease Graphics::CreateManagedTexture2D(const GpuTextureDesc& d){return impl_->CreateManagedTexture2D(d);}GpuResourceLease Graphics::CreateManagedTexture3D(const GpuTextureDesc& d){return impl_->CreateManagedTexture3D(d);}GpuResourceLease Graphics::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){return impl_->UploadStaticBuffer(bytes,stride,name);}GpuResourceLease Graphics::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){return impl_->UploadStaticTexture2D(d,subresources);}uint32_t Graphics::DescriptorIndex(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorIndex(h):Invalid;}uint32_t Graphics::DescriptorGeneration(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorGeneration(h):0;}ID3D12CommandQueue* Graphics::Queue()const{return impl_->direct.queue.Get();}
}

namespace lab {
uint64_t Graphics::RequestTextureReadback(ID3D12Resource* source,uint32_t subresource,uint64_t frameId,void** context){
    if(context)*context=nullptr;if(!source||!context)return 0;auto d=source->GetDesc();
    // OpenXR creates typeless storage for typed sRGB swapchains. Preserve its
    // format in the copy footprint, and type only the returned CPU pixel data.
    auto pixelFormat=d.Format;
    if(pixelFormat==DXGI_FORMAT_R8G8B8A8_TYPELESS)pixelFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
    else if(pixelFormat==DXGI_FORMAT_B8G8R8A8_TYPELESS)pixelFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||subresource>=uint32_t(d.MipLevels)*d.DepthOrArraySize||
       (pixelFormat!=DXGI_FORMAT_R8G8B8A8_UNORM&&pixelFormat!=DXGI_FORMAT_B8G8R8A8_UNORM&&pixelFormat!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&pixelFormat!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))return 0;
    Impl::TextureReadback* slot=nullptr;auto completed=impl_->direct.fence->GetCompletedValue();
    for(auto& item:impl_->textureReadbacks)if(item->released&&(!item->fenceValue||completed>=item->fenceValue)&&!item->recorded){slot=item.get();break;}
    if(!slot){impl_->textureReadbacks.push_back(std::make_unique<Impl::TextureReadback>());slot=impl_->textureReadbacks.back().get();}
    if(slot->mapped){slot->buffer->Unmap(0,nullptr);slot->mapped=nullptr;}
    UINT rows{};UINT64 rowBytes{},bytes{};impl_->device->GetCopyableFootprints(&d,subresource,1,0,&slot->footprint,&rows,&rowBytes,&bytes);
    if(!slot->buffer||slot->capacity<bytes){
        slot->buffer.Reset();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(impl_->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->buffer)))){slot->released=true;return 0;}slot->capacity=bytes;
    }
    slot->source=source;slot->pixelFormat=pixelFormat;slot->handle=impl_->nextTextureReadback++;slot->frameId=frameId;slot->subresource=subresource;slot->tightRowBytes=uint32_t(rowBytes);slot->bytes=bytes;slot->fenceValue=0;slot->recorded=false;slot->released=false;slot->failed=false;*context=slot;return slot->handle;
}
void Graphics::RecordTextureReadback(void* context,ID3D12GraphicsCommandList7* commands){
    auto* slot=static_cast<Impl::TextureReadback*>(context);if(!slot||!commands||!slot->source)return;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=slot->buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=slot->footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=slot->source.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=slot->subresource;
    commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);slot->recorded=true;
}
uint32_t Graphics::PollTextureReadback(uint64_t handle,TextureReadbackResult& out){
    out={};for(auto& item:impl_->textureReadbacks){auto& slot=*item;if(slot.handle!=handle||slot.released)continue;
        if(slot.failed||impl_->direct.fence->GetCompletedValue()==UINT64_MAX)return 2;
        if(!slot.fenceValue||impl_->direct.fence->GetCompletedValue()<slot.fenceValue)return 0;
        if(!slot.mapped){D3D12_RANGE read{0,size_t(slot.bytes)};if(FAILED(slot.buffer->Map(0,&read,&slot.mapped))){slot.failed=true;return 2;}}
        out={slot.footprint.Footprint.Width,slot.footprint.Footprint.Height,uint32_t(slot.pixelFormat),slot.footprint.Footprint.RowPitch,slot.tightRowBytes,slot.bytes,slot.frameId,slot.mapped};return 1;
    }return 2;
}
void Graphics::PreparePerformanceCapture(){impl_->performanceProfiler.Prepare(impl_->device.Get(),impl_->copy,impl_->compute,impl_->direct);}
void Graphics::BeginPerformanceCapture(){impl_->performanceProfiler.Begin(impl_->copy,impl_->compute,impl_->direct);}
void Graphics::EndPerformanceCapture(){impl_->performanceProfiler.End();}
void Graphics::CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out){
    std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
    CollectPerformanceCapture(out,queues,gaps);
}
void Graphics::CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out,
    std::vector<GpuQueueFrameAnalysis>& queues,std::vector<GpuGapSample>& gaps){
    WaitIdle();impl_->performanceProfiler.Collect(out);queues.clear();gaps.clear();
    std::map<std::pair<uint64_t,PerformanceQueue>,std::vector<const GpuPerformancePassSample*>> groups;
    for(const auto& sample:out)groups[{sample.frameId,sample.queue}].push_back(&sample);
    const double toMs=1000.0/double(impl_->performanceProfiler.qpcFrequency);
    for(auto& [key,passes]:groups){
        std::stable_sort(passes.begin(),passes.end(),[](const auto* a,const auto* b){return a->startQpc<b->startQpc;});
        const double spanMs=(passes.back()->endQpc-passes.front()->startQpc)*toMs;
        double workMs=0;for(const auto* pass:passes)workMs+=(pass->endQpc-pass->startQpc)*toMs;
        queues.push_back({key.first,key.second,spanMs,workMs,std::max(0.0,spanMs-workMs),spanMs>0?workMs/spanMs*100.0:0.0});
        for(size_t i=1;i<passes.size();++i){
            double gapMs=(passes[i]->startQpc-passes[i-1]->endQpc)*toMs;
            if(gapMs>.05){auto boundary=passes[i]->batchIndex==passes[i-1]->batchIndex?GpuGapSample::Boundary::IntraBatch:passes[i]->crossQueueWait?GpuGapSample::Boundary::CrossQueue:GpuGapSample::Boundary::InterBatch;gaps.push_back({key.first,key.second,passes[i-1]->name,passes[i]->name,gapMs,boundary});}
        }
    }
}
GraphicsPerformanceInfo Graphics::PerformanceInfo() const {
    const auto& description=impl_->adapterDescription;
    int size=WideCharToMultiByte(CP_UTF8,0,description.Description,-1,nullptr,0,nullptr,nullptr);
    std::string name(size>0?size_t(size):0,'\0');
    if(size>0){WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name.data(),size,nullptr,nullptr);name.pop_back();}
    return {std::move(name),uint64_t(description.DedicatedVideoMemory),impl_->renderWidth,impl_->renderHeight,impl_->viewCount,impl_->copyQueueTimestamps,impl_->performanceProfiler.droppedPasses};
}
void Graphics::ReleaseTextureReadback(uint64_t handle){for(auto& item:impl_->textureReadbacks)if(item->handle==handle){item->released=true;return;}}
}
