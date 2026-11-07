#pragma once
#include "Core.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "GpuPublication.h"
#include "GpuLifetime.h"
namespace lab {
class Residency {
    template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
    struct Entry { ID3D12Pageable* object{}; uint64_t bytes{},residencyFence{},lastUse{};GpuFenceValues fences{}; bool resident{true},required{};GpuResidencyClass memoryClass{GpuResidencyClass::Required}; };
    std::vector<Entry> entries_;std::vector<ID3D12Pageable*> restoreObjects_;
    uint32_t count_{};
    ID3D12Device3* device_{}; IDXGIAdapter3* adapter_{};
    ComPtr<ID3D12Fence> fence_; uint64_t serial_{},useSerial_{};
    DXGI_QUERY_VIDEO_MEMORY_INFO budget_{};
    uint64_t evictions_{},restores_{};
    GpuFenceValues completed_{};
    uint32_t OldestEvictable(bool cacheOnly)const;
public:
    void Initialize(ID3D12Device3*,IDXGIAdapter3*);
    uint32_t Add(ID3D12Pageable*,uint64_t,GpuResidencyClass,bool resident=true);
    uint32_t Add(ID3D12Pageable* object,uint64_t bytes,bool pinned,bool resident=true){return Add(object,bytes,pinned?GpuResidencyClass::Pinned:GpuResidencyClass::Required,resident);}
    bool Reserve(uint64_t bytes,uint64_t completed,uint64_t headroom=0);
    void SetCompleted(const GpuFenceValues& completed){completed_=completed;}
    void Remove(uint32_t id);
    bool CanRemove(uint32_t id)const;
    bool Resident(uint32_t id)const;
    void RefreshBudget();
    void Begin();
    void Require(uint32_t id);
    bool Prepare(uint64_t completed,uint64_t headroom=0);
    void Wait(ID3D12CommandQueue*);
    void Submitted(uint64_t fence);
    void Submitted(const GpuFenceValues&);
    uint64_t Used() const {return budget_.CurrentUsage;}
    uint64_t Budget() const {return budget_.Budget;}
    uint64_t Evictions() const {return evictions_;}
    uint64_t Restores() const {return restores_;}
};

struct GpuCacheAcquire {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint32_t residency{Invalid};
    bool reused{};
    explicit operator bool()const{return resource!=nullptr;}
};

// Soft, content-addressed VRAM cache. Entries are committed resources so an idle
// entry can be destroyed independently of the renderer's pinned page heaps. The
// cache owns one COM reference; users explicitly Acquire/Release around scene use.
class GpuVramCache {
    using ComPtr=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Entry {
        GpuCacheKey key{};D3D12_RESOURCE_DESC desc{};ComPtr resource;GpuFenceValues fences{};
        uint64_t bytes{},lastUse{};uint32_t residency{Invalid},users{};bool valid{},autoRequire{};
    };
    ID3D12Device3* device_{};Residency* residency_{};std::vector<Entry> entries_;
    uint64_t useSerial_{};
    static bool Compatible(const D3D12_RESOURCE_DESC&,const D3D12_RESOURCE_DESC&);
    Entry* FindResource(ID3D12Resource*);
    const Entry* FindResource(ID3D12Resource*)const;
    bool Destroy(uint32_t,const GpuFenceValues&);
public:
    void Initialize(ID3D12Device3*,Residency&);
    GpuCacheAcquire AcquireExisting(const GpuCacheKey&,bool autoRequire);
    GpuCacheAcquire Acquire(const GpuCacheKey&,const D3D12_RESOURCE_DESC&,D3D12_RESOURCE_STATES,bool autoRequire,const GpuFenceValues& completed,uint64_t headroom=0,const wchar_t* name=nullptr);
    void Commit(ID3D12Resource*);
    void Release(ID3D12Resource*,const GpuFenceValues& submitted);
    void RequireActive();
    void Collect(const GpuFenceValues& completed);
    bool MakeRoom(uint64_t bytes,const GpuFenceValues& completed,uint64_t headroom=0);
    uint32_t ResidencyId(ID3D12Resource*)const;
    void Shutdown(const GpuFenceValues& completed);
};

class GpuManagedMemory {
    using ComPtr=Microsoft::WRL::ComPtr<ID3D12Resource>;
    struct Entry {
        ComPtr resource;std::weak_ptr<const GpuManagedResource> lease;
        std::vector<GpuResourceHandle> views;GpuFenceValues retirement{};
        uint64_t bytes{};uint32_t residency{Invalid};bool local{},retired{},pendingSubmission{};
    };
    ID3D12Device3* device_{};Residency* residency_{};GpuDomainServices* services_{};
    void* forgetContext_{};void(*forget_)(void*,ID3D12Resource*){};
    std::vector<Entry> entries_;std::vector<GpuResourceLease> framePins_;
    GpuFenceValues submitted_{},completed_{};uint64_t generation_{},physicalVram_{},refusals_{};
    GpuBudgetPolicy policy_;
    bool frameOpen_{};
    GpuResourceLease Create(const D3D12_RESOURCE_DESC&,D3D12_HEAP_TYPE,uint32_t stride,bool srv,bool uav,const wchar_t*);
public:
    void Initialize(ID3D12Device3*,Residency&,GpuDomainServices&,uint64_t physical,void* context,void(*forget)(void*,ID3D12Resource*));
    void Collect(const GpuFenceValues& submitted,const GpuFenceValues& completed);
    void BeginFrame(){framePins_.clear();frameOpen_=true;}
    void Pin(ID3D12Resource*);
    void EndFrame(const GpuFenceValues& submitted);
    void Shutdown(); // called after all queues are drained, before services die
    GpuResourceLease Buffer(const GpuBufferDesc&);
    GpuResourceLease Texture(const GpuTextureDesc&,bool volume);
    GpuMemoryStats Stats()const;
};
class TransientArena {
    template<class T> using ComPtr=Microsoft::WRL::ComPtr<T>;
    struct Request { D3D12_RESOURCE_DESC desc{}; D3D12_RESOURCE_STATES state{}; uint32_t first{},last{}; uint64_t size{},alignment{},offset{}; ComPtr<ID3D12Resource>* destination{}; };
    std::array<Request,64> requests_{}; uint32_t count_{};
    ComPtr<ID3D12Heap> heap_;
public:
    void Add(const D3D12_RESOURCE_DESC&,D3D12_RESOURCE_STATES,uint32_t first,uint32_t last,ComPtr<ID3D12Resource>&);
    void Build(ID3D12Device*,Residency&,uint64_t completed);
};
void AliasMemory(ID3D12GraphicsCommandList7*);
}
