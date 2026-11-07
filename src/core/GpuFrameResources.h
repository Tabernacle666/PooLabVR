#pragma once
#include "GpuPublication.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <d3d12.h>
#include <dxgiformat.h>

namespace lab {

// Stable semantic identities for renderer products a pluggable domain may consume.
// New identities append; domains never depend on Graphics members or descriptor slots.
enum class GpuFrameResourceId:uint16_t {
    SceneDepth,
    DepthResolved,
    HdrColor,
    ResolvedColor,
    Reflections,
    ShadowAtlas,
    Lighting,
    Clusters,
    TLAS,
    FrameConstants,
    ViewConstants,
    PopulationOpticalFields,
    Count
};

struct GpuFrameResource {
    GpuFrameResourceId id{GpuFrameResourceId::Count};
    ID3D12Resource* resource{};
    GpuPublishedResourceType type{GpuPublishedResourceType::Buffer};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    D3D12_BARRIER_LAYOUT initialLayout{D3D12_BARRIER_LAYOUT_COMMON};
    GpuResourceHandle srv{},uav{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{},dsv{};
    uint64_t offset{},bytes{};
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress{};
    uint32_t width{},height{},layers{};
    explicit operator bool()const{return resource!=nullptr;}
};

class GpuFrameResources {
    std::array<GpuFrameResource,std::size_t(GpuFrameResourceId::Count)> entries_{};
public:
    void Clear(){entries_={};}
    void Set(const GpuFrameResource& value){
        auto index=std::size_t(value.id);if(index>=entries_.size())return;entries_[index]=value;
    }
    const GpuFrameResource* Find(GpuFrameResourceId id)const{
        auto index=std::size_t(id);if(index>=entries_.size()||!entries_[index])return nullptr;return &entries_[index];
    }
    const GpuFrameResource& Require(GpuFrameResourceId id)const{
        auto* value=Find(id);if(!value)throw "GPU frame semantic resource unavailable";return *value;
    }
};

struct GpuFrameContext {
    const GpuFrameResources* resources{};
    uint64_t frameId{};
    uint32_t width{},height{},viewCount{};
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants{};
    const GpuPublicationCatalog* publications{};
    uint32_t uploadSlot{},uploadSlotCount{};
    double timeSeconds{};
    std::span<const GpuResourceLease> lightOcclusionTextures{};
    std::span<const GpuPublication* const> lightOcclusionPublications{};
    const GpuFrameResource& Require(GpuFrameResourceId id)const{return resources->Require(id);}
    const GpuPublication* FindPublication(GpuWorldDomain domain,uint32_t ordinal=0)const{return publications?publications->FindPublication(domain,ordinal):nullptr;}
    const GpuPublishedResource* FindResource(GpuWorldDomain domain,uint32_t binding,uint32_t ordinal=0)const{return publications?publications->FindResource(domain,binding,ordinal):nullptr;}
};

}
