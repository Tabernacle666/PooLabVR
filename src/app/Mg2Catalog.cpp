#include "Mg2Catalog.h"
#include "AppSession.h"
#include "SolarSystem.h"
#include "building/BuildingPlan.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
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

std::optional<float> GroundHeight(std::span<const TerrainTriangle> triangles,float x,float z){
    std::optional<float> highest;
    for(const auto& [a,b,c]:triangles){
        const auto ab=Sub(b,a),ac=Sub(c,a);
        const float dx=x-a.x,dz=z-a.z;
        const float area=ab.x*ac.z-ab.z*ac.x;
        if(std::abs(area)<=1.e-8f)continue;
        const float u=(dx*ac.z-dz*ac.x)/area,v=(ab.x*dz-ab.z*dx)/area;
        if(u<-1.e-5f||v<-1.e-5f||u+v>1.f+1.e-5f)continue;
        const float y=a.y+u*ab.y+v*ac.y;
        if(!highest||y>*highest)highest=y;
    }
    return highest;
}
}

Mg2Catalog::Mg2Catalog(const std::filesystem::path& directory):directory_(directory){
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
        const auto entry=EntryIndex(placement.at("asset").get<std::string>());
        PrepareEntry(entry);
        worktop_.push_back({entry,ReadPoint(placement.at("offset_m"))});
    }
    if(worktop_.empty())throw std::runtime_error("MG2: no office desk placements");
    prepareMs_=1000.0*QpcSeconds(QpcCounter()-started);
}

size_t Mg2Catalog::EntryIndex(std::string_view key)const{
    const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& candidate){return candidate.key==key;});
    if(entry==entries_.end())throw std::runtime_error("MG2: unknown recipe asset "+std::string(key));
    return size_t(entry-entries_.begin());
}

void Mg2Catalog::PrepareEntry(size_t index){
    auto& entry=entries_[index];
    if(entry.scene)return;
    auto loaded=LoadSceneGLTF(entry.path.wstring());
    if(loaded.objects.empty()||loaded.meshes.indices.size()/3u!=entry.triangles)
        throw std::runtime_error("MG2: triangle count mismatch for "+entry.key);
    entry.scene.emplace(std::move(loaded));
}

