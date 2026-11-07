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

namespace lab {
inline constexpr uint32_t MaxViews=ConsoleSpec::MaxViews;
inline constexpr uint32_t HandCount=InputSourceCount;
struct XRPerformanceTiming {
    double waitFrameMs{},beginFrameMs{},syncActionsMs{},locateViewsMs{};
    double acquireColorMs{},waitColorMs{},acquireDepthMs{},waitDepthMs{};
    bool valid{};
};
struct XRFrame {XRPerformanceTiming performanceTiming{};bool begun{},render{},ready{},tracked{},submitted{},performanceGestureDown{};uint32_t color{},depth{},viewCount{};XrTime displayTime{};XrDuration displayPeriod{13888889};std::array<XMFLOAT4X4,MaxViews> vp{},inverseVP{};std::array<XMFLOAT3,MaxViews> eyes{};FrameInput input;};
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
    bool performanceCaptureActive{};uint64_t performanceQpcFrequency{};
    uint64_t frameSerial_{};uint32_t width_{},height_{};DXGI_FORMAT format_{};bool running_{},exit_{},stage_{},touchPlusExtension_{};
    XMFLOAT4X4 trackingToWorld_{};PFN_xrRequestDisplayRefreshRateFB refresh_{};PFN_xrConvertTimeToWin32PerformanceCounterKHR timeToQpc_{};PFN_xrConvertWin32PerformanceCounterToTimeKHR qpcToTime_{};
    XrPath Path(const char*);std::string PathString(XrPath)const;void Actions();void SuggestProfile(const char*,uint32_t);void UpdateInteractionProfiles();void Swapchains();void LocateInput(XRFrame&);LARGE_INTEGER ToQpc(XrTime)const;XrTime ToXrTime(const LARGE_INTEGER&)const;
    XrAction Action(const char* name,XrActionType type);
public:
    ~XR();void InitializeInstance(XRIdentity,InputSystem&,TrackingMapping={});void InitializeSession(ID3D12Device*,ID3D12CommandQueue*);void PollEvents();
    void SetPerformanceCaptureActive(bool active);
    bool BeginFrame(XRFrame&);void LocateLate(XRFrame&);void SetDiagnosticCamera(XRFrame&,XMFLOAT3,XMFLOAT3);bool AcquireSwapchains(XRFrame&);void EndFrame(const XRFrame&);void Haptic(const HapticEvent&);
    bool Running()const{return running_;}bool ExitRequested()const{return exit_;}
    const auto& GraphicsRequirements()const{return requirements_;}const auto& Colors()const{return colors_;}const auto& Depths()const{return depths_;}
    DXGI_FORMAT Format()const{return format_;}uint32_t Width()const{return width_;}uint32_t Height()const{return height_;}uint32_t ViewCount()const{return viewCount_;}
};
}
