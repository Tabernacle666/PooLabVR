#pragma once
#include "XR.h"
#include "../cartridge_api/LaboVRTerminal.h"
#include "../cartridge_api/LaboVRCapture.h"
#include "../cartridge_api/LaboVRRendering.h"
#include "../cartridge_api/LaboVRAtmosphere.h"
#include "../cartridge_api/LaboVROptical.h"
#include "RenderContributions.h"
#include <memory>
#include <span>
#include <cstddef>
#include <string>
#include <vector>
#include <array>
#include "GpuPublication.h"
#include "Assets.h"
#include "Jobs.h"
#include "GpuValidation.h"
namespace lab {
enum class PerformanceQueue : uint32_t {Copy,Compute,Direct};
struct GpuPerformancePassSample {
    uint64_t frameId{};
    PerformanceQueue queue{};
    std::string name;
    // Queue clock calibration places every timestamp on the same QPC timeline.
    double startQpc{},endQpc{},milliseconds{};
    uint32_t batchIndex{},nodeIndex{};
    bool crossQueueWait{};
    std::array<uint32_t,3> batchesPerQueue{};
};
struct GpuQueueFrameAnalysis {
    uint64_t frameId{};
    PerformanceQueue queue{};
    double spanMs{},workMs{},gapMs{},occupancyPercent{};
};
struct GpuGapSample {
    uint64_t frameId{};
    PerformanceQueue queue{};
    std::string previousPass,nextPass;
    double gapMs{};
    enum class Boundary {IntraBatch,InterBatch,CrossQueue};
    Boundary boundary{Boundary::InterBatch};
};
struct GraphicsPerformanceInfo {
    std::string adapter;
    uint64_t dedicatedVramBytes{};
    uint32_t renderWidth{},renderHeight{},viewCount{};
    bool copyQueueTimestamps{};
    uint64_t droppedGpuPasses{};
};
class Graphics {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    struct TextureReadbackResult {uint32_t width{},height{},format{},rowPitch{},tightRowBytes{};uint64_t byteCount{},frameId{};const void* bytes{};};
    uint64_t RequestTextureReadback(ID3D12Resource*,uint32_t subresource,uint64_t frameId,void** recordContext);
    static void RecordTextureReadback(void*,ID3D12GraphicsCommandList7*);
    uint32_t PollTextureReadback(uint64_t,TextureReadbackResult&);
    void ReleaseTextureReadback(uint64_t);
    uint32_t PublishTerminal(uint64_t,const LabTerminalFrameV1&);
    uint32_t PublishTerminal(uint64_t,const LabTerminalFrameV2&);
    uint32_t HitTestTerminal(uint64_t,const LabTerminalHitQueryV2&,LabTerminalHitV2&)const;
    uint32_t RequestCapture(uint64_t,const LabCaptureRequestV1&,LabU64&);
    uint32_t CaptureStatus(uint64_t,LabCaptureStatusV1&)const;
    void ClearCapture(uint64_t);
    void PumpCaptures();
    void ClearTerminal(uint64_t);
    uint32_t SetFogSettings(uint64_t,const LabFogSettingsV1&);
    uint32_t SetFogInput(uint64_t,const LabFogInputV1&);
    uint32_t SetFogWind(uint64_t,const LabFogWindV1&);
    uint32_t SetFogTransport(uint64_t,const LabFogTransportV1&);
    uint32_t SetMultisampling(uint64_t,const LabMultisamplingV1&);
    uint32_t SetAntialiasing(uint64_t,const LabAntialiasingV1&);
    uint32_t SetSurfaceSettings(uint64_t,const LabSurfaceSettingsV1&);
    void ClearRendering(uint64_t);
    void ClearAtmosphere(uint64_t);
    uint32_t SetPopulationOpticalSettings(uint64_t,const LabPopulationOpticalSettingsV1&);
    Graphics();~Graphics();
    void Initialize(const XrGraphicsRequirementsD3D12KHR&,bool gpuValidation=false);
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
    // Derives conservative per-frame render requirements from already staged scene metadata.
    bool Prepare(const FrameInput&);
    void Render(XRFrame&);
    GpuMemoryStats MemoryStats();
    GpuValidationState ValidationState()const;
    void WaitIdle();
    void PreparePerformanceCapture();
    void BeginPerformanceCapture();
    void EndPerformanceCapture();
    void CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out);
    void CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out,
        std::vector<GpuQueueFrameAnalysis>& queues,std::vector<GpuGapSample>& gaps);
    GraphicsPerformanceInfo PerformanceInfo() const;
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
