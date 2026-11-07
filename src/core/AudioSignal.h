#pragma once
#include "Core.h"
#include <span>
namespace lab {
inline constexpr uint32_t AudioVoiceCount=48;
struct AudioSignalRequest {uint32_t voice{},wave{};float frequency{},doppler{1},decay{1};bool loop{};float frequencyEnd{},dopplerEnd{1};};
struct AudioSignalProvider {
    void* context{};
    void (*reset)(void*,uint32_t,uint32_t,bool){};
    bool (*generate)(void*,const AudioSignalRequest&,std::span<float>){};
};
}
