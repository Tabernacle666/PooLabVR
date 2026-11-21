#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace OASIS {
struct AudioClip {
    uint32_t sampleRate{24000};
    std::vector<int16_t> samples;
};
AudioClip LoadMonoWav(const std::filesystem::path&);
std::vector<int16_t> LoadMonoWavAtRate(const std::filesystem::path&,uint32_t rate);
}
