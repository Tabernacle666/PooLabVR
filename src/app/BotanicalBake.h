#pragma once
#include "Assets.h"
#include "DiskBake.h"

namespace OASIS::BotanicalBake {
// Persistent, assembly-local records. No world IDs, material indices or GPU
// addresses are stored. The recipe is authoritative; meshlets and LODs are
// derived, independently versioned products. Bump only the affected revision
// when changing a producer (including FinishRange for the mesh revision).
inline constexpr std::array<unsigned,3> RecipeRevision{2,2,2};
inline constexpr std::array<unsigned,3> AtlasRevision{1,1,1};
inline constexpr unsigned MeshRevision=2;
struct Recipe {
    std::vector<CanopyPart> parts;
    CanopyAssembly assembly;
};
struct Geometry {
    std::array<MeshData,2> parts;
    std::vector<CanopyLodNode> lods;
};
struct Stats {
    uint64_t hits{},misses{},writes{},readBytes{},writtenBytes{};
};
inline std::filesystem::path Key(const wchar_t* group,std::string identity){
    // No dependency on executable timestamps, campus, density, or placement.
    return DiskBake::Key(group,identity.c_str(),{});
}
inline std::string RecipeIdentity(unsigned species,unsigned variant){
    return "botanical-recipe-v1/"+std::to_string(species)+"/"+std::to_string(variant)+"/"+
        std::to_string(RecipeRevision.at(species))+"/"+std::to_string(sizeof(CanopyPartPlacement));
}
inline std::filesystem::path RecipeKey(unsigned species,unsigned variant){
    return Key(L"botanical-recipes",RecipeIdentity(species,variant));
}
inline std::filesystem::path GeometryKey(unsigned species,unsigned variant){
    return Key(L"botanical-geometry",RecipeIdentity(species,variant)+"/mesh-v"+
        std::to_string(MeshRevision)+"/"+std::to_string(sizeof(Vertex))+"/"+
        std::to_string(sizeof(MeshRange))+"/"+std::to_string(sizeof(Meshlet)));
}
inline std::filesystem::path AtlasKey(unsigned species){
    return Key(L"botanical-atlases","botanical-atlas-v1/"+std::to_string(species)+"/"+
        std::to_string(AtlasRevision.at(species)));
}
template<class IO,class Part> bool PartFields(IO& io,Part& p){
    return io.Pod(p.kind)&&io.Pod(p.sides)&&io.Vec(p.ring);
}
template<class IO,class Assembly> bool AssemblyFields(IO& io,Assembly& a){
    return io.Pod(a.species)&&io.Pod(a.variation)&&io.Pod(a.seed)&&io.Pod(a.rigidity)&&io.Vec(a.placements);
}
template<class IO,class Mesh> bool MeshFields(IO& io,Mesh& m){
    return io.Vec(m.vertices)&&io.Vec(m.indices)&&io.Vec(m.remap)&&io.Vec(m.triangles)&&
        io.Vec(m.meshlets)&&io.Vec(m.ranges);
}
template<class IO,class Lod> bool LodFields(IO& io,Lod& n){
    return io.Pod(n.mesh)&&io.Pod(n.components)&&io.Pod(n.error)&&io.Vec(n.vertices)&&io.Vec(n.indices);
}
template<class IO,class Texture> bool AtlasFields(IO& io,Texture& t){
    return io.Pod(t.width)&&io.Pod(t.height)&&io.Pod(t.mips)&&io.Pod(t.normal)&&
        io.Pod(t.format)&&io.Vec(t.bytes)&&io.Vec(t.offsets);
}
inline bool Valid(const Recipe& r,unsigned species,unsigned variant){
    if(r.assembly.species!=species||r.assembly.variation!=variant||r.assembly.placements.empty())return false;
    for(const auto& p:r.parts){
        if(unsigned(p.kind)>unsigned(CanopyPartKind::Leaves))return false;
        const bool foliage=p.kind==CanopyPartKind::Leaves||p.kind==CanopyPartKind::Needles;
        if(foliage?(p.sides!=0||!p.ring.empty()):(p.sides<3||p.ring.size()!=size_t(p.sides)+1))return false;
    }
    for(size_t i=0;i<r.assembly.placements.size();++i){
        const auto& p=r.assembly.placements[i];
        if(p.part>=r.parts.size()||(p.parent!=Invalid&&p.parent>=i))return false;
    }
    return true;
}
inline bool Read(DiskBake::Reader& io,Recipe& r){
    uint64_t n{};
    if(!io.Pod(n)||n>io.remaining/(sizeof(CanopyPartKind)+sizeof(uint32_t)+sizeof(uint64_t))||n>r.parts.max_size())return false;
    r.parts.resize(size_t(n));
    for(auto& p:r.parts)if(!PartFields(io,p))return false;
    return AssemblyFields(io,r.assembly);
}
inline bool Write(DiskBake::Writer& io,const Recipe& r){
    if(!io.Pod(uint64_t(r.parts.size())))return false;
    for(const auto& p:r.parts)if(!PartFields(io,p))return false;
    return AssemblyFields(io,r.assembly);
}
inline bool Read(DiskBake::Reader& io,Geometry& g){
    for(auto& m:g.parts)if(!MeshFields(io,m))return false;
    uint64_t n{};
    if(!io.Pod(n)||n>io.remaining/(sizeof(uint32_t)*2+sizeof(float)+sizeof(uint64_t)*2)||n>g.lods.max_size())return false;
    g.lods.resize(size_t(n));for(auto& l:g.lods)if(!LodFields(io,l))return false;
    return true;
}
inline bool Write(DiskBake::Writer& io,const Geometry& g){
    for(const auto& m:g.parts)if(!MeshFields(io,m))return false;
    if(!io.Pod(uint64_t(g.lods.size())))return false;
    for(const auto& l:g.lods)if(!LodFields(io,l))return false;
    return true;
}
inline bool Read(DiskBake::Reader& io,TextureData& t){return AtlasFields(io,t);}
inline bool Write(DiskBake::Writer& io,const TextureData& t){return AtlasFields(io,t);}
inline bool Valid(const Geometry& g){
    for(const auto& m:g.parts){
        if(m.ranges.size()!=1||m.vertices.empty()||m.indices.empty()||m.indices.size()%3||m.meshlets.empty())return false;
        const auto& r=m.ranges.front();
        if(r.firstVertex||r.firstIndex||r.firstMeshlet||r.vertexCount!=m.vertices.size()||
           r.indexCount!=m.indices.size()||r.meshletCount!=m.meshlets.size())return false;
        // Payload checksum protects the cooked data; producer guarantees the
        // internal meshlet topology. Check the local route envelope here.
    }
    for(const auto& l:g.lods)if(l.mesh>=g.parts.size()||l.vertices.empty()||l.indices.empty()||l.indices.size()%3)return false;
    return true;
}
inline bool Valid(const TextureData& t){
    if(!t.width||!t.height||!t.mips||t.mips!=t.offsets.size()||t.format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return false;
    uint64_t offset=0;unsigned w=t.width,h=t.height;
    for(auto o:t.offsets){
        if(o!=offset||offset>t.bytes.size()||uint64_t(w)*h>(t.bytes.size()-offset)/4)return false;
        offset+=uint64_t(w)*h*4;
        w=(std::max)(1u,w/2);h=(std::max)(1u,h/2);
    }
    return offset==t.bytes.size();
}
inline uint64_t Size(const std::filesystem::path& path){
    std::error_code error;const auto n=std::filesystem::file_size(path,error);
    return error?0:uint64_t(n);
}
template<class T,class Validator> bool Load(const std::filesystem::path& path,T& value,Stats& stats,Validator valid){
    if(!path.empty()){
        DiskBake::Reader io(path);
        T candidate{};
        if(Read(io,candidate)&&io.Finish()&&valid(candidate)){
            value=std::move(candidate);++stats.hits;stats.readBytes+=Size(path);return true;
        }
    }
    ++stats.misses;return false;
}
template<class T> void Save(const std::filesystem::path& path,const T& value,Stats& stats){
    if(path.empty())return;
    DiskBake::Writer io(path);
    if(Write(io,value)&&io.Finish()){++stats.writes;stats.writtenBytes+=Size(path);}
}
}
