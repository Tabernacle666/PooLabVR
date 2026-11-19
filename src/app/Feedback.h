#pragma once
#include "XR.h"
#include <filesystem>
#include <memory>
#include <optional>

namespace OASIS {
enum class FeedbackCue : uint32_t {CaptureSaved,SurfaceEnter,SurfaceSlide,SurfaceExit,Count};
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
    ActionId hapticAction_;
    std::array<double,HandCount> nextPulse_{};
public:
    Feedback(const std::filesystem::path& audioDirectory,ActionId hapticAction);
    ~Feedback();
    void Emit(XR& xr,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter=1.f);
};
}
