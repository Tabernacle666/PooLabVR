#include "Mg2Catalog.h"
#include "AppSession.h"
#include <algorithm>
#include <cmath>
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

XMFLOAT3 ReadPoint(const Json& values){
    const auto point=values.get<std::array<float,3>>();
    if(!std::isfinite(point[0])||!std::isfinite(point[1])||!std::isfinite(point[2]))
        throw std::runtime_error("MG2: invalid position");
    return {point[0],point[1],point[2]};
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
    const auto axes=pack.at("axes").get<std::string>();
    if(pack.at("schema_version")!=1||pack.at("units")!="m"||
       (axes!="right_handed_y_up"&&axes!="glTF_right_handed_y_up"))
        throw std::runtime_error("MG2: unsupported catalogue schema or coordinates");
    const auto& assets=pack.at("assets");
    if(!assets.is_array()||assets.empty())throw std::runtime_error("MG2: empty catalogue");
    for(const auto& asset:assets){
        auto key=asset.at("asset_key").get<std::string>();
        if(key.empty()||std::any_of(entries_.begin(),entries_.end(),[&](const Entry& entry){return entry.key==key;}))
            throw std::runtime_error("MG2: empty or duplicate asset key");
        const auto glb=asset.at("glb").get<std::string>();
        const auto metaPath=asset.at(asset.contains("metadata")?"metadata":"sidecar").get<std::string>();
        const auto& revision=asset.at(asset.contains("asset_revision")?"asset_revision":"revision");
        const auto meta=ReadJson(Resolve(directory,metaPath));
        if(meta.at("schema_version")!=1||meta.at("asset_key")!=key||
           meta.at("glb")!=glb||meta.at("asset_revision")!=revision)
            throw std::runtime_error("MG2: asset metadata mismatch");
        const auto path=Resolve(directory,glb);
        if(path.extension()!=".glb"||std::filesystem::file_size(path)!=meta.at("geometry").at("bytes").get<uint64_t>())
            throw std::runtime_error("MG2: invalid GLB or byte count");
        auto loaded=LoadSceneGLTF(path.wstring(),true);
        if(loaded.objects.empty()||loaded.meshes.indices.size()/3u!=meta.at("geometry").at("triangles").get<uint64_t>())
            throw std::runtime_error("MG2: triangle count mismatch");
        const auto& bounds=meta.at("bounds_m");
        entries_.push_back({std::move(key),std::move(loaded),ReadPoint(bounds.at("min")),ReadPoint(bounds.at("max"))});
    }
    const auto office=ReadJson(directory/"recipes"/"office_single01.json");
    for(const auto& placement:office.at("placements")){
        if(placement.value("socket",std::string{})!="socket_desktop")continue;
        const auto key=placement.at("asset").get<std::string>();
        const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& candidate){return candidate.key==key;});
        if(entry==entries_.end())throw std::runtime_error("MG2: unknown office asset");
        worktop_.push_back({size_t(entry-entries_.begin()),ReadPoint(placement.at("offset_m"))});
    }
    if(worktop_.empty())throw std::runtime_error("MG2: no office desk placements");
}

Mg2Worktop Mg2Catalog::AppendWorktop(SceneData& scene)const{
    Mg2Worktop result;
    for(const auto& placement:worktop_){
        const auto& entry=entries_[placement.entry];
        auto source=entry.scene;
        const auto [firstMesh,firstMaterial]=AppendRigidAsset(scene,source);
        const auto p=placement.offset;
        result.halfWidth=std::max({result.halfWidth,std::abs(p.x+entry.lower.x),std::abs(p.x+entry.upper.x)});
        result.halfDepth=std::max({result.halfDepth,std::abs(p.z+entry.lower.z),std::abs(p.z+entry.upper.z)});
        for(const auto& item:source.objects){
            WorldObject part;
            part.current.p={p.x,p.y,-p.z};
            part.previous=part.current;
            part.mesh=firstMesh+item.mesh;
            part.material=firstMaterial+item.material;
            part.boundRadius=std::max(.001f,source.meshes.ranges[item.mesh].bounds.w);
            part.flags=BuildingObject|NoCollision;
            if(scene.materials[part.material].alphaMode==2u)part.flags|=AlphaBlendObject;
            result.parts.push_back(part);
        }
    }
    return result;
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
