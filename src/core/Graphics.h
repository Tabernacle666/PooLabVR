#pragma once
#include "XR.h"
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
namespace OASIS {
struct GpuProducerCoordinator {
    std::shared_ptr<GpuSubmissionProgress> progress;
    std::function<bool()> fallbackSubmitted;
    std::function<void()> finishFallback;
};
struct SurfaceSettings {
    uint32_t material{};
    XMFLOAT3 color{};
    float roughness{},normalAmplitude{},normalFrequency{},junctionWidth{},junctionContrast{};
};
struct GraphicsMetrics {
    float fps{-1};
    uint64_t vramUsed{},vramBudget{};
    uint32_t width{},height{};
};
struct DlssDiagnosticState {
    float requestedScale{},gpuScale{};
    bool initialized{},fencePending{},reconstructed{};
    uint32_t releaseResult{},shutdownResult{};
    uint64_t gpuFrame{};
    float directMs{-1.f},computeMs{-1.f},dlssGpuMs{-1.f},dlssCpuMs{-1.f};
};
struct AtmosphereSettings {
    XMFLOAT3 center{};
    float radius{};
    XMFLOAT3 color{1,1,1};
    float density{};
    float emissionReferencePower{};
};
struct OutputSurface {
    uint32_t width{},height{},viewCount{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    bool desktop{};
    std::vector<ID3D12Resource*> colors,depths;
};
class Graphics {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    bool SetMultisampling(uint32_t samples);
    bool SetAntialiasing(uint32_t mode);
    bool SetSurfaceSettings(const SurfaceSettings&);
    void UpdateDlss(int direction);
    bool SetDlssScale(float scale);
    void SetDlssDiagnostic(bool enabled);
    DlssDiagnosticState GetDlssDiagnostic()const;
    void SetDeveloperMode(bool enabled);
    void SetAtmosphere(const AtmosphereSettings& settings);
    Graphics();~Graphics();
    void Initialize(const XrGraphicsRequirementsD3D12KHR&);
    void InitializeDesktop();
    void Configure(const XR&,const SceneData&,JobSystem&,RenderContributions={});
    void ConfigureOutput(const OutputSurface&,const SceneData&,JobSystem&,RenderContributions={});
    void ReleaseOutput();

    void ReplaceScene(const SceneData&,RenderContributions={});
    void UnloadScene();
    bool SceneLoaded()const;

    void RegisterDomain(std::unique_ptr<GpuDomain>);
    void RegisterSharedDomain(std::shared_ptr<GpuDomain>);
    void InitializeDomain(GpuDomain&);
    bool UnregisterDomain(GpuWorldDomain id);
    void AttachStream(GpuStream&);
    void DetachStream(GpuStream&);
    void ReserveStreams();
    void CancelReservedStreams();

    bool Prepare(const FrameInput&);
    void Render(XRFrame&,const GpuProducerCoordinator& coordinator={});
    void WaitIdle();
    void RequestCapture(const std::wstring& path);
    bool CaptureDone()const;
    bool CaptureFailed()const;
    bool SaveCapture();
    GraphicsMetrics GetMetrics()const;

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
