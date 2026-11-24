#include "Mg2Catalog.h"
#include "OrganicForest.h"
#include "AppSession.h"
#include "SolarSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace OASIS {
namespace {
using Json=nlohmann::json;
constexpr std::array<std::string_view,Mg2Worktop::RoomProfileCount> RoomProfileNames{
    "organise","bazar","direction","creatif","technique","quasi_vide","abandonne","communs",
    "atelier_design","archives","startup","comptabilite","consultation","informatique",
    "production","accueil","grand_hall","depanneur","conciergerie","cuisine",
    "gym","spa","sauna","suite","salon_penthouse","maintenance_hvac","cafe"};
constexpr std::array<std::string_view,Mg2Worktop::HeroCount> MandatoryHeroKeys{
    "oasis.services.fire_extinguisher01","oasis.lighting.light_panel01",
    "oasis.arch.door_office_solid01","oasis.furniture.desk_office01",
    "oasis.furniture.chair_ergonomic01",
    "oasis.services.server_rack_cluster01","oasis.restroom.toilet_floor01",
    "oasis.restroom.sink_wall01","oasis.restroom.mirror_wall01",
    "oasis.restroom.soap_dispenser01","oasis.services.service_pipe01",
    "oasis.services.network_bundle01","oasis.electronics.monitor_24in01",
    "oasis.electronics.keyboard_full01","oasis.electronics.mouse_office01"};

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

Mg2Catalog::Mg2Catalog(const std::filesystem::path& directory,bool deferGeometry):directory_(directory){
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
        const auto path=Resolve(directory,glb);
        try{
        const auto meta=ReadJson(Resolve(directory,metaPath));
        if(meta.at("schema_version")!=1||meta.at("asset_key")!=key||
           meta.at("glb")!=glb||meta.at("asset_revision")!=revision)
            throw std::runtime_error("MG2: asset metadata mismatch");
        // Actual availability is checked per asset during preparation, not while reading the catalogue.
        const auto& bounds=meta.at("bounds_m");
        bool realPbr=false;
        if(asset.contains("provenance")){
            const auto provenancePath=Resolve(directory,asset.at("provenance").get<std::string>());
            if(std::filesystem::exists(provenancePath))
                realPbr=ReadJson(provenancePath).contains("pbr_lot1");
        }
        entries_.push_back({key,path,meta.at("geometry").at("triangles").get<uint64_t>(),
            ReadPoint(bounds.at("min")),ReadPoint(bounds.at("max")),{},realPbr});
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){
            entries_.push_back({key,path,0,{},{},{},false,true});
            const auto line="asset_unavailable asset="+key+" reason="+error.what()+"\r\n";DiagnosticLine(line.c_str());
        }
    }
    using Buildings::MountSurface;
    const auto bindSurface=[&](MountSurface surface,const Json& placements,std::string_view socket){
        auto& recipe=worktops_[size_t(surface)];
        for(const auto& placement:placements){
            if(!socket.empty()&&placement.value("socket",std::string{})!=socket)continue;
            const auto entry=EntryIndex(placement.at("asset").get<std::string>());
            if(entry>=entries_.size()){degraded_=true;continue;}
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
                    const std::string mount=item.value("mount",std::string("floor"));
                    const auto kind=mount=="floor"?Mg2Worktop::RoomMount::Floor:
                        mount=="wall"?Mg2Worktop::RoomMount::Wall:
                        mount=="countertop"?Mg2Worktop::RoomMount::Countertop:
                        mount=="service"?Mg2Worktop::RoomMount::Service:
                        throw std::runtime_error("MG2: unknown room mounting constraint "+mount);
                    const float height=item.value("at_height_m",1.65f);
                    if(!std::isfinite(height)||height<0.f||height>3.8f)
                        throw std::runtime_error("MG2: invalid wall mount height");
                    const auto entry=EntryIndex(item.at("asset").get<std::string>());
                    if(entry>=entries_.size()){degraded_=true;continue;}
                    destinations.push_back({entry,
                                            coordinates[0],coordinates[1],chance,yaw,height,kind});
                }
            }
        }
        for(const auto& binding:runtime.at("forest")){
            ForestRow row{Resolve(directory,binding.at("recipe").get<std::string>()),{}};
            if(binding.contains("additional"))for(const auto& item:binding.at("additional")){
                const float jitter=item.value("jitter_m",0.f);
                if(!std::isfinite(jitter)||jitter<0.f)throw std::runtime_error("MG2: invalid forest jitter");
                const auto entry=EntryIndex(item.at("asset").get<std::string>());
                if(entry>=entries_.size()){degraded_=true;continue;}
                row.additional.push_back({entry,
                    ReadPoint(item.at("at_m")),jitter});
            }
            forestRows_.push_back(std::move(row));
        }
    }else{
        const auto office=ReadJson(directory/"recipes"/"office_single01.json");
        bindSurface(MountSurface::OfficeDesktop,office.at("placements"),"socket_desktop");
        forestRows_={{directory/"recipes"/"edge_cell01.json",{}},{directory/"recipes"/"interior_cell01.json",{}}};
    }
    prepareMs_=1000.0*QpcSeconds(QpcCounter()-started);
    if(!deferGeometry)PrepareGeometry();
}

