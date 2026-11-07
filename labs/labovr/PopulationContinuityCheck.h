#pragma once
#include "../../src/cartridge_api/LaboVRPopulation.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace lab {
// Compare real GPU readback voices from the same hidden scene and same insect.
// A scene tick alone cannot prove that its population simulation has advanced.
class PopulationContinuityCheck {
    LabPopulationSnapshotV1 baseline_{};
    bool haveBaseline_{};
    uint64_t comparisons_{};
    uint32_t witness_{};
    double distance_{};
public:
    bool Observe(const LabPopulationSnapshotV1& sample,uint64_t hiddenStartFrame) noexcept {
        constexpr uint64_t MinimumFrameSeparation=8;
        if(sample.sourceFrame<=hiddenStartFrame||sample.voiceCount==0)return false;
        if(!haveBaseline_){baseline_=sample;haveBaseline_=true;return false;}
        if(sample.sourceFrame<=baseline_.sourceFrame||
           sample.sourceFrame-baseline_.sourceFrame<MinimumFrameSeparation)return false;
        const uint32_t a=std::min<uint32_t>(baseline_.voiceCount,LAB_PHYSICS_POPULATION_AUDIO_MAX_V1);
        const uint32_t b=std::min<uint32_t>(sample.voiceCount,LAB_PHYSICS_POPULATION_AUDIO_MAX_V1);
        for(uint32_t i=0;i<a;i++)for(uint32_t j=0;j<b;j++){
            const auto& before=baseline_.voices[i];const auto& after=sample.voices[j];
            if(before.insect!=after.insect)continue;
            const double dx=double(after.position.x)-before.position.x;
            const double dy=double(after.position.y)-before.position.y;
            const double dz=double(after.position.z)-before.position.z;
            const double squared=dx*dx+dy*dy+dz*dz;
            if(!std::isfinite(squared))continue;
            ++comparisons_;
            if(squared>1e-10){witness_=before.insect;distance_=std::sqrt(squared);return true;}
        }
        return false;
    }
    uint64_t BaselineFrame()const noexcept{return haveBaseline_?baseline_.sourceFrame:0;}
    uint64_t Comparisons()const noexcept{return comparisons_;}
    uint32_t Witness()const noexcept{return witness_;}
    double Distance()const noexcept{return distance_;}
};
}
