#pragma once
#include "Core.h"
#include "SphereLod.h"
#include "InputSystem.h"
#include "PhosphorOrbit.h"
#include "LaserSweep.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <optional>
#include <string>
#include <vector>

namespace OASIS {
inline constexpr uint32_t MaxViews=RuntimeConfig::MaxViews;
inline constexpr uint32_t HandCount=InputSourceCount;
inline constexpr uint32_t MembranePassagePulseCount=4;
inline constexpr float RequestedHeadsetHz=72.f;
struct LunarBattleView;
struct XRShotHand {PoseActionState aim{};bool fired{};};
struct XRCaptureView {
    Pose eye;
    XrFovf fov;
};
struct XRFrame {
    std::array<XRShotHand,HandCount> shotHands{};
    std::array<std::optional<Pose>,HandCount> glovePoses{};
    RightHandTool rightTool{RightHandTool::Empty};
    std::optional<Pose> toolExit;
    bool spherePerception{};
    uint32_t spherePerceptionZone{},spherePerceptionDistance{};
    bool headLockedDiagnostic{};
    bool lunarGeometry{};
    const LunarBattleView* lunarBattle{};
    uint32_t sphereBakeView{Invalid};
    std::optional<SphereRepresentation> sphereForce;
    SphereRepresentation sphereRepresentation{SphereRepresentation::Geometry};
    std::array<std::optional<SphereRepresentation>,SpectacleSphereCount> sphereForces{};
    std::array<SphereRepresentation,SpectacleSphereCount> sphereLevels{};
    std::array<SphereZone,SpectacleSphereCount> sphereZones{};
    std::array<uint32_t,MaxViews> visibleSpheres{};
    uint32_t sphereBakeOwner{},sphereNative{Invalid},sphereOrbitBand{Invalid},sphereOrbitOwner{};
    uint32_t membraneHandSphere{},membranePassageSphere{};
    std::optional<LaserRay> basaltRay,previousLaserRay;
    bool laserHeld{},laserCut{};
    std::array<LaserAfterimage,LaserAfterimageCount> laserAfterimages{};
    uint32_t laserAfterimageCount{};
    OrbitDisplay orbit;
    std::optional<XMFLOAT4X4> mountedOrbit;
    std::array<XMFLOAT4,HandCount*2u> membraneHandPulses{};
    std::array<XMFLOAT4,MembranePassagePulseCount> membranePassagePulses{};
    std::array<XMFLOAT4,HandCount> membraneStrokeStart{},membraneStrokeEnd{};
    bool begun{},render{},ready{},tracked{},submitted{},captureOnly{};
    uint32_t color{},depth{},viewCount{};
    XrTime displayTime{};
    XrDuration displayPeriod{13888889};
    float headsetTargetHz{RequestedHeadsetHz};
    std::array<XMFLOAT4X4,MaxViews> vp{},inverseVP{};
    std::array<XMFLOAT3,MaxViews> eyes{};
    std::array<XrFovf,MaxViews> fov{};
    FrameInput input;
};
struct XRIdentity {const char* application{"Core"};const char* engine{"Core"};uint32_t version{};};
class XR {
    struct SuggestedProfile {std::string path;std::array<std::vector<PhysicalControlId>,HandCount> available;};
    XRIdentity identity_;InputSystem* inputSystem_{};TrackingMapping mapping_;
    XrInstance instance_{};XrSystemId system_{};XrSession session_{};XrSpace space_{},viewSpace_{};XrActionSet physicalActionsSet_{};
    std::array<XrAction,MaxPhysicalControls> physicalActions_{};
    std::array<XrPath,HandCount> hands_{};std::array<std::array<XrSpace,HandCount>,MaxPhysicalControls> poseSpaces_{};
    std::vector<SuggestedProfile> suggestedProfiles_;
    XrSwapchain color_{},depth_{};std::vector<XrSwapchainImageD3D12KHR> colors_,depths_;
    XrGraphicsRequirementsD3D12KHR requirements_{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    XrViewConfigurationType viewConfigurationType_{};uint32_t viewCount_{};std::array<XrView,MaxViews> views_{};XrSessionState state_{XR_SESSION_STATE_UNKNOWN};
    uint64_t frameSerial_{};uint32_t width_{},height_{};DXGI_FORMAT format_{};bool running_{},exit_{},stage_{},touchPlusExtension_{};
    XMFLOAT4X4 trackingToWorld_{};PFN_xrRequestDisplayRefreshRateFB refresh_{};
    PFN_xrGetDisplayRefreshRateFB getRefresh_{};
    float headsetTargetHz_{RequestedHeadsetHz};PFN_xrConvertTimeToWin32PerformanceCounterKHR timeToQpc_{};
    XrPath Path(const char*);std::string PathString(XrPath)const;void Actions();void SuggestProfile(const char*,uint32_t);void UpdateInteractionProfiles();void Swapchains();void LocateInput(XRFrame&,bool pollControllers);LARGE_INTEGER ToQpc(XrTime)const;
    XrAction Action(const char* name,XrActionType type);
public:
    ~XR();void InitializeInstance(XRIdentity,InputSystem&,TrackingMapping={});void InitializeSession(ID3D12Device*,ID3D12CommandQueue*);void PollEvents();void RequestExit();
    bool BeginFrame(XRFrame&,double* waitMilliseconds=nullptr,bool pollControllers=true);
    void LocateLate(XRFrame&,const XRCaptureView* capture=nullptr);
    bool AcquireSwapchains(XRFrame&);void EndFrame(const XRFrame&);XrResult Haptic(const HapticEvent&);
    bool Running()const{return running_;}bool ExitRequested()const{return exit_;}
    const auto& GraphicsRequirements()const{return requirements_;}const auto& Colors()const{return colors_;}const auto& Depths()const{return depths_;}
    DXGI_FORMAT Format()const{return format_;}uint32_t Width()const{return width_;}uint32_t Height()const{return height_;}uint32_t ViewCount()const{return viewCount_;}
};
}
