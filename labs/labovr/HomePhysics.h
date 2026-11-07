#pragma once
#include "LaboVRPhysicsService.h"
#include "LaboVRInputService.h"
#include "LaboVRAudioService.h"
#include "Assets.h"
#include "LaboVRPopulation.h"
#include "ContentIds.h"
#include "LaboVRStorageService.h"
#include <string>
#include <array>
#include <vector>
namespace lab {
class HomePhysics {
    const LabPhysicsServiceV1* physics_{};
    const LabInputServiceV1* input_{};
    const LabAudioServiceV1* audio_{};
    const LabGpuServiceV1* gpu_{};
    const LabStorageServiceV1* storage_{};
    LabPhysicsSceneV1 scene_{};
    struct Entity {std::string id,name;LabPhysicsBodyV1 body{};LabPoseV1 pose{};uint32_t group{};LabVec3V1 acousticHalfExtent{};LabPhysicsBodyDescV1 shape{};};
    struct ConnectionRecord {std::string id;LabPhysicsJointV1 joint{};LabPoseV1 frameA{},frameB{};LabPhysicsBodyV1 bodyA{},bodyB{};};
    struct Selection {LabU64 id{};std::string name;std::vector<LabPhysicsBodyV1> bodies;std::vector<LabPhysicsEditBodyV1> validBodies;std::vector<LabPhysicsEditJointV1> validJoints;};
    std::vector<Entity> entities_;std::vector<ConnectionRecord> connections_;std::vector<Selection> selections_;
    LabPoseV1 panelPose_{{0,1.875f,3.96f},{0,0,0,1}};
    uint32_t panelTheme_{},panelFace_{};bool panelSelected_{};
    uint32_t selection_{},editSpeed_{1},buildingGroup_{};bool frozen_{};
    std::string status_{"READY"};double simulationSeconds_{};
    LabPhysicsBodyV1 fanBase_{},fanRotor_{};LabPhysicsJointV1 fanJoint_{};
    std::array<LabPhysicsBodyV1,4> fanBodies_{};bool fanCcd_{};
    LabPhysicsBodyV1 vaseBase_{},bonsaiBody_{};
    std::array<LabPhysicsBodyV1,40> rockBodies_{};
    float fanRpm_{900},fanActualRpm_{},fanWind_{1};bool fanEnabled_{true};
    LabMultisamplingV1 multisampling_{sizeof(LabMultisamplingV1),1};
    LabPopulationSpaceV1 populationSpace_{sizeof(LabPopulationSpaceV1),1,{0,1.875f,0},{3.7f,1.60f,3.7f},4};
    LabAntialiasingV1 antialiasing_{sizeof(LabAntialiasingV1),LAB_AA_LOW_V1};
    LabFogTransportV1 transport_{sizeof(LabFogTransportV1),1,.90f,.70f,2.f,0};
    std::array<LabSurfaceSettingsV1,5> surfaces_{{
        {sizeof(LabSurfaceSettingsV1),uint32_t(Material::Floor),{.075f,.085f,.095f},.30f,.07f,35,.012f,.25f},
        {sizeof(LabSurfaceSettingsV1),uint32_t(Material::Ceiling),{.14f,.15f,.17f},.90f,.025f,55,.009f,.12f},
        {sizeof(LabSurfaceSettingsV1),uint32_t(Material::WallZMinus),{.115f,.105f,.095f},.80f,.045f,28,.012f,.22f},
        {sizeof(LabSurfaceSettingsV1),uint32_t(Material::WallXMinus),{.08f,.105f,.12f},.72f,.055f,32,.012f,.22f},
        {sizeof(LabSurfaceSettingsV1),uint32_t(Material::WallXPlus),{.125f,.095f,.085f},.84f,.05f,26,.012f,.22f}
    }};
    void PublishRendering();
    LabFogWindV1 wind_{sizeof(LabFogWindV1),0,{},{0,0,-1},0,.3f,4,.12f};
    LabPopulationOpticalSettingsV1 optical_{sizeof(LabPopulationOpticalSettingsV1),1,1,0,LABOVR_GPU_DOMAIN_MESH_SOLID_V1,LAB_POPULATION_OPTICAL_BINDING_V1,0,0,{.0012f,.0009f,.0035f},.000012f,.35f,.025f,1.f,.02f};
    LabPoseV1 validPanelPose_{{0,1.875f,3.96f},{0,0,0,1}};
    std::array<uint32_t,2> touchMasks_{31,31};
    std::array<LabInputPhysicalSourceV1,32> physicalSources_{};uint32_t physicalSourceCount_{};
    uint32_t captureEye_{},captureInterface_{1};
    LabCaptureStatusV1 captureStatus_{sizeof(LabCaptureStatusV1)};
    void SyncTouchSources();void ApplyTouchSources();void PollPhysicalSources();void CacheSelection(Selection&);void CacheValidSelections();
    void PublishOptical();
    void PublishAcoustics();bool RefreshEditor();void BuildSelections();void UpdateFan(uint64_t);
    bool ReadProfile(uint32_t,bool);bool WriteProfile(uint32_t);
    LabPhysicsBodyV1 bulb_{},socket_{},pullSlider_{},pullHandle_{};
    std::array<LabPhysicsBodyV1,2> hands_{};
    std::array<LabPhysicsJointV1,2> grips_{};
    std::array<LabPoseV1,2> handTargets_{};
    std::array<bool,2> handTracked_{},handColliding_{},handBlocked_{};
    std::array<LabPhysicsStateV1,LABOVR_PHYSICS_MAX_PROBES_V1> states_{};uint32_t stateCount_{};
    LabInputActionHandleV1 handPose_{},physicalAction_{},grabAction_{};
    LabGpuPublicationProviderV1 provider_{};
    LabPhysicsLightV1 lampLight_{};
    float lampPower_{26};
    uint64_t trackingRevision_{},sourceFrame_{};
    double lastUpdateSeconds_{},stateSeconds_{},diagnosticMechanismAt_{};
    bool visible_{true},pullArmed_{true};
    struct HandSettings {
        float positionTau{.018f},rotationTau{.024f},maxSpeed{7.f},maxAngularSpeed{20.f};
        float grabRadius{.10f},grabStiffness{250.f},grabDamping{5.f},grabMaxForce{12.f};
    } handSettings_;
    std::vector<LabPhysicsVisualV1> visuals_;
    LabPhysicsBodyV1 Body(const LabPhysicsBodyDescV1&);
    LabPhysicsJointV1 Joint(const LabPhysicsD6DescV1&);
    void Visual(LabPhysicsBodyV1,uint32_t,Material,XMFLOAT3,LabPoseV1={{0,0,0},{0,0,0,1}},uint32_t=DynamicObject);
    void Build(SceneData&);
    void ReleaseHand(uint32_t,bool);
    void UpdateHands(const LabInputSnapshotV1&,double,bool,bool,std::vector<LabPhysicsCommandV1>&);
    LabFlightSettingsV1 flight_{sizeof(LabFlightSettingsV1),0x76a019abu,1.4f,3.8f,.22f,1.5f,.24f,1.2f,.16f,.75f,.7f,.8f,.08f,8.f,2.5f,.000018f,.000030f,1.5f,.15f,180.f,.0035f,.25f,1.2f};
    LabPopulationEffectsV1 effects_{sizeof(LabPopulationEffectsV1),0,.18f,1.f,12.f,1.3f,.55f,.7f,.35f};
    LabPopulationHandsV1 sweeps_{sizeof(LabPopulationHandsV1)};
    LabFogSettingsV1 fog_{sizeof(LabFogSettingsV1),1,1,0,{0,1.875f,0},{3.95f,1.875f,3.95f},.065f,2.2f,.35f,1.f,.25f,.15f,.25f,.3f,1.f,1.f};
    std::array<XMFLOAT3,2> sweepLast_{},sweepVelocity_{};
    std::array<bool,2> sweepTracked_{};
    LabPopulationSnapshotV1 population_{};
    uint32_t populationCount_{1},nextFlyAudioId_{1},nextFanAudioId_{},fanAudioBase_{};std::vector<uint32_t> flyAudioIds_;
    LabPopulationInputV1 populationInput_{sizeof(LabPopulationInputV1),1};
public:
    uint32_t Antialiasing()const{return antialiasing_.mode;}void AdjustAntialiasing(float);void SetAntialiasingMode(uint32_t);
    uint32_t Multisampling()const{return multisampling_.samples;}void SetMultisampling(uint32_t);
    const LabPopulationSpaceV1& PopulationSpace()const{return populationSpace_;}void AdjustPopulationSpace(uint32_t,float);
    bool HandPhysical(uint32_t h)const{return handColliding_[h&1];}
    const LabFogTransportV1& Transport()const{return transport_;}const LabFogWindV1& Wind()const{return wind_;}
    const LabSurfaceSettingsV1& Surface(uint32_t i)const{return surfaces_[i%5];}
    void AdjustTransport(uint32_t,float);void AdjustSurface(uint32_t,uint32_t,float);void RotateFan(float);
    void ResetSelection();void AdjustSize(float,double=0);float DiameterMm()const{return flight_.bodyRadius*2000;}
    bool IsGrabbing()const{return grips_[0]||grips_[1];}
    void Capture();void ToggleCaptureEye(){captureEye_^=1;}void ToggleCaptureInterface(){captureInterface_^=1;}
    uint32_t CaptureEye()const{return captureEye_;}uint32_t CaptureInterface()const{return captureInterface_;}
    const LabCaptureStatusV1& CaptureStatus()const{return captureStatus_;}
    void ToggleTouch(uint32_t,uint32_t);uint32_t TouchMask(uint32_t h)const{return touchMasks_[h&1];}
    uint32_t TouchAvailable(uint32_t)const;uint32_t TouchDown(uint32_t)const;
    float HandPositionTau()const{return handSettings_.positionTau;}float HandRotationTau()const{return handSettings_.rotationTau;}
    void AdjustHandResponse(uint32_t,float);
    bool SetFrozen(bool);bool Frozen()const{return frozen_;}
    void Select(int);void TranslateSelection(uint32_t,float,double);void CycleEditSpeed();
    uint32_t EditSpeed()const{return editSpeed_;}const char* SelectionName()const;
    XMFLOAT3 SelectionPosition()const;
    std::vector<LabTerminalNodeV2> EditorNodes()const;
    void SelectNode(LabU64);
    LabPoseV1 PanelPose()const{return panelPose_;}
    LabPoseV1 PanelReadingPose()const;
    uint32_t PanelTheme()const{return panelTheme_;}uint32_t PanelFace()const{return panelFace_;}
    void TogglePanelTheme(){panelTheme_^=1;}void TogglePanelFace(){panelFace_^=1;}
    void RotatePanel(float);

