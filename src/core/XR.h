#pragma once
#include "Core.h"
#include "InputSystem.h"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include <d3d12.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <string>
#include <vector>

namespace OASIS {
inline constexpr uint32_t MaxViews=RuntimeConfig::MaxViews;
inline constexpr uint32_t HandCount=InputSourceCount;
struct XRShotHand {PoseActionState aim{};float trigger{};bool fired{};};
struct XRFrame {std::array<XRShotHand,HandCount> shotHands{};bool begun{},render{},ready{},tracked{},submitted{};uint32_t color{},depth{},viewCount{};XrTime displayTime{};XrDuration displayPeriod{13888889};std::array<XMFLOAT4X4,MaxViews> vp{},inverseVP{};std::array<XMFLOAT3,MaxViews> eyes{};std::array<XrFovf,MaxViews> fov{};FrameInput input;};
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
    std::array<bool,HandCount> triggerLatched_{};uint64_t frameSerial_{};uint32_t width_{},height_{};DXGI_FORMAT format_{};bool running_{},exit_{},stage_{},touchPlusExtension_{};
    XMFLOAT4X4 trackingToWorld_{};PFN_xrRequestDisplayRefreshRateFB refresh_{};PFN_xrConvertTimeToWin32PerformanceCounterKHR timeToQpc_{};
    XrPath Path(const char*);std::string PathString(XrPath)const;void Actions();void SuggestProfile(const char*,uint32_t);void UpdateInteractionProfiles();void Swapchains();void LocateInput(XRFrame&);LARGE_INTEGER ToQpc(XrTime)const;
    XrAction Action(const char* name,XrActionType type);
public:
    ~XR();void InitializeInstance(XRIdentity,InputSystem&,TrackingMapping={});void InitializeSession(ID3D12Device*,ID3D12CommandQueue*);void PollEvents();void RequestExit();
    bool BeginFrame(XRFrame&);void LocateLate(XRFrame&);bool AcquireSwapchains(XRFrame&);void EndFrame(const XRFrame&);void Haptic(const HapticEvent&);
    bool Running()const{return running_;}bool ExitRequested()const{return exit_;}
    const auto& GraphicsRequirements()const{return requirements_;}const auto& Colors()const{return colors_;}const auto& Depths()const{return depths_;}
    DXGI_FORMAT Format()const{return format_;}uint32_t Width()const{return width_;}uint32_t Height()const{return height_;}uint32_t ViewCount()const{return viewCount_;}
};
}
