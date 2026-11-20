#include "ShuttleAudio.h"
#include "ShuttlePilot.h"
#include "shuttle/ShuttleLayout.h"
#include <xaudio2.h>

namespace OASIS {
namespace {
constexpr uint32_t Rate=24000;
constexpr float UpdatePeriod=1.f/36.f;

float Tone(float hz,float t) {return std::sin(XM_2PI*hz*t);}

float Noise(uint32_t& seed) {
    seed=1664525u*seed+1013904223u;
    return float(seed>>8u)*(1.f/8388608.f)-1.f;
}

std::vector<int16_t> MotorLoop(bool fan) {
    constexpr size_t LoopSize=Rate*2u,FadeSize=480u;
    std::vector<float> noise(LoopSize+FadeSize);
    uint32_t seed=fan?173u:1367u;
    float slow=0.f,fast=0.f;
    for(auto& sample:noise){
        const float n=Noise(seed);
        slow=std::lerp(slow,n,fan?.13f:.036f);
        fast=std::lerp(fast,n,.24f);
        sample=fan?.55f*fast+.11f*n:.8f*slow+.24f*fast;
    }
    std::vector<int16_t> samples(LoopSize);
    for(size_t i=0;i<samples.size();++i){
        const float t=float(i)/Rate;
        const float turbulence=i<FadeSize?std::lerp(noise[LoopSize+i],noise[i],float(i)/FadeSize):noise[i];
        const float harmonics=fan?.16f*Tone(84.f,t)+.11f*Tone(168.f,t)+.065f*Tone(252.f,t):
            .23f*Tone(46.f,t)+.10f*Tone(69.f,t)+.045f*Tone(138.f,t);
        const float signal=harmonics*(.90f+.10f*Tone(fan?4.f:2.5f,t))+turbulence;
        samples[i]=int16_t(26000.f*std::clamp(signal,-1.f,1.f));
    }
    return samples;
}

std::vector<int16_t> Cue(bool boot,bool glass) {
    std::vector<int16_t> samples(boot?Rate*62u/100u:Rate*13u/100u);
    uint32_t seed=113u;
    float low=0.f;
    for(size_t i=0;i<samples.size();++i){
        const float t=float(i)/Rate;
        const float burst=Noise(seed);
        low=std::lerp(low,burst,glass?.36f:.10f);
        float sound{};
        if(boot){
            const float click=(.45f*low+.23f*Tone(280.f,t))*std::exp(-46.f*t);
            const float t2=std::max(0.f,t-.115f);
            const float relay=(.22f*Tone(780.f,t2)+.16f*Tone(1170.f,t2))*std::min(1.f,t2*180.f)*std::exp(-9.f*t2);
            const float swell=.18f*Tone(95.f+22.f*t,t)*std::sin(XM_PI*std::min(1.f,t/.62f));
            sound=click+relay+swell;
        }else{
            const float ring=glass?.28f*Tone(840.f,t)+.14f*Tone(1530.f,t):.38f*Tone(220.f,t);
            sound=(.46f*low+ring)*std::exp(-(glass?46.f:34.f)*t);
        }
        samples[i]=int16_t(26000.f*std::min(1.f,t*2600.f)*std::clamp(sound,-1.f,1.f));
    }
    return samples;
}
}

struct ShuttleAudio::Mixer {
    static constexpr uint32_t MotorCount=ShuttlePilot::PropulsorCount;
    struct Voice {
        IXAudio2SourceVoice* source{};
        bool playing{};
    };
    IXAudio2MasteringVoice& output;
    std::array<Voice,MotorCount> motors{};
    std::array<Voice,6> impacts{};
    Voice ignition;
    const std::vector<int16_t> jetLoop=MotorLoop(false),fanLoop=MotorLoop(true);
    const std::vector<int16_t> glassHit=Cue(false,true),hullHit=Cue(false,false),boot=Cue(true,false);
    uint32_t channels{};
    double nextUpdate_{};

    Mixer(IXAudio2& engine,IXAudio2MasteringVoice& master):output(master){
        XAUDIO2_VOICE_DETAILS details{};
        output.GetVoiceDetails(&details);
        channels=details.InputChannels;
        const WAVEFORMATEX format{WAVE_FORMAT_PCM,1,Rate,Rate*2u,2,16,0};
        auto create=[&](Voice& voice){
            engine.CreateSourceVoice(&voice.source,&format,XAUDIO2_VOICE_USEFILTER,2.f);
        };
        for(auto& voice:motors)create(voice);
        for(auto& voice:impacts)create(voice);
        create(ignition);
    }

    ~Mixer(){
        for(auto& voice:motors)if(voice.source)voice.source->DestroyVoice();
        for(auto& voice:impacts)if(voice.source)voice.source->DestroyVoice();
        if(ignition.source)ignition.source->DestroyVoice();
    }

    static void Play(Voice& voice,std::span<const int16_t> clip,bool loop){
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes=uint32_t(clip.size_bytes());
        buffer.pAudioData=reinterpret_cast<const BYTE*>(clip.data());
        buffer.Flags=loop?0u:XAUDIO2_END_OF_STREAM;
        buffer.LoopCount=loop?XAUDIO2_LOOP_INFINITE:0u;
        if(SUCCEEDED(voice.source->SubmitSourceBuffer(&buffer))){
            voice.source->Start();
            voice.playing=loop;
        }
    }