void Mg2Catalog::PrepareGeometry(){
    if(geometryPrepared_)return;
    const auto started=QpcCounter();
    // Fail fast on incompatible required geometry before decoding the full
    // optional catalogue. Decoded GLBs are kept and reused for real placement.
    DiagnosticLine("mg2_hero_preflight=begin\r\n");
    for(const auto key:MandatoryHeroKeys){
        char progress[320];
        std::snprintf(progress,sizeof(progress),"mg2_hero_preflight_asset=%.*s\r\n",
            int(key.size()),key.data());
        DiagnosticLine(progress);
        MarkDiagnosticStartupProgress();
        try{
            TryPrepareEntry(EntryIndex(key));
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){
            char failure[640];
            std::snprintf(failure,sizeof(failure),
                "mg2_hero_preflight_failed asset=%.*s reason=%.360s\r\n",
                int(key.size()),key.data(),error.what());
            DiagnosticLine(failure);
        }
    }
    DiagnosticLine("mg2_hero_preflight=ready\r\n");
    for(const auto& surface:worktops_)for(const auto& placement:surface)TryPrepareEntry(placement.entry);
    geometryPrepared_=true;
    prepareMs_+=1000.0*QpcSeconds(QpcCounter()-started);
}
void Mg2Catalog::ReleasePreparedGeometry() noexcept {
    for(auto& entry:entries_)entry.scene.reset();
    geometryPrepared_=false;
}

size_t Mg2Catalog::EntryIndex(std::string_view key)const{
    const auto entry=std::find_if(entries_.begin(),entries_.end(),[&](const Entry& candidate){return candidate.key==key;});
    if(entry==entries_.end()){
        const auto line="asset_unavailable asset="+std::string(key)+" reason=missing_catalog_entry\r\n";DiagnosticLine(line.c_str());
        return entries_.size();
    }
    return size_t(entry-entries_.begin());
}

bool Mg2Catalog::TryPrepareEntry(size_t index){
    if(index>=entries_.size()){degraded_=true;return false;}
    auto& entry=entries_[index];
    if(entry.unavailable){degraded_=true;return false;}
    try{PrepareEntry(index);return true;}
    catch(const std::bad_alloc&){throw;}
    catch(const std::exception& error){
        entry.unavailable=true;degraded_=true;
        const auto message="asset_unavailable source="+entry.path.string()+" asset="+entry.key+" reason="+error.what()+"\r\n";
        DiagnosticLine(message.c_str());return false;
    }
}