bool Mg2Catalog::PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites){
    trees_.clear();
    plantedCells_=0;
    if(sites.empty()||terrain.soil.empty())return false;
    const auto started=QpcCounter();
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    constexpr unsigned columns=5;
    float left=sites.front().origin.x,right=left,front=sites.front().origin.z,back=front;
    for(const auto& site:sites){
        left=std::min(left,site.origin.x);
        right=std::max(right,site.origin.x+site.width);
        front=std::min(front,site.origin.z);
        back=std::max(back,site.origin.z+site.depth);
    }

    // Two rows of cells in each grove flank the parvis and leave the middle open.
    constexpr std::array recipes{"edge_cell01.json","interior_cell01.json"};
    std::array<float,2> cellSize{};
    for(size_t row=0;row<recipes.size();++row){
        const auto recipe=ReadJson(directory_/"recipes"/recipes[row]);
        const auto size=recipe.at("cell_size_m").get<std::array<float,2>>();
        if(!std::isfinite(size[0])||!std::isfinite(size[1])||size[0]<=0.f||size[1]<=0.f||
           (row!=0&&size!=cellSize))throw std::runtime_error("MG2: incompatible forest cells");
        cellSize=size;
        std::vector<Placement> recipeTrees;
        for(const auto& placement:recipe.at("placements")){
            const auto key=placement.at("asset").get<std::string>();
            if(!key.starts_with("oasis.forest.tree_")||!placement.contains("at_m")||
               placement.value("optional",false))continue;
            const float jitter=placement.value("jitter_m",0.f);
            if(!std::isfinite(jitter)||jitter<0.f)
                throw std::runtime_error("MG2: invalid forest jitter");
            recipeTrees.push_back({EntryIndex(key),ReadPoint(placement.at("at_m")),jitter});
        }
        if(recipeTrees.empty())throw std::runtime_error("MG2: no fixed trees in forest cell");

        const float facadeDistance=(float(row)+1.5f)*cellSize[1]*units;
        for(unsigned facade=0;facade<2;++facade)for(unsigned wing=0;wing<2;++wing){
            const float z=facade==0?front-facadeDistance:back+facadeDistance;
            for(unsigned column=0;column<columns;++column){
                const float inset=(float(column)+.5f)*cellSize[0]*units;
                if(inset*2.f>=right-left)break;
                const float x=wing==0?left+inset:right-inset;
                const size_t first=trees_.size();
                const auto cell=uint32_t((row*4u+facade*2u+wing)*columns+column);
                uint32_t seed=0x4f415349u^(0x9e3779b9u*(cell+1u));
                for(const auto& placement:recipeTrees){
                    const float dx=(placement.offset.x+(2.f*SolarRandom(seed)-1.f)*placement.jitter)*units;
                    const float dz=-(placement.offset.z+(2.f*SolarRandom(seed)-1.f)*placement.jitter)*units;
                    const float halfYaw=XM_PI*SolarRandom(seed);
                    const float scale=units*std::lerp(.82f,1.08f,SolarRandom(seed));
                    const auto ground=GroundHeight(terrain.soil,x+dx,z+dz);
                    if(!ground||GroundHeight(terrain.waterway,x+dx,z+dz))continue;
                    const auto& entry=entries_[placement.entry];
                    const float crown=scale*std::hypot(std::max(std::abs(entry.lower.x),std::abs(entry.upper.x)),
                                                       std::max(std::abs(entry.lower.z),std::abs(entry.upper.z)));
                    const XMFLOAT3 root{x+dx,*ground+placement.offset.y*units,z+dz};
                    constexpr float margin=1.5f*units;
                    const bool atFoundation=std::any_of(sites.begin(),sites.end(),[&](const BuildingFootprint& site){
                        const float nearestX=std::clamp(root.x,site.origin.x,site.origin.x+site.width);
                        const float nearestZ=std::clamp(root.z,site.origin.z,site.origin.z+site.depth);
                        return std::hypot(root.x-nearestX,root.z-nearestZ)<crown+margin;
                    });
                    if(atFoundation)continue;
                    const auto local=Sub(root,SpectacleCenter);
                    const float horizontal=std::hypot(local.x,local.z)+crown;
                    const float vertical=std::max(std::abs(local.y+entry.lower.y*scale),
                                                  std::abs(local.y+entry.upper.y*scale));
                    if(std::hypot(horizontal,vertical)>SpectacleRadius-margin)continue;
                    PrepareEntry(placement.entry);
                    trees_.push_back({placement.entry,{root,{0.f,std::sin(halfYaw),0.f,std::cos(halfYaw)}},scale});
                }
                plantedCells_+=trees_.size()!=first;
            }
        }
    }
    prepareMs_+=1000.0*QpcSeconds(QpcCounter()-started);
    return !trees_.empty();
}

std::vector<WorldObject> Mg2Catalog::AppendParts(SceneData& scene,size_t index)const{
    auto source=entries_[index].scene.value();
    const auto [firstMesh,firstMaterial]=AppendRigidAsset(scene,source);
    std::vector<WorldObject> parts;
    parts.reserve(source.objects.size());
    for(const auto& item:source.objects){
        WorldObject part;
        part.mesh=firstMesh+item.mesh;
        part.material=firstMaterial+item.material;
        part.boundRadius=std::max(.001f,source.meshes.ranges[item.mesh].bounds.w);
        part.flags=BuildingObject|NoCollision;
        if(scene.materials[part.material].alphaMode==2u)part.flags|=AlphaBlendObject;
        parts.push_back(part);
    }
    return parts;
}

Mg2Worktop Mg2Catalog::AppendWorktop(SceneData& scene)const{
    Mg2Worktop result;
    for(const auto& placement:worktop_){
        const auto& entry=entries_[placement.entry];
        const auto p=placement.offset;
        const auto kind=entry.key.starts_with("oasis.electronics.keyboard_")?Mg2Worktop::PropKind::Keyboard:
            entry.key.starts_with("oasis.electronics.")?Mg2Worktop::PropKind::Equipment:Mg2Worktop::PropKind::Accessory;
        result.props.push_back({AppendParts(scene,placement.entry),{p.x,p.y,-p.z},
            {entry.lower.x,entry.lower.y,-entry.upper.z},{entry.upper.x,entry.upper.y,-entry.lower.z},kind});
    }
    std::stable_sort(result.props.begin(),result.props.end(),[](const auto& a,const auto& b){return a.kind<b.kind;});
    return result;
}

