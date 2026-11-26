#pragma once
#include "Mg2Catalog.h"
#include "SceneBake.h"

namespace OASIS::CampusBake {
namespace fs=std::filesystem;
// This snapshot stops before any WorldRegistry publication or live building
// install. It contains immutable merged geometry, exact deduplicated textures
// and placement recipes whose mesh/material indices refer to that geometry.
struct Data {
    SceneData scene;
    Mg2Worktop worktop;
    Mg2ArchitecturalPalette palette;
};
inline fs::path Key(const fs::path& pack,const fs::path& profile,bool desktop,bool replacesLowVegetation){
    try{
        std::vector<fs::path> files{
            AssetPath(L"oasis_logo.glb"),AssetPath(L"navette_organes.glb"),
            AssetPath(L"vaisseau2_z31.glb")
        };
        auto collect=[&](const fs::path& root){
            if(!fs::exists(root))return;
            for(const auto& entry:fs::recursive_directory_iterator(root))
                if(entry.is_regular_file())files.push_back(entry.path());
        };
        collect(pack);
        collect(pack.parent_path()/L"oasis_dressing_v1");
        collect(fs::path(AssetPath(L"buildings/oasis")));
        collect(fs::path(AssetPath(L"materials")));
        if(!profile.empty())collect(profile.parent_path()/L"mods"/L"textures");
        std::sort(files.begin(),files.end());
        // File identity participates as well as bytes, so a renamed override
        // or a removed recipe cannot accidentally reuse another file's bake.
        std::string version=OASIS_CAMPUS_BAKE_KEY;
        version+=desktop?":flat_fixed_v1":":vr_fixed_v1";
        version+=profile.empty()?":factory":":profile";
        version+=replacesLowVegetation?":forest_replaces_low_vegetation":":original_low_vegetation";
        for(const auto& file:files){
            const auto name=file.generic_u8string();
            version+=":"+std::to_string(name.size())+":";
            version.append(reinterpret_cast<const char*>(name.data()),name.size());
        }
        return DiskBake::Key(L"campus",version.c_str(),files);
    }catch(const std::exception&){return {};}
}
inline fs::path RecipePath(fs::path path){path+=L".recipes";return path;}
inline bool Exists(const fs::path& path){
    if(path.empty())return false;
    std::error_code error;
    return fs::is_regular_file(path,error)&&fs::is_regular_file(RecipePath(path),error);
}
template<class IO,class W> bool Fixed(IO& io,W& w){
    if(!io.Pod(w.deskSurfaceY)||!io.Pod(w.tabletopClearance)||
       !io.Pod(w.networkChaseFaceGap)||!io.Pod(w.networkCableBaseHeight)||
       !io.Pod(w.doorSwingAwayFromStairs)||!io.Pod(w.screenSideZ)||
       !io.Pod(w.seatDistance)||!io.Pod(w.seatJitterX)||!io.Pod(w.seatJitterZ)||
       !io.Pod(w.seatClearance)||!io.Pod(w.seatScreenGap)||
       !io.Pod(w.extinguisherWallGap)||!io.Pod(w.extinguisherEndGap)||!io.Pod(w.extinguisherHeight)||
       !io.Pod(w.corridorExtinguisherBaseOffset)||!io.Pod(w.corridorTrafficZ)||
       !io.Vec(w.rooftopHvac)||!io.Vec(w.doorFrameParts)||
       !io.Vec(w.unlitFixture))return false;
    for(auto& hero:w.heroes)
        if(!io.Vec(hero.parts)||!io.Pod(hero.lower)||!io.Pod(hero.upper)||!io.Pod(hero.engineFront))return false;
    for(auto& parts:w.posterParts)if(!io.Vec(parts))return false;
    return true;
}
template<class IO,class P> bool Prop(IO& io,P& p){
    return io.Vec(p.parts)&&io.Pod(p.offset)&&io.Pod(p.lower)&&io.Pod(p.upper)&&
        io.Pod(p.kind)&&io.Pod(p.chance)&&io.Pod(p.sourceEntry);
}
template<class IO,class P> bool RoomProp(IO& io,P& p){
    return io.Vec(p.parts)&&io.Pod(p.lower)&&io.Pod(p.upper)&&io.Pod(p.nx)&&io.Pod(p.nz)&&
        io.Pod(p.chance)&&io.Pod(p.yawDeg)&&io.Pod(p.mountHeight)&&io.Pod(p.mount)&&io.Pod(p.providesCountertop);
}
inline bool Read(const fs::path& path,Data& data){
    try{
        if(!Exists(path))return false;
        Data restored;
        DiskBake::Reader io(RecipePath(path));
        if(!io.Pod(restored.palette)||!Fixed(io,restored.worktop))return false;
        for(auto& props:restored.worktop.surfaces){
            uint64_t n{};if(!io.Count(n,100000))return false;
            props.resize(size_t(n));for(auto& p:props)if(!Prop(io,p))return false;
        }
        for(auto& props:restored.worktop.roomProfiles){
            uint64_t n{};if(!io.Count(n,100000))return false;
            props.resize(size_t(n));for(auto& p:props)if(!RoomProp(io,p))return false;
        }
        if(!io.Finish()||!SceneBake::Read(path,restored.scene))return false;
        data=std::move(restored);return true;
    }catch(const std::exception&){return false;}
}
inline bool Write(const fs::path& path,const SceneData& scene,const Mg2Worktop& w,
                  const Mg2ArchitecturalPalette& palette){
    try{
        if(path.empty()||!SceneBake::Write(path,scene))return false;
        DiskBake::Writer io(RecipePath(path));
        if(!io.Pod(palette)||!Fixed(io,w))return false;
        for(const auto& props:w.surfaces){
            if(!io.Pod(uint64_t(props.size())))return false;
            for(const auto& p:props)if(!Prop(io,p))return false;
        }
        for(const auto& props:w.roomProfiles){
            if(!io.Pod(uint64_t(props.size())))return false;
            for(const auto& p:props)if(!RoomProp(io,p))return false;
        }
        return io.Finish();
    }catch(const std::exception&){return false;}
}
}
