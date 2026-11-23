#include "BuildingPlan.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <nlohmann/json.hpp>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace OASIS::Buildings {
namespace {
using Json=nlohmann::json;
Json Read(const std::filesystem::path& path){
    std::ifstream in(path,std::ios::binary);
    if(!in)throw std::runtime_error("Cannot open building data");
    return Json::parse(in);
}
uint32_t Random(uint64_t seed){
    seed=(seed^(seed>>30))*0xbf58476d1ce4e5b9ull;
    seed=(seed^(seed>>27))*0x94d049bb133111ebull;
    return uint32_t(seed^(seed>>31));
}
Rect ReadRect(const Json& value){
    if(!value.is_array()||value.size()!=4)throw std::runtime_error("Building rectangle");
    Rect r{value[0],value[1],value[2],value[3]};
    if(!std::isfinite(r.left)||!std::isfinite(r.front)||!std::isfinite(r.right)||
       !std::isfinite(r.back)||r.left>=r.right||r.front>=r.back)
        throw std::runtime_error("Invalid building rectangle");
    return r;
}
uint64_t RoomId(const Definition& d,int level,int slot){
    return (d.id<<32)|(uint64_t(level+2)<<16)|uint64_t(slot);
}
Use Program(const Definition& d,int floor,int slot,uint32_t variant,bool merged){
    if(floor==-1)return slot==1?Use::Machine:Use::Service;
    if(floor>=d.floors-2)return Use::Penthouse;
    if(slot==1&&(floor==0||floor==15||floor%int(d.receptionEvery)==0))return Use::Reception;
    if(merged)return Use::Meeting;
    const auto total=std::accumulate(d.roomWeights.begin(),d.roomWeights.end(),0u);
    auto draw=Random(d.seed^RoomId(d,floor,slot)^uint64_t(variant)*19139u)%total;
    constexpr std::array types{Use::Office,Use::Meeting,Use::Research,Use::Service};
    for(size_t i=0;i<types.size();++i){
        if(draw<d.roomWeights[i])return types[i];
        draw-=d.roomWeights[i];
    }
    return Use::Service;
}
bool VoidSlot(const Definition& d,int level,int slot){
    const int column=(slot-1)%6;
    return d.HasVoid(level)&&(column==2||column==3);
}
std::vector<Room> Rooms(const Definition& d,const Level& level,uint32_t buildingVariant){
    const float margin=.29f,span=(d.width-2*margin)/6.f;
    std::vector<Room> result;
    for(int side=0;side<2;++side){
        for(int i=0;i<6;){
            const int slot=1+side*6+i;
            if(VoidSlot(d,level.number,slot)){++i;continue;}
            const auto pinned=std::find_if(level.rooms.begin(),level.rooms.end(),[&](const Room& r){
                return r.id==RoomId(d,level.number,slot)&&r.approved;
            });
            const float x=margin+span*i;
            if(pinned!=level.rooms.end()){
                result.push_back(*pinned);
                i+=int(std::round((pinned->bounds.right-pinned->bounds.left+.035f)/span));
                continue;
            }
            const auto next=RoomId(d,level.number,slot+1);
            const bool nextPinned=std::any_of(level.rooms.begin(),level.rooms.end(),[&](const Room& r){
                return r.id==next&&r.approved;
            });
            const auto key=uint64_t(level.variant)*0x9e3779b9u+uint64_t(buildingVariant)*0x85ebca6bu;
            const auto roll=Random(d.seed^key^RoomId(d,level.number,slot));
            const bool merge=i<5&&!VoidSlot(d,level.number,slot+1)&&!nextPinned&&(roll%d.mergeEvery==0||(side==0&&i==2&&level.number==0));
            const int width=merge?2:1;
            const auto use=Program(d,level.number,slot,level.variant+buildingVariant,merge);
            result.push_back({RoomId(d,level.number,slot),
                {x, side?12.05f:.24f, margin+span*(i+width)-.035f, side?d.depth-.24f:3.0f},
                roll,use,false});
            i+=width;
        }
    }
    return result;
}
uint64_t Fingerprint(const Definition& d,bool legacy){
    uint64_t hash=14695981039346656037ull;
    auto profile=Json::array({d.id,d.origin.x,d.origin.y,d.origin.z,d.height,d.width,d.depth,
        d.floors,d.voidStart,d.voidEnd,d.atrium.left,d.atrium.front,d.atrium.right,d.atrium.back,
        d.slab,d.plenum});
    if(!legacy)for(const auto& field:Json::array({d.xScale,d.letter,d.seed,d.mergeEvery,d.receptionEvery,d.roomWeights}))
        profile.push_back(field);
    const auto data=profile.dump();
    for(unsigned char b:data){hash^=b;hash*=1099511628211ull;}
    return hash;
}
}
float Definition::Elevation(int level)const{
    return std::round(1000.f*height*float(level)/float(floors))*.001f;
}
const Level* Plan::At(int level)const{
    return level>=-1&&size_t(level+1)<levels.size()?&levels[size_t(level+1)]:nullptr;
}
bool Plan::Locked(Scope scope,Target t)const{
    if(!t.inside||approved)return true;
    if(scope==Scope::Building)return false;
    const auto* floor=At(t.level);
    if(!floor||floor->approved)return true;
    if(scope==Scope::Floor)return false;
    const auto room=std::find_if(floor->rooms.begin(),floor->rooms.end(),[&](const Room& r){return r.id==t.room;});
    return room==floor->rooms.end()||room->approved;
}
Definition ReadDefinition(const std::filesystem::path& path){
    const auto data=Read(path);
    if(data.at("version")!=1)throw std::runtime_error("Building definition version");
    const auto& origin=data.at("origin");
    const auto& atriumLevels=data.at("atrium_levels");
    Definition d{data.at("id"),data.at("seed"),{origin.at(0),origin.at(1),origin.at(2)},
        data.at("height"),data.at("width"),data.at("depth"),data.at("floors"),
        atriumLevels.at(0),atriumLevels.at(1),ReadRect(data.at("atrium")),data.at("slab"),data.at("plenum"),
        data.at("merge_every"),data.at("reception_every"),data.at("room_weights").get<std::array<uint32_t,4>>()};
    d.xScale=data.value("x_scale",1.f);
    const auto letter=data.value("letter",std::string("O"));
    if(letter.size()!=1||std::string_view("OASI").find(letter.front())==std::string_view::npos||
       !(d.xScale>=.50f&&d.xScale<=1.f))throw std::runtime_error("Building profile");
    d.letter=letter.front();
    // All profiles share the circulation kit and fixed service interfaces.
    if(!d.id||d.id>UINT32_MAX||!std::isfinite(d.origin.x)||!std::isfinite(d.origin.y)||
       !std::isfinite(d.origin.z)||!(d.height>0.f)||d.floors<17||d.floors>60||
       std::abs(d.height/d.floors-100.f/30.f)>.01f||!(d.width>=32.f&&d.width<=36.f)||
       !(d.depth>=14.9f&&d.depth<=16.f)||d.atrium.left<8.5f||d.atrium.right>25.5f||
       std::abs(d.atrium.front)>.001f||std::abs(d.atrium.back-d.depth)>.001f||d.voidStart<0||(d.voidStart==0&&d.voidEnd!=0)||
       d.voidStart>d.voidEnd||d.voidEnd>=d.floors||
       !(d.slab>=.15f&&d.slab<=.32f&&d.plenum>=.3f&&d.plenum<=.6f)||
       !d.receptionEvery||d.receptionEvery>60||!d.mergeEvery||d.mergeEvery>100||
       std::any_of(d.roomWeights.begin(),d.roomWeights.end(),[](uint32_t v){return v>100u;})||
       std::accumulate(d.roomWeights.begin(),d.roomWeights.end(),0u)==0u||
       std::abs(d.atrium.left-(.29f+(d.width-.58f)/3.f))>.005f||
       std::abs(d.atrium.right-(.29f+(d.width-.58f)*2.f/3.f))>.005f||
       d.atrium.right>StairAt(EastStairOffset).left-.12f)
        throw std::runtime_error("Pilot building envelope");
    return d;
}
Plan Generate(const Definition& d){
    Plan plan;
    for(int i=-1;i<d.floors;++i){
        Level l{i,0,false,{}};
        l.rooms=Rooms(d,l,0);
        plan.levels.push_back(std::move(l));
    }
    return plan;
}
namespace {
std::filesystem::path CurrentSave(const std::filesystem::path& legacy){
    auto current=legacy;
    current.replace_extension(".v2.json");
    return current;
}
}

