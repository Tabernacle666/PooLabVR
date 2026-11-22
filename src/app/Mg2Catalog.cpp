#include "Mg2Catalog.h"
#include "AppSession.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace OASIS {
namespace {
using Json=nlohmann::json;

Json ReadJson(const std::filesystem::path& path){
    std::ifstream stream(path);
    if(!stream)throw std::runtime_error("MG2: missing catalogue or metadata");
    return Json::parse(stream);
}

std::filesystem::path Resolve(const std::filesystem::path& root,const std::string& name){
    const std::filesystem::path relative(name);
    if(relative.empty()||relative.has_root_path())
        throw std::runtime_error("MG2: invalid asset path");
    for(const auto& part:relative)
        if(part==".."||part==".")
            throw std::runtime_error("MG2: invalid asset path component");
    return root/relative;
}
}

Mg2Catalog::Mg2Catalog(const std::filesystem::path& directory){
    const auto pack=ReadJson(directory/"pack.json");
    if(pack.at("schema_version")!=1||pack.at("units")!="m"||pack.at("axes")!="right_handed_y_up")
        throw std::runtime_error("MG2: unsupported catalogue schema or coordinates");
    const auto& assets=pack.at("assets");
    if(!assets.is_array()||assets.empty())throw std::runtime_error("MG2: empty catalogue");
    for(const auto& asset:assets){
        auto key=asset.at("asset_key").get<std::string>();
        if(key.empty()||std::any_of(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.key==key;}))
            throw std::runtime_error("MG2: empty or duplicate asset key");
        const auto glb=asset.at("glb").get<std::string>();
        const auto meta=ReadJson(Resolve(directory,asset.at("metadata").get<std::string>()));
        if(meta.at("schema_version")!=1||meta.at("asset_key")!=key||
           meta.at("glb")!=glb||meta.at("asset_revision")!=asset.at("asset_revision"))
            throw std::runtime_error("MG2: asset metadata mismatch");
        const auto path=Resolve(directory,glb);
        if(path.extension()!=".glb"||std::filesystem::file_size(path)!=meta.at("geometry").at("bytes").get<uint64_t>())
            throw std::runtime_error("MG2: invalid GLB or byte count");
        auto loaded=LoadSceneGLTF(path.wstring(),true);
        if(loaded.objects.empty()||loaded.meshes.indices.size()/3u!=meta.at("geometry").at("triangles").get<uint64_t>())
            throw std::runtime_error("MG2: triangle count mismatch");
        entries_.push_back({std::move(key),std::move(loaded)});
    }
}

void Mg2Catalog::WriteSummary()const{
    size_t triangles=0,meshes=0,materials=0,textures=0;
    for(const auto& entry:entries_){
        triangles+=entry.scene.meshes.indices.size()/3u;
        meshes+=entry.scene.meshes.ranges.size();
        materials+=entry.scene.materials.size();
        textures+=entry.scene.textures.size();
    }
    char line[256];
    std::snprintf(line,sizeof(line),"mg2_catalog assets=%zu triangles=%zu meshes=%zu materials=%zu textures=%zu\r\n",
        entries_.size(),triangles,meshes,materials,textures);
    DiagnosticLine(line);
}
}
