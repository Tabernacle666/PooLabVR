#pragma once
#include "Graphics.h"
#include "FrameGraphDependencies.h"
#include "Jobs.h"
#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace lab::graphics_internal {
struct TextureResource;
struct QueueState;
struct Commands;
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
    GraphResourceState& State(ID3D12Resource* resource,uint32_t subresource=UINT32_MAX);
    static bool Self(const GraphStamp& stamp,const GraphPass& pass);
    void Merge(GraphPass& pass,const GraphStamp& stamp);
    GraphStamp PendingStamp(const GraphPass& pass)const;
    void AutoBufferBarrier(GraphPass& pass,GraphResourceState& state,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers);
    void CompileTextureBarrier(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,const D3D12_BARRIER_SUBRESOURCE_RANGE* range=nullptr);
    void RecordTextureState(GraphPass& pass,GraphResourceState& state,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access);
    void Hazard(GraphPass& pass,GraphResourceState& state,GraphMode mode);
    void Pending(GraphPass& pass,GraphResourceState& state,const GraphBarrierState& finalState,TextureResource* texture);
public:
    void Initialize(QueueState& copy,QueueState& compute,QueueState& direct);
    void BeginFrame();
    void Forget(ID3D12Resource* resource);
    GraphPass BeginPass(const char* name,GraphQueue queue);
    void Buffer(GraphPass& pass,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,std::vector<CompiledBarrier>& barriers);
    void TextureRange(GraphPass& pass,ID3D12Resource* resource,TextureResource* canonical,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers,GpuTextureSubresources requested={});
    void Dependency(GraphPass& pass,ID3D12Resource* resource,GraphMode mode);
    void Dependency(GraphPass& pass,const GraphStamp& producer);
    void ExternalWait(GraphPass& pass,ID3D12Fence* fence,UINT64 value);
    void ExternalSignal(GraphPass& pass,ID3D12Fence* fence,UINT64 value);
    void TransitionTexture(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers);
    void TransitionTextureSubresource(GraphPass& pass,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& requested,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,bool completesWholeTexture=false);
    void Seal(GraphPass& pass);
    UINT64 FenceValue(GraphQueue queue)const;
    bool CrossQueueWait(const GraphPass& pass)const;
    void CompileSubmission(GraphPass& pass);
    UINT64 Submit(const GraphPass& pass,bool signalRequired,
        std::span<ID3D12CommandList* const> reusedDirectLists={},
        Commands* afterReusableLists=nullptr);

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
    // Closed direct lists are replayed only after the wrapper's graph barriers.
    std::vector<ID3D12CommandList*> reusableDirectLists;
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

    void Prepare(ID3D12Device* device,QueueState&,QueueState&,QueueState&);
    void Begin(QueueState& copy,QueueState& compute,QueueState& direct);
    void End();
    void PrepareFrame(uint32_t activePasses);
    Token ReservePass(GraphQueue queue);
    void FinishReservations();
    void BeginPass(Token token,uint64_t frameId,const char* name,ID3D12GraphicsCommandList7* commands,uint32_t batchIndex,uint32_t nodeIndex,bool crossQueueWait);
    void EndPass(Token token,ID3D12GraphicsCommandList7* commands);
    void Collect(std::vector<GpuPerformancePassSample>& out);
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
    Compiled& Compile();
public:
    explicit FrameGraphPlan(PassGraph& executor);
    void SetDiagnostics(bool enabled);
    void BeginFrame();
    uint32_t AddPass(const char* name,GraphQueue queue,uint64_t stableKey,std::function<void(const CompiledNode&,ID3D12GraphicsCommandList7*)> record={});
    FrameGraphNode& Node(uint32_t index);
    void SetStableKey(uint32_t index,uint64_t key);
    void Buffer(uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,uint32_t contentVersion=GpuContentVersion::Current);
    void Texture(uint32_t node,TextureResource* canonical,ID3D12Resource* resource,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,GpuTextureSubresources requested={},uint32_t contentVersion=GpuContentVersion::Current);
    void ExternalWait(uint32_t node,ID3D12Fence* fence,UINT64 value);
    void TransitionTexture(uint32_t node,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool during=false);
    void TransitionTextureSubresource(uint32_t node,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& range,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool completesWholeTexture=false);
    void ExternalSignal(uint32_t node,ID3D12Fence* fence,UINT64 value);
    void AfterAllUsers(uint32_t node,ID3D12Resource* resource);
    void Dependency(uint32_t from,uint32_t to);
    void Resolve();
    struct ExecutionBatch {
        GraphQueue queue{};
        std::vector<uint32_t> nodes;
        std::vector<CompiledNode> compiledNodes;
        std::vector<ID3D12CommandList*> reusableDirectLists;
        GraphPass pass{};
        struct Wait {GraphQueue sourceQueue{};uint64_t fenceValue{};};
        std::vector<Wait> waits;
        bool signalRequired{};
        uint64_t signalValue{};
    };
    struct BatchRecording {
        JobSystem::Group group;
        Commands* commands{};
        Commands* afterReusableLists{};
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

    void PublicationComplete(const GpuPublication& publication,uint32_t fallback);
    void CompileExecutionBatches();
    static void RecordExecutionBatch(void* context,uint32_t,uint32_t);
    void Execute(JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,
        const std::function<bool(const GraphPass&)>& canSubmit,const std::shared_ptr<GpuSubmissionProgress>& progress);
    void ExecuteProfiled(BurstGpuProfiler& profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,
        const std::function<bool(const GraphPass&)>& canSubmit,const std::shared_ptr<GpuSubmissionProgress>& progress);
    void ExecuteImpl(BurstGpuProfiler* profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,
        const std::function<bool(const GraphPass&)>& canSubmit,const std::shared_ptr<GpuSubmissionProgress>& progress);
    const std::vector<uint32_t>& ResolvedOrder()const;
    bool HasDependency(uint32_t from,uint32_t to)const;
    UINT64 Signal(uint32_t node)const;
    const std::array<UINT64,GraphQueueCount>& LastFences()const;
};
}