Plan Load(const std::filesystem::path& path,const Definition& d){
    if(path.empty())return Generate(d);
    const auto current=CurrentSave(path);
    const bool legacy=!std::filesystem::exists(current);
    if(legacy&&!std::filesystem::exists(path))return Generate(d);
    const auto j=Read(legacy?path:current);
    // One-time, versioned migration. This regenerates *all levels* from
    // the authored recipe and clears every old perforation and test breach.
    // Save immediately so reopening never repeats the reset.
    if(j.value("placement_recipe_revision",0u)!=PlacementRecipeRevision){
        auto fresh=Generate(d);
        if(!Save(path,d,fresh))
            throw std::runtime_error("Building placement regeneration could not be saved");
        return fresh;
    }
    if(j.at("version")!=(legacy?1:2)||j.at("definition")!=Fingerprint(d,legacy))
        throw std::runtime_error("Building save does not match building definition");
    Plan result;
    result.revision=j.at("revision");result.variant=j.at("variant");result.approved=j.at("approved");
    result.jointRepairs=j.value("joint_repairs",result.jointRepairs);
    if(std::any_of(result.jointRepairs.begin(),result.jointRepairs.end(),[](uint8_t quality){
        return quality>uint8_t(PatchQuality::Detailed);
    }))throw std::runtime_error("Invalid saved building repair");
    const auto cuts=j.value("damage",Json::array());
    if(!cuts.is_array()||(legacy&&cuts.size()>LegacyMaxDamageRecords))throw std::runtime_error("Building damage journal");
    auto readBox=[](const Json& a){
        Box b{{a.at(0),a.at(1),a.at(2)},{a.at(3),a.at(4),a.at(5)}};
        if(!(b.lo.x<b.hi.x&&b.lo.y<b.hi.y&&b.lo.z<b.hi.z)||
           !std::isfinite(b.lo.x)||!std::isfinite(b.lo.y)||!std::isfinite(b.lo.z)||
           !std::isfinite(b.hi.x)||!std::isfinite(b.hi.y)||!std::isfinite(b.hi.z))
            throw std::runtime_error("Building damage bounds");
        return b;
    };
    for(const auto& cut:cuts){
        const uint64_t part=cut.at("part");
        if(part>>32!=d.id)throw std::runtime_error("Building damage owner");
        result.damage.push_back({part,readBox(cut.at("from")),readBox(cut.at("hole"))});
    }
    const auto& floors=j.at("levels");
    if(!result.revision||!floors.is_array()||floors.size()!=size_t(d.floors+1))
        throw std::runtime_error("Invalid building save");
    for(const auto& f:floors){
        const int number=int(result.levels.size())-1;
        Level l{number,f.at("variant"),f.at("approved"),{}};
        int slot=1;
        const float span=(d.width-.58f)/6.f;
        for(const auto& r:f.at("rooms")){
            const Rect bounds=ReadRect(r.at("bounds"));
            const uint64_t id=r.at("id");
            const auto use=r.at("use").get<unsigned>();
            const int width=int(std::round((bounds.right-bounds.left+.035f)/span));
            while(slot<=12&&VoidSlot(d,number,slot))++slot;
            const bool back=slot>6;
            if(id!=RoomId(d,number,slot)||use>unsigned(Use::Penthouse)||width<1||width>2||
               slot+width>(back?13:7)||(width==2&&VoidSlot(d,number,slot+1))||
               std::abs(bounds.left-(.29f+span*((slot-1)%6)))>.001f||
               std::abs(bounds.right-(bounds.left+span*width-.035f))>.001f||
               std::abs(bounds.front-(back?12.05f:.24f))>.001f||
               std::abs(bounds.back-(back?d.depth-.24f:3.f))>.001f)
                throw std::runtime_error("Invalid saved room");
            l.rooms.push_back({id,bounds,r.at("variant"),Use(use),r.at("approved")});
            slot+=width;
        }
        if(slot!=13)throw std::runtime_error("Invalid saved floor");
        result.levels.push_back(std::move(l));
    }
    return result;
}
bool Save(const std::filesystem::path& path,const Definition& d,const Plan& p){
    if(path.empty())return true;
    const auto current=CurrentSave(path);
    Json levels=Json::array();
    for(const auto& l:p.levels){
        Json rooms=Json::array();
        for(const auto& r:l.rooms)
            rooms.push_back({{"id",r.id},{"bounds",{r.bounds.left,r.bounds.front,r.bounds.right,r.bounds.back}},
                {"variant",r.variant},{"use",unsigned(r.use)},{"approved",r.approved}});
        levels.push_back({{"variant",l.variant},{"approved",l.approved},{"rooms",std::move(rooms)}});
    }
    Json cuts=Json::array();
    auto box=[](Box a){return Json::array({a.lo.x,a.lo.y,a.lo.z,a.hi.x,a.hi.y,a.hi.z});};
    for(const auto& cut:p.damage)
        cuts.push_back({{"part",cut.part},{"from",box(cut.original)},{"hole",box(cut.opening)}});
    const Json j{{"damage",std::move(cuts)},{"version",2},
        {"placement_recipe_revision",PlacementRecipeRevision},
        {"definition",Fingerprint(d,false)},{"revision",p.revision},
        {"variant",p.variant},{"approved",p.approved},{"joint_repairs",p.jointRepairs},{"levels",std::move(levels)}};
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if(error)return false;
    auto temp=current;
#ifdef _WIN32
    temp+=L"."+std::to_wstring(GetCurrentProcessId())+L".tmp";
#else
    temp+=".tmp";
#endif
    std::ofstream out(temp,std::ios::binary|std::ios::trunc);
    out<<j.dump(2)<<'\n';out.close();
    bool ok=false;
    if(out){
#ifdef _WIN32
        ok=MoveFileExW(temp.c_str(),current.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        std::filesystem::rename(temp,current,error);ok=!error;
#endif
    }
    if(!ok)std::filesystem::remove(temp,error);
    return ok;
}
bool RecordDamage(std::vector<Damage>& journal,Damage damage){
    auto& hole=damage.opening;
    const auto [lo,hi]=damage.original;
    hole.lo={std::max(hole.lo.x,lo.x),std::max(hole.lo.y,lo.y),std::max(hole.lo.z,lo.z)};
    hole.hi={std::min(hole.hi.x,hi.x),std::min(hole.hi.y,hi.y),std::min(hole.hi.z,hi.z)};
    if(hole.lo.x>=hole.hi.x||hole.lo.y>=hole.hi.y||hole.lo.z>=hole.hi.z)return false;
    const auto contains=[](const Box& outside,const Box& inside){
        return outside.lo.x<=inside.lo.x&&outside.lo.y<=inside.lo.y&&outside.lo.z<=inside.lo.z&&
            outside.hi.x>=inside.hi.x&&outside.hi.y>=inside.hi.y&&outside.hi.z>=inside.hi.z;
    };
    const auto same=[&](const Damage& record){
        return record.part==damage.part&&record.original==damage.original;
    };
    if(std::any_of(journal.begin(),journal.end(),[&](const Damage& record){
        return same(record)&&contains(record.opening,hole);
    }))return false;
    std::erase_if(journal,[&](const Damage& record){
        return same(record)&&contains(hole,record.opening);
    });
    journal.push_back(damage);
    return true;
}

bool Apply(const Definition& d,Plan& plan,const Command& c,std::string& status){
    const auto t=c.target;
    const auto* level=plan.At(t.level);
    if(c.revision!=plan.revision||!t.inside||(c.scope!=Scope::Building&&!level)){
        status="CIBLE PERIMEE";return false;
    }
    auto* floor=level?&plan.levels[size_t(t.level+1)]:nullptr;
    Room* room=nullptr;
    if(c.scope==Scope::Room){
        for(auto& item:floor->rooms)if(item.id==t.room){room=&item;break;}
        if(!room){status="HORS SALLE";return false;}
    }
    if((c.scope<Scope::Building&&plan.approved)||(room&&floor->approved)){
        status="VERROU PARENT";return false;
    }
    auto& approved=room?room->approved:c.scope==Scope::Floor?floor->approved:plan.approved;
    if(c.action!=Action::Regenerate){
        const bool value=c.action==Action::Approve;
        if(approved==value){status=value?"DEJA CANNE":"DEJA LIBRE";return false;}
        approved=value;
        status=value?"CANNE":"DECANNE";
    }else{
        if(approved){status="PORTEE CANNEE";return false;}
        const auto step=uint32_t(std::clamp(c.step,1,99));
        if(room){
            room->variant+=step;
            const int slot=int(room->id&0xffffu);
            room->use=Program(d,floor->number,slot,room->variant,
                room->bounds.right-room->bounds.left>(d.width-.58f)/4.f);
        }else if(c.scope==Scope::Floor){
            floor->variant+=step;
            floor->rooms=Rooms(d,*floor,plan.variant);
        }else{
            plan.variant+=step;
            for(auto& item:plan.levels)if(!item.approved)item.rooms=Rooms(d,item,plan.variant);
        }
        status="REGENERE";
    }
    ++plan.revision;
    return true;
}
const char* Label(Use use){
    switch(use){
    case Use::Office:return "BUREAU";
    case Use::Meeting:return "REUNION";
    case Use::Reception:return "RECEPTION";
    case Use::Research:return "LABORATOIRE";
    case Use::Service:return "SERVICE";
    case Use::Machine:return "MACHINES";
    case Use::Penthouse:return "PENTHOUSE";
    }
    return "";
}
Rect StairAt(float x){
    auto r=StairOpening;r.left+=x;r.right+=x;return r;
}
bool Serves(unsigned lift,int level,int floors){
    return lift==0?level>=-1&&level<=15:level>=0&&level<floors&&(level==0||level>=15);
}
}
