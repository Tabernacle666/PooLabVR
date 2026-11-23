#include "Mg2Catalog.h"
#include "AppSession.h"
#include "SolarSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace OASIS {
namespace {
using Json=nlohmann::json;
constexpr std::array<std::string_view,Mg2Worktop::RoomProfileCount> RoomProfileNames{
    "organise","bazar","direction","creatif","technique","quasi_vide","abandonne","communs",
    "atelier_design","archives","startup","comptabilite","consultation","informatique",
    "production","accueil"};
constexpr std::array<std::string_view,10> WearNames{
    "corridor_wooden_floor","corridor_carpet","wall_behind_chair","door_and_threshold",
    "pot_ceramic","elevator_metal","moulding_rare_nail","sink_porcelain",
    "coffee_mug","office_fabric"};

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
        bool realPbr=false;
        if(asset.contains("provenance")){
            const auto provenancePath=Resolve(directory,asset.at("provenance").get<std::string>());
            if(std::filesystem::exists(provenancePath))
                realPbr=ReadJson(provenancePath).contains("pbr_lot1");
        }
        entries_.push_back({std::move(key),path,meta.at("geometry").at("triangles").get<uint64_t>(),
            ReadPoint(bounds.at("min")),ReadPoint(bounds.at("max")),{},realPbr});
    }
    using Buildings::MountSurface;
    const auto bindSurface=[&](MountSurface surface,const Json& placements,std::string_view socket){
        auto& recipe=worktops_[size_t(surface)];
        for(const auto& placement:placements){
            if(!socket.empty()&&placement.value("socket",std::string{})!=socket)continue;
            const auto entry=EntryIndex(placement.at("asset").get<std::string>());
            PrepareEntry(entry);
            const float chance=placement.value("chance",1.f);
            if(!std::isfinite(chance)||chance<0.f||chance>1.f)
                throw std::runtime_error("MG2: worktop probability invalid");
            recipe.push_back({entry,placement.contains("offset_m")?ReadPoint(placement.at("offset_m")):XMFLOAT3{},
                              0.f,chance});
        }
    };
    if(const auto path=directory/"runtime.json";std::filesystem::exists(path)){
        const auto runtime=ReadJson(path);
        if(runtime.at("schema_version")!=1)throw std::runtime_error("MG2: unsupported runtime bindings schema");
        for(const auto& binding:runtime.at("surfaces")){
            const auto target=binding.at("target").get<std::string>();
            MountSurface surface{};
            if(target=="office_desktop")surface=MountSurface::OfficeDesktop;
            else if(target=="meeting_table")surface=MountSurface::MeetingTable;
            else throw std::runtime_error("MG2: unknown placement target "+target);
            if(binding.contains("recipe")){
                const auto recipe=ReadJson(Resolve(directory,binding.at("recipe").get<std::string>()));
                bindSurface(surface,recipe.at("placements"),binding.at("socket").get<std::string>());
            }else bindSurface(surface,binding.at("placements"),{});
            // Optional shared desk/meeting props: one ingestion of the whole
            // candidate catalogue; stable host seed selects the visible subset.
            if(binding.contains("additional"))bindSurface(surface,binding.at("additional"),{});
        }
        if(runtime.contains("room_profiles")){
            const auto definitions=ReadJson(Resolve(directory,runtime.at("room_profiles").get<std::string>()));
            if(definitions.at("schema_version")!=1)throw std::runtime_error("MG2: room profiles schema");
            for(const auto& profile:definitions.at("profiles")){
                const auto id=profile.at("id").get<std::string>();
                const auto found=std::find(RoomProfileNames.begin(),RoomProfileNames.end(),std::string_view(id));
                if(found==RoomProfileNames.end())throw std::runtime_error("MG2: unsupported room profile "+id);
                const auto index=size_t(found-RoomProfileNames.begin());
                auto& destinations=roomPlacements_[index];
                for(const auto& item:profile.at("placements")){
                    const auto coordinates=item.at("at_normalized").get<std::array<float,2>>();
                    const float chance=item.value("chance",1.f);
                    const float yaw=item.value("yaw_deg",0.f);
                    if(!std::isfinite(coordinates[0])||!std::isfinite(coordinates[1])||
                       std::abs(coordinates[0])>1.f||std::abs(coordinates[1])>1.f||
                       !std::isfinite(chance)||chance<0.f||chance>1.f||!std::isfinite(yaw))
                        throw std::runtime_error("MG2: invalid room placement");
                    destinations.push_back({EntryIndex(item.at("asset").get<std::string>()),
                                            coordinates[0],coordinates[1],chance,yaw});
                }
            }
        }
        for(const auto& binding:runtime.at("forest")){
            ForestRow row{Resolve(directory,binding.at("recipe").get<std::string>()),{}};
            if(binding.contains("additional"))for(const auto& item:binding.at("additional")){
                const float jitter=item.value("jitter_m",0.f);
                if(!std::isfinite(jitter)||jitter<0.f)throw std::runtime_error("MG2: invalid forest jitter");
                row.additional.push_back({EntryIndex(item.at("asset").get<std::string>()),
                    ReadPoint(item.at("at_m")),jitter});
            }
            forestRows_.push_back(std::move(row));
        }
    }else{
        const auto office=ReadJson(directory/"recipes"/"office_single01.json");
        bindSurface(MountSurface::OfficeDesktop,office.at("placements"),"socket_desktop");
        forestRows_={{directory/"recipes"/"edge_cell01.json",{}},{directory/"recipes"/"interior_cell01.json",{}}};
    }
    const auto wearPath=directory.parent_path()/"oasis_dressing_v1"/"wear_rules.json";
    if(std::filesystem::exists(wearPath)){
        const auto rules=ReadJson(wearPath);
        if(rules.at("schema_version")!=1)throw std::runtime_error("MG2: wear rule schema");
        for(const auto& rule:rules.at("wear_classes")){
            const auto id=rule.at("name").get<std::string>();
            const auto found=std::find(WearNames.begin(),WearNames.end(),std::string_view(id));
            if(found!=WearNames.end()){
                const float likelihood=rule.at("likelihood_per_host").get<float>();
                if(!std::isfinite(likelihood)||likelihood<0.f||likelihood>1.f)
                    throw std::runtime_error("MG2: wear likelihood");
                wearLikelihood_[size_t(found-WearNames.begin())]=likelihood;
            }
        }
    }
    const auto shadePath=directory.parent_path()/"oasis_dressing_v1"/"white_shades_80.json";
    if(std::filesystem::exists(shadePath)){
        const auto shades=ReadJson(shadePath);
        if(shades.at("schema_version")!=1)throw std::runtime_error("MG2: white shade schema");
        constexpr std::array<std::string_view,8> families{
            "warm_porcelain","cold_plaster","chalk","linen_white",
            "bone_white","pearl","grey_white","blue_white"};
        for(const auto& shade:shades.at("shades")){
            const auto color=shade.at("linear_rgb").get<std::array<float,3>>();
            const auto roughness=shade.at("roughness").get<float>();
            const auto family=shade.at("family").get<std::string>();
            const auto it=std::find(families.begin(),families.end(),std::string_view(family));
            if(it==families.end()||!std::isfinite(roughness)||roughness<0.f||roughness>1.f||
               !std::all_of(color.begin(),color.end(),[](float v){return std::isfinite(v)&&v>=0.f&&v<=1.f;}))
                throw std::runtime_error("MG2: invalid white shade");
            whiteShades_.push_back({{color[0],color[1],color[2]},roughness,
                                   uint32_t(it-families.begin())});
        }
        if(whiteShades_.size()!=80u)throw std::runtime_error("MG2: expected 80 white shades");
    }
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
    MarkDiagnosticStartupProgress();
}

