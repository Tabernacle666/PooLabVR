#pragma once
#include <future>
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
#include "MaterialTuning.h"
#include "Jobs.h"
#include "RuntimeConfig.h"
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
struct BuildingImpact {
    static constexpr uint32_t GroundCraterHit=0xfffffffeu;
    XMFLOAT3 position{};
    uint32_t renderId{};
    XMFLOAT3 direction{};
    uint32_t projectile{};
    XMFLOAT3 origin{};
    float hitTime{};
    uint64_t geometryRevision{},reserved{};
};
static_assert(sizeof(BuildingImpact)==64);
struct GraphicsMetrics {
    float fps{-1};
    uint64_t vramUsed{},vramBudget{};
    uint32_t width{},height{};
};
enum class DlssPreset { Default, E };
enum class SceneReflections : uint32_t { Full, Selective, Ambient };
enum class SceneSolarVisibility : uint32_t { Adaptive, Exact, NoShadows };
enum class SceneAntialiasing : uint32_t { Native, FxaaLight, FxaaStrong, Msaa4 };
enum class PeripheralShading : uint32_t { Auto, FullRate };
enum class DlssRenderStatus : uint32_t { Pending, Disabled, Unavailable, SceneBlocked, MsaaBlocked, Active, EvaluationFailed };

struct RenderingOptions {
    SceneReflections reflections{SceneReflections::Full};
    SceneAntialiasing antialiasing{SceneAntialiasing::Native};
    PeripheralShading peripheral{PeripheralShading::Auto};
    SceneSolarVisibility solarVisibility{SceneSolarVisibility::Adaptive};
};

struct RenderingState {
    uint64_t frame{};
    uint32_t width{},height{},outputWidth{},outputHeight{};
    RenderingOptions applied;
    DlssRenderStatus dlss{DlssRenderStatus::Pending};
    bool canAdjustDlss{},msaa4Supported{},msaa4AllocationFailed{};
    uint32_t opticalCacheBytes{};
    // Actual submitted render plan, not just requested diagnostic settings.
    bool dxrEnabled{true},dxrQueries{},dxrTlasWork{},dxrDynamicBlasWork{},dxrForestBuildPending{};
    uint32_t dxrRaySlots{},dxrProbeDispatches{};
    uint32_t ptlasCapacity{},ptlasWrites{},ptlasAddressUpdates{};
    bool ptlasActive{};
    uint32_t solarHistoryRejectMask{};
    bool solarDisplayGap{},solarPrepassActive{};
};

struct DlssScaleControl {
    float value{1.f},minimum{},maximum{};
    bool available{};
};

enum class DirectGpuPass : uint32_t {
    Present,Membrane,OpaqueTail,Transparent,ResolveColor,ReflectionPost,ReflectionComposite,
    TlasBuild,TlasUpdate,Opaque,OpaqueResident,CpuDepth,DepthRemaining,DepthCut,DepthOccluders,SolarVisibility,RasterCull,RasterTotal,Count
};
constexpr uint32_t DirectGpuPassBit(DirectGpuPass pass){return 1u<<static_cast<uint32_t>(pass);}

