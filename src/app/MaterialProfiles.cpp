#include "MaterialProfiles.h"
#include "Assets.h"
#include "LocalProfile.h"
#include <algorithm>
#include <utility>

namespace OASIS {
namespace {
struct Slot {
    const char* key;

};
constexpr std::array<Slot,MaterialProfiles::SlotCount> Slots{{
    {"shuttle.hull"},
    {"shuttle.metal"},
    {"shuttle.trim"},
    {"shuttle.seat"},
    {"shuttle.controls"},
    {"shuttle.canopy"},
    {"shuttle.crt_glass"},
    {"oasis.logo"},
    {"oasis.windows"},
    {"oasis.soil"},
    {"oasis.stone"},
    {"oasis.vegetation"},
    {"oasis.river"},
    {"moon.ground"},
    {"moon.wall"},
    {"moon.equipment"},
    {"moon.markings"}
}};
struct Field {const char* name;int min,max,step;};
constexpr std::array<Field,MaterialProfiles::FieldCount> Fields{{
    {"red",0,300,5},{"green",0,300,5},{"blue",0,300,5},{"gamma",25,300,5},
    {"uv",10,400,5},{"roughness",-100,100,5},{"normal",0,300,5},{"metallic",-100,100,5},
    {"emission",0,400,5},{"opacity",0,200,5}
}};
struct Channel {
    const char* name;
    TextureUse use;
    uint32_t AssetMaterial::* index;
    uint32_t mapping;
};
constexpr Channel Channels[]{
    {"basecolor",TextureUse::Color,&AssetMaterial::albedo,0},
    {"normal",TextureUse::Normal,&AssetMaterial::normal,1},
    {"roughness",TextureUse::Linear,&AssetMaterial::roughnessOverride,Invalid},
    {"emissive",TextureUse::Color,&AssetMaterial::emissive,3}
};
}

void MaterialProfiles::Load(const std::filesystem::path& folder){
    values_.fill(DefaultMaterialTuning);
    factory_=values_;
    if(folder.empty())return;
    path_=folder/"materials.ini";
    textures_=folder.parent_path()/"mods"/"textures";
    std::error_code error;
    dirty_=!std::filesystem::exists(path_,error)&&!error;
    for(bool factory:{true,false})ReadLocalProfile(path_,[&](std::string_view key,std::string_view text){
        if(factory!=key.starts_with("factory."))return;
        if(factory)key.remove_prefix(8);
        const auto last=key.rfind('.');
        int number{};
        if(last==std::string_view::npos||!ProfileInt(text,number))return;
        for(uint32_t slot=0;slot<Slots.size();++slot)if(key.substr(0,last)==Slots[slot].key){
            for(uint32_t field=0;field<Fields.size();++field)if(key.substr(last+1)==Fields[field].name){
                const auto& range=Fields[field];
                const int value=std::clamp(number,range.min,range.max);
                values_[slot][field]=value;
                if(factory)factory_[slot][field]=value;
                return;
            }
        }
    });
}

void MaterialProfiles::Install(SceneData& scene) const {
    // Source declarations assign profile slots. Multiple materials can share one override texture.
    for(uint32_t slot=0;slot<Slots.size();++slot){
        std::vector<uint32_t> indices;
        for(uint32_t i=0;i<scene.materialNames.size()&&i<scene.materials.size();++i){
            if(scene.materials[i].authoringSlot==slot&&!scene.materials[i].profileInstalled)indices.push_back(i);
        }
        if(indices.empty())continue;
        for(uint32_t i:indices)scene.materials[i].profileInstalled=1u;
        if(textures_.empty())continue;
        for(const auto& channel:Channels){
            const auto stem=std::string(Slots[slot].key)+'.'+channel.name;
            TextureData texture;
            for(const char* extension:{".png",".jpg",".jpeg"}){
                texture=TryLoadOverrideTexture(textures_/(stem+extension),channel.use);
                if(texture.mips)break;
            }
            if(!texture.mips)continue;
            const uint32_t textureIndex=uint32_t(scene.textures.size());
            scene.textures.push_back(std::move(texture));
            for(uint32_t i:indices){
                auto& material=scene.materials[i];
                material.*channel.index=textureIndex;
                if(channel.mapping!=Invalid){
                    material.mapping[channel.mapping]={};
                    material.samplers[channel.mapping]=Invalid;
                }
                if(channel.mapping==0u)material.colorRoughness.x=material.colorRoughness.y=material.colorRoughness.z=1.f;
                if(channel.mapping==3u&&std::max({material.emissionMetallic.x,material.emissionMetallic.y,
                                                material.emissionMetallic.z})<=0.f)
                    material.emissionMetallic.x=material.emissionMetallic.y=material.emissionMetallic.z=1.f;
            }
        }
    }
}

bool MaterialProfiles::Edit(uint32_t slot,uint32_t field,int direction,bool restore){
    auto& value=values_[slot][field];
    const auto& range=Fields[field];
    const int next=restore?factory_[slot][field]:std::clamp(value+direction*range.step,range.min,range.max);
    if(next==value)return false;
    value=next;
    ++revision_;
    dirty_=true;
    return true;
}

void MaterialProfiles::OverwriteFactory(){
    factory_=values_;
    dirty_=true;
}

bool MaterialProfiles::Save(){
    if(!dirty_)return true;
    if(!WriteLocalProfile(path_,[&](std::ostream& out){
        for(uint32_t slot=0;slot<Slots.size();++slot){
            for(uint32_t field=0;field<Fields.size();++field){
                out<<Slots[slot].key<<'.'<<Fields[field].name<<'='<<values_[slot][field]<<'\n';
                out<<"factory."<<Slots[slot].key<<'.'<<Fields[field].name<<'='<<factory_[slot][field]<<'\n';
            }
            out<<'\n';
        }
    }))return false;
    dirty_=false;
    return true;
}

}
