#pragma once
#include "Graphics.h"
#include <array>
#include <string>
#include <vector>
#include <filesystem>

namespace lab {
enum class CpuStage : uint32_t {
    WorldFrame,HomeUpdate,PhysicsAdvance,AudioAndEvents,GraphicsPrepare,
    LateLocateAcquire,GraphicsRender,XrEndFrame,Count
};
enum class PerformanceFrameClass {Normal,MissedPeriod,CpuStall,GpuDeadline,MultiCause};
struct CpuFrameSample {
    uint64_t frameId{};
    std::array<double,uint32_t(CpuStage::Count)> milliseconds{};
    double activeFrameMs{};
    double xrWaitFrameMs{},xrBeginFrameMs{},xrSyncActionsMs{},xrLocateViewsMs{};
    double displayIntervalMs{},gpuSpanMs{};
    uint32_t displayPeriods{1},missedPeriods{};
    PerformanceFrameClass classification{};
    bool xrTimingValid{};
};
class PerformanceCapture {
public:
    enum class State {Idle,Holding,AwaitRelease,BaselineBefore,Telemetry,BaselineAfter};
    static constexpr double HoldSeconds=3.0,BaselineSeconds=1.0,TelemetrySeconds=3.0;
    class CpuScope {
        PerformanceCapture* capture_{};
        CpuStage stage_{};
        int64_t started_{};
    public:
        CpuScope(PerformanceCapture*,CpuStage);
        ~CpuScope();
        CpuScope(const CpuScope&)=delete;
        CpuScope& operator=(const CpuScope&)=delete;
    };
    CpuScope Measure(CpuStage stage){return CpuScope(this,stage);}
    void BeginFrame(const XRFrame&,const std::string& worldId,const std::string& worldName,
                    const std::wstring& scene,Graphics&,Channels&);
    void EndFrame(const XRFrame&,Graphics&,XR&,Channels&);
private:
    struct Segment {
        uint64_t firstFrame{},lastFrame{};
        XrTime firstDisplayTime{},lastDisplayTime{},previousDisplayTime{};
        XrDuration firstDisplayPeriod{};
        XrTime intervalStartDisplayTime{};
        uint64_t frames{},renderRequested{},submitted{},tracked{},skippedDisplayPeriods{};
        std::vector<double> displayIntervalsMs;
        std::vector<double> normalIntervalsMs;
        void Reset();
        double Add(const XRFrame&);
    };
    struct RawFrame {
        CpuFrameSample cpu;
        XrTime displayTime{};
        XrDuration displayPeriod{};
        double displayIntervalMs{};
        bool renderRequested{},submitted{},tracked{};
    };
    State state_{State::Idle};
    double holdStarted_{};
    XrTime segmentStarted_{};
    XrTime previousDisplayTime_{};
    uint32_t captureSerial_{};
    std::array<Segment,3> segments_;
    std::vector<RawFrame> frames_;
    CpuFrameSample currentCpu_;
    int64_t activeStarted_{},qpcFrequency_{};
    bool telemetryPosePending_{};
    Pose startHead_,telemetryStartHead_,telemetryEndHead_,endHead_;
    Pose lastHeadPose_;
    uint64_t lastInputFrameId_{};
    uint64_t startFrame_{};
    XrDuration displayPeriod_{};
    SYSTEMTIME captureUtc_{};
    std::string worldId_,worldName_;
    std::wstring scene_;
    void PrepareStorage();
    void StartSequence(const XRFrame&,const std::string&,const std::string&,const std::wstring&);
    bool WriteCapture(const GraphicsPerformanceInfo&,const std::vector<GpuPerformancePassSample>&,
        const std::vector<GpuQueueFrameAnalysis>&,const std::vector<GpuGapSample>&,std::filesystem::path& writtenPath);
};
}
