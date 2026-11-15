#pragma once
#include "XR.h"
#include "../cartridge_api/LaboVRRendering.h"
#include "RenderContributions.h"
#include <memory>
#include <functional>
#include <span>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include "GpuPublication.h"
#include "Assets.h"
#include "Jobs.h"
namespace lab {
struct GpuProducerCoordinator {
    std::shared_ptr<GpuSubmissionProgress> progress;
    std::function<bool()> fallbackSubmitted;
    std::function<void()> finishFallback;
};
class Graphics {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    uint32_t SetMultisampling(uint64_t,const LabMultisamplingV1&);
    uint32_t SetAntialiasing(uint64_t,const LabAntialiasingV1&);
    uint32_t SetSurfaceSettings(uint64_t,const LabSurfaceSettingsV1&);
    void ClearRendering(uint64_t);
    Graphics();~Graphics();
    void Initialize(const XrGraphicsRequirementsD3D12KHR&);
    void Configure(const XR&,const SceneData&,JobSystem&,RenderContributions={});
    // Explicit persistent-scene boundary: rebuilds the canonical static GPU region once.
    void ReplaceScene(const SceneData&,RenderContributions={});
    void UnloadScene();
    bool SceneLoaded()const;
    // GPU world domains register once, then contribute publications and pass declarations without renderer-core switches.
    void RegisterDomain(std::unique_ptr<GpuDomain>);
    void RegisterSharedDomain(std::shared_ptr<GpuDomain>);
    void InitializeDomain(GpuDomain&);
    bool UnregisterDomain(GpuWorldDomain id);
    void AttachStream(GpuStream&);
    void DetachStream(GpuStream&);
    void ReserveStreams();
    void CancelReservedStreams();
    // Derives conservative per-frame render requirements from already staged scene metadata.
    bool Prepare(const FrameInput&);
    void Render(XRFrame&,const GpuProducerCoordinator& coordinator={});
    GpuMemoryStats MemoryStats();

    ID3D12Device* Device()const;
    ID3D12RootSignature* BindlessRootSignature()const;
    ID3D12DescriptorHeap* BindlessResourceHeap()const;
    ID3D12DescriptorHeap* BindlessSamplerHeap()const;
    GpuResourceLease CreateManagedBuffer(const GpuBufferDesc&);
    GpuResourceLease CreateManagedTexture2D(const GpuTextureDesc&);
    GpuResourceLease CreateManagedTexture3D(const GpuTextureDesc&);
    GpuResourceLease UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name);
    GpuResourceLease UploadStaticTexture2D(const GpuTextureDesc&,std::span<const GpuSubresourceUpload> subresources);
    uint32_t DescriptorIndex(GpuResourceHandle)const;
    uint32_t DescriptorGeneration(GpuResourceHandle)const;
    ID3D12CommandQueue* Queue()const;
};
}
