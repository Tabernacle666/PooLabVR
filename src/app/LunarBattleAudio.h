#pragma once
#include "LunarBattle.h"
#include <memory>
#include <filesystem>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace OASIS {
class FeedbackProfiles;
class LunarBattleAudio {
    struct Mixer;
    std::unique_ptr<Mixer> mixer_;
public:
    LunarBattleAudio(IXAudio2&,IXAudio2MasteringVoice&,const std::filesystem::path&,const FeedbackProfiles&);
    ~LunarBattleAudio();
    void Submit(const LunarBattle& battle,const FrameInput& input,float level);
};
}
