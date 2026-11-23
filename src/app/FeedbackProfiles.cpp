#include "FeedbackProfiles.h"
#include "LocalProfile.h"
#include <algorithm>

namespace OASIS {
namespace {
using Audio=FeedbackProfiles::Audio;
using Haptic=FeedbackProfiles::Haptic;
struct Cue {const char* key;const char* file;bool spatial;Haptic haptic;};
constexpr std::array<Cue,FeedbackCueCount> Catalogue{{
    {"capture","capture.wav",false,{1,32,32}},
    {"surface_enter","surface-enter.wav",true,{1,16,31}},
    {"surface_slide","surface-slide.wav",true,{3,0,45,0,100,32,65}},
    {"surface_exit","surface-exit.wav",true,{1,12,25}},
    {"shuttle_boot","shuttle-boot.wav",false,{}},
    {"shuttle_ignition","shuttle-ignition.wav",false,{2,100,140,220,100,320}},
    {"rotor_reverse","rotor-reverse.wav",false,{1,100,240}},
    {"shuttle_jet","shuttle-jet.wav",true,{}},
    {"shuttle_fan","shuttle-fan.wav",true,{}},
    {"dust_glass","dust-glass.wav",true,{}},
    {"dust_hull","dust-hull.wav",true,{}},
    {"menu_detent","menu-detent.wav",false,{1,22,28,0,100,32,26}}
}};
template<class T>
struct Field {const char* key;int T::* value;int min,max,step;};
constexpr std::array<Field<Audio>,FeedbackProfiles::AudioFields> SoundFields{{
    {"gain",&Audio::gain,0,200,5},{"pitch",&Audio::pitch,25,200,5}
}};
constexpr std::array<Field<Haptic>,FeedbackProfiles::HapticFields> PulseFields{{
    {"mode",&Haptic::mode,0,3,1},{"force",&Haptic::strength,0,100,5},
    {"duree_ms",&Haptic::duration,10,2000,10},{"delai_ms",&Haptic::delay,0,2000,10},
    {"echo_force",&Haptic::echo,0,100,5},{"echo_duree_ms",&Haptic::echoDuration,10,2000,10},
    {"intervalle_ms",&Haptic::interval,10,2000,10}
}};
constexpr const char* ModeCodes="-TSD";

template<class T,size_t N>
void SetField(T& target,std::string_view key,std::string_view text,const std::array<Field<T>,N>& fields){
    for(const auto& field:fields)if(key==field.key){
        int value{};
        if(key=="mode"&&text.size()==1&&std::string_view(ModeCodes).find(text)!=std::string_view::npos)
            value=int(std::string_view(ModeCodes).find(text));
        else if(!ProfileInt(text,value))return;
        target.*(field.value)=std::clamp(value,field.min,field.max);
    }
}

template<class T,size_t N>
void WriteFields(std::ostream& out,std::string_view prefix,const T& value,const std::array<Field<T>,N>& fields){
    for(const auto& field:fields){
        out<<prefix<<'.'<<field.key<<'=';
        const int number=value.*(field.value);
        if(std::string_view(field.key)=="mode")out<<ModeCodes[number];
        else out<<number;
        out<<'\n';
    }
}

template<class T>
bool Adjust(T& value,const T& factory,const Field<T>& field,int steps,bool reset){
    auto& current=value.*(field.value);
    const auto next=reset?factory.*(field.value):std::clamp(current+steps*field.step,field.min,field.max);
    if(next==current)return false;
    current=next;
    return true;
}
}

FeedbackProfiles::FeedbackProfiles(){
    for(uint32_t i=0;i<FeedbackCueCount;++i){
        audio_[i].file=Catalogue[i].file;
        haptic_[i]=Catalogue[i].haptic;
    }
    factoryAudio_=audio_;
    factoryHaptic_=haptic_;
}
const char* FeedbackProfiles::Key(FeedbackCue cue){return Catalogue[uint32_t(cue)].key;}
bool FeedbackProfiles::Spatial(FeedbackCue cue){return Catalogue[uint32_t(cue)].spatial;}

void FeedbackProfiles::Load(const std::filesystem::path& folder){
    if(folder.empty())return;
    folder_=folder;
    std::error_code audioError,hapticError;
    audioDirty_=!std::filesystem::exists(folder/"audio.ini",audioError)&&!audioError;
    hapticDirty_=!std::filesystem::exists(folder/"haptics.ini",hapticError)&&!hapticError;
    auto each=[&](const auto& path,auto read){
        for(const bool factory:{true,false})ReadLocalProfile(path,[&](std::string_view key,std::string_view value){
            if(factory!=key.starts_with("factory."))return;
            if(factory)key.remove_prefix(8);
            const auto separator=key.find('.');
            if(separator==std::string_view::npos)return;
            for(uint32_t i=0;i<FeedbackCueCount;++i)
                if(key.substr(0,separator)==Catalogue[i].key)read(factory,i,key.substr(separator+1),value);
        });
    };
    each(folder/"audio.ini",[&](bool factory,uint32_t i,std::string_view field,std::string_view value){
        auto apply=[&](Audio& sound){
            if(field=="file"){
                // WAV assets are looked up by basename inside the audio folder.
                if(!value.empty()&&value.ends_with(".wav")&&
                   value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-")==std::string_view::npos)
                    sound.file=value;
            }else SetField(sound,field,value,SoundFields);
        };
        if(factory)apply(factoryAudio_[i]);
        apply(audio_[i]);
    });
    each(folder/"haptics.ini",[&](bool factory,uint32_t i,std::string_view field,std::string_view value){
        if(factory)SetField(factoryHaptic_[i],field,value,PulseFields);
        SetField(haptic_[i],field,value,PulseFields);
    });
}

bool FeedbackProfiles::Save(){
    if(folder_.empty())return true;
    if(audioDirty_&&WriteLocalProfile(folder_/"audio.ini",[&](std::ostream& out){
        for(uint32_t i=0;i<FeedbackCueCount;++i){
            const auto key=std::string(Catalogue[i].key);
            out<<key<<".file="<<audio_[i].file<<'\n';
            WriteFields(out,key,audio_[i],SoundFields);
            WriteFields(out,"factory."+key,factoryAudio_[i],SoundFields);
            out<<"factory."<<key<<".file="<<factoryAudio_[i].file<<"\n\n";
        }
    }))audioDirty_=false;
    if(hapticDirty_&&WriteLocalProfile(folder_/"haptics.ini",[&](std::ostream& out){
        for(uint32_t i=0;i<FeedbackCueCount;++i){
            const auto key=std::string(Catalogue[i].key);
            WriteFields(out,key,haptic_[i],PulseFields);
            WriteFields(out,"factory."+key,factoryHaptic_[i],PulseFields);
            out<<'\n';
        }
    }))hapticDirty_=false;
    return !audioDirty_&&!hapticDirty_;
}

void FeedbackProfiles::OverwriteFactory(){
    factoryAudio_=audio_;
    factoryHaptic_=haptic_;
    audioDirty_=hapticDirty_=true;
}

int FeedbackProfiles::Value(bool haptic,uint32_t cue,int field) const{
    return haptic?haptic_[cue].*(PulseFields[field].value):audio_[cue].*(SoundFields[field].value);
}

void FeedbackProfiles::Edit(bool haptic,uint32_t cue,int field,int steps,bool reset){
    if(haptic)hapticDirty_|=Adjust(haptic_[cue],factoryHaptic_[cue],PulseFields[field],steps,reset);
    else audioDirty_|=Adjust(audio_[cue],factoryAudio_[cue],SoundFields[field],steps,reset);
}

}
