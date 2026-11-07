#pragma once
#include "Core.h"
#include "Assets.h"
#include <phonon.h>
#include <memory>
#include <span>
namespace lab {
struct ReverbSend {
    uint64_t id{},owner{},generation{};XMFLOAT3 position{};uint32_t flags{};
    const float* mono{};float level{};
};
class Reverb {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    Reverb();~Reverb();
    void Initialize(IPLContext,IPLHRTF,const SceneData&,uint32_t frameSize);
    void Process(const Pose&,std::span<const ReverbSend>,float* interleavedStereo);
};
}
