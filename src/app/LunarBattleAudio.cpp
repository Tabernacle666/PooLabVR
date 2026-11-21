#include "LunarBattleAudio.h"
#include "AudioClip.h"
#include "FeedbackProfiles.h"
#define XAUDIO2_HELPER_FUNCTIONS
#include <xaudio2.h>
#if __has_include(<hrtfapoapi.h>)
#include <xapo.h>
#include <hrtfapoapi.h>
#define OASIS_LUNAR_HRTF
#endif
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace OASIS {
namespace {
constexpr uint32_t ClipRate=48000,Operations=1u;
constexpr size_t MotorCount=LunarTankCount*2u,TransientCount=20u;
constexpr float ExteriorRange=64.f;
using Clip=std::vector<float>;
using Clock=std::chrono::steady_clock;

Clip Synthesize(LunarCue cue,uint32_t variation){
    constexpr std::array<float,size_t(LunarCue::Count)> duration{.16f,.58f,.21f,.38f};
    Clip samples(size_t(ClipRate*duration[size_t(cue)]));
    uint32_t seed=0xa75bc246u+variation*5179u+uint32_t(cue)*3239u;
    const float pitch=1.f+.09f*float(variation);
    float low=0.f,body=0.f;
    for(size_t i=0;i<samples.size();++i){
        seed^=seed<<13u;seed^=seed>>17u;seed^=seed<<5u;
        const float noise=float(seed>>8u)*(2.f/16777216.f)-1.f;
        const float t=float(i)/float(ClipRate);
        low+=.021f*(noise-low);
        body+=.13f*(noise-body);
        float sample=0.f;
        switch(cue){
        case LunarCue::Rifle:
            sample=.64f*(noise-.6f*body)*std::exp(-155.f*t)+
                (.34f*body+.27f*std::sin(XM_2PI*183.f*pitch*t))*std::exp(-57.f*t)+
                .17f*body*std::exp(-25.f*std::abs(t-.044f));
            break;
        case LunarCue::Cannon:
            sample=.46f*noise*std::exp(-54.f*t)+
                (.65f*std::sin(XM_2PI*(50.f*t+1.3f*(1.f-std::exp(-11.f*t)))*pitch)+
                1.2f*low)*std::exp(-11.f*t);
            break;
        case LunarCue::Surface:
            sample=(.39f*body+.21f*std::sin(XM_2PI*290.f*pitch*t))*std::exp(-44.f*t)+
                .33f*noise*std::exp(-112.f*t);
            break;
        case LunarCue::Membrane:
            sample=(.31f*std::sin(XM_2PI*(84.f*t+1.8f*(1.f-std::exp(-17.f*t)))*pitch)+
                .16f*std::sin(XM_2PI*293.f*pitch*t)+.30f*body)*std::exp(-16.5f*t);
            break;
        case LunarCue::Count:break;
        }
        const float taper=std::min(1.f,float(samples.size()-i-1u)/640.f);
        samples[i]=std::clamp(sample*std::min(1.f,1750.f*t)*taper,-1.f,1.f);
    }
    return samples;
}

Clip SynthesizeMotor(){
    Clip samples(ClipRate);
    for(size_t i=0;i<samples.size();++i){
        const float phase=XM_2PI*float(i)/float(samples.size());
        samples[i]=.25f*std::sin(61.f*phase+.26f*std::sin(5.f*phase))+
            .13f*std::sin(122.f*phase)+.075f*std::sin(184.f*phase)+
            (.31f+.25f*std::sin(31.f*phase))*(.15f*std::sin(217.f*phase)+
                .08f*std::sin(313.f*phase)+.045f*std::sin(543.f*phase));
    }
    return samples;
}

struct ListeningPose {
    Pose head;
    XMFLOAT3 center;
    std::array<LunarMotor,MotorCount> motors{};
    bool inside{},audible{};
    float level{1.f};
    struct Mix {float gain{},pitch{1.f};};
    std::array<Mix,size_t(LunarCue::Count)> effects{};
    Mix motor;
};

float ShellDistance(const ListeningPose& listener){
    return std::max(0.f,Length(Sub(listener.center,listener.head.p))-LunarRadius);
}

float ExteriorGain(float distance){
    const float edge=std::clamp((ExteriorRange-distance)/24.f,0.f,1.f);
    return .74f*edge*edge*(3.f-2.f*edge)/std::pow(1.f+distance/3.8f,1.33f);
}

bool WallBetween(XMFLOAT3 ear,XMFLOAT3 sound){
    if(ear.x*sound.x>=0.f)return false;
    const float at=ear.x/(ear.x-sound.x);
    const auto hit=Add(ear,Scale(Sub(sound,ear),at));
    return LunarWallFace(hit);
}
}

struct LunarBattleAudio::Mixer {
    enum class DirectFilter {Open,Rear,Wall};
    struct Voice {
        IXAudio2SourceVoice* source{};
        IXAudio2SubmixVoice* spatial{};
#ifdef OASIS_LUNAR_HRTF
        IXAPOHrtfParameters* hrtf{};
#endif
        XMFLOAT3 position{};
        float strength{};
        bool playing{};
        DirectFilter filter{};
    };
    IXAudio2& engine;
    IXAudio2MasteringVoice& output;
    IXAudio2SubmixVoice* exterior{};
    uint32_t outputChannels{},outputRate{};
#ifdef OASIS_LUNAR_HRTF
    HMODULE spatialModule{};
    decltype(&CreateHrtfApo) createSpatial{};
#endif
    std::array<std::array<Clip,2>,size_t(LunarCue::Count)> effects;
    Clip motor;
    const FeedbackProfiles& profiles;
    std::array<Voice,MotorCount> engines{};
    std::array<Voice,TransientCount> transients{};
    uint32_t variation{};

    static constexpr size_t MaxEvents=96;
    std::array<LunarSoundEvent,MaxEvents> pending{},messages{};
    size_t pendingCount{},messageCount{};
    ListeningPose submitted{};
    std::mutex mailbox;
    std::condition_variable ready;
    bool running{},updated{},wasAudible{};
    std::thread worker;

    Mixer(IXAudio2& audio,IXAudio2MasteringVoice& master,const std::filesystem::path& directory,
          const FeedbackProfiles& settings):engine(audio),output(master),profiles(settings){
        XAUDIO2_VOICE_DETAILS details{};
        output.GetVoiceDetails(&details);
        outputChannels=details.InputChannels;
        outputRate=details.InputSampleRate;
        if(FAILED(engine.CreateSubmixVoice(&exterior,1,outputRate,XAUDIO2_VOICE_USEFILTER)))return;
        const XAUDIO2_FILTER_PARAMETERS damping{LowPassFilter,
            XAudio2CutoffFrequencyToRadians(740.f,outputRate),1.27f};
        exterior->SetFilterParameters(&damping);
        exterior->SetVolume(0.f);
#ifdef OASIS_LUNAR_HRTF
        if(outputRate==ClipRate&&outputChannels>=2u){
            spatialModule=LoadLibraryExW(L"HrtfApo.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
            if(spatialModule){
                const auto symbol=GetProcAddress(spatialModule,"CreateHrtfApo");
                static_assert(sizeof(symbol)==sizeof(createSpatial));
                std::memcpy(&createSpatial,&symbol,sizeof(symbol));
            }
        }
#endif
        auto load=[&](FeedbackCue cue){
            const auto input=LoadMonoWavAtRate(directory/profiles.Sound(cue).file,ClipRate);
            Clip clip;
            clip.reserve(input.size());
            for(const auto value:input)clip.push_back(float(value)/32768.f);
            return clip;
        };
        for(size_t i=0;i<effects.size();++i){
            const auto clip=load(LunarFeedback(LunarCue(i)));
            for(uint32_t j=0;j<effects[i].size();++j)
                effects[i][j]=clip.empty()?Synthesize(LunarCue(i),j):clip;
        }
        motor=load(FeedbackCue::LunarMotor);
        if(motor.empty())motor=SynthesizeMotor();
        for(auto& voice:engines)Create(voice);
        for(auto& voice:transients)Create(voice);
        running=true;
        worker=std::thread([this]{Run();});
    }

    ~Mixer(){
        if(worker.joinable()){
            {
                std::lock_guard lock(mailbox);
                running=false;
            }
            ready.notify_one();
            worker.join();
        }
        for(auto& voice:engines)Destroy(voice);
        for(auto& voice:transients)Destroy(voice);
        if(exterior)exterior->DestroyVoice();
#ifdef OASIS_LUNAR_HRTF
        if(spatialModule)FreeLibrary(spatialModule);
#endif
    }

    void Create(Voice& voice){
#ifdef OASIS_LUNAR_HRTF
        if(createSpatial){
            const HrtfApoInit init{};
            IXAPO* effect{};
            if(SUCCEEDED(createSpatial(&init,&effect))){
                IXAPOHrtfParameters* parameters{};
                if(SUCCEEDED(effect->QueryInterface(IID_PPV_ARGS(&parameters)))){
                    XAUDIO2_EFFECT_DESCRIPTOR descriptor{effect,TRUE,2u};
                    XAUDIO2_EFFECT_CHAIN chain{1u,&descriptor};
                    if(SUCCEEDED(engine.CreateSubmixVoice(&voice.spatial,1,ClipRate,0,0,nullptr,&chain)))
                        voice.hrtf=parameters;
                    else parameters->Release();
                }
                effect->Release();
            }
        }
#endif
        XAUDIO2_SEND_DESCRIPTOR sends[]{
            {XAUDIO2_SEND_USEFILTER,Direct(voice)},
            {0u,exterior}
        };
        const XAUDIO2_VOICE_SENDS routing{uint32_t(std::size(sends)),sends};
        const WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,1,ClipRate,ClipRate*4u,4,32,0};
        engine.CreateSourceVoice(&voice.source,&format,0u,2.f,nullptr,&routing);
    }

    static void Destroy(Voice& voice){
        if(voice.source)voice.source->DestroyVoice();
        if(voice.spatial)voice.spatial->DestroyVoice();
#ifdef OASIS_LUNAR_HRTF
        if(voice.hrtf)voice.hrtf->Release();
#endif
    }

    IXAudio2Voice* Direct(const Voice& voice)const{
        if(voice.spatial)return voice.spatial;
        return &output;
    }

    void Submit(const LunarBattle& battle,const FrameInput& input,float level){
        if(!exterior)return;
        ListeningPose pose{input.head,SolarCenter(SolarMoon,input.solarTime),battle.Motors()};
        pose.inside=input.insideSphere==SolarMoon;
        pose.level=level;
        const auto snapshot=[&](FeedbackCue cue){
            const auto& sound=profiles.Sound(cue);
            return ListeningPose::Mix{.01f*sound.gain,.01f*sound.pitch};
        };
        for(size_t i=0;i<pose.effects.size();++i)pose.effects[i]=snapshot(LunarFeedback(LunarCue(i)));
        pose.motor=snapshot(FeedbackCue::LunarMotor);
        pose.audible=level>0.f&&input.focused&&!battle.Paused()&&(pose.inside||ShellDistance(pose)<ExteriorRange);
        if(!pose.audible)pendingCount=0;
        if(!pose.audible&&!wasAudible)return;
        for(const auto& event:battle.Sounds()){
            if(!pose.audible||pendingCount==pending.size())break;
            pending[pendingCount++]=event;
        }
        // The renderer never waits for the audio thread. A missed handoff is
        // retried with the next pose and the events queued by this frame.
        std::unique_lock lock(mailbox,std::try_to_lock);
        if(!lock)return;
        wasAudible=pose.audible;
        if(!pose.audible)messageCount=0;
        const size_t count=std::min(pendingCount,messages.size()-messageCount);
        std::copy_n(pending.begin(),count,messages.begin()+messageCount);
        messageCount+=count;
        pendingCount=0;
        submitted=pose;
        updated=true;
        lock.unlock();
        ready.notify_one();
    }

    void Pan(IXAudio2Voice& voice,float bearing,float gain){
        std::array<float,XAUDIO2_MAX_AUDIO_CHANNELS> matrix{};
        matrix[0]=gain;
        if(outputChannels>=2u){
            const float pan=.5f*(1.f+std::clamp(bearing,-1.f,1.f));
            matrix[0]=gain*std::sqrt(1.f-pan);
            matrix[1]=gain*std::sqrt(pan);
        }
        voice.SetOutputMatrix(&output,1,outputChannels,matrix.data(),Operations);
    }

    void Route(Voice& voice,const ListeningPose& pose,FXMMATRIX headRotation,float blend){
        const auto ear=Sub(pose.head.p,pose.center);
        const auto offset=Sub(voice.position,ear);
        const float distance=Length(offset)/LunarInteriorUnitsPerMeter;
        const auto direction=distance>.001f?
            TransformDirection(Unit(offset),headRotation):XMFLOAT3{0.f,0.f,1.f};
        const bool occluded=blend>0.f&&WallBetween(ear,voice.position);
        const float directGain=blend*voice.strength*(occluded?.40f:1.f)/(1.f+.22f*distance);
        const float sharedGain=.54f*voice.strength;
        voice.source->SetOutputMatrix(exterior,1,1,&sharedGain,Operations);
        if(voice.spatial){
#ifdef OASIS_LUNAR_HRTF
            if(directGain>0.f){
                const HrtfPosition position{direction.x,direction.y,-direction.z};
                voice.hrtf->SetSourcePosition(&position);
            }
#endif
            voice.source->SetOutputMatrix(voice.spatial,1,1,&directGain,Operations);
        }else{
            Pan(*voice.source,direction.x,directGain);
        }
        const bool behind=!voice.spatial&&blend>0.f&&direction.z<-.25f;
        const auto damping=occluded?DirectFilter::Wall:behind?DirectFilter::Rear:DirectFilter::Open;
        if(voice.filter!=damping){
            const float cutoff=damping==DirectFilter::Wall?1300.f:4300.f;
            const XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter,
                damping==DirectFilter::Open?XAUDIO2_MAX_FILTER_FREQUENCY:
                    XAudio2CutoffFrequencyToRadians(cutoff,outputRate),1.f};
            voice.source->SetOutputFilterParameters(Direct(voice),&filter,Operations);
            voice.filter=damping;
        }
    }

    static bool Begin(Voice& voice,const Clip& clip,bool loop,uint32_t begin=0u){
        XAUDIO2_BUFFER buffer{};
        buffer.Flags=loop?0u:XAUDIO2_END_OF_STREAM;
        buffer.AudioBytes=uint32_t(clip.size()*sizeof(float));
        buffer.pAudioData=reinterpret_cast<const BYTE*>(clip.data());
        buffer.PlayBegin=begin;
        buffer.PlayLength=uint32_t(clip.size())-begin;
        if(loop)buffer.LoopCount=XAUDIO2_LOOP_INFINITE;
        if(FAILED(voice.source->SubmitSourceBuffer(&buffer)))return false;
        voice.playing=SUCCEEDED(voice.source->Start(0,Operations));
        if(!voice.playing)voice.source->FlushSourceBuffers();
        return voice.playing;
    }

    static void Retire(Voice& voice){
        if(!voice.source||!voice.playing)return;
        voice.source->Stop();
        voice.source->FlushSourceBuffers();
        voice.playing=false;
    }

    void Mix(const ListeningPose& pose,std::span<const LunarSoundEvent> events,float blend,float gain){
        if(gain<=0.f){
            exterior->SetVolume(0.f);
            for(auto& voice:engines)Retire(voice);
            for(auto& voice:transients)Retire(voice);
            return;
        }
        const auto toHead=XMMatrixTranspose(XMMatrixRotationQuaternion(XMLoadFloat4(&pose.head.q)));
        const auto direction=TransformDirection(Unit(Sub(pose.center,pose.head.p)),toHead);
        exterior->SetVolume(1.f,Operations);
        Pan(*exterior,direction.x,gain*(1.f-blend)*ExteriorGain(ShellDistance(pose)));
        const float direct=gain*blend;
        for(uint32_t i=0;i<engines.size();++i){
            auto& voice=engines[i];
            if(!voice.source)continue;
            const auto& source=pose.motors[i];
            voice.position=source.position;
            voice.strength=pose.motor.gain*(.20f+.13f*source.effort);
            Route(voice,pose,toHead,direct);
            voice.source->SetFrequencyRatio(std::clamp(pose.motor.pitch*(.84f+.34f*source.effort+.025f*float(i)),
                                                       .25f,2.f),Operations);
            if(!voice.playing)Begin(voice,motor,true,uint32_t(i*motor.size()/MotorCount));
        }
        for(auto& voice:transients){
            if(!voice.playing)continue;
            XAUDIO2_VOICE_STATE state{};
            voice.source->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if(state.BuffersQueued)Route(voice,pose,toHead,direct);
            else{
                voice.source->Stop(0,Operations);
                voice.playing=false;
            }
        }
        if(pose.audible){
            for(const auto& event:events){
                const auto& sound=pose.effects[size_t(event.cue)];
                if(sound.gain<=0.f)continue;
                const auto voice=std::find_if(transients.begin(),transients.end(),
                    [](const Voice& candidate){return candidate.source&&!candidate.playing;});
                if(voice==transients.end())break;
                voice->position=event.position;
                voice->strength=sound.gain*event.strength;
                voice->source->SetFrequencyRatio(sound.pitch,Operations);
                Route(*voice,pose,toHead,direct);
                Begin(*voice,effects[size_t(event.cue)][variation++%2u],false);
            }
        }
        engine.CommitChanges(Operations);
    }

    void Run(){
        ListeningPose pose{};
        auto last=Clock::now(),stamp=last;
        float blend=0.f,gain=0.f;
        while(true){
            std::array<LunarSoundEvent,MaxEvents> events;
            size_t count=0;
            {
                std::unique_lock lock(mailbox);
                ready.wait_for(lock,std::chrono::milliseconds(24),[this]{return updated||!running;});
                if(!running)break;
                if(updated){
                    pose=submitted;
                    count=messageCount;
                    std::copy_n(messages.begin(),count,events.begin());
                    messageCount=0;
                    updated=false;
                    stamp=Clock::now();
                }
            }
            const auto now=Clock::now();
            if(now-stamp>std::chrono::milliseconds(145))pose.audible=false;
            const float elapsed=std::clamp(std::chrono::duration<float>(now-last).count(),0.f,.050f);
            last=now;
            const auto advance=[elapsed](float from,float target,float rate){
                return from+std::clamp(target-from,-elapsed*rate,elapsed*rate);
            };
            if(!pose.audible&&gain<=0.f)continue;
            blend=advance(blend,pose.inside?1.f:0.f,5.6f);
            gain=advance(gain,pose.audible?pose.level:0.f,13.f);
            Mix(pose,{events.data(),count},blend,gain);
        }
    }
};

LunarBattleAudio::LunarBattleAudio(IXAudio2& engine,IXAudio2MasteringVoice& output,
                                 const std::filesystem::path& directory,const FeedbackProfiles& profiles)
    :mixer_(std::make_unique<Mixer>(engine,output,directory,profiles)){}
LunarBattleAudio::~LunarBattleAudio()=default;
void LunarBattleAudio::Submit(const LunarBattle& battle,const FrameInput& input,float level){
    mixer_->Submit(battle,input,level);
}
}
