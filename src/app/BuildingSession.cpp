#include "BuildingSession.h"
#include "SphereLod.h"
#include "SolarSystem.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace OASIS {
namespace {
using namespace Buildings;
constexpr float EyeHeight=1.52f,Radius=.25f;
constexpr float Ground=-.015f;
bool In(float x,float z,Rect r){return x>=r.left&&x<r.right&&z>=r.front&&z<r.back;}
bool HorizontalOverlap(Point p,const Box& b){
    const auto x=p.x-std::clamp(p.x,b.lo.x,b.hi.x);
    const auto z=p.z-std::clamp(p.z,b.lo.z,b.hi.z);
    return x*x+z*z<Radius*Radius;
}
bool Blocks(Point feet,float height,const Part& part){
    return part.solid&&feet.y+.003f<part.bounds.hi.y&&feet.y+height>part.bounds.lo.y+.002f&&
        HorizontalOverlap(feet,part.bounds);
}
}
BuildingSession::BuildingSession(const std::filesystem::path& profile,std::filesystem::path save)
    :definition_(ReadDefinition(profile)),savePath_(std::move(save)),
     plan_(Load(savePath_,definition_)),sectors_(Build(definition_,plan_)){}
XMFLOAT3 BuildingSession::Scene(Point p)const{
    const auto o=definition_.origin;
    return Add({o.x,o.y,o.z},Scale({p.x,p.y,p.z},SpectacleInteriorUnitsPerMeter));
}
Buildings::Point BuildingSession::Local(XMFLOAT3 p)const{
    const auto o=definition_.origin;
    const auto value=Scale(Sub(p,{o.x,o.y,o.z}),1.f/SpectacleInteriorUnitsPerMeter);
    return {value.x,value.y,value.z};
}
WorldObject BuildingSession::Visual(const Part& part,int level)const{
    const auto [a,b]=part.bounds;
    WorldObject o{};
    o.current.p=Scene({(a.x+b.x)*.5f,(a.y+b.y)*.5f,(a.z+b.z)*.5f});
    o.previous=o.current;
    o.scale=Scale({b.x-a.x,b.y-a.y,b.z-a.z},SpectacleInteriorUnitsPerMeter);
    o.mesh=mesh_;o.material=materials_[size_t(part.material)];o.boundRadius=.87f;
    if(!part.solid)o.flags|=NoCollision;
    if(part.material==Material::Glass)o.flags|=AlphaBlendObject|NoShadow;
    o.tileLayer=level+2;
    return o;
}
void BuildingSession::Install(SceneData& scene,WorldRegistry& world){
    constexpr std::array<XMFLOAT4,9> colors{{
        {.34f,.34f,.32f,.87f},{.13f,.16f,.19f,.47f},{.50f,.49f,.44f,.89f},
        {.28f,.32f,.36f,.79f},{.12f,.19f,.21f,.23f},{.34f,.19f,.11f,.63f},
        {.86f,.72f,.48f,.72f},{.66f,.04f,.025f,.44f},{.36f,.23f,.12f,.64f}}};
    constexpr std::array names{"OASIS beton","OASIS aluminium","OASIS cloison","OASIS sol",
        "OASIS verre","OASIS mobilier","OASIS luminaire","OASIS extincteur","OASIS reseaux"};
    mesh_=AppendBuildingBox(scene);
    for(size_t i=0;i<materials_.size();++i){
        AssetMaterial m{};
        m.colorRoughness=colors[i];
        if(i==size_t(Material::Glass)){m.alphaMode=2;m.alpha=.32f;m.doubleSided=1;}
        if(i==size_t(Material::Lamp))m.emissionMetallic={1.6f,1.12f,.58f,0};
        materials_[i]=uint32_t(scene.materials.size());
        scene.materials.push_back(m);scene.materialNames.emplace_back(names[i]);
    }
    for(int level=-1;level<definition_.floors;++level){
        const float y=definition_.Elevation(level+1)-definition_.slab-definition_.plenum-.18f;
        for(unsigned wing=0;wing<2;++wing){
            const float x=wing?definition_.width-5.3f:5.3f;
            for(unsigned aisle=0;aisle<2;++aisle){
                SceneLight light{};
                light.position=Scene({x,y,aisle?12.37f:3.91f});
                light.radius=10.f*SpectacleInteriorUnitsPerMeter;
                light.color={1.f,.79f,.55f};
                light.power=.72f;
                light.flags=0;
                light.id=0x71000000u+uint32_t(level+1)*4u+wing*2u+aisle;
                scene.lights.push_back(light);
            }
        }
    }
    Publish(world,sectors_);
    PublishMoving(world);
}
void BuildingSession::Publish(WorldRegistry& world,std::span<const Sector> sectors){
    std::unordered_set<uint64_t> keep;
    std::vector<std::pair<Part,int>> added;
    std::vector<WorldObject> visuals;
    for(const auto& sector:sectors)for(const auto& part:sector.parts){
        if(!keep.insert(part.id).second)Fail("Duplicate building component identity");
        const auto i=resident_.find(part.id);
        if(i==resident_.end()){
            visuals.push_back(Visual(part,sector.level));
            added.emplace_back(part,sector.level);
        }else if(i->second.part!=part){
            world.Update(i->second.handle,Visual(part,sector.level));
            i->second.part=part;
        }
    }
    std::vector<uint64_t> removed;
    std::erase_if(resident_,[&](const auto& entry){
        if(entry.first>>32!=definition_.id||keep.contains(entry.first))return false;
        removed.push_back(entry.second.handle);
        return true;
    });
    world.EraseBatch(removed);
    const auto handles=world.CreateBatch(visuals);
    for(size_t i=0;i<handles.size();++i)
        resident_.emplace(added[i].first.id,Resident{added[i].first,handles[i]});
}
void BuildingSession::Request(Command command){
    if(Busy()){status_="TRAVAIL EN COURS";return;}
    if(unsaved_){
        status_="SAUVEGARDE";
        saving_=std::async(std::launch::async,[path=savePath_,d=definition_,p=plan_]{return Save(path,d,p);});
        return;
    }
    status_="PREPARATION";
    job_=std::async(std::launch::async,[d=definition_,p=plan_,command]() mutable{
        Candidate result;
        result.changed=Apply(d,p,command,result.status);
        if(result.changed&&command.action==Action::Regenerate)result.sectors=Build(d,p);
        result.plan=std::move(p);
        return result;
    });
}
void BuildingSession::Tick(WorldRegistry& world,XMFLOAT3 head,float dt){
    AdvanceLifts(world,dt);
    const auto ready=[](const auto& f){return f.valid()&&f.wait_for(std::chrono::seconds(0))==std::future_status::ready;};
    if(ready(saving_)){
        try{unsaved_=!saving_.get();}catch(...){unsaved_=true;}
        status_=unsaved_?"SAUVEGARDE ECHEC":savedStatus_;
    }
    if(ready(job_)){
        try{candidate_=job_.get();}catch(...){status_="GENERATION ECHEC";}
    }
    if(!candidate_)return;
    auto& next=*candidate_;
    if(!next.changed){status_=next.status;candidate_.reset();return;}
    auto feet=Local(head);feet.y-=EyeHeight;
    for(const auto& sector:next.sectors){
        if(std::abs(definition_.Elevation(sector.level)-feet.y)>4.f)continue;
        for(const auto& part:sector.parts){
            const auto old=resident_.find(part.id);
            if((old==resident_.end()||old->second.part!=part)&&Blocks(feet,EyeHeight+.18f,part)){
                status_="ZONE OCCUPEE";
                return;
            }
        }
    }
    if(!next.sectors.empty()){
        Publish(world,next.sectors);
        sectors_=std::move(next.sectors);
    }
    plan_=std::move(next.plan);
    savedStatus_=std::move(next.status);
    candidate_.reset();
    status_="SAUVEGARDE";
    saving_=std::async(std::launch::async,[path=savePath_,d=definition_,p=plan_]{return Save(path,d,p);});
}
void BuildingSession::PublishMoving(WorldRegistry& world){
    movingParts_.clear();
    uint64_t id=(definition_.id+1ull)<<32;
    for(unsigned bank=0;bank<lifts_.size();++bank){
        const auto& lift=lifts_[bank];
        const auto r=Shafts[bank];
        const float y=lift.height,mid=(r.left+r.right)*.5f;
        auto add=[&](Box box,Material material){movingParts_.push_back({++id,box,material,true});};
        add({{r.left+.135f,y-.16f,r.front+.04f},{r.right-.135f,y,r.back-.135f}},Material::Floor);
        add({{r.left+.14f,y,r.back-.25f},{r.right-.14f,y+2.34f,r.back-.14f}},Material::Metal);
        add({{r.left+.14f,y+2.34f,r.front+.10f},{r.right-.14f,y+2.43f,r.back-.14f}},Material::Lamp);
        for(int level=-1;level<definition_.floors;++level){
            if(!Serves(bank,level,definition_.floors))continue;
            const float v=definition_.Elevation(level);
            const float slide=level==lift.stop?.68f*lift.door:0.f;
            add({{mid-.71f-slide,v,r.front+.014f},{mid-slide,v+2.12f,r.front+.095f}},Material::Metal);
            add({{mid+slide,v,r.front+.014f},{mid+.71f+slide,v+2.12f,r.front+.095f}},Material::Metal);
        }
    }
    for(const auto& p:movingParts_){
        auto found=resident_.find(p.id);
        if(found==resident_.end())resident_.emplace(p.id,Resident{p,world.Create(Visual(p,definition_.floors))});
        else if(found->second.part!=p){
            world.Update(found->second.handle,Visual(p,definition_.floors));
            found->second.part=p;
        }
    }
}
void BuildingSession::AdvanceLifts(WorldRegistry& world,float dt){
    bool dirty=false;
    for(auto& lift:lifts_){
        if(!lift.phase)continue;
        dirty=true;
        if(lift.phase==1){
            lift.door=std::max(0.f,lift.door-2.5f*dt);
            if(lift.door==0)lift.phase=2;
        }else if(lift.phase==2){
            const float target=definition_.Elevation(lift.destination);
            lift.height+=std::clamp(target-lift.height,-4.5f*dt,4.5f*dt);
            if(std::abs(target-lift.height)<.001f){
                lift.height=target;
                lift.stop=lift.destination;lift.phase=3;
            }
        }else{
            lift.door=std::min(1.f,lift.door+2.5f*dt);
            if(lift.door==1.f)lift.phase=0;
        }
    }
    if(dirty)PublishMoving(world);
}
void BuildingSession::RequestLift(XMFLOAT3 head,int direction){
    auto p=Local(head);p.y-=EyeHeight;
    const int level=std::clamp(int(std::floor((p.y+.17f)*definition_.floors/definition_.height)),-1,definition_.floors-1);
    for(unsigned i=0;i<lifts_.size();++i){
        const auto r=Shafts[i];
        auto& lift=lifts_[i];
        if(!In(p.x,p.z,{r.left-.32f,r.front-1.30f,r.right+.32f,r.back})||
           lift.phase||!Serves(i,level,definition_.floors))continue;
        if(In(p.x,p.z,r)&&p.z>=r.front+.36f&&lift.stop==level&&lift.door>.8f){
            for(int n=level+direction;n>=-1&&n<definition_.floors;n+=direction)
                if(Serves(i,n,definition_.floors)){
                    lift.destination=n;lift.phase=1;return;
                }
        }else if(lift.stop!=level){
            lift.destination=level;lift.phase=1;return;
        }else{
            lift.phase=3;return;
        }
    }
}
bool BuildingSession::Clear(Point feet,float height)const{
    for(const auto& sector:sectors_){
        const float y=definition_.Elevation(sector.level);
        if(feet.y+height<y-.63f||feet.y>definition_.Elevation(sector.level+1)+.7f)continue;
        for(const auto& part:sector.parts)if(Blocks(feet,height,part))return false;
    }
    for(const auto& part:movingParts_)if(Blocks(feet,height,part))return false;
    return true;
}
float BuildingSession::Support(Point p,float bottom,float top)const{
    const bool overSite=In(p.x,p.z,{0,0,definition_.width,definition_.depth});
    const auto radius=std::hypot(p.x-definition_.width*.5f,p.z-definition_.depth*.5f);
    float best=!overSite&&radius<24.f&&bottom<=Ground&&top>=Ground?Ground:-std::numeric_limits<float>::infinity();
    auto check=[&](const Part& part){
        const float y=part.bounds.hi.y;
        if(part.solid&&y>=bottom&&y<=top&&y>best&&HorizontalOverlap(p,part.bounds))best=y;
    };
    for(const auto& sector:sectors_){
        if(top<definition_.Elevation(sector.level)-.64f||bottom>definition_.Elevation(sector.level+1)+.7f)continue;
        for(const auto& part:sector.parts)check(part);
    }
    for(const auto& part:movingParts_)check(part);
    return best;
}
XMFLOAT3 BuildingSession::Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset){
    const auto a=Local(previous),b=Local(proposed);
    if(reset){fallSpeed_=0;grounded_=false;riding_=-1;return proposed;}
    if(!gravity){fallSpeed_=0;grounded_=false;riding_=-1;return proposed;}
    Point p{a.x,a.y-EyeHeight,a.z};
    if(riding_>=0)p.y=lifts_[size_t(riding_)].height;
    const float dt=float(std::clamp(elapsed,0.,.08));
    const float dx=b.x-a.x,dz=b.z-a.z;
    const int steps=std::clamp(int(std::ceil(std::hypot(dx,dz)/.10f)),1,64);
    for(int n=0;n<steps;++n){
        auto move=[&](float x,float z){
            Point trial{p.x+x,p.y,p.z+z};
            if(Clear(trial,EyeHeight+.13f)){p=trial;return;}
            if(!grounded_)return;
            const float level=Support(trial,p.y+.004f,p.y+.23f);
            if(std::isfinite(level)){
                trial.y=level;
                if(Clear(trial,EyeHeight+.13f))p=trial;
            }
        };
        move(dx/steps,0);move(0,dz/steps);
    }
    fallSpeed_=std::min(12.f,fallSpeed_+9.81f*dt);
    const float reach=std::max(fallSpeed_*dt,grounded_?.19f:.012f);
    const float floor=Support(p,p.y-reach,p.y+.011f);
    grounded_=std::isfinite(floor);
    if(grounded_){p.y=floor;fallSpeed_=0;}
    else p.y-=fallSpeed_*dt;
    riding_=-1;
    for(unsigned i=0;i<lifts_.size();++i)
        if(In(p.x,p.z,Shafts[i])&&std::abs(p.y-lifts_[i].height)<.025f)riding_=int(i);
    if(p.y<-19.f){fallSpeed_=0;return Spawn().p;}
    p.y+=EyeHeight;
    return Scene(p);
}
Buildings::Target BuildingSession::Locate(XMFLOAT3 head){
    auto p=Local(head);
    if(!In(p.x,p.z,{0,0,definition_.width,definition_.depth})||
       p.y<definition_.Elevation(-1)||p.y>=definition_.height+.1f)return {};
    int floor=std::clamp(int(std::floor(p.y*definition_.floors/definition_.height)),-1,definition_.floors-1);
    if(In(p.x,p.z,StairOpening)||In(p.x,p.z,StairAt(EastStairOffset))){
        const auto feet=p.y-EyeHeight;
        if(std::abs(feet-definition_.Elevation(floor))>.21f)floor=lastLevel_;
        return {floor,UINT64_MAX,true};
    }
    for(unsigned i=0;i<Shafts.size();++i)if(In(p.x,p.z,Shafts[i])){
        return {lifts_[i].stop,UINT64_MAX-1-i,true};
    }
    lastLevel_=floor;
    const auto* l=plan_.At(floor);
    for(const auto& r:l->rooms)if(In(p.x,p.z,r.bounds))return {floor,r.id,true};
    if(floor>=definition_.voidStart&&floor<=definition_.voidEnd&&In(p.x,p.z,definition_.atrium)&&!(floor==15&&p.z>=4.02f&&p.z<=6.02f))
        return {floor,UINT64_MAX-3,true};
    return {floor,0,true};
}
const char* BuildingSession::RoomLabel(Target t)const{
    if(!t.inside)return "DEHORS";
    if(t.room==UINT64_MAX)return "ESCALIER";
    if(t.room==UINT64_MAX-1||t.room==UINT64_MAX-2)return "ASCENSEUR";
    if(t.room==UINT64_MAX-3)return "VIDE";
    if(t.room)for(const auto& r:plan_.At(t.level)->rooms)if(r.id==t.room)return Label(r.use);
    return "CIRCULATION";
}
Pose BuildingSession::Spawn()const{
    return SolarLookAt(Scene({7.75f,EyeHeight,-3.1f}),Scene({7.75f,EyeHeight,3.8f}));
}
}
