#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <span>
#include <d3d12.h>
#include <dxgiformat.h>
#include <wrl/client.h>
#include "GpuLifetime.h"
#include "GpuPublicationIndex.h"
#include "GpuSubresources.h"
#include "FrameGraphDependencies.h"

namespace lab {

// Domain IDs are open numeric identities. New domains define their ID in their own
// files; registering one never edits this generic contract.
using GpuWorldDomain=uint32_t;
namespace GpuDomainId {
inline constexpr GpuWorldDomain Invalid=0;
}

enum class GpuPublishedResourceType:uint8_t { Buffer,Texture2D,Texture3D };

struct GpuBufferDesc {
    uint64_t bytes{};uint32_t stride{4};
    D3D12_HEAP_TYPE heap{D3D12_HEAP_TYPE_DEFAULT};
    bool srv{true},uav{true};const wchar_t* name{};
};
struct GpuTextureDesc {
    uint32_t width{},height{},depthOrArraySize{1},mips{1};
    DXGI_FORMAT format{DXGI_FORMAT_R32_FLOAT};
    bool srv{true},uav{true};const wchar_t* name{};
    D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
};
struct GpuSubresourceUpload {
    const void* data{};
    uint64_t rowPitch{};
    uint64_t slicePitch{};
};
// The engine owns the COM resource, views, residency entry and retirement.
// A lease pins their common lifetime; published/frame leases must be retained
// until submission. Clients never release these descriptor handles themselves.
struct GpuManagedResource {
    ID3D12Resource* resource{};D3D12_RESOURCE_DESC desc{};
    uint64_t generation{},allocationBytes{};
    GpuResourceHandle srv{};std::vector<GpuResourceHandle> uavs;
    GpuResourceHandle Uav(uint32_t mip=0)const{return mip<uavs.size()?uavs[mip]:GpuResourceHandle{};}
};
using GpuResourceLease=std::shared_ptr<const GpuManagedResource>;
struct GpuMemoryStats {
    uint64_t physicalVram{},budget{},usage{},domainBudget{},managedBytes{},managedResidentBytes{},retiredBytes{},headroom{},allocationRefusals{};
    uint64_t descriptorsLive{},descriptorsRetired{},descriptorsRecycled{};
};

// The generic publication only transports resources and synchronization. Binding IDs
// and metadata are interpreted by the owning domain; there are no mesh-specific fields.
struct GpuPublishedResource {
    uint32_t binding{},flags{};
    GpuPublishedResourceType type{GpuPublishedResourceType::Buffer};
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    uint64_t offset{},bytes{}; // buffer range only; textures use the complete resource
    D3D12_BARRIER_LAYOUT initialLayout{D3D12_BARRIER_LAYOUT_COMMON};
    GpuResourceLease lease; // managed publications pin the engine generation
    uint64_t generation{};
    D3D12_RESOURCE_DESC description{};
    GpuResourceHandle srv{},uav{};
    std::shared_ptr<const void> metadata;
    explicit operator bool()const{
        if(!resource)return false;
        const auto desc=resource->GetDesc();
        if(type==GpuPublishedResourceType::Buffer)
            return desc.Dimension==D3D12_RESOURCE_DIMENSION_BUFFER&&bytes&&offset<=desc.Width&&bytes<=desc.Width-offset;
        if(type==GpuPublishedResourceType::Texture2D)
            return desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        return desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    }
};

struct GpuPublication {
    GpuWorldDomain domain{GpuDomainId::Invalid};
    // Dynamic storage: domains can publish any number of resources without changing
    // this contract or growing an ABI-wide fixed array.
    std::vector<GpuPublishedResource> resources;
    uint64_t generation{};
    Microsoft::WRL::ComPtr<ID3D12Fence> producerFence,consumerFence;
    uint64_t producerValue{},consumerValue{};
    // Optional domain-owned metadata. The generic renderer never casts or interprets it.
    std::shared_ptr<const void> domainMetadata;
    std::shared_ptr<void> keepAlive;
    void* token{};
    // Identity for returning the provider lease. This is independent of the
    // publication sequence carried in generation.
    uint64_t tokenGeneration{};
    uint64_t sourceOwner{}; // host identity, copied independently of public resource generations

