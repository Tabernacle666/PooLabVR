#pragma once
#include "World.h"
#include "Assets.h"
#include "RenderContributions.h"
#include "HomePhysics.h"
#include <array>
#include <span>
#include <string>
#include <vector>
namespace lab {
enum class HomeCommandKind:uint32_t {None,NewSession,LoadWorld,Quit};
struct HomeCommand {HomeCommandKind kind{};uint32_t world{Invalid};};
inline constexpr uint32_t HomeMenuMaxLines=8;
inline constexpr uint32_t HomeMenuMaxChars=32;
inline constexpr uint32_t HomeMenuRenderLines=HomeMenuMaxLines;
struct HomeMenuSnapshot {
    std::array<std::array<char,HomeMenuMaxChars>,HomeMenuRenderLines> labels{};
    uint32_t count{},hover{Invalid};
    float time{};
};
class HomeWorld final:public WorldLayer {
    const LabGpuServiceV1* terminalGpu_{};
    SceneData scene_;
    HomePhysics physics_;
    std::vector<std::string> worlds_;
    XMFLOAT3 listenerVelocity_{};
    Pose lastListener_{};
    double lastListenerAt_{},panelStartSeconds_{};
    HomeCommand command_{};
    uint32_t hover_{Invalid};
    bool loaded_{},worldPage_{},visualPage_{},menuVisible_{true},ambientVisible_{true};
    bool diagnosticHands_{};
    Pose diagnosticCamera_{};
    uint32_t worldPageIndex_{};
    enum class PanelAction:uint32_t {None,Select,Expand,TreeScroll,Tab,Freeze,Theme,Face,Profile,Translate,Speed,Flight,Effects,Fog,Fan,Optical,Population,Lamp,Rotate,LocalReset,Size,Capture,CaptureEye,CaptureInterface,Touch,HandResponse,Antialiasing,Transport,Surface,FanYaw,Multisampling,PopulationSpace,FanCCD};
    struct PanelControl {LabU64 id{},node{};PanelAction action{};uint32_t index{};float sign{};};
    LabU64 selectedNode_{3},panelHover_{};
    uint32_t inspectorTab_{},treeScroll_{};
    std::vector<LabU64> expanded_{37,1,3,21};
    std::vector<LabTerminalNodeV2> panelNodes_;
    std::vector<LabTerminalElementV2> panelElements_;
    std::vector<PanelControl> panelControls_;
    std::array<LabU64,2> heldControl_{},repeatControl_{};
    std::array<double,2> repeatAt_{};
    void BuildPanel();void PublishPanel()const;void InvokePanel(const PanelControl&,double);

    double lastPanelAt_{},panelDt_{};
    void UpdateCaptureButtons(const FrameInput&);
    std::array<bool,2> panelTouch_{};
    std::array<float,2> panelDistance_{};
    void UpdatePanel(const FrameInput&);
    void UpdateMenu(const FrameInput&);
    PoseActionState AimState(const FrameInput&,uint32_t)const;
    void ResetMenu();
    void Append(RenderScene&,const FrameInput&,double)const;
    static void AppendCallback(void*,RenderScene&,const FrameInput&,double);
public:
    void Load(WorldServices&) override;
    void Unload(WorldServices&) override;
    void Reset(WorldServices&) override;
    const SceneData& Scene()const;
    void SetMenuVisible(bool value)noexcept{menuVisible_=value;if(!value)hover_=Invalid;}
    void SetAmbientVisible(bool value){ambientVisible_=value;physics_.SetVisible(value);}
    void SetLampEnabled(bool value){physics_.SetLampEnabled(value);}
    void LoadConfiguration(const std::wstring& path){physics_.LoadConfiguration(path);}
    void ApplyDiagnosticDefaults(){physics_.ApplyDiagnosticDefaults();BuildPanel();}
    void SetDiagnosticHands(bool enabled,XMFLOAT3 position={},XMFLOAT3 target={}){
        diagnosticHands_=enabled;
        if(enabled)diagnosticCamera_=FromMatrix(XMMatrixInverse(nullptr,XMMatrixLookAtLH(XMLoadFloat3(&position),XMLoadFloat3(&target),XMVectorSet(0,1,0,0))));
    }
    void SetDiagnosticFogEnabled(bool enabled){physics_.SetDiagnosticFogEnabled(enabled);}
    uint32_t DiagnosticPopulation()const{return physics_.PopulationCount();}
    const LabPopulationSnapshotV1& DiagnosticPopulationSnapshot()const{return physics_.Population();}
    const LabFogSettingsV1& DiagnosticFog()const{return physics_.Fog();}
    uint32_t DiagnosticTheme()const{return physics_.PanelTheme();}
    void SetWorldNames(std::vector<std::string>);
    void Update(const FrameInput&,double,std::span<const SceneLight>);
    HomeCommand ConsumeCommand()noexcept;
    HomeMenuSnapshot MenuSnapshot()const;
    void PublishMenu()const;
    void FillAudio(AudioFrame&,const FrameInput&,double)noexcept;
    RenderContributions Contributions();
};
}
