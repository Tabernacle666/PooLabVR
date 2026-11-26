#include "Mg2Catalog.h"
#include "OrganicForest.h"
#include "AppSession.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace OASIS {
bool Mg2Catalog::PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,
                              unsigned treeDensity){
    forest_.clear();
    plantedCells_=0;
    if(terrain.soil.empty()||sites.empty())return false;
    const auto started=QpcCounter();
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    // Opaque branches and fixed 3D OMM sprigs; planting density is unchanged.
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

void Mg2Catalog::InstallForest(SceneData& scene,WorldRegistry& world,unsigned variantsPerSpecies)const{
    constexpr int32_t forestLayer=64;
    const auto before=scene.mg2ForestParts.size();
    if(forest_.empty())return;
    const auto models=BuildOrganicTreeSources(scene,variantsPerSpecies);
    std::vector<WorldObject> instances;
    instances.reserve(forest_.size()*2u);
    std::vector<uint32_t> assemblies;
    assemblies.reserve(forest_.size());
    uint64_t sourceTriangles=0,instanceTriangles=0;
    for(const auto& model:models)sourceTriangles+=model.triangles;
    for(const auto& plant:forest_){
        uint32_t seed=plant.cell^0x43414e4fu;
        seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;
        seed*=0x846ca68bu;seed^=seed>>16;
        const auto& variants=scene.canopy.species.at(plant.entry).assemblies;
        const auto assembly=variants.at(seed%variants.size());
        const auto& model=models.at(assembly);
        assemblies.push_back(assembly);
        instanceTriangles+=model.triangles;
        for(auto part:model.parts){
            part.current=part.previous=plant.pose;
            part.scale={plant.scale,plant.scale,plant.scale};
            part.tileLayer=forestLayer;
            instances.push_back(part);
        }
    }
    world.CreateCanopyBatch(instances,assemblies);
    char line[320];
    std::snprintf(line,sizeof(line),
        "mg2_foret_3d cellules=%zu arbres=%zu especes=%zu instances=%zu "
        "triangles_3d_source=%llu triangles_instancies=%llu sources_clas=%zu cartes_alpha=1\r\n",
        plantedCells_,forest_.size(),scene.canopy.species.size(),instances.size(),
        static_cast<unsigned long long>(sourceTriangles),
        static_cast<unsigned long long>(instanceTriangles),
        scene.mg2ForestParts.size()-before);
    DiagnosticLine(line);
    std::snprintf(line,sizeof(line),
        "canopy_plants_total=%zu canopy_parts_unique=%zu canopy_assemblies=%zu canopy_variants_per_species=%u\r\n",
        forest_.size(),scene.canopy.parts.size(),scene.canopy.assemblies.size(),variantsPerSpecies);
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
