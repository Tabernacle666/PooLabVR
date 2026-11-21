#pragma once
#include "XR.h"
#include "FeedbackProfiles.h"
#include <filesystem>
#include <memory>
#include <optional>

namespace OASIS {
class LunarBattle;
class ShuttlePilot;
struct FeedbackEvent {
    FeedbackCue cue;
    uint32_t hand;
    double time;
    uint64_t frameId;
    float strength{1.f};
    std::optional<XMFLOAT3> position;
};

class Feedback {
    struct Audio;
    std::unique_ptr<Audio> audio_;
    const FeedbackProfiles& profiles_;
    ActionId hapticAction_;
    struct Hand {
        std::array<double,FeedbackCueCount> next{};
        double echoTime{},busyUntil{},driveUntil{};
        float echoStrength{},echoDuration{};
        FeedbackCue echoCue{};
        bool reportedHapticError{};
    };
    std::array<Hand,HandCount> hands_{};
    double lastTime_{};
    ControlContext context_{ControlContext::Pedestrian};
    void Send(XR&,FeedbackCue,uint32_t hand,float strength,float duration,uint64_t frame);
public:
    Feedback(const std::filesystem::path& audioDirectory,const FeedbackProfiles&,ActionId hapticAction,bool lunarAudio=false,bool shuttleAudio=false,
             const std::filesystem::path& packagedAudio={});
    ~Feedback();
    void Emit(XR& xr,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter=1.f);
    void Tick(XR&,const FrameInput&);
    void UpdateShuttle(XR&,const ShuttlePilot&,const FrameInput&,bool enabled,bool boot,bool ignition,uint32_t reversals);
    void UpdateLunar(XR&,const LunarBattle& battle,const FrameInput& input,float level,bool live);
};
}
