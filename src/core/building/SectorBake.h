#pragma once
#include "BuildingPlan.h"
#include "DiskBake.h"
#include <memory>

namespace OASIS::Buildings::SectorBake {
template<class IO,class S> bool Fields(IO& io,S& s){
    return io.Pod(s.level)&&io.Vec(s.rooms)&&io.Vec(s.parts)&&io.Vec(s.joints)&&
        io.Vec(s.services)&&io.Vec(s.mounts)&&io.Vec(s.lights)&&io.Vec(s.contactNodes)&&io.Vec(s.contactParts);
}
inline bool Read(const std::filesystem::path& path,std::vector<Sector>& sectors,
                 std::vector<std::shared_ptr<const Sector>>& sources,int floors){
    DiskBake::Reader io(path);
    uint64_t n{};
    if(!io.Count(n,10000)||n!=uint64_t(floors+1))return false;
    std::vector<Sector> a(size_t(n),Sector{});
    std::vector<std::shared_ptr<const Sector>> b;
    for(size_t i=0;i<a.size();++i){
        auto source=std::make_shared<Sector>();
        if(!Fields(io,a[i])||!Fields(io,*source)||a[i].level!=int(i)-1||source->level!=a[i].level)return false;
        b.push_back(std::move(source));
    }
    if(!io.Finish())return false;
    sectors=std::move(a);sources=std::move(b);return true;
}
inline bool Write(const std::filesystem::path& path,const std::vector<Sector>& sectors,
                  const std::vector<std::shared_ptr<const Sector>>& sources){
    DiskBake::Writer io(path);
    if(sectors.size()!=sources.size()||!io.Pod(uint64_t(sectors.size())))return false;
    for(size_t i=0;i<sectors.size();++i)if(!Fields(io,sectors[i])||!Fields(io,*sources[i]))return false;
    return io.Finish();
}
}
