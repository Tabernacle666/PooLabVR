#include "AudioClip.h"
#include <xaudio2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <utility>

namespace OASIS {
AudioClip LoadMonoWav(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);
    char riff[12]{};
    if(!file.read(riff,sizeof(riff))||std::memcmp(riff,"RIFF",4)||std::memcmp(riff+8,"WAVE",4))return {};
    WAVEFORMATEX format{};
    AudioClip result;
    for(unsigned chunk=0;chunk<64&&file;++chunk){
        char id[4]{};
        uint32_t size{};
        if(!file.read(id,4)||!file.read(reinterpret_cast<char*>(&size),4)||size>2u*1024u*1024u)return {};
        const auto next=file.tellg()+std::streamoff(size+(size&1u));
        if(!std::memcmp(id,"fmt ",4)&&size>=16u){
            if(!file.read(reinterpret_cast<char*>(&format),16))return {};
        }else if(!std::memcmp(id,"data",4)&&size>=2u&&!(size&1u)){
            result.samples.resize(size/2u);
            if(!file.read(reinterpret_cast<char*>(result.samples.data()),size))return {};
        }
        if(format.wFormatTag&& !result.samples.empty()){
            if(format.wFormatTag!=WAVE_FORMAT_PCM||format.nChannels!=1||
               format.wBitsPerSample!=16||format.nSamplesPerSec<8000||format.nSamplesPerSec>96000)return {};
            result.sampleRate=format.nSamplesPerSec;
            return result;
        }
        file.seekg(next);
    }
    return {};
}

std::vector<int16_t> LoadMonoWavAtRate(const std::filesystem::path& path,uint32_t rate){
    auto clip=LoadMonoWav(path);
    if(clip.samples.empty()||rate==clip.sampleRate)return std::move(clip.samples);
    std::vector<int16_t> samples(size_t(uint64_t(clip.samples.size())*rate/clip.sampleRate));
    for(size_t i=0;i<samples.size();++i){
        const double source=double(i)*clip.sampleRate/rate;
        const size_t index=size_t(source);
        samples[i]=int16_t(std::lerp(double(clip.samples[index]),
            double(clip.samples[std::min(index+1,clip.samples.size()-1)]),source-index));
    }
    return samples;
}
}
