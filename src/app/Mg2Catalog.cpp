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
    const auto started=QpcCounter();
    const auto pack=ReadJson(directory/"pack.json");
    const auto axes=pack.at("axes").get<std::string>();
    if(pack.at("schema_version")!=1||pack.at("units")!="m"||
       (axes!="right_handed_y_up"&&axes!="glTF_right_handed_y_up"))
        throw std::runtime_error("MG2: unsupported catalogue schema or coordinates");
    packRevision_=pack.at("pack_revision").get<uint64_t>();
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
        const auto& bounds=meta.at("bounds_m");
        entries_.push_back({std::move(key),path,meta.at("geometry").at("triangles").get<uint64_t>(),
            ReadPoint(bounds.at("min")),ReadPoint(bounds.at("max")),{}});
    }
    const auto office=ReadJson(directory/"recipes"/"office_single01.json");
    for(const auto& placement:office.at("placements")){
        if(placement.value("socket",std::string{})!="socket_desktop")continue;
        const auto key=placement.at("asset").get<std::string>();
        const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& candidate){return candidate.key==key;});
        if(entry==entries_.end())throw std::runtime_error("MG2: unknown office asset");
        if(!entry->scene){
            auto loaded=LoadSceneGLTF(entry->path.wstring());
            if(loaded.objects.empty()||loaded.meshes.indices.size()/3u!=entry->triangles)
                throw std::runtime_error("MG2: triangle count mismatch");
            entry->scene.emplace(std::move(loaded));
        }
        worktop_.push_back({size_t(entry-entries_.begin()),ReadPoint(placement.at("offset_m"))});
    }
    if(worktop_.empty())throw std::runtime_error("MG2: no office desk placements");
    prepareMs_=1000.0*QpcSeconds(QpcCounter()-started);
}

Mg2Worktop Mg2Catalog::AppendWorktop(SceneData& scene)const{
    Mg2Worktop result;
    for(const auto& placement:worktop_){
        const auto& entry=entries_[placement.entry];
        auto source=entry.scene.value();
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
    uint64_t triangles=0,preparedTriangles=0;
    size_t prepared=0,meshes=0,materials=0,textures=0;
    for(const auto& entry:entries_){
        triangles+=entry.triangles;
        if(!entry.scene)continue;
        ++prepared;
        preparedTriangles+=entry.triangles;
        meshes+=entry.scene->meshes.ranges.size();
        materials+=entry.scene->materials.size();
        textures+=entry.scene->textures.size();
    }
    char line[320];
    std::snprintf(line,sizeof(line),"mg2_catalog pack_revision=%llu assets=%zu triangles=%llu assets_prepares=%zu "
        "triangles_prepares=%llu meshes_prepares=%zu materials_prepares=%zu textures_preparees=%zu preparation_ms=%.2f\r\n",
        static_cast<unsigned long long>(packRevision_),entries_.size(),static_cast<unsigned long long>(triangles),
        prepared,static_cast<unsigned long long>(preparedTriangles),meshes,materials,textures,prepareMs_);
    DiagnosticLine(line);
}
}