bool Mg2Catalog::PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,
                              unsigned treeDensity){
    forest_.clear();
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

    // Rows of cells flank the parvis and leave the middle open.
    std::array<float,2> cellSize{};
    for(size_t row=0;row<forestRows_.size();++row){
        const auto recipe=ReadJson(forestRows_[row].recipe);
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
        const auto trees=recipeTrees;
        const auto& additional=forestRows_[row].additional;
        recipeTrees.insert(recipeTrees.end(),additional.begin(),additional.end());
        const auto extraTrees=[&](float gain,bool swapAxes){
            for(auto placement:trees){
                auto& offset=placement.offset;
                const float x=offset.x,z=offset.z;
                offset.x=(swapAxes?-z:x)*gain;
                offset.z=(swapAxes?x:z)*gain;
                placement.jitter=std::min(.24f,placement.jitter);
                recipeTrees.push_back(placement);
            }
        };
        if(treeDensity>=2u)extraTrees(1.f,true);
        if(treeDensity>=4u){
            extraTrees(.49f,false);
            extraTrees(.49f,true);
        }
        const auto denseFir=std::find_if(entries_.begin(),entries_.end(),[](const auto& entry){
            return entry.key=="oasis.forest.tree_fir_hero01";
        });
        auto pilotCell=Invalid;

        const float facadeDistance=(float(row)+1.5f)*cellSize[1]*units;
        for(unsigned facade=0;facade<2;++facade)for(unsigned wing=0;wing<2;++wing){
            const float z=facade==0?front-facadeDistance:back+facadeDistance;
            for(unsigned column=0;column<columns;++column){
                const float inset=(float(column)+.5f)*cellSize[0]*units;
                if(inset*2.f>=right-left)break;
                const float x=wing==0?left+inset:right-inset;
                const size_t first=forest_.size();
                const auto cell=uint32_t((row*4u+facade*2u+wing)*columns+column);
                uint32_t seed=0x4f415349u^(0x9e3779b9u*(cell+1u));
                for(const auto& placement:recipeTrees){
                    const float dx=(placement.offset.x+(2.f*SolarRandom(seed)-1.f)*placement.jitter)*units;
                    const float dz=-(placement.offset.z+(2.f*SolarRandom(seed)-1.f)*placement.jitter)*units;
                    const float halfYaw=XM_PI*SolarRandom(seed);
                    float scale=units*std::lerp(.82f,1.08f,SolarRandom(seed));
                    const auto ground=GroundHeight(terrain.soil,x+dx,z+dz);
                    if(!ground||GroundHeight(terrain.waterway,x+dx,z+dz)||
                       GroundHeight(terrain.banks,x+dx,z+dz))continue;
                    const auto& source=entries_[placement.entry];
                    const bool pilotCandidate=row==1u&&facade==0u&&denseFir!=entries_.end()&&
                        (pilotCell==Invalid||pilotCell==cell)&&
                        (source.key=="oasis.forest.tree_fir01"||source.key=="oasis.forest.tree_fir02");
                    const size_t model=pilotCandidate?size_t(denseFir-entries_.begin()):placement.entry;
                    const auto& entry=entries_[model];
                    if(pilotCandidate)scale*=std::min(1.f,
                        (source.upper.y-source.lower.y)/std::max(.01f,entry.upper.y-entry.lower.y));
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
                    PrepareEntry(model);
                    if(pilotCandidate)pilotCell=cell;
                    forest_.push_back({model,{root,{0.f,std::sin(halfYaw),0.f,std::cos(halfYaw)}},scale,cell});
                }
                plantedCells_+=forest_.size()!=first;
            }
        }
    }
    prepareMs_+=1000.0*QpcSeconds(QpcCounter()-started);
    return !forest_.empty();
}