Mg2Worktop::Layout Mg2Worktop::Fit(float width,float depth)const{
    Layout result;
    std::vector<Buildings::Rect> occupied;
    constexpr float gap=.006f;
    constexpr float separation=gap-1.e-6f;
    for(const auto& prop:props){
        const float left=-width*.5f+gap-prop.lower.x,right=width*.5f-gap-prop.upper.x;
        const float front=-depth*.5f+gap-prop.lower.z,back=depth*.5f-gap-prop.upper.z;
        if(left>right||front>back)continue;
        std::vector<float> xs{std::clamp(prop.offset.x,left,right),left,right};
        std::vector<float> zs{std::clamp(prop.offset.z,front,back),front,back};
        // A free position nearest the authored offset lies on that offset
        // or on a boundary of the plate or an already placed footprint.
        for(const auto& used:occupied){
            xs.push_back(std::clamp(used.left-prop.upper.x-gap,left,right));
            xs.push_back(std::clamp(used.right-prop.lower.x+gap,left,right));
            zs.push_back(std::clamp(used.front-prop.upper.z-gap,front,back));
            zs.push_back(std::clamp(used.back-prop.lower.z+gap,front,back));
        }
        float best=std::numeric_limits<float>::infinity();
        XMFLOAT3 offset{};
        Buildings::Rect footprint{};
        for(const float x:xs)for(const float z:zs){
            const float dx=x-prop.offset.x,dz=z-prop.offset.z,cost=dx*dx+dz*dz;
            if(cost>=best)continue;
            const Buildings::Rect bounds{x+prop.lower.x,z+prop.lower.z,x+prop.upper.x,z+prop.upper.z};
            const bool overlaps=std::any_of(occupied.begin(),occupied.end(),[&](const auto& used){
                return bounds.left<used.right+separation&&bounds.right>used.left-separation&&
                       bounds.front<used.back+separation&&bounds.back>used.front-separation;
            });
            if(overlaps)continue;
            best=cost;
            offset={x,prop.offset.y,z};
            footprint=bounds;
        }
        if(!std::isfinite(best))continue;
        occupied.push_back(footprint);
        result.keyboards+=(prop.kind==PropKind::Keyboard);
        for(auto part:prop.parts){
            part.current.p=offset;
            part.previous=part.current;
            result.parts.push_back(part);
        }
    }
    return result;
}

void Mg2Catalog::InstallForest(SceneData& scene,WorldRegistry& world)const{
    constexpr int32_t forestLayer=64; // Use the campus visibility and ray residency paths.
    std::vector<std::vector<WorldObject>> models(entries_.size());
    std::vector<WorldObject> instances;
    size_t species=0;
    uint64_t sourceTriangles=0,instanceTriangles=0;
    for(const auto& tree:trees_){
        auto& model=models[tree.entry];
        const auto triangles=entries_[tree.entry].triangles;
        if(model.empty()){
            model=AppendParts(scene,tree.entry);
            sourceTriangles+=triangles;
            ++species;
        }
        instanceTriangles+=triangles;
        for(auto part:model){
            part.current=part.previous=tree.pose;
            part.scale={tree.scale,tree.scale,tree.scale};
            part.tileLayer=forestLayer;
            instances.push_back(part);
        }
    }
    if(!instances.empty())world.CreateBatch(instances);
    char line[256];
    std::snprintf(line,sizeof(line),"mg2_foret cellules=%zu arbres=%zu modeles=%zu instances=%zu "
        "triangles_source=%llu triangles_instances=%llu\r\n",plantedCells_,trees_.size(),species,
        instances.size(),static_cast<unsigned long long>(sourceTriangles),static_cast<unsigned long long>(instanceTriangles));
    DiagnosticLine(line);
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
