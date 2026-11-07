#pragma once
#include "LaboVRServices.h"
#include "LaboVRVisualService.h"
#include "LaboVRInputService.h"
#include "LaboVRAudioService.h"
#include "LaboVRGpuService.h"
#include "VisualDomain.h"
#include "LaboVRGpuInteropService.h"
#include "LaboVRPhysicsService.h"
#include "ProfileStorage.h"
#include <map>
#include <mutex>
#include "Assets.h"
#include "InputSystem.h"
#include "GpuPublication.h"
#include <array>
#include <vector>
#include <string>
#include <filesystem>
#include <span>
namespace lab {
class Graphics;class Audio;
class PhysicsRuntime;
class PluginHost {
    struct ServiceEntry {LabU32 id{},version{};const char* manifestName{};const void* table{};};
    std::string activeWorldId_;
    std::filesystem::path activePackageRoot_;
    std::vector<LabU32> allowedServices_;
    std::vector<ServiceEntry> services_;
    LabHostApiV1 api_{};
    std::shared_ptr<VisualDomain> visualDomain_;
    bool visualAttached_{};
    LabVisualServiceV1 visualService_{};
    LabVisualContributionV1 visualContribution_{};
    const LabVisualPackageV1* visualPackage_{};
    LabInputServiceV1 inputService_{};
    LabAudioServiceV1 audioService_{};
    LabGpuServiceV1 gpuService_{};
    LabGpuInteropServiceV1 gpuInteropService_{};
    LabPhysicsServiceV1 physicsService_{};
    ProfileStorage profileStorage_;LabStorageServiceV1 storageService_{};
    PhysicsRuntime* physics_{};
    uint64_t physicsOwner_{};
    struct InteropResource {uint64_t worldGeneration{},allocationBytes{},bytes{};Microsoft::WRL::ComPtr<ID3D12Resource> resource;HANDLE shared{};~InteropResource(){if(shared)CloseHandle(shared);}};
    struct InteropFence {uint64_t worldGeneration{};Microsoft::WRL::ComPtr<ID3D12Fence> fence;HANDLE shared{};~InteropFence(){if(shared)CloseHandle(shared);}};
    std::map<LabU64,std::unique_ptr<InteropResource>> interopResources_;
    std::map<LabU64,std::unique_ptr<InteropFence>> interopFences_;
    mutable std::mutex interopMutex_;
    LabU64 nextInteropHandle_{1},worldGeneration_{};
    LabU32 CreateSharedBuffer(LabU64,LabGpuInteropResourceHandleV1*);
    LabU32 CreateSharedFence(LabU64,LabGpuInteropFenceHandleV1*);
    LabU32 GetCudaMemoryHandle(LabGpuInteropResourceHandleV1,LabGpuInteropMemoryExportV1*);
    LabU32 GetCudaFenceHandle(LabGpuInteropFenceHandleV1,LabU32,LabGpuInteropFenceExportV1*);
    LabU32 ReleaseInteropResource(LabGpuInteropResourceHandleV1);
    LabU32 ReleaseInteropFence(LabGpuInteropFenceHandleV1);
    void ClearInteropGeneration() noexcept;
    LabGpuPublicationProviderV1 gpuProvider_{};
    Graphics* graphics_{};Audio* audio_{};
    Channels* channels_{};
    InputSystem* inputSystem_{};
    bool inputCaptured_{};
    std::array<LabInputSampleV1,MaxLabActions*InputSourceCount> inputSamples_{};
    std::array<LabInputPoseStateV1,MaxLabActions*InputSourceCount> inputPoses_{};
    std::array<LabAudioEmitterV1,MaxAudioEmitters> audioEmitters_{};
    LabU32 audioEmitterCount_{};uint64_t audioGeneration_{1};
    static const void* LABOVR_PLUGIN_CALL QueryServiceThunk(void*,LabU32,LabU32);
    static LabU32 LABOVR_PLUGIN_CALL GetInputSnapshotThunk(void*,LabInputSnapshotV1*);
    static LabU32 LABOVR_PLUGIN_CALL EmitHapticThunk(void*,const LabInputHapticV1*);
    static LabU32 LABOVR_PLUGIN_CALL ResolveActionThunk(void*,const char*,LabInputActionHandleV1*);
    static LabU32 LABOVR_PLUGIN_CALL GetActionInfoThunk(void*,LabInputActionHandleV1,LabInputActionInfoV1*);
    static LabU32 LABOVR_PLUGIN_CALL PublishAudioEventsThunk(void*,const LabAudioEventBatchV1*);
    static LabU32 LABOVR_PLUGIN_CALL PublishAudioEmittersThunk(void*,const LabAudioEmitterBatchV1*);
    static LabU32 LABOVR_PLUGIN_CALL SetGpuProviderThunk(void*,const LabGpuPublicationProviderV1*);
    const void* QueryService(LabU32,LabU32)const noexcept;
    LabU32 GetInputSnapshot(LabInputSnapshotV1*);
    LabU32 EmitHaptic(const LabInputHapticV1*);
    // input_v1: handles crossing this boundary are opaque runtime tokens.
    LabU32 ResolveAction(const char*,LabInputActionHandleV1*)const;
    LabU32 GetActionInfo(LabInputActionHandleV1,LabInputActionInfoV1*)const;
    LabU32 PublishAudioEvents(const LabAudioEventBatchV1*);
    LabU32 PublishAudioEmitters(const LabAudioEmitterBatchV1*);
public:
    PluginHost()noexcept;
    void SetInputCaptured(bool value)noexcept{inputCaptured_=value;}
    const LabHostApiV1& Api()const noexcept{return api_;}
    bool RegisterService(LabU32 serviceId,LabU32 version,const char* manifestName,const void* table);
    void ConfigureWorldServices(std::string worldId,std::filesystem::path packageRoot,std::span<const std::string> requiredServices);
    void ClearWorldServices() noexcept;
    void BindChannels(Channels& channels)noexcept{channels_=&channels;}
    void BindInputSystem(InputSystem& inputSystem)noexcept{inputSystem_=&inputSystem;}
    void BindAudio(Audio& a)noexcept{audio_=&a;}
    void BindGraphics(Graphics& graphics)noexcept;
    void BindPhysics(PhysicsRuntime&);
    Microsoft::WRL::ComPtr<ID3D12Resource> ResolveInteropResource(LabGpuInteropResourceHandleV1)const;
    Microsoft::WRL::ComPtr<ID3D12Fence> ResolveInteropFence(LabGpuInteropFenceHandleV1)const;
    GpuPublication AcquireGpuPublication();
    void ReleaseGpuPublication(const GpuPublication&);
    void AppendAudio(AudioFrame& frame)const noexcept;
    LabU32 SetVisualContribution(const LabVisualContributionV1*);
    SceneData ConfigureVisualPackage(const LabVisualPackageV1&);
    void BorrowVisualBatch(const LabVisualBatchV1&,uint64_t,const Pose&);
    void AppendVisualStandard(RenderScene&,uint32_t meshBase,uint32_t materialBase) const;
    bool BuildVisualBatch(uint64_t frameId,double timeSeconds,LabVisualBatchV1&) const;
    void ClearVisualPackage() noexcept;
    const LabGpuPublicationProviderV1* GpuProvider()const noexcept{return gpuProvider_.Acquire&&gpuProvider_.Release?&gpuProvider_:nullptr;}
};
}