struct SphereGpuTimings {
    uint64_t frame{};
    float directMs{-1.f},computeMs{-1.f};
    // Existing batch timestamps: first begin to last end on each queue.
    // Includes gaps; not a cross-queue critical path or GPU-present latency.
    float directSpanMs{-1.f},computeSpanMs{-1.f};
    std::array<float,static_cast<size_t>(DirectGpuPass::Count)> directPassMs=[]{
        std::array<float,static_cast<size_t>(DirectGpuPass::Count)> values{};
        values.fill(-1.f);
        return values;
    }();
    float PassMs(DirectGpuPass pass)const{return directPassMs[static_cast<size_t>(pass)];}
};
struct DlssDiagnosticState {
    float requestedScale{},gpuScale{};
    bool initialized{},fencePending{},reconstructed{};
    uint32_t releaseResult{},shutdownResult{};
    uint64_t gpuFrame{};
    float directMs{-1.f},computeMs{-1.f},dlssGpuMs{-1.f},dlssCpuMs{-1.f};
    float directPeakMs{-1.f},computePeakMs{-1.f};
    std::array<char,64> directPeakBatch{},computePeakBatch{};
};
struct MembraneSettings {
    float patternSpeed{.8f},opacity{.05f},deformation{8.f},glitter{1.55f};
    float glitterWidth{28.f},waveStrength{1.f},waveSpeed{1.f},waveDuration{1.05f};
    XMFLOAT3 tint{1,1,1};
};
struct AtmosphereSettings {
    XMFLOAT3 center{};
    float radius{};
    XMFLOAT3 color{1,1,1};
    float density{};
    float emissionReferencePower{};
    bool membrane{};
    MembraneSettings film;
};
struct OutputSurface {
    uint32_t width{},height{},viewCount{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    bool desktop{};
    std::vector<ID3D12Resource*> colors,depths;
};
struct DepthDiagnosticSample {
    uint64_t frame{};
    // Eligible candidates, dispatched instances, native meshlets, MG2 meshlets,
    // meshlets rejected by instance frustum before indirect dispatch.
    std::array<uint64_t,14> counts{};
};
struct OpticalDiagnosticSample {
    uint64_t frame{};
    // Actual sampled invocations, not extrapolated whole-frame ray totals.
    std::array<uint64_t,38> counts{};
};
struct CutDiagnosticSample {
    uint64_t frame{};
    uint32_t generation{},flags{},cacheError{},triangles{},removed{},blocks{},status{};
    uint32_t needed{},poolCapacity{},poolFree{},largestRange{};
    bool laser{};
};
struct Mg2ClusterStatus {
    Mg2RayBackend configured{},lastPublished{};
    const char* request{"auto"};
    const char* capability{"aucune_geometrie"};
    const char* lastFailure{"aucune"};
    int32_t code{},buildCode{};
    uint32_t driver{},adapterVendor{},adapterDevice{},meshes{},sourcePages{},publications{};
    uint64_t clasBytes{},blasBytes{},buildRequiredBytes{},buildAvailableBytes{};
    uint32_t ommMaterials{},ommNativeTriangles{},ommClasClusters{},ommClasTriangles{};
    uint64_t ommArrayBytes{};
    uint32_t variants{},routedInstances{},selectionCandidates{},routeOnlyPublications{},rasterMeshlets{};
    // Last completed selection snapshot, not the currently published generation.
    uint32_t selectionEligible{},selectionRouted{};
    uint32_t clasBuilt{},clasReused{},clasDuplicate{},generationReusePublications{};
    uint64_t blasBuilt{},blasReused{},clasSkippedByBlasReuse{};
    uint64_t accelerationCacheBytes{},accelerationCacheClas{},accelerationCacheBlas{},accelerationCacheEvictions{};
    float clasMs{-1.f},blasMs{-1.f};
    float selectWorkerMs{},packWorkerMs{};
    uint64_t sourceGeometryBytes{},rasterCacheBytes{},rasterCacheHits{},rasterCacheMisses{},
        rasterCacheEvictions{},rasterCacheEntries{};
    float buildWorkerMs{},allocateWorkerMs{},uploadWorkerMs{},submitWorkerMs{};
    float waitWorkerMs{},validateWorkerMs{},cleanupWorkerMs{};
    float candidateWorkerMs{},retireWorkerMs{},generationAgeMs{};
    uint32_t retiredGenerations{},retireBacklog{},unchangedSelections{},discardedSelections{};
    // CPU timings of the last render invocation, no GPU wait or query.
    uint64_t traceRenderFrame{};
    float traceResidentMs{},traceForestMs{},traceRayMs{},traceGraphMs{};
    float traceForestPublishMs{},traceForestRetireMs{},traceForestRequestMs{};
    uint32_t traceForestRayDirty{},traceForestRayCompared{},traceForestShadowDirty{};
    bool traceHudTick{};
};
class Graphics {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    RenderingOptions GetRenderingOptions()const;
    RenderingState GetRenderingState()const;
    bool SetRenderingOptions(const RenderingOptions&);
    void SetPrismProfile(uint32_t profile);
    void SetPrismSceneMask(uint32_t mask);
    bool SetMultisampling(uint32_t samples);
    bool SetAntialiasing(uint32_t mode);
    bool SetSurfaceSettings(const SurfaceSettings&);
    void SetMaterialTuning(std::span<const MaterialTuning>);
    void UpdateDlss(int direction);
    bool PrepareDlss(DlssPreset preset);
    bool SetDlssScale(float scale);
    DlssScaleControl GetDlssScaleControl()const;
    void SetDlssDiagnostic(bool enabled);
    void EnableSphereBaselineTiming(bool scenePasses=false);
    void EnableDepthDiagnostics();
    enum class DepthMode { Baseline, Selective, Hybrid };
    DepthMode RequestedDepthMode()const;
    DepthMode ActiveDepthMode()const;
    size_t ReadDepthDiagnostics(std::array<DepthDiagnosticSample,6>& samples);
    void SetDiagnosticRayTracing(bool enabled);
    void EnableMg2Diagnostic();
    Mg2ClusterStatus GetMg2ClusterStatus()const;
    void SetBuildingRevision(uint64_t revision,std::span<const SceneLight> lights);
    std::vector<BuildingImpact> DrainBuildingImpacts();
    void EnableSpherePair(const AtmosphereSettings& companion,uint32_t count=SpectaclePairCount);
    size_t ReadSphereGpuTimings(std::array<SphereGpuTimings,12>& output);
    void SetCutDiagnostic(bool enabled);
    void SetOpticalDiagnostic(bool enabled);
    size_t ReadCutDiagnostics(std::array<CutDiagnosticSample,6>& samples);
    size_t ReadOpticalDiagnostics(std::array<OpticalDiagnosticSample,6>& samples);
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
    bool CapturePending()const;
    std::future<bool> FinishCaptureAsync();
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
