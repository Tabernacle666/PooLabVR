#pragma once
#include "LaboVRPlugin.h"
#include "LaboVRWorld.h"
#include "LaboVRVisualService.h"
#include "Assets.h"
#include "RenderContributions.h"
#include "GpuPublication.h"
#include "World.h"
#include "InputSystem.h"
#include <vector>
#include <string>
namespace lab {
class PluginHost;class Graphics;
class PluginWorldAdapter final:public WorldLayer,public GpuStream {
    const LabPluginApiV1* plugin_{};
    const LabHostApiV1* host_{};
    PluginHost* hostImpl_{};
    Graphics* graphics_{};
    InputSystem* inputSystem_{};
    LabPluginWorldHandle instance_{};
    SceneData scene_{};
    std::vector<uint32_t> streamMaterials_;
    std::vector<LabVisualLightModifierV1> lightModifiers_;
    std::vector<SceneLight> attractiveLights_;
    LabVisualBatchV1 frameBatch_{};
    uint64_t batchFrame_{UINT64_MAX};
    uint32_t materialBase_{},meshBase_{};
    bool active_{},gpuAttached_{};
    std::wstring packageRoot_;
    std::wstring sceneOverride_;
    std::vector<std::string> requiredInputActions_;
    static void AppendThunk(void*,RenderScene&,const FrameInput&,double);
    static void LightThunk(void*,SceneLight&,double);
    void Append(RenderScene&,const FrameInput&,double);
    void ModifyLight(SceneLight&,double)const;
    void UpdateBatch(uint64_t,double);
public:
    PluginWorldAdapter(const LabPluginApiV1&,const LabHostApiV1&,std::wstring packageRoot,std::wstring sceneOverride,std::vector<std::string> requiredInputActions={});
    void Load(WorldServices&) override;
    void Unload(WorldServices&) override;
    void Activate(uint32_t meshBase,uint32_t materialBase,uint32_t lightBase);
    void Deactivate() noexcept;
    void Frame(uint64_t frameId,double timeSeconds) noexcept;
    LabPluginWorldHandle Instance()const noexcept{return instance_;}
    const LabWorldApiV1* WorldApi()const noexcept;
    const SceneData& Scene()const;
    std::span<const SceneLight> AttractiveLights()const{return attractiveLights_;}
    RenderContributions Contributions()const;
    bool Loaded()const noexcept{return instance_!=nullptr;}
    bool Active()const noexcept{return active_;}
    GpuWorldDomain Domain()const noexcept override;
    GpuPublication Acquire() override;
    void Release(const GpuPublication&) override;
};
}