std::vector<WorldObject> Mg2Catalog::AppendParts(SceneData& scene,size_t index)const{
    auto source=entries_[index].scene.value();
    const auto [firstMesh,firstMaterial]=AppendRigidAsset(scene,source);
    // Only workstation screens receive the original 1990s-inspired
    // animated screensaver; conference, telephone and other displays retain
    // their authored black/off state.
    const auto& key=entries_[index].key;
    if(key=="oasis.services.server_rack_cluster01"){
        for(size_t material=0;material<source.materialNames.size();++material){
            const auto& name=source.materialNames[material];
            if(name=="rack_blue_led"||name=="rack_green_led")
                scene.materials[firstMaterial+uint32_t(material)].unlit=11u;
        }
    }
    if(key=="oasis.electronics.monitor_24in01"||
       key=="oasis.electronics.monitor_32in02"){
        for(size_t material=0;material<source.materialNames.size();++material){
            if(source.materialNames[material]!="screen_black")continue;
            auto& screen=scene.materials[firstMaterial+uint32_t(material)];
            screen.unlit=10u;
        }
    }
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

Mg2ArchitecturalPalette Mg2Catalog::AppendArchitecturalMaterials(SceneData& scene){
    using Material=Buildings::Material;
    Mg2ArchitecturalPalette palette;
    palette.material.fill(Invalid);
    // Only use maps with a recorded CC0 PBR acquisition, never the old fake maps.
    std::map<size_t,uint32_t> appended;
    auto assign=[&](std::string_view key,std::string_view surface,
                    std::initializer_list<Material> roles){
        const auto index=EntryIndex(key);
        if(!entries_[index].realPbr)return;
        PrepareEntry(index);
        const auto& source=*entries_[index].scene;
        const auto it=std::find(source.materialNames.begin(),source.materialNames.end(),surface);
        if(it==source.materialNames.end())return;
        const auto materialIndex=uint32_t(it-source.materialNames.begin());
        if(materialIndex>=source.materials.size()||source.materials[materialIndex].albedo==Invalid)return;
        auto pos=appended.find(index);
        if(pos==appended.end()){
            auto copied=source;
            const auto first=AppendAssetMaterials(scene,copied);
            pos=appended.emplace(index,first).first;
            ++palette.sources;
        }
        for(const auto role:roles){
            palette.material[size_t(role)]=pos->second+materialIndex;
            ++palette.mapped;
        }
    };
    assign("oasis.arch.slab_floor01","wood_oiled",{Material::Floor});
    assign("oasis.arch.slab_floor01","stone",{Material::Concrete});
    assign("oasis.arch.partition_solid01","ceramic",
        {Material::Wall,Material::Ceiling,Material::Sage,Material::Sand,Material::Mist,
         Material::Lilac,Material::Cherry});
    assign("oasis.furniture.desk_office01","wood_oiled",{Material::Wood,Material::Moulding});
    assign("oasis.furniture.meeting_table01","steel_brushed",{Material::Metal});
    assign("oasis.furniture.lounge_rug01","fabric_dark",
        {Material::BlueCarpet,Material::GreyCarpet,Material::VioletCarpet});
    assign("oasis.restroom.sink_wall01","ceramic",{Material::Ceramic});
    assign("oasis.services.service_pipe01","steel_brushed",{Material::WaterPipe});
    assign("oasis.electronics.power_strip01","plastic_dark",
           {Material::Cable,Material::DrainPipe});
    char line[180];
    std::snprintf(line,sizeof(line),"mg2_pbr_arch sources=%zu surfaces=%zu statut=%s\r\n",
        palette.sources,palette.mapped,palette.sources?"material_maps_embedded":"fallback_original");
    DiagnosticLine(line);
    return palette;
}

Mg2Worktop Mg2Catalog::AppendWorktop(SceneData& scene){
    DiagnosticLine("mg2_worktop_base=begin\r\n");
    Mg2Worktop result;
    std::vector<std::vector<WorldObject>> models(entries_.size());
    for(size_t surface=0;surface<worktops_.size();++surface){
        auto& props=result.surfaces[surface];
        for(const auto& placement:worktops_[surface]){
            const auto& entry=entries_[placement.entry];
            auto& model=models[placement.entry];
            if(model.empty()){
                char phase[256];
                std::snprintf(phase,sizeof(phase),"mg2_worktop_prepare asset=%s\r\n",entry.key.c_str());
                DiagnosticLine(phase);
                model=AppendParts(scene,placement.entry);
                MarkDiagnosticStartupProgress();
            }
            const auto p=placement.offset;
            const auto kind=entry.key.starts_with("oasis.electronics.keyboard_")?Mg2Worktop::PropKind::Keyboard:
                entry.key.starts_with("oasis.electronics.")?Mg2Worktop::PropKind::Equipment:Mg2Worktop::PropKind::Accessory;
            props.push_back({model,{p.x,p.y,-p.z},
                {entry.lower.x,entry.lower.y,-entry.upper.z},{entry.upper.x,entry.upper.y,-entry.lower.z},
                kind,placement.chance,placement.entry});
        }
        std::stable_sort(props.begin(),props.end(),[](const auto& a,const auto& b){return a.kind<b.kind;});
    }
    DiagnosticLine("mg2_worktop_base=ready\r\n");
    constexpr std::array<std::string_view,Mg2Worktop::HeroCount> keys{
        "oasis.services.fire_extinguisher01","oasis.lighting.light_panel01",
        "oasis.arch.door_office_solid01","oasis.furniture.desk_office01",
        "oasis.furniture.chair_ergonomic01",
        "oasis.services.server_rack_cluster01","oasis.restroom.toilet_floor01",
        "oasis.restroom.sink_wall01","oasis.restroom.mirror_wall01",
        "oasis.restroom.soap_dispenser01","oasis.services.service_pipe01",
        "oasis.services.network_bundle01","oasis.electronics.monitor_24in01",
        "oasis.electronics.keyboard_full01","oasis.electronics.mouse_office01"};
    const auto placementBindings=ReadJson(directory_/"runtime.json");
    const auto placementRecipe=ReadJson(Resolve(directory_,
        placementBindings.at("placement_recipe").get<std::string>()));
    if(placementRecipe.at("schema_version").get<unsigned>()!=2u||
       placementRecipe.at("coordinate_contract").at("engine")!="engine_uses_negated_gltf_z")
        throw std::runtime_error("MG2: incompatible hero placement coordinate contract");
    const auto& workRule=placementRecipe.at("workstation");
    result.seatDistance=workRule.at("seat_distance_m").get<float>();
    result.seatJitterX=workRule.at("seat_lateral_jitter_m").get<float>();
    result.seatJitterZ=workRule.at("seat_depth_jitter_m").get<float>();
    result.seatClearance=workRule.at("seat_clearance_m").get<float>();
    if(!(result.seatDistance>.45f&&result.seatDistance<1.5f&&
         result.seatJitterX>=0.f&&result.seatJitterX<.65f&&
         result.seatJitterZ>=0.f&&result.seatJitterZ<.40f&&
         result.seatClearance>=.35f&&result.seatClearance<.8f))
        throw std::runtime_error("MG2: invalid workstation clearance recipe");
    for(const auto& placement:worktops_[size_t(Buildings::MountSurface::OfficeDesktop)]){
        const auto& key=entries_[placement.entry].key;
        if(key=="oasis.electronics.monitor_24in01"||
           key=="oasis.electronics.monitor_32in02"){
            // See AppendWorktop prop conversion: p.z becomes -p.z.
            // A negative authored monitor offset is therefore world +Z.
            result.screenSideZ=placement.offset.z<0.f?1.f:-1.f;
            break;
        }
    }
    for(size_t i=0;i<keys.size();++i){
        const auto entry=EntryIndex(keys[i]);
        if(!entries_[entry].scene)PrepareEntry(entry);
        auto& model=models[entry];
        if(model.empty())model=AppendParts(scene,entry);
        const auto& facing=placementRecipe.at("heroes").at(std::string(keys[i]));
        const auto axis=facing.at("gltf_front_axis").get<std::string>();
        XMFLOAT2 engineFront{};
        if(axis=="+Z")engineFront={0.f,-1.f};
        else if(axis=="-Z")engineFront={0.f,1.f};
        else if(axis=="+X")engineFront={1.f,0.f};
        else if(axis=="-X")engineFront={-1.f,0.f};
        else throw std::runtime_error("MG2: invalid GLB front axis for "+std::string(keys[i]));
        result.heroes[i]={model,entries_[entry].lower,entries_[entry].upper,engineFront};
        if(i==Mg2Worktop::CeilingFixture){
            result.unlitFixture=model;
            for(auto& part:result.unlitFixture){
                auto material=scene.materials.at(part.material);
                material.emissionMetallic.x=material.emissionMetallic.y=material.emissionMetallic.z=0.f;
                material.emissive=Invalid;
                const auto name=scene.materialNames.at(part.material);
                part.material=uint32_t(scene.materials.size());
                scene.materials.push_back(material);
                scene.materialNames.push_back(name);
            }
        }
    }
    DiagnosticLine("mg2_hero=extinguisher_fixture_door_desk_chair_loaded\r\n");
    result.wearLikelihood=wearLikelihood_;
    result.whiteShades=whiteShades_;
    // Prepare only real, rigid sources. One unsupported optional prop must
    // never prevent an otherwise healthy campus from reaching the first frame.
    std::vector<bool> unavailable(entries_.size(),false);
    size_t profilesLoaded=0,profilesSkipped=0;
    for(size_t profile=0;profile<roomPlacements_.size();++profile){
        auto& props=result.roomProfiles[profile];
        for(const auto& placement:roomPlacements_[profile]){
            const auto& entry=entries_[placement.entry];
            if(unavailable[placement.entry]){++profilesSkipped;continue;}
            auto& model=models[placement.entry];
            if(model.empty()){
                try {
                    // First resolve and validate the source before mutating
                    // the shared scene through AppendRigidAsset.
                    char phase[256];
                    std::snprintf(phase,sizeof(phase),"mg2_room_prepare asset=%s\r\n",entry.key.c_str());
                    DiagnosticLine(phase);
                    MarkDiagnosticStartupProgress();
                    if(!entry.scene)PrepareEntry(placement.entry);
                    const auto& source=*entries_[placement.entry].scene;
                    if(!source.animations.empty()||!source.bones.empty()||
                       !source.meshes.morphs.empty())
                        throw std::runtime_error("non-rigid GLB in static room profile");
                    model=AppendParts(scene,placement.entry);
                }catch(const std::exception& error){
                    char message[512];
                    std::snprintf(message,sizeof(message),
                        "mg2_room_asset_failed asset=%s reason=%.240s\r\n",
                        entry.key.c_str(),error.what());
                    DiagnosticLine(message);
                    unavailable[placement.entry]=true;
                    ++profilesSkipped;
                    continue;
                }
            }
            props.push_back({model,entry.lower,entry.upper,
                             placement.nx,placement.nz,placement.chance,placement.yawDeg});
            ++profilesLoaded;
        }
    }
    char roomSummary[160];
    std::snprintf(roomSummary,sizeof(roomSummary),
        "mg2_room_profiles loaded=%zu skipped=%zu\r\n",profilesLoaded,profilesSkipped);
    DiagnosticLine(roomSummary);
    MarkDiagnosticStartupProgress();
    const auto wearFile=directory_.parent_path()/"oasis_dressing_v1"/"geometry"/"wear_examples01.glb";
    if(std::filesystem::exists(wearFile)){
        DiagnosticLine("mg2_wear_import=begin\r\n");
        try {
            auto source=LoadSceneGLTF(wearFile.wstring(),true);
            if(!source.animations.empty()||!source.bones.empty()||
               !source.meshes.morphs.empty())
                throw std::runtime_error("wear GLB requires rigid node selection");
            const auto appended=AppendRigidAsset(scene,source);
            for(size_t i=0;i<source.objects.size();++i){
                if(i>=source.meshNodeNames.size())break;
                const auto& name=source.meshNodeNames[i];
                const int type=name=="chair_back_wall_scuffs"?0:
                    name=="parquet_walkway_scratches"?1:name=="carpet_center_fade"?2:-1;
                if(type<0)continue;
                const auto& object=source.objects[i];
                WorldObject part{};
                part.mesh=appended.firstMesh+object.mesh;
                part.material=appended.firstMaterial+object.material;
                part.boundRadius=std::max(.001f,source.meshes.ranges[object.mesh].bounds.w);
                part.flags=BuildingObject|NoCollision;
                if(scene.materials[part.material].alphaMode==2u)
                    part.flags|=AlphaBlendObject|NoShadow;
                result.wearOverlays[size_t(type)].push_back(part);
            }
            DiagnosticLine("mg2_wear_import=ready\r\n");
        }catch(const std::exception& error){
            char message[384];
            std::snprintf(message,sizeof(message),"mg2_wear_import=failed reason=%.240s\r\n",
                error.what());
            DiagnosticLine(message);
        }
    }else DiagnosticLine("mg2_wear_import=absent\r\n");
    // Original 100-poster source is already in oasis_dressing_v1. Import
    // each atlas only once and register named paper+frame components.
    size_t posterMeshes=0,posterSheets=0;
    for(unsigned sheet=1;sheet<=5;++sheet){
        char filename[32];
        std::snprintf(filename,sizeof(filename),"poster_sheet_%02u.glb",sheet);
        const auto file=directory_.parent_path()/"oasis_dressing_v1"/"geometry"/filename;
        if(!std::filesystem::exists(file))continue;
        MarkDiagnosticStartupProgress();
        try{
            auto source=LoadSceneGLTF(file.wstring(),true);
            if(!source.animations.empty()||!source.bones.empty()||
               source.objects.size()!=source.meshNodeNames.size())
                throw std::runtime_error("poster atlas requires rigid named components");
            for(auto& material:source.materials)material.doubleSided=1u;
            const auto appended=AppendRigidAsset(scene,source);
            for(size_t i=0;i<source.objects.size();++i){
                const auto& name=source.meshNodeNames[i];
                if(name.size()!=10u||!name.starts_with("poster_"))continue;
                unsigned number=0;
                for(size_t j=7;j<10;++j){
                    if(name[j]<'0'||name[j]>'9'){number=0;break;}
                    number=10u*number+unsigned(name[j]-'0');
                }
                if(number<1u||number>result.posterParts.size())continue;
                const auto& object=source.objects[i];
                WorldObject part{};
                part.mesh=appended.firstMesh+object.mesh;
                part.material=appended.firstMaterial+object.material;
                part.boundRadius=std::max(.001f,source.meshes.ranges[object.mesh].bounds.w);
                part.flags=BuildingObject|NoCollision|NoShadow;
                result.posterParts[number-1u].push_back(part);
                ++posterMeshes;
            }
            ++posterSheets;
        }catch(const std::exception& ex){
            char line[384];
            std::snprintf(line,sizeof(line),"mg2_poster_atlas_failed sheet=%u reason=%.240s\r\n",
                sheet,ex.what());
            DiagnosticLine(line);
        }
    }
    char posterSummary[144];
    std::snprintf(posterSummary,sizeof(posterSummary),
        "mg2_posters sheets=%zu identifiers=%zu components=%zu\r\n",posterSheets,
        size_t(std::count_if(result.posterParts.begin(),result.posterParts.end(),
            [](const auto& parts){return parts.size()==2u;})),posterMeshes);
    DiagnosticLine(posterSummary);
    return result;
}

Mg2Worktop::Layout Mg2Worktop::Fit(Buildings::MountSurface surface,float width,float depth,uint64_t stableHost)const{
    Layout result;
    std::vector<Buildings::Rect> occupied;
    std::vector<size_t> placedEntries;
    constexpr float gap=.006f;
    constexpr float separation=gap-1.e-6f;
    const auto& candidates=surfaces[size_t(surface)];
    const size_t limit=surface==Buildings::MountSurface::OfficeDesktop?12u:11u;
    for(size_t i=0;i<candidates.size()&&occupied.size()<limit;++i){
        const auto& prop=candidates[i];
        // Keep every source asset unique per desk, even if the optional
        // authoring catalogue contains additional mugs, notebooks or mice.
        if(std::find(placedEntries.begin(),placedEntries.end(),prop.sourceEntry)!=placedEntries.end())
            continue;
        uint64_t seed=stableHost^(uint64_t(i+1u)*0x9e3779b97f4a7c15ull);
        seed^=seed>>30;seed*=0xbf58476d1ce4e5b9ull;
        seed^=seed>>27;seed*=0x94d049bb133111ebull;seed^=seed>>31;
        if(double(uint32_t(seed))/4294967296.0>=double(prop.chance))continue;
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
        placedEntries.push_back(prop.sourceEntry);
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
    size_t species=0,trees=0;
    uint64_t sourceTriangles=0,instanceTriangles=0,farTriangles=0;
    uint32_t pilotCell=Invalid;
    XMFLOAT3 pilotRoot{};
    for(const auto& plant:forest_){
        auto& model=models[plant.entry];
        const auto& entry=entries_[plant.entry];
        const auto triangles=entry.triangles;
        if(model.empty()){
            model=AppendParts(scene,plant.entry);
            for(auto& part:model){
                if((part.flags&AlphaBlendObject)==0u)part.farMesh=AppendForestLod(scene,part.mesh);
                part.flags|=OASIS_INSTANCE_BUILDING_FACADE|OASIS_INSTANCE_FOREST;
                if(entry.key=="oasis.forest.tree_fir_hero01"&&(part.flags&AlphaBlendObject)==0u)
                    scene.mg2ForestParts.push_back({part.mesh,part.material});
            }
            if(entry.key=="oasis.forest.tree_fir_hero01"){pilotCell=plant.cell;pilotRoot=plant.pose.p;}
            sourceTriangles+=triangles;
            ++species;
        }
        trees+=entry.key.starts_with("oasis.forest.tree_");
        instanceTriangles+=triangles;
        for(auto part:model){
            const auto mesh=part.farMesh==Invalid?part.mesh:part.farMesh;
            farTriangles+=scene.meshes.ranges[mesh].indexCount/3u;
            part.current=part.previous=plant.pose;
            part.scale={plant.scale,plant.scale,plant.scale};
            part.tileLayer=forestLayer;
            instances.push_back(part);
        }
    }
    if(!instances.empty())world.CreateBatch(instances);
    char line[256];
    std::snprintf(line,sizeof(line),"mg2_foret cellules=%zu arbres=%zu sous_bois=%zu modeles=%zu instances=%zu "
        "triangles_source=%llu triangles_instances=%llu triangles_loin=%llu\r\n",plantedCells_,trees,
        forest_.size()-trees,species,instances.size(),static_cast<unsigned long long>(sourceTriangles),
        static_cast<unsigned long long>(instanceTriangles),static_cast<unsigned long long>(farTriangles));
    DiagnosticLine(line);
    if(pilotCell!=Invalid){
        std::snprintf(line,sizeof(line),"mg2_foret_pilote cellule=%u x=%.2f y=%.2f z=%.2f parties=%zu\r\n",
            pilotCell,pilotRoot.x,pilotRoot.y,pilotRoot.z,scene.mg2ForestParts.size());
        DiagnosticLine(line);
    }
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
    using Buildings::MountSurface;
    std::snprintf(line,sizeof(line),"mg2_liaisons bureau=%zu reunion=%zu bandes_foret=%zu\r\n",
        worktops_[size_t(MountSurface::OfficeDesktop)].size(),worktops_[size_t(MountSurface::MeetingTable)].size(),
        forestRows_.size());
    DiagnosticLine(line);
}
}
