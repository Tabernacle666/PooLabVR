#pragma once

#include <memory>
#include <filesystem>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace OASIS {
class ShuttlePilot;
class FeedbackProfiles;
struct FrameInput;

class ShuttleAudio {
    struct Mixer;
    std::unique_ptr<Mixer> mixer_;
public:
    ShuttleAudio(IXAudio2&,IXAudio2MasteringVoice&,const std::filesystem::path&,const FeedbackProfiles&);
    ~ShuttleAudio();
    void Update(const ShuttlePilot&,const FrameInput&,bool enabled,bool ignition);
};
}
