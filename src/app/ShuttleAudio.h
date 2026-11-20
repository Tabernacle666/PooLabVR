#pragma once

#include <memory>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace OASIS {
class ShuttlePilot;
struct FrameInput;

class ShuttleAudio {
    struct Mixer;
    std::unique_ptr<Mixer> mixer_;
public:
    ShuttleAudio(IXAudio2&,IXAudio2MasteringVoice&);
    ~ShuttleAudio();
    void Update(const ShuttlePilot&,const FrameInput&,bool enabled,bool ignition);
};
}
