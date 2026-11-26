#include "Mg2Catalog.h"
#include "OrganicForest.h"
#include "AppSession.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace OASIS {
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
