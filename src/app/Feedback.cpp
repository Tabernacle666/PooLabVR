#include "Feedback.h"
#include "LunarBattleAudio.h"
#include "ShuttleAudio.h"
#include "XR.h"
#include <xaudio2.h>
#include <algorithm>
#include <fstream>
#include <vector>

namespace OASIS {
namespace {
struct CueProfile {
    const wchar_t* sound;
    float amplitude,duration;
    bool spatial;
};
constexpr CueProfile Profiles[]{
    {L"capture.wav",.32f,.032f,false},
    {L"surface-enter.wav",.16f,.031f,true},
    {nullptr,0.f,0.f,true},
    {L"surface-exit.wav",.12f,.025f,true}
};
static_assert(std::size(Profiles)==size_t(FeedbackCue::Count));

struct Clip {
    uint32_t sampleRate{24000};
    std::vector<int16_t> samples;
};
Clip LoadMonoWav(const std::filesystem::path& path){
    std::ifstream file(path,std::ios::binary);
    char riff[12]{};
    if(!file.read(riff,sizeof(riff))||std::memcmp(riff,"RIFF",4)||std::memcmp(riff+8,"WAVE",4))return {};
    WAVEFORMATEX format{};
    Clip result;
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

Clip CaptureClick(){
    Clip clip;
    clip.samples.resize(880);
    for(size_t i=0;i<clip.samples.size();++i){
        const float t=float(i)/float(clip.sampleRate);
        const float envelope=std::min(1.f,t*1900.f)*std::exp(-155.f*t);
        const float sound=.72f*std::sin(XM_2PI*780.f*t)+.28f*std::sin(XM_2PI*1280.f*t);
        clip.samples[i]=int16_t(10500.f*envelope*sound);
    }
    return clip;
}
}

struct Feedback::Audio {
    struct Voice {
        Clip clip;
        IXAudio2SourceVoice* source{};
    };
    IXAudio2* engine{};
    IXAudio2MasteringVoice* master{};
    std::array<Voice,size_t(FeedbackCue::Count)> voices{};
    std::unique_ptr<LunarBattleAudio> lunar;
    std::unique_ptr<ShuttleAudio> shuttle;

    Audio(const std::filesystem::path& directory,bool lunarAudio,bool shuttleAudio){
        if(FAILED(XAudio2Create(&engine))||FAILED(engine->CreateMasteringVoice(&master)))return;
        for(size_t i=0;i<voices.size();++i){
            if(!Profiles[i].sound)continue;
            auto& voice=voices[i];
            voice.clip=LoadMonoWav(directory/Profiles[i].sound);
            if(i==size_t(FeedbackCue::CaptureSaved)&&voice.clip.samples.empty())voice.clip=CaptureClick();
            if(voice.clip.samples.empty())continue;
            const auto rate=voice.clip.sampleRate;
            const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,rate,rate*2u,2,16,0};
            if(SUCCEEDED(engine->CreateSourceVoice(&voice.source,&format)))voice.source->Start();
        }
        if(lunarAudio)lunar=std::make_unique<LunarBattleAudio>(*engine,*master);
        if(shuttleAudio)shuttle=std::make_unique<ShuttleAudio>(*engine,*master);
    }
    ~Audio(){
        shuttle.reset();
        lunar.reset();
        for(auto& voice:voices)if(voice.source)voice.source->DestroyVoice();
        if(master)master->DestroyVoice();
        if(engine)engine->Release();
    }
    void Play(size_t index,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter){
        auto& voice=voices[index];
        if(!voice.source)return;
        XAUDIO2_VOICE_STATE state{};
        voice.source->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if(state.BuffersQueued)return;
        float volume=event.strength;
        if(Profiles[index].spatial&&event.position){
            const auto offset=Sub(*event.position,listener.p);
            volume/=1.f+Length(offset)/sceneUnitsPerMeter;
            XAUDIO2_VOICE_DETAILS details{};
            master->GetVoiceDetails(&details);
            if(details.InputChannels==2){
                const auto right=Sub(Transform({1.f,0.f,0.f},listener),listener.p);
                const float pan=std::clamp(Dot(Unit(offset),right),-1.f,1.f);
                const float matrix[]{std::sqrt(.5f*(1.f-pan)),std::sqrt(.5f*(1.f+pan))};
                voice.source->SetOutputMatrix(master,1,2,matrix);
            }
        }
        voice.source->SetVolume(volume);
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=UINT32(voice.clip.samples.size()*sizeof(int16_t));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(voice.clip.samples.data());
        buffer.Flags=XAUDIO2_END_OF_STREAM;
        voice.source->SubmitSourceBuffer(&buffer);
    }
};

Feedback::Feedback(const std::filesystem::path& directory,ActionId hapticAction,bool lunarAudio,bool shuttleAudio)
    :audio_(std::make_unique<Audio>(directory,lunarAudio,shuttleAudio)),hapticAction_(hapticAction){}
Feedback::~Feedback()=default;

void Feedback::UpdateShuttle(const ShuttlePilot& pilot,const FrameInput& input,bool enabled,bool ignition){
    if(audio_->shuttle)audio_->shuttle->Update(pilot,input,enabled,ignition);
}

void Feedback::UpdateLunar(const LunarBattle& battle,const FrameInput& input){
    if(audio_->lunar)audio_->lunar->Submit(battle,input);
}

void Feedback::Emit(XR& xr,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter){
    const auto index=size_t(event.cue);
    if(index>=std::size(Profiles))return;
    const auto& profile=Profiles[index];
    audio_->Play(index,listener,event,sceneUnitsPerMeter);
    if(event.hand>=HandCount||profile.amplitude<=0.f)return;
    auto& next=nextPulse_[event.hand];
    const bool confirmation=event.cue==FeedbackCue::CaptureSaved;
    if(event.time<next&&!confirmation)return;
    next=event.time+.065;
    xr.Haptic({event.hand,profile.amplitude*std::clamp(event.strength,0.f,1.f),
                profile.duration,event.frameId,hapticAction_});
}
}
