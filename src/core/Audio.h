#pragma once
#include "Core.h"
#include <memory>
#include "Assets.h"
#include "AudioSignal.h"
#include "../cartridge_api/LaboVRAudioService.h"
namespace lab {
struct AudioEventSound {float gain{},frequency{},decay{1};};
using AudioEventDecoder=bool(*)(const AudioEvent&,AudioEventSound&);
class Audio {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Audio();~Audio();
    void InitializeService(Channels&,AudioEventDecoder=nullptr,AudioSignalProvider={});
    uint32_t SetAcousticBoxes(uint64_t,const LabAcousticBoxV1*,uint32_t);
    void LoadScene(const SceneData&);
    void UnloadScene();
    void Initialize(Channels&,const SceneData&,AudioEventDecoder=nullptr,AudioSignalProvider={});
    void Start();void Stop();
};}