void Mg2Catalog::PrepareEntry(size_t index){
    auto& entry=entries_[index];
    if(entry.scene)return;
    if(entry.path.extension()!=".glb"||!std::filesystem::is_regular_file(entry.path))throw AssetImportError("missing GLB");
    const bool keepNames=entry.key=="oasis.arch.door_office_solid01"||
                         entry.key=="oasis.furniture.desk_office01";
    auto loaded=LoadSceneGLTF(entry.path.wstring(),keepNames);
    if(!loaded.animations.empty()||!loaded.bones.empty()||!loaded.meshes.morphs.empty()||
       loaded.objects.empty()||loaded.meshes.indices.size()/3u!=entry.triangles||
       loaded.meshes.vertices.empty())
        throw std::runtime_error("MG2: invalid rigid geometry for "+entry.key);
    if(entry.key=="oasis.arch.door_office_solid01"){
        // The GLB generator groups primitives by (node name, material). Thus
        // the two physical rosettes are stored in ONE "lever_rosette" mesh.
        // Select the two hardware sides by vertex Z, not by node count.
        if(loaded.meshNodeNames.size()!=loaded.objects.size())
            throw std::runtime_error("MG2: portal assembly requires named meshes");
        const auto bounds=[&](size_t j){
            const auto mesh=loaded.objects[j].mesh;
            if(mesh>=loaded.meshes.ranges.size())
                throw std::runtime_error("MG2: invalid portal mesh range");
            const auto& range=loaded.meshes.ranges[mesh];
            if(!range.vertexCount||size_t(range.firstVertex)+range.vertexCount>
                   loaded.meshes.vertices.size())
                throw std::runtime_error("MG2: empty portal mesh range");
            XMFLOAT3 lower{std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::infinity()};
            XMFLOAT3 upper{-lower.x,-lower.y,-lower.z};
            for(uint32_t k=range.firstVertex;k<range.firstVertex+range.vertexCount;++k){
                const auto v=loaded.meshes.vertices[k].position;
                lower.x=std::min(lower.x,v.x);lower.y=std::min(lower.y,v.y);
                lower.z=std::min(lower.z,v.z);
                upper.x=std::max(upper.x,v.x);upper.y=std::max(upper.y,v.y);
                upper.z=std::max(upper.z,v.z);
            }
            return std::pair{lower,upper};
        };
        float leafNear=std::numeric_limits<float>::infinity();
        float leafFar=-leafNear;
        float negativeMin=leafNear,negativeMax=-leafNear;
        float positiveMin=leafNear,positiveMax=-leafNear;
        unsigned rosetteNodes=0;
        for(size_t i=0;i<loaded.objects.size();++i){
            const auto& name=loaded.meshNodeNames[i];
            if(name=="leaf"||name=="solid_wood_stile"||name=="leaf_trim"){
                const auto [lo,hi]=bounds(i);
                leafNear=std::min(leafNear,lo.z);
                leafFar=std::max(leafFar,hi.z);
            }
            if(name!="lever_rosette")continue;
            ++rosetteNodes;
            const auto& range=loaded.meshes.ranges[loaded.objects[i].mesh];
            for(uint32_t k=range.firstVertex;k<range.firstVertex+range.vertexCount;++k){
                const float z=loaded.meshes.vertices[k].position.z;
                if(z<0.f){
                    negativeMin=std::min(negativeMin,z);
                    negativeMax=std::max(negativeMax,z);
                }else{
                    positiveMin=std::min(positiveMin,z);
                    positiveMax=std::max(positiveMax,z);
                }
            }
        }
        if(!std::isfinite(leafNear)||!std::isfinite(leafFar)||
           !rosetteNodes||!std::isfinite(negativeMin)||!std::isfinite(negativeMax)||
           !std::isfinite(positiveMin)||!std::isfinite(positiveMax))
            throw std::runtime_error("MG2: portal leaf or bilateral rosette geometry missing");
        const float negativeShift=(leafNear-.0055f)-.5f*(negativeMin+negativeMax);
        const float positiveShift=(leafFar-.0015f)-.5f*(positiveMin+positiveMax);
        if(std::abs(negativeShift)>.12f||std::abs(positiveShift)>.12f)
            throw std::runtime_error("MG2: portal hardware outside 12cm contact tolerance");
        std::vector<uint32_t> adjusted;
        for(size_t i=0;i<loaded.objects.size();++i){
            const auto& name=loaded.meshNodeNames[i];
            if(name!="lever_rosette"&&name!="lever_spindle"&&
               name!="precision_lever"&&name!="lever_end"&&name!="lock_bezel")
                continue;
            const uint32_t mesh=loaded.objects[i].mesh;
            if(std::find(adjusted.begin(),adjusted.end(),mesh)!=adjusted.end())continue;
            adjusted.push_back(mesh);
            auto& range=loaded.meshes.ranges[mesh];
            XMFLOAT3 lo{std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::infinity()};
            XMFLOAT3 hi{-lo.x,-lo.y,-lo.z};
            for(uint32_t k=range.firstVertex;k<range.firstVertex+range.vertexCount;++k){
                auto& p=loaded.meshes.vertices[k].position;
                p.z+=p.z<0.f?negativeShift:positiveShift;
                lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
                hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
            }
            const auto center=Scale(Add(lo,hi),.5f);
            float radius=0.f;
            for(uint32_t k=range.firstVertex;k<range.firstVertex+range.vertexCount;++k)
                radius=std::max(radius,Length(Sub(loaded.meshes.vertices[k].position,center)));
            range.bounds={center.x,center.y,center.z,radius};
        }
        char report[240];
        std::snprintf(report,sizeof(report),
            "mg2_portal_contact rosette_nodes=%u sides=2 negative_shift_m=%.4f positive_shift_m=%.4f\r\n",
            rosetteNodes,negativeShift,positiveShift);
        DiagnosticLine(report);
    }
    XMFLOAT3 lo{std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()};
    XMFLOAT3 hi{-lo.x,-lo.y,-lo.z};
    for(const auto& vertex:loaded.meshes.vertices){
        const auto p=vertex.position;
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
            throw std::runtime_error("MG2: nonfinite transformed vertex "+entry.key);
        lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);lo.z=std::min(lo.z,p.z);
        hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);hi.z=std::max(hi.z,p.z);
    }
    entry.runtimeLower=lo;entry.runtimeUpper=hi;
    entry.scene.emplace(std::move(loaded));
    MarkDiagnosticStartupProgress();
}

