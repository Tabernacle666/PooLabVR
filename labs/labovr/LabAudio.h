#pragma once
#include "AudioSignal.h"
#include "LaboVRAudioService.h"
namespace lab {
namespace LabWaves {inline constexpr SoundWave Buzz=LABOVR_AUDIO_WAVE_BUZZ_V1,Sine=LABOVR_AUDIO_WAVE_SINE_V1,Noise=LABOVR_AUDIO_WAVE_NOISE_V1,Fire=LABOVR_AUDIO_WAVE_FIRE_V1;}
class ClientAudio {
    struct Voice {float phase{},modulation{},fireBody{},crackle{},envelope{};uint32_t noise{0x5a769fe1};};
    std::array<Voice,AudioVoiceCount> voices_;
public:
    void Reset(uint32_t voice,uint32_t identity,bool loop);
    bool Generate(const AudioSignalRequest&,std::span<float>);
    AudioSignalProvider Provider();
};
}