    void Spatial(Voice& voice,const Pose& listener,XMFLOAT3 source,float pitch,float gain,float cutoff){
        const auto direction=Unit(Sub(source,listener.p));
        const auto view=Matrix(listener);
        const float pan=std::clamp(Dot(direction,TransformDirection({1,0,0},view)),-1.f,1.f);
        const float front=Dot(direction,TransformDirection({0,0,1},view));
        const float up=Dot(direction,TransformDirection({0,1,0},view));
        if(channels>=2u&&channels<=XAUDIO2_MAX_AUDIO_CHANNELS){
            std::array<float,XAUDIO2_MAX_AUDIO_CHANNELS> volumes{};
            volumes[0]=std::sqrt(.5f*(1.f-.87f*pan));
            volumes[1]=std::sqrt(.5f*(1.f+.87f*pan));
            voice.source->SetOutputMatrix(&output,1,channels,volumes.data());
        }
        const float frequency=std::clamp(pitch*(1.f+.09f*up),.25f,2.f);
        const float hz=std::clamp(cutoff*(.69f+.27f*front+.14f*up),210.f,7200.f);
        const XAUDIO2_FILTER_PARAMETERS filter{LowPassFilter,
            XAudio2CutoffFrequencyToRadians(hz,Rate),1.f};
        voice.source->SetFilterParameters(&filter);
        voice.source->SetFrequencyRatio(frequency);
        voice.source->SetVolume(gain);
    }

    void OneShot(Voice& voice,std::span<const int16_t> clip,const Pose& head,XMFLOAT3 source,
                 float pitch,float volume,float cutoff){
        if(!voice.source||volume<=0.f)return;
        XAUDIO2_VOICE_STATE state{};
        voice.source->GetState(&state,XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if(state.BuffersQueued)return;
        Spatial(voice,head,source,pitch,volume,cutoff);
        Play(voice,clip,false);
    }

    void Update(const ShuttlePilot& pilot,const FrameInput& input,bool enabled,bool booting){
        using Channel=ShuttlePilot::Mixer;
        if(!enabled||!pilot.AvionicsOn()){
            for(auto& voice:motors)if(voice.playing){
                voice.source->Stop();
                voice.source->FlushSourceBuffers();
                voice.playing=false;
            }
            nextUpdate_=0.;
            return;
        }
        const auto root=pilot.ModelToWorld();
        if(booting)
            OneShot(ignition,boot,input.head,TransformPoint({1.1f,.16f,0.f},root),1.f,
                    .57f*pilot.Mix(Channel::Ignition),6800.f);
        const auto& hit=pilot.Wake().LastImpact();
        if(hit.strength>0.f){
            const auto local=TransformPoint(hit.position,XMMatrixInverse(nullptr,root));
            const auto offset=Sub(local,ShuttleLayout::Eye);
            const auto x=std::abs(offset.x),y=std::abs(offset.y),z=std::abs(offset.z);
            const size_t side=y>=x&&y>=z?(offset.y>0.f?0u:1u):z>=x?(offset.z>0.f?2u:3u):(offset.x>0.f?4u:5u);
            OneShot(impacts[side],hit.glass?glassHit:hullHit,input.head,hit.position,
                    .84f+.17f*hit.strength,pilot.Mix(Channel::DustImpact)*hit.strength*.55f,
                    hit.glass?6500.f:2900.f);
        }
        if(input.time<nextUpdate_)return;
        nextUpdate_=input.time+UpdatePeriod;
        for(size_t i=0;i<motors.size();++i){
            auto& voice=motors[i];
            if(!voice.source)continue;
            const auto& load=pilot.Loads()[i];
            const bool fan=Vaisseaux::Lunaire::Mounts[i].organ==Vaisseaux::Organ::Fan;
            const bool front=load.positionMetres.x>=0.;
            const float rpm=float(load.rpm/(fan?Vaisseaux::Lunaire::Propulsion.fan.maxRpm:
                                              Vaisseaux::Lunaire::Propulsion.reactor.maxRpm));
            const float power=fan?std::clamp(rpm,0.f,1.f):std::clamp(float(std::hypot(load.forceN.x,load.forceN.y)/
                Vaisseaux::Lunaire::Propulsion.reactor.curve.back().forceN),0.f,1.f);
            const float gain=std::lerp(pilot.Mix(fan?Channel::FanMin:Channel::JetMin),
                pilot.Mix(fan?Channel::FanMax:Channel::JetMax),std::sqrt(power));
            const XMFLOAT3 p{float(load.positionMetres.x),float(load.positionMetres.z),float(load.positionMetres.y)};
            const auto origin=TransformPoint(p,root);
            const float distance=Length(Sub(origin,input.head.p))/input.sceneUnitsPerMeter;
            const float loudness=(fan?.28f:.33f)*gain*std::clamp(rpm*8.f,0.f,1.f)/(1.f+.23f*distance);
            Spatial(voice,input.head,origin,(fan?.55f+1.12f*power:.61f+.83f*power)*(front?1.11f:.92f),
                    loudness,fan?(front?6400.f:4400.f):(front?5000.f:2950.f));
            if(!voice.playing)Play(voice,fan?fanLoop:jetLoop,true);
        }
    }
};

ShuttleAudio::ShuttleAudio(IXAudio2& engine,IXAudio2MasteringVoice& output)
    :mixer_(std::make_unique<Mixer>(engine,output)){}
ShuttleAudio::~ShuttleAudio()=default;
void ShuttleAudio::Update(const ShuttlePilot& pilot,const FrameInput& input,bool enabled,bool ignition){
    mixer_->Update(pilot,input,enabled,ignition);
}
}
