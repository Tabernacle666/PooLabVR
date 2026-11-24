#pragma once
#include "Assets.h"
#include "DiskBake.h"

namespace OASIS::SceneBake {
template<class IO,class Scene> bool Mesh(IO& io,Scene& s){
    auto& m=s.meshes;
    return io.Vec(m.vertices)&&io.Vec(m.indices)&&io.Vec(m.remap)&&io.Vec(m.triangles)&&
        io.Vec(m.meshlets)&&io.Vec(m.ranges)&&io.Vec(m.deformations)&&io.Vec(m.skin)&&
        io.Vec(m.morphs)&&io.Vec(m.motions)&&io.Vec(s.materials)&&io.Vec(s.objects)&&
        io.Vec(s.lights)&&io.Vec(s.triangleSolids)&&io.Pod(s.writableStaticGeometry)&&io.Pod(s.assetDegraded)&&
        io.Vec(s.samplers)&&io.Vec(s.nodes)&&io.Vec(s.nodeOrder)&&io.Vec(s.bones)&&
        io.Vec(s.weights)&&io.Vec(s.mg2ForestParts)&&io.Pod(s.laserWallMaterials)&&
        io.Vec(s.sphereObjectOffsets)&&io.Pod(s.solarSystem);
}
template<class IO,class Texture> bool Tex(IO& io,Texture& t){
    return io.Pod(t.width)&&io.Pod(t.height)&&io.Pod(t.mips)&&io.Pod(t.normal)&&
        io.Pod(t.format)&&io.Vec(t.bytes)&&io.Vec(t.offsets);
}
template<class IO,class Track> bool TrackFields(IO& io,Track& t){
    return io.Pod(t.node)&&io.Pod(t.components)&&io.Pod(t.path)&&io.Pod(t.interpolation)&&
        io.Vec(t.times)&&io.Vec(t.values);
}
inline bool Read(const std::filesystem::path& path,SceneData& s){
    DiskBake::Reader io(path);
    if(!Mesh(io,s))return false;
    uint64_t n{};
    if(!io.Count(n,65536))return false;
    s.textures.resize(size_t(n));for(auto& t:s.textures)if(!Tex(io,t))return false;
    if(!io.Count(n,65536))return false;
    s.animations.resize(size_t(n));
    for(auto& a:s.animations){
        if(!io.Pod(a.duration)||!io.Count(n,1000000))return false;
        a.tracks.resize(size_t(n));for(auto& t:a.tracks)if(!TrackFields(io,t))return false;
    }
    for(auto* list:{&s.materialNames,&s.meshNodeNames}){
        if(!io.Count(n,1000000))return false;
        list->resize(size_t(n));for(auto& x:*list)if(!io.String(x))return false;
    }
    return io.Finish();
}
inline bool Write(const std::filesystem::path& path,const SceneData& s){
    if(s.assetDegraded)return false;
    DiskBake::Writer io(path);
    if(!Mesh(io,s)||!io.Pod(uint64_t(s.textures.size())))return false;
    for(const auto& t:s.textures)if(!Tex(io,t))return false;
    if(!io.Pod(uint64_t(s.animations.size())))return false;
    for(const auto& a:s.animations){
        if(!io.Pod(a.duration)||!io.Pod(uint64_t(a.tracks.size())))return false;
        for(const auto& t:a.tracks)if(!TrackFields(io,t))return false;
    }
    for(const auto* list:{&s.materialNames,&s.meshNodeNames}){
        if(!io.Pod(uint64_t(list->size())))return false;
        for(const auto& x:*list)if(!io.String(x))return false;
    }
    return io.Finish();
}
}
