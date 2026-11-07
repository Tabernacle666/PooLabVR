#include "LabAudio.h"
namespace lab {
void ClientAudio::Reset(uint32_t index,uint32_t identity,bool loop){auto& voice=voices_[index];if(loop){voice.phase=voice.modulation=voice.fireBody=voice.crackle=0;voice.noise=0x5a769fe1u^identity;}else{voice.phase=voice.modulation=voice.fireBody=voice.crackle=0;voice.envelope=1;voice.noise=0x5a769fe1u^identity;}}
bool ClientAudio::Generate(const AudioSignalRequest& request,std::span<float> samples){auto& voice=voices_[request.voice];uint32_t offset=0;for(float& sample:samples){
                float t=float(++offset)/float(samples.size());float frequency=request.frequency+(request.frequencyEnd-request.frequency)*t;float doppler=request.doppler+(request.dopplerEnd-request.doppler)*t;
                voice.phase=std::fmod(voice.phase+XM_2PI*std::clamp(frequency*doppler,10.f,18000.f)/48000.f,XM_2PI);float value=0;
                if(request.loop){
                    voice.modulation=std::fmod(voice.modulation+XM_2PI*11.7f/48000.f,XM_2PI);
                    if(request.wave==LabWaves::Buzz)value=(std::sin(voice.phase)+.4f*std::sin(2*voice.phase)+.23f*std::sin(3*voice.phase)+.1f*std::sin(7*voice.phase))*(.75f+.25f*std::sin(voice.modulation));
                    else if(request.wave==LabWaves::Sine)value=std::sin(voice.phase);
                    else if(request.wave==LabWaves::Fire){
                        auto& seed=voice.noise;seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
                        float noise=float(seed&0xffff)/32767.5f-1;
                        voice.fireBody+=(noise-voice.fireBody)*.016f;
                        if((seed>>16)<9)voice.crackle=.30f+.70f*float(seed&1023)/1023.f;
                        voice.crackle*=.991f;
                        value=voice.fireBody*1.8f+voice.crackle*(noise*.65f+std::sin(voice.phase*7.f)*.22f);
                    }
                    else {auto& noise=voice.noise;noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;value=float(noise&0xffff)/32767.5f-1;}
                }else{if(request.wave==LabWaves::Noise||request.wave==LabWaves::Fire){auto& noise=voice.noise;noise^=noise<<13;noise^=noise>>17;noise^=noise<<5;float sample=float(noise&0xffff)/32767.5f-1;float cutoff=std::clamp(frequency*doppler*XM_2PI/48000.f,.005f,1.f);voice.fireBody+=(sample-voice.fireBody)*cutoff;value=voice.fireBody*voice.envelope;}else value=(std::sin(voice.phase)+.3f*std::sin(voice.phase*2.7f))*voice.envelope;voice.envelope*=request.decay;}
    sample=value;
    }return request.loop||voice.envelope>=.00001f;
}
AudioSignalProvider ClientAudio::Provider(){return {this,
    [](void* context,uint32_t voice,uint32_t identity,bool loop){static_cast<ClientAudio*>(context)->Reset(voice,identity,loop);},
    [](void* context,const AudioSignalRequest& request,std::span<float> samples){return static_cast<ClientAudio*>(context)->Generate(request,samples);}};}
}