bool Mg2Catalog::PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,
                              unsigned treeDensity){
    forest_.clear();
    plantedCells_=0;
    if(terrain.soil.empty()||sites.empty())return false;
    const auto started=QpcCounter();
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    // True opaque 3D source geometry (branches, solid needles, convex leaves).
    // Density is physical spacing rather than a global triangle or tree cap.
    const float spacing=(treeDensity>=4u?3.25f:treeDensity<=1u?4.8f:3.65f)*units;
    float left=std::numeric_limits<float>::max(),right=-left,front=left,back=-left;
    for(const auto& triangle:terrain.soil)for(const auto& p:triangle){
        left=std::min(left,p.x);right=std::max(right,p.x);
        front=std::min(front,p.z);back=std::max(back,p.z);
    }
    // Bin triangles for fast height sampling after the surface has been
    // tessellated. Avoid the previous candidates x all-triangles scan.
    constexpr float binWidth=.15f;
    const auto coordinate=[=](float p){return int32_t(std::floor(p/binWidth));};
    const auto key=[](int32_t x,int32_t z){return (uint64_t(uint32_t(x))<<32)|uint32_t(z);};
    std::unordered_map<uint64_t,std::vector<uint32_t>> bins;
    for(uint32_t i=0;i<terrain.soil.size();++i){
        const auto& t=terrain.soil[i];
        const int32_t loX=coordinate(std::min({t[0].x,t[1].x,t[2].x}));
        const int32_t hiX=coordinate(std::max({t[0].x,t[1].x,t[2].x}));
        const int32_t loZ=coordinate(std::min({t[0].z,t[1].z,t[2].z}));
        const int32_t hiZ=coordinate(std::max({t[0].z,t[1].z,t[2].z}));
        for(int32_t x=loX;x<=hiX;++x)for(int32_t z=loZ;z<=hiZ;++z)
            bins[key(x,z)].push_back(i);
    }
    const auto height=[&](float x,float z)->std::optional<float>{
        const auto cell=bins.find(key(coordinate(x),coordinate(z)));
        if(cell==bins.end())return {};
        std::optional<float> best;
        for(const auto index:cell->second){
            const auto& [a,b,c]=terrain.soil[index];
            const auto ab=Sub(b,a),ac=Sub(c,a);
            const float det=ab.x*ac.z-ab.z*ac.x;
            if(std::abs(det)<1.e-8f)continue;
            const float dx=x-a.x,dz=z-a.z;
            const float u=(dx*ac.z-dz*ac.x)/det,v=(ab.x*dz-ab.z*dx)/det;
            if(u<-1.e-5f||v<-1.e-5f||u+v>1.00001f)continue;
            const float y=a.y+u*ab.y+v*ac.y;
            if(!best||y>*best)best=y;
        }
        return best;
    };
    const auto hash=[](uint32_t n){
        n^=n>>16;n*=0x7feb352du;n^=n>>15;
        n*=0x846ca68bu;n^=n>>16;
        return float(n&0x00ffffffu)/16777215.f;
    };
    uint32_t cell=0;
    for(float z=front+.5f*spacing;z<back-.5f*spacing;z+=spacing){
        for(float x=left+.5f*spacing;x<right-.5f*spacing;x+=spacing,++cell){
            const uint32_t seed=0x4f415349u^((cell+1u)*0x9e3779b9u);
            const float px=x+(hash(seed^0x314u)-.5f)*spacing*.70f;
            const float pz=z+(hash(seed^0x928u)-.5f)*spacing*.70f;
            const auto y=height(px,pz);
            if(!y)continue;
            const size_t species=size_t(hash(seed^0x611u)*3.f)%3u;
            const float scale=units*(.83f+.38f*hash(seed^0x21au));
            const float crown=(species==2u?3.6f:3.3f)*scale;
            constexpr float foundationMargin=.65f*units;
            const bool foundation=std::any_of(sites.begin(),sites.end(),[&](const auto& site){
                const float sx=std::clamp(px,site.origin.x,site.origin.x+site.width);
                const float sz=std::clamp(pz,site.origin.z,site.origin.z+site.depth);
                return std::hypot(px-sx,pz-sz)<crown+foundationMargin;
            });
            if(foundation)continue;
            const XMFLOAT3 root{px,*y,pz};
            const auto local=Sub(root,SpectacleCenter);
            const float horizontal=std::hypot(local.x,local.z)+crown;
            const float vertical=std::max(std::abs(local.y),
                                          std::abs(local.y+(species==1u?12.6f:11.8f)*scale));
            if(std::hypot(horizontal,vertical)>SpectacleRadius-.055f)continue;
            const float halfYaw=XM_PI*hash(seed^0x816u);
            forest_.push_back({species,{root,{0.f,std::sin(halfYaw),0.f,std::cos(halfYaw)}},scale,cell});
            ++plantedCells_;
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
    // Use PBR maps only when provenance confirms an acquired material set.
    std::map<size_t,uint32_t> appended;
    auto assign=[&](std::string_view key,std::string_view surface,
                    std::initializer_list<Material> roles){
        const auto index=EntryIndex(key);
        if(index>=entries_.size()||!entries_[index].realPbr)return;
        if(!TryPrepareEntry(index))return;
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
    PrepareGeometry();
    DiagnosticLine("mg2_worktop_base=begin\r\n");
    Mg2Worktop result;
    std::vector<std::vector<WorldObject>> models(entries_.size());
    for(size_t surface=0;surface<worktops_.size();++surface){
        auto& props=result.surfaces[surface];
        for(const auto& placement:worktops_[surface]){
            const auto& entry=entries_[placement.entry];
            if(!TryPrepareEntry(placement.entry))continue;
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
                entry.runtimeLower,entry.runtimeUpper,
                kind,placement.chance,placement.entry});
        }
        std::stable_sort(props.begin(),props.end(),[](const auto& a,const auto& b){return a.kind<b.kind;});
    }
    DiagnosticLine("mg2_worktop_base=ready\r\n");
    DiagnosticLine("mg2_hero_recipe=begin\r\n");
    MarkDiagnosticStartupProgress();
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
    result.tabletopClearance=workRule.at("tabletop_clearance_m").get<float>();
    result.seatScreenGap=workRule.at("minimum_screen_separation_m").get<float>();
    const auto& extinguisher=placementRecipe.at("extinguisher");
    result.extinguisherWallGap=extinguisher.at("office_wall_gap_m").get<float>();
    result.extinguisherEndGap=extinguisher.at("office_end_gap_m").get<float>();
    result.extinguisherHeight=extinguisher.at("office_mount_height_m").get<float>();
    result.corridorExtinguisherBaseOffset=extinguisher.at("corridor_base_offset_m").get<float>();
    result.corridorTrafficZ=extinguisher.at("corridor_traffic_z_m").get<float>();
    if(!(result.seatScreenGap>0.f&&result.seatScreenGap<result.seatDistance&&
         result.extinguisherWallGap>0.f&&result.extinguisherWallGap<.5f&&
         result.extinguisherEndGap>0.f&&result.extinguisherEndGap<1.f&&
         result.extinguisherHeight>0.f&&result.extinguisherHeight<2.f&&
         result.corridorExtinguisherBaseOffset>=0.f&&result.corridorExtinguisherBaseOffset<.2f&&
         std::isfinite(result.corridorTrafficZ)))
        throw std::runtime_error("MG2: invalid canonical hero anchors");
    const auto& doorRule=placementRecipe.at("door");
    result.doorSwingAwayFromStairs=doorRule.at("swing_away_from_stairwell").get<bool>();
    const auto& networkRule=placementRecipe.at("network_bundle");
    result.networkChaseFaceGap=networkRule.at("chase_front_clearance_m").get<float>();
    result.networkCableBaseHeight=networkRule.at("base_height_above_floor_m").get<float>();
    if(!(result.tabletopClearance>=0.f&&result.tabletopClearance<=.0001f&&
         result.networkChaseFaceGap>.20f&&result.networkChaseFaceGap<.40f&&
         result.networkCableBaseHeight>=.05f&&result.networkCableBaseHeight<.6f))
        throw std::runtime_error("MG2: invalid placement contact or riser recipe");
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
    DiagnosticLine("mg2_hero_recipe=ready\r\n");
    MarkDiagnosticStartupProgress();
    for(size_t i=0;i<MandatoryHeroKeys.size();++i){
        const std::string key(MandatoryHeroKeys[i]);
        char progress[256];
        std::snprintf(progress,sizeof(progress),"mg2_hero_prepare asset=%s\r\n",key.c_str());
        DiagnosticLine(progress);
        MarkDiagnosticStartupProgress();
        const auto entry=EntryIndex(MandatoryHeroKeys[i]);
        if(entry>=entries_.size()){degraded_=true;continue;}
        try{
        if(!entries_[entry].scene){
            std::snprintf(progress,sizeof(progress),"mg2_hero_load asset=%s\r\n",key.c_str());
            DiagnosticLine(progress);
            if(!TryPrepareEntry(entry))continue;
        }
        auto& model=models[entry];
        if(model.empty()){
            std::snprintf(progress,sizeof(progress),"mg2_hero_append asset=%s\r\n",key.c_str());
            DiagnosticLine(progress);
            model=AppendParts(scene,entry);
        }
        MarkDiagnosticStartupProgress();
        const auto& facing=placementRecipe.at("heroes").at(key);
        const auto axis=facing.at("gltf_front_axis").get<std::string>();
        XMFLOAT2 engineFront{};
        if(axis=="+Z")engineFront={0.f,-1.f};
        else if(axis=="-Z")engineFront={0.f,1.f};
        else if(axis=="+X")engineFront={1.f,0.f};
        else if(axis=="-X")engineFront={-1.f,0.f};
        else throw std::runtime_error("MG2: invalid GLB front axis for "+std::string(MandatoryHeroKeys[i]));
        result.heroes[i]={model,entries_[entry].runtimeLower,entries_[entry].runtimeUpper,engineFront};
        if(i==Mg2Worktop::Desk){
            // Vertex-defined top surface, independent from grommets.
            const auto& source=*entries_[entry].scene;
            float top=-std::numeric_limits<float>::infinity();
            for(size_t j=0;j<source.objects.size();++j){
                const auto& name=source.meshNodeNames.at(j);
                if(name!="top"&&name!="precision_oak_laminate_top")continue;
                const auto& range=source.meshes.ranges.at(source.objects[j].mesh);
                for(uint32_t k=range.firstVertex;k<range.firstVertex+range.vertexCount;++k)
                    top=std::max(top,source.meshes.vertices.at(k).position.y);
            }
            if(!std::isfinite(top))
                throw std::runtime_error("MG2: source desk has no actual top surface");
            result.deskSurfaceY=top;
        }
        if(i==Mg2Worktop::Door){
            const auto& source=*entries_[entry].scene;
            if(source.meshNodeNames.size()!=model.size())
                throw std::runtime_error("MG2: door mesh names missing");
            const auto& fixedNames=doorRule.at("stationary_nodes");
            const auto& leafNames=doorRule.at("leaf_nodes");
            std::vector<WorldObject> fixed,leaf;
            for(size_t j=0;j<model.size();++j){
                const auto& name=source.meshNodeNames[j];
                const auto contains=[&](const Json& names){
                    return std::any_of(names.begin(),names.end(),
                        [&](const Json& value){return value==name;});
                };
                const bool stationary=contains(fixedNames),swings=contains(leafNames);
                if(stationary==swings)
                    throw std::runtime_error("MG2: unclassified portal geometry "+name);
                (stationary?fixed:leaf).push_back(model[j]);
            }
            if(fixed.empty()||leaf.empty())
                throw std::runtime_error("MG2: portal without frame or leaf");
            result.doorFrameParts=std::move(fixed);
            result.heroes[i].parts=std::move(leaf);
            DiagnosticLine("mg2_door_split=fixed_frame_leaf_only\r\n");
        }
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
        std::snprintf(progress,sizeof(progress),"mg2_hero_ready asset=%s\r\n",key.c_str());
        DiagnosticLine(progress);
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){
            result.heroes[i]={};
            if(i==Mg2Worktop::Door)result.doorFrameParts.clear();
            if(i==Mg2Worktop::CeilingFixture)result.unlitFixture.clear();
            entries_[entry].unavailable=true;degraded_=true;
            const auto line="asset_unavailable asset="+key+" reason="+error.what()+" action=retain_structural_recipe\r\n";DiagnosticLine(line.c_str());
        }
    }
    DiagnosticLine("mg2_hero=preparation_complete\r\n");
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
                    if(!TryPrepareEntry(placement.entry)){++profilesSkipped;continue;}
                    const auto& source=*entries_[placement.entry].scene;
                    if(!source.animations.empty()||!source.bones.empty()||
                       !source.meshes.morphs.empty())
                        throw std::runtime_error("non-rigid GLB in static room profile");
                    model=AppendParts(scene,placement.entry);
                }catch(const std::bad_alloc&){throw;}
                catch(const std::exception& error){
                    char message[512];
                    std::snprintf(message,sizeof(message),
                        "mg2_room_asset_failed asset=%s reason=%.240s\r\n",
                        entry.key.c_str(),error.what());
                    DiagnosticLine(message);
                    unavailable[placement.entry]=true;degraded_=true;
                    ++profilesSkipped;
                    continue;
                }
            }
            // Sidecar boxes use glTF Z; engine rigid imports invert it.
            const bool tabletop=entry.key=="oasis.furniture.reception_counter01"||
                entry.key=="oasis.furniture.kitchenette_base01"||
                entry.key=="oasis.furniture.desk_corner02"||
                entry.key=="oasis.furniture.desk_sitstand03"||
                entry.key=="oasis.furniture.coffee_table_round01"||
                entry.key=="oasis.furniture.meeting_table01";
            props.push_back({model,entry.runtimeLower,entry.runtimeUpper,
                             placement.nx,placement.nz,placement.chance,placement.yawDeg,
                             placement.mountHeight,placement.mount,tabletop});
            ++profilesLoaded;
        }
    }
    // Reuse the same validated GLB already required by maintenance rooms.
    // Only the rooftop runtime placement changes; no new mesh copy per tower.
    {
        const auto index=EntryIndex("oasis.hospitality.roof_hvac01");
        if(TryPrepareEntry(index)){
            auto& model=models[index];
            if(model.empty())model=AppendParts(scene,index);
            result.rooftopHvac=model;
            DiagnosticLine("mg2_rooftop_hvac=ready\r\n");
        }
    }
    char roomSummary[160];
    std::snprintf(roomSummary,sizeof(roomSummary),
        "mg2_room_profiles loaded=%zu skipped=%zu\r\n",profilesLoaded,profilesSkipped);
    DiagnosticLine(roomSummary);
    MarkDiagnosticStartupProgress();
    // Original 100-poster source is already in oasis_dressing_v1. Import
    // each atlas only once and register named paper+frame components.
    size_t posterMeshes=0,posterSheets=0;
    for(unsigned sheet=1;sheet<=5;++sheet){
        char filename[32];
        std::snprintf(filename,sizeof(filename),"poster_sheet_%02u.glb",sheet);
        const auto file=directory_.parent_path()/"oasis_dressing_v1"/"geometry"/filename;
        if(!std::filesystem::exists(file)){degraded_=true;const auto line="asset_unavailable source="+file.string()+" reason=missing_poster_atlas\r\n";DiagnosticLine(line.c_str());continue;}
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
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& ex){
            char line[384];
            std::snprintf(line,sizeof(line),"mg2_poster_atlas_failed sheet=%u reason=%.240s\r\n",
                sheet,ex.what());
            DiagnosticLine(line);degraded_=true;
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
            // Place the lowest actual vertex on the desktop surface.
            offset={x,prop.offset.y-prop.lower.y+tabletopClearance,z};
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
    constexpr int32_t forestLayer=64;
    const auto before=scene.mg2ForestParts.size();
    if(forest_.empty())return;
    const auto species=BuildOrganicTreeSources(scene);
    std::vector<WorldObject> instances;
    instances.reserve(forest_.size()*2u);
    uint64_t sourceTriangles=0,instanceTriangles=0;
    for(const auto& model:species)sourceTriangles+=model.triangles;
    for(const auto& plant:forest_){
        const auto& model=species.at(plant.entry);
        instanceTriangles+=model.triangles;
        for(auto part:model.parts){
            part.current=part.previous=plant.pose;
            part.scale={plant.scale,plant.scale,plant.scale};
            part.tileLayer=forestLayer;
            instances.push_back(part);
        }
    }
    world.CreateBatch(instances);
    char line[320];
    std::snprintf(line,sizeof(line),
        "mg2_foret_3d cellules=%zu arbres=%zu especes=%zu instances=%zu "
        "triangles_3d_source=%llu triangles_instancies=%llu sources_clas=%zu cartes_alpha=0\r\n",
        plantedCells_,forest_.size(),species.size(),instances.size(),
        static_cast<unsigned long long>(sourceTriangles),
        static_cast<unsigned long long>(instanceTriangles),
        scene.mg2ForestParts.size()-before);
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
    using Buildings::MountSurface;
    std::snprintf(line,sizeof(line),"mg2_liaisons bureau=%zu reunion=%zu bandes_foret=%zu\r\n",
        worktops_[size_t(MountSurface::OfficeDesktop)].size(),worktops_[size_t(MountSurface::MeetingTable)].size(),
        forestRows_.size());
    DiagnosticLine(line);
}
}
