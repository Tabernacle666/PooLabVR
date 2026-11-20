#pragma once
#include "LunarBattle.h"
#include <memory>

struct IXAudio2;
struct IXAudio2MasteringVoice;

namespace OASIS {
class LunarBattleAudio {
    struct Mixer;
    std::unique_ptr<Mixer> mixer_;
public:
    LunarBattleAudio(IXAudio2& engine,IXAudio2MasteringVoice& output);
    ~LunarBattleAudio();
    void Submit(const LunarBattle& battle,const FrameInput& input,float level);
};
}