    const GpuPublishedResource* Find(uint32_t binding)const{
        for(const auto& resource:resources)if(resource.binding==binding)return &resource;
        return nullptr;
    }
    explicit operator bool()const{
        if(domain==GpuDomainId::Invalid||resources.empty())return false;
        for(const auto& resource:resources)if(!resource)return false;
        return true;
    }
};

using GpuPublicationCatalog=GpuPublicationIndex<GpuPublication>;
inline void DescribePublication(GpuPublication& publication){
    for(auto& r:publication.resources){
        if(!r.resource)continue;
        r.description=r.resource->GetDesc();
        if(!r.generation)r.generation=r.lease?r.lease->generation:publication.generation;
        if(r.lease){if(!r.srv.Valid())r.srv=r.lease->srv;if(!r.uav.Valid())r.uav=r.lease->Uav();}
        if(!r.metadata)r.metadata=publication.domainMetadata;
    }
}

class GpuStream {
public:
    virtual ~GpuStream()=default;
    virtual GpuWorldDomain Domain()const noexcept=0;
    virtual GpuPublication Acquire()=0;
    virtual void Release(const GpuPublication&)=0;
};

enum class GpuPassQueue:uint8_t { Copy,Compute,Direct };
enum class GpuPassResourceAccess:uint8_t { Read,Write,ReadWrite };

struct GpuPassCallback {
    void* context{};
    void (*record)(void*,ID3D12GraphicsCommandList7*){};
    explicit operator bool()const{return record!=nullptr;}
};

// Domains can only describe work. Queue scheduling, DAG resolution, waits, signals
// and barriers remain renderer/pass-graph responsibilities.
class GpuPassBuilder {
public:
    virtual ~GpuPassBuilder()=default;
    virtual void BeginPass(const char* name,GpuPassQueue queue,GpuPassCallback callback={})=0;
    // Current binds at declaration; an explicit ordinal names an out-of-order
    // producer. ReadWrite ordinal N consumes N-1 and produces N on the same resource.
    virtual void Buffer(ID3D12Resource*,GpuPassResourceAccess,D3D12_BARRIER_SYNC,D3D12_BARRIER_ACCESS,uint32_t contentVersion=GpuContentVersion::Current)=0;
    virtual void Texture(ID3D12Resource*,GpuPassResourceAccess,D3D12_BARRIER_LAYOUT initialLayout,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC,D3D12_BARRIER_ACCESS,GpuTextureSubresources range={},uint32_t contentVersion=GpuContentVersion::Current)=0;
    virtual void EndPass()=0;
};

// Descriptor handles are logical/recyclable. A domain may build a private table of
// {resolved descriptor index,generation} entries without consuming a global ABI slot.
class GpuDomainServices {
public:
    virtual ~GpuDomainServices()=default;
    virtual ID3D12Device* Device()const noexcept=0;
    virtual ID3D12RootSignature* BindlessRootSignature()const noexcept=0;
    virtual ID3D12DescriptorHeap* BindlessResourceHeap()const noexcept=0;
    virtual ID3D12DescriptorHeap* BindlessSamplerHeap()const noexcept=0;
    virtual GpuResourceLease CreateManagedBuffer(const GpuBufferDesc&)=0;
    virtual GpuResourceLease CreateManagedTexture2D(const GpuTextureDesc&)=0;
    virtual GpuResourceLease CreateManagedTexture3D(const GpuTextureDesc&)=0;
    virtual GpuResourceLease UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name)=0;
    virtual GpuResourceLease UploadStaticTexture2D(const GpuTextureDesc&,std::span<const GpuSubresourceUpload> subresources)=0;
    virtual GpuMemoryStats MemoryStats()const=0;
    virtual GpuResourceHandle CreateBufferSrv(ID3D12Resource*,uint32_t count,uint32_t stride)=0;
    virtual GpuResourceHandle CreateBufferUav(ID3D12Resource*,uint32_t count,uint32_t stride)=0;
    virtual GpuResourceHandle CreateTextureSrv(ID3D12Resource*,DXGI_FORMAT)=0;
    virtual GpuResourceHandle CreateTextureUav(ID3D12Resource*,DXGI_FORMAT,uint32_t mip=0)=0;
    virtual uint32_t DescriptorIndex(GpuResourceHandle)const=0;
    virtual uint32_t DescriptorGeneration(GpuResourceHandle)const=0;
    virtual void ReleaseDescriptor(GpuResourceHandle&)=0;
};

struct GpuFrameContext;

class GpuDomain {
public:
    virtual ~GpuDomain()=default;
    virtual GpuWorldDomain Identity()const noexcept=0;
    virtual const char* Name()const noexcept=0;
    virtual void Initialize(GpuDomainServices&){}
    virtual void BeginFrame(uint64_t){}
    virtual GpuPublication Publish(){return {};}
    virtual bool ValidatePublication(const GpuPublication& publication)const{return publication&&publication.domain==Identity();}
    virtual void BindPublication(const GpuPublication&){}
    virtual void BuildPasses(GpuPassBuilder&,const GpuFrameContext&){}
    virtual void EndFrame(){}
    virtual int PresentationOrder()const noexcept{return 0;}
};

}
