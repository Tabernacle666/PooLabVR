#include "PhysicalMaterials.h"
#include "Assets.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace OASIS {
namespace {
using Json=nlohmann::json;
Json Read(const std::filesystem::path& path){
    std::ifstream input(path);
    if(!input)throw std::runtime_error("Missing canonical material data: "+path.string());
    auto data=Json::parse(input);
    if(data.at("schema_version")!=1)throw std::runtime_error("Unsupported canonical material schema");
    return data;
}
float Range(const Json& object,const char* key,float lo,float hi){
    const auto value=object.at(key).get<float>();
    if(!std::isfinite(value)||value<lo||value>hi)
        throw std::runtime_error(std::string("Invalid physical material parameter: ")+key);
    return value;
}
uint64_t StableId(std::string_view id){
    uint64_t h=14695981039346656037ull;
    for(unsigned char c:id){h^=c;h*=1099511628211ull;}
    if(!h)throw std::runtime_error("Reserved material identity");
    return h;
}
struct Optical {
    uint64_t id;
    float roughness,metallic;
    XMFLOAT4 policy,layers;
    XMFLOAT2 transmission;
    bool special;
};
const auto& Registry(){
    static const auto registry=[]{
        std::unordered_map<std::string,Optical> result;
        std::unordered_map<uint64_t,std::string> ids;
        const auto data=Read(PhysicalMaterialLibraryPath());
        for(const auto& [id,m]:data.at("materials").items()){
            const auto& o=m.at("optical");
            const auto mode=o.at("reflection").get<std::string>();
            if(mode!="none"&&mode!="broad"&&mode!="structured"&&mode!="special")
                throw std::runtime_error("Unknown reflection domain: "+id);
            const auto roughness=Range(o,"roughness",.04f,1.f);
            const auto floor=Range(o,"roughness_min",.04f,roughness);
            const auto metal=Range(o,"metallic",0.f,1.f);
            const auto ior=Range(o,"ior",1.f,4.f);
            const auto r=(ior-1.f)/(ior+1.f);
            const auto f0=r*r*Range(o,"dielectric_specular_weight",0.f,1.f);
            const auto hash=StableId(id);
            if(!ids.emplace(hash,id).second)throw std::runtime_error("Material identity collision");
            result.emplace(id,Optical{hash,roughness,metal,
                {mode=="structured"?1.f:mode=="broad"?.25f:0.f,f0,floor,metal},
                {Range(o,"clearcoat",0.f,1.f),Range(o,"clearcoat_roughness",.04f,1.f),
                 Range(o,"anisotropy",0.f,1.f),Range(o,"diffuse_transmission",0.f,1.f)},
                {ior,Range(o,"transmission",0.f,1.f)},mode=="special"});
        }
        return result;
    }();
    return registry;
}
const Json& Bindings(){
    static const auto bindings=Read(PhysicalMaterialBindingsPath());
    return bindings.at("sources");
}
}
std::filesystem::path PhysicalMaterialLibraryPath(){return AssetPath(L"materials/library.json");}
std::filesystem::path PhysicalMaterialBindingsPath(){return AssetPath(L"materials/bindings.json");}
std::string PhysicalMaterialBinding(const std::filesystem::path& source,std::string_view slot,std::string_view node){
    const auto filename=source.filename().string();
    const auto key=slot.empty()?std::string("$default"):std::string(slot);
    const auto& all=Bindings();
    const auto file=all.find(filename);
    if(file==all.end())return {};
    if(file->contains("nodes")){
        const auto& nodes=file->at("nodes");
        const auto n=nodes.find(std::string(node));
        if(n!=nodes.end()){
            const auto v=n->find(key);
            if(v!=n->end())return v->get<std::string>();
        }
    }
    if(file->contains("slots")){
        const auto& slots=file->at("slots");
        const auto value=slots.find(key);
        if(value!=slots.end())return value->get<std::string>();
    }
    return {};
}
void ApplyMaterialSourceRoles(AssetMaterial& m,const std::filesystem::path& path,std::string_view slot){
    const auto& all=Bindings();
    const auto source=all.find(path.filename().string());
    if(source==all.end())return;
    const auto read=[&](const char* group,uint32_t fallback,uint32_t limit){
        const auto table=source->find(group);
        if(table==source->end())return fallback;
        const auto value=table->find(std::string(slot));
        if(value==table->end())return fallback;
        const auto n=value->get<uint32_t>();
        return n<limit?n:fallback;
    };
    m.authoringSlot=read("profile_slots",Invalid,17u);
    m.viewRole=read("view_roles",0u,7u);
}
void ApplyPhysicalMaterial(AssetMaterial& m,std::string_view id){
    const auto& registry=Registry();
    const auto it=registry.find(std::string(id));
    if(it==registry.end())throw std::runtime_error("Unknown canonical material: "+std::string(id));
    const auto& o=it->second;
    m.physicalMaterialId=o.id;
    m.opticalPolicy=o.policy;
    m.opticalLayers=o.layers;
    m.opticalTransmission=o.transmission;
    // Dedicated glass, water, screens and volumetric paths retain their
    // established authored rendering until their respective feature is integrated.
    if(o.special){m.opticalPolicy={0.f,.04f,.04f,1.f};return;}
    if(m.metalRough==Invalid&&m.roughnessOverride==Invalid)m.colorRoughness.w=o.roughness;
    else m.colorRoughness.w=std::max(o.policy.z,m.colorRoughness.w);
    m.emissionMetallic.w=o.metallic;
}
}
