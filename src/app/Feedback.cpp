#include "Feedback.h"
#include "AudioClip.h"
#include "ShuttlePilot.h"
#include "ShuttleAudio.h"
#include <xaudio2.h>
#include <algorithm>
#include <vector>

namespace OASIS {
void DiagnosticLine(const char*) noexcept;
namespace {
AudioClip CaptureClick(){
    AudioClip clip;
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
        AudioClip clip;
        IXAudio2SourceVoice* source{};
    };
    IXAudio2* engine{};
    IXAudio2MasteringVoice* master{};
    std::array<Voice,size_t(FeedbackCue::Count)> voices{};
    std::unique_ptr<ShuttleAudio> shuttle;

    Audio(const std::filesystem::path& directory,const FeedbackProfiles& profiles,bool shuttleAudio,
          const std::filesystem::path& packagedAudio){
        if(FAILED(XAudio2Create(&engine))||FAILED(engine->CreateMasteringVoice(&master)))return;
        for(size_t i=0;i<voices.size();++i){
            const auto cue=FeedbackCue(i);
            // The shuttle mixer owns engine loops, startup audio and spatial hull impacts.
            if(cue==FeedbackCue::ShuttleBoot||(cue>=FeedbackCue::ShuttleJet&&cue<=FeedbackCue::DustHull))continue;
            auto& voice=voices[i];
            const auto& sound=profiles.Sound(cue);
            voice.clip=LoadMonoWav(directory/sound.file);
            if(voice.clip.samples.empty()&&!packagedAudio.empty())
                voice.clip=LoadMonoWav(packagedAudio/sound.file);
            if((cue==FeedbackCue::CaptureSaved||cue==FeedbackCue::MenuDetent)&&voice.clip.samples.empty())
                voice.clip=CaptureClick();
            if(voice.clip.samples.empty())continue;
            const auto rate=voice.clip.sampleRate;
            const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,rate,rate*2u,2,16,0};
            if(SUCCEEDED(engine->CreateSourceVoice(&voice.source,&format)))voice.source->Start();
        }
        if(shuttleAudio)shuttle=std::make_unique<ShuttleAudio>(*engine,*master,directory,profiles);
    }
    ~Audio(){
        shuttle.reset();
        for(auto& voice:voices)if(voice.source)voice.source->DestroyVoice();
        if(master)master->DestroyVoice();
        if(engine)engine->Release();
    }
    void Play(size_t index,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter,const FeedbackProfiles::Audio& sound){
        auto& voice=voices[index];
        if(!voice.source)return;
        XAUDIO2_VOICE_STATE state{};
        voice.source->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if(state.BuffersQueued)return;
        float volume=event.strength*.01f*sound.gain;
        if(volume<=0.f)return;
        if(FeedbackProfiles::Spatial(event.cue)&&event.position){
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
        voice.source->SetFrequencyRatio(.01f*sound.pitch);
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=UINT32(voice.clip.samples.size()*sizeof(int16_t));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(voice.clip.samples.data());
        buffer.Flags=XAUDIO2_END_OF_STREAM;
        voice.source->SubmitSourceBuffer(&buffer);
    }
};

Feedback::Feedback(const std::filesystem::path& directory,const FeedbackProfiles& profiles,
                   ActionId hapticAction,bool shuttleAudio,const std::filesystem::path& packagedAudio)
    :audio_(std::make_unique<Audio>(directory,profiles,shuttleAudio,packagedAudio)),
     profiles_(profiles),hapticAction_(hapticAction){}
Feedback::~Feedback()=default;

void Feedback::Tick(XR& xr,const FrameInput& input){
    if(!input.focused||input.controlContext!=context_||(lastTime_&&
       (input.time<lastTime_||input.time-lastTime_>.25))){
        hands_={};
    }
    lastTime_=input.time;
    context_=input.controlContext;
    if(!input.focused)return;
    for(uint32_t i=0;i<HandCount;++i){
        auto& hand=hands_[i];
        if(!hand.echoStrength||input.time<hand.echoTime)continue;
        Send(xr,hand.echoCue,i,hand.echoStrength,hand.echoDuration,input.frameId);
        hand.busyUntil=input.time+hand.echoDuration;
        hand.echoStrength=0.f;
    }
}

void Feedback::Send(XR& xr,FeedbackCue cue,uint32_t hand,float strength,float duration,uint64_t frame){
    const auto result=xr.Haptic({hand,strength,duration,frame,hapticAction_});
    const bool report=cue==FeedbackCue::ShuttleIgnition||cue==FeedbackCue::RotorReverse;
    auto& reported=hands_[hand].reportedHapticError;
    if(!report&&(result==XR_SUCCESS||reported))return;
    reported|=result!=XR_SUCCESS;
    char line[180];
    std::snprintf(line,sizeof(line),"haptique=%s xr_result=%d main=%u force=%.2f duree_ms=%.0f image=%llu\r\n",
        FeedbackProfiles::Key(cue),int(result),hand,strength,1000.f*duration,static_cast<unsigned long long>(frame));
    DiagnosticLine(line);
}

void Feedback::UpdateShuttle(XR& xr,const ShuttlePilot& pilot,const FrameInput& input,bool enabled,bool boot,bool ignition,uint32_t reversals){
    if(audio_->shuttle)audio_->shuttle->Update(pilot,input,enabled,boot);
    if(!enabled)return;
    const auto event=[&](FeedbackCue cue,uint32_t hand,float strength=1.f,std::optional<XMFLOAT3> position={}){
        Emit(xr,input.head,{cue,hand,input.time,input.frameId,strength,position},input.sceneUnitsPerMeter);
    };
    if(boot)event(FeedbackCue::ShuttleBoot,HandCount);
    const auto& impact=pilot.Wake().LastImpact();
    if(impact.strength>0.f)
        event(impact.glass?FeedbackCue::DustGlass:FeedbackCue::DustHull,HandCount,impact.strength,impact.position);
    if(pilot.MenuChanged())event(FeedbackCue::MenuDetent,1u);
    for(uint32_t hand=0;hand<HandCount;++hand){
        if(ignition)event(FeedbackCue::ShuttleIgnition,hand);
        else if(reversals&(1u<<hand))event(FeedbackCue::RotorReverse,hand);
        const auto& loads=pilot.Loads();
        const auto jet=[&](uint32_t i){return std::hypot(loads[i].forceN.x,loads[i].forceN.y);};
        const auto fan=[&](uint32_t i){return std::abs(loads[i].rpm);};
        const float jets=float(std::max(jet(2u*hand),jet(2u*hand+1u))/
            Vaisseaux::Lunaire::Propulsion.reactor.curve.back().forceN);
        const float fans=float(std::max(fan(4u+2u*hand),fan(5u+2u*hand))/
            Vaisseaux::Lunaire::Propulsion.fan.maxRpm);
        if(profiles_.Pulse(FeedbackCue::ShuttleJet).mode==int(FeedbackProfiles::HapticMode::Drive)&&jets>.01f)
            event(FeedbackCue::ShuttleJet,hand,jets);
        if(profiles_.Pulse(FeedbackCue::ShuttleFan).mode==int(FeedbackProfiles::HapticMode::Drive)&&fans>.01f)
            event(FeedbackCue::ShuttleFan,hand,fans);
    }
}

void Feedback::Emit(XR& xr,const Pose& listener,const FeedbackEvent& event,float sceneUnitsPerMeter){
    const auto index=size_t(event.cue);
    if(index>=FeedbackCueCount)return;
    audio_->Play(index,listener,event,sceneUnitsPerMeter,profiles_.Sound(event.cue));
    const auto& profile=profiles_.Pulse(event.cue);
    const auto mode=FeedbackProfiles::HapticMode(profile.mode);
    if(profile.strength<=0||event.strength<=0.f||mode==FeedbackProfiles::HapticMode::Off)return;
    const float strength=.01f*profile.strength*std::clamp(event.strength,0.f,1.f);
    const float duration=.001f*profile.duration;
    const auto pulse=[&](uint32_t i){
        auto& hand=hands_[i];
        if(event.time<hand.next[index])return;
        if(mode==FeedbackProfiles::HapticMode::Drive&&
           (event.time<hand.busyUntil||hand.echoStrength>0.f||event.time<hand.driveUntil))return;
        hand.next[index]=event.time+.001*profile.interval;
        if(mode!=FeedbackProfiles::HapticMode::Shake&&profile.delay>0){
            if(hand.echoStrength>0.f)return;
            hand.echoTime=event.time+.001*profile.delay;
            hand.echoStrength=strength;
            hand.echoCue=event.cue;
            hand.echoDuration=duration;
            return;
        }
        Send(xr,event.cue,i,strength,duration,event.frameId);
        if(mode==FeedbackProfiles::HapticMode::Drive)hand.driveUntil=event.time+std::min(.05f,duration);
        else hand.busyUntil=event.time+duration;
        if(mode==FeedbackProfiles::HapticMode::Shake&&profile.echo>0){
            hand.echoTime=event.time+.001*profile.delay;
            hand.echoStrength=strength*.01f*profile.echo;
            hand.echoCue=event.cue;
            hand.echoDuration=.001f*profile.echoDuration;
        }
    };
    if(event.hand<HandCount)pulse(event.hand);
    else for(uint32_t i=0;i<HandCount;++i)pulse(i);
}
}