    const char* Status()const{return status_.c_str();}
    void ProfileCommand(uint32_t);void AdjustFan(uint32_t,float);
    bool FanEnabled()const{return fanEnabled_;}bool FanCCD()const{return fanCcd_;}void ToggleFanCCD();
    float FanRpm()const{return fanRpm_;}float FanActualRpm()const{return fanActualRpm_;}float FanWind()const{return fanWind_;}
    double SimulationSeconds()const{return simulationSeconds_;}
    void LoadConfiguration(const std::wstring&);
    // Transient diagnostics only: never writes the creator's saved profile.
    void ApplyDiagnosticDefaults();
    void SetDiagnosticFogEnabled(bool enabled);
    void Load(const LabHostApiV1&,SceneData&);
    void Unload();
    void Update(uint64_t,double,std::span<const SceneLight>);
    void AddPopulation(int32_t);
    void AdjustFlight(uint32_t,float);
    void AdjustEffects(uint32_t,float);
    void AdjustFog(uint32_t,float);
    void AdjustOptical(uint32_t,float);
    const LabPopulationOpticalSettingsV1& Optical()const{return optical_;}
    const LabPopulationEffectsV1& Effects()const{return effects_;}
    const LabFogSettingsV1& Fog()const{return fog_;}
    const LabFlightSettingsV1& Flight()const{return flight_;}
    const LabPopulationSnapshotV1& Population()const{return population_;}
    uint32_t PopulationCount()const{return populationCount_;}
    LabU64 TargetIdentity()const{return population_.targetId<flyAudioIds_.size()?0x100000000ull+flyAudioIds_[population_.targetId]:0;}
    void SetPopulationView(XMFLOAT3,XMFLOAT3,XMFLOAT3,bool);
    void SetVisible(bool);
    void SetLampEnabled(bool);
    bool LampEnabled()const noexcept{return lampPower_>0;}
    PoseActionState HandState(uint32_t)const noexcept;
    std::span<const uint32_t> StreamMaterials()const{return {provider_.streamMaterials,provider_.streamMaterialCount};}
};
}
