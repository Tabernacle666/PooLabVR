#include "BuildingSession.h"
#include "building/BuildingMaterials.h"
#include "SphereLod.h"
#include "SolarSystem.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace OASIS {
namespace {
using namespace Buildings;
constexpr float EyeHeight=1.52f,Radius=.25f;
bool In(float x,float z,Rect r){return x>=r.left&&x<r.right&&z>=r.front&&z<r.back;}
bool LiftLanding(const Definition& definition,unsigned bank,int level){
    if(!Serves(bank,level,definition.floors))return false;
    const auto r=Shafts[bank];
    const auto bands=GlyphBands(definition,level);
    return std::any_of(bands.begin(),bands.end(),[&](Rect band){
        return r.left>=band.left&&r.right<=band.right;
    });
}
bool HorizontalOverlap(Point p,const Box& b,float xScale){
    const auto x=p.x-std::clamp(p.x,b.lo.x,b.hi.x);
    const auto z=p.z-std::clamp(p.z,b.lo.z,b.hi.z);
    return (x*xScale)*(x*xScale)+z*z<Radius*Radius;
}
bool Blocks(Point feet,float height,const Part& part,float xScale){
    return part.solid&&feet.y+.003f<part.bounds.hi.y&&feet.y+height>part.bounds.lo.y+.002f&&
        HorizontalOverlap(feet,part.bounds,xScale);
}
}
BuildingSession::BuildingSession(const std::filesystem::path& profile,std::filesystem::path save)
    :definition_(ReadDefinition(profile)),savePath_(std::move(save)),
     plan_(Load(savePath_,definition_)),sectors_(Build(definition_,plan_)){}
XMFLOAT3 BuildingSession::Scene(Point p)const{
    const auto o=definition_.origin;
    return Add({o.x,o.y,o.z},Scale({p.x*definition_.xScale,p.y,p.z},SpectacleInteriorUnitsPerMeter));
}
Buildings::Point BuildingSession::Local(XMFLOAT3 p)const{
    const auto o=definition_.origin;
    const auto value=Scale(Sub(p,{o.x,o.y,o.z}),1.f/SpectacleInteriorUnitsPerMeter);
    return {value.x/definition_.xScale,value.y,value.z};
}
WorldObject BuildingSession::Visual(const Part& part,int level)const{
    const auto [a,b]=part.bounds;
    WorldObject o{};
    o.current.p=Scene({(a.x+b.x)*.5f,(a.y+b.y)*.5f,(a.z+b.z)*.5f});
    o.previous=o.current;
    o.scale=Scale({(b.x-a.x)*definition_.xScale,b.y-a.y,b.z-a.z},SpectacleInteriorUnitsPerMeter);
    o.flags=BuildingObject;
    o.mesh=mesh_;o.material=materials_[size_t(part.material)];o.boundRadius=.87f;
    if(!part.solid)o.flags|=NoCollision;
    if(part.material==Material::Glass){
        o.flags|=AlphaBlendObject|NoShadow;
        o.mesh=paneMesh_;
        if(b.x-a.x<b.z-a.z){
            std::swap(o.scale.x,o.scale.z);
            o.current.q=o.previous.q={0.f,.70710678f,0.f,.70710678f};
        }
    }
    o.tileLayer=int32_t(definition_.id%16384u)*64+level+2;
    return o;
}
void BuildingSession::Install(SceneData& scene,WorldRegistry& world,uint32_t carpetNormal){
    resident_.clear();
    mesh_=AppendBuildingBox(scene);
    paneMesh_=AppendBuildingBox(scene,true);
    for(size_t i=0;i<materials_.size();++i){
        const auto& spec=Describe(Material(i));
        AssetMaterial m{};
        m.colorRoughness={spec.color[0],spec.color[1],spec.color[2],spec.roughness};
        m.emissionMetallic={spec.emission[0],spec.emission[1],spec.emission[2],0.f};
        m.macroVariation=spec.textureVariation;
        const auto material=Material(i);
        if(material==Material::BlueCarpet||material==Material::GreyCarpet||material==Material::VioletCarpet){
            const bool deep=material==Material::VioletCarpet;
            m.normal=carpetNormal;
            m.normalScale=deep?.88f:.34f;
            m.mapping[1].offsetScale={0.f,0.f,deep?10.f:20.f,deep?8.f:16.f};
        }
        if(material==Material::Glass){
            m.alphaMode=2;m.alpha=.32f;m.doubleSided=1;m.unlit=5u;
        }
        if((material>=Material::Ceiling&&material<=Material::Ceramic)||material==Material::Wall||material==Material::Wood)
            m.unlit=7u;
        if(material==Material::Lamp||material==Material::LampAmber||material==Material::LampRed||
           material==Material::LampOff)m.unlit=6u;
        materials_[i]=uint32_t(scene.materials.size());
        scene.materials.push_back(m);scene.materialNames.emplace_back(spec.name);
    }
    Publish(world,sectors_);
    PublishMoving(world);
}
void BuildingSession::AppendLights(std::vector<SceneLight>& lights)const{
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    for(const auto& sector:sectors_)for(size_t i=0;i<sector.lights.size();++i){
        const auto& source=sector.lights[i];
        if(!resident_.contains(source.host))continue;
        const auto [lo,hi]=source.room;
        SceneLight light{};
        light.position=Scene(source.position);
        light.direction=Scale({(hi.x-lo.x)*definition_.xScale*.5f,source.position.y-lo.y,
            (hi.z-lo.z)*.5f},units);
        light.outerCos=(hi.y-source.position.y)*units;
        light.radius=Length(light.direction)*1.40f;
        switch(source.lens){
        case Material::LampRed:light.color={1.f,.045f,.095f};light.power=.026f;break;
        case Material::LampAmber:light.color={1.f,.73f,.46f};light.power=.023f;break;
        default:light.color={1.f,.94f,.84f};light.power=.021f;break;
        }
        light.flags=BuildingLightFlag;
        light.id=0x71000000u+((uint32_t(definition_.id)&0xfffu)<<12u)+
            uint32_t(sector.level+1)*64u+uint32_t(i);
        lights.push_back(light);
    }
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
        if((entry.first&0xff000000u)!=0u||keep.contains(entry.first))return false;
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
const Buildings::Joint* BuildingSession::Inspect(const Pose& aim)const{
    const auto origin=Local(aim.p);
    const auto forward=Sub(Transform({0,0,1},aim),aim.p);
    const std::array p{origin.x,origin.y,origin.z};
    const std::array direction{forward.x,forward.y,forward.z};
    float nearest=4.f;
    const Joint* result=nullptr;
    for(const auto& sector:sectors_){
        if(definition_.Elevation(sector.level)>origin.y+4.f||
           definition_.Elevation(sector.level+1)<origin.y-4.f)continue;
        for(const auto& joint:sector.joints){
            const auto& b=joint.seam;
            const std::array lower{b.lo.x-.09f,b.lo.y,b.lo.z-.09f},upper{b.hi.x+.09f,b.hi.y,b.hi.z+.09f};
            float entry=0.f,exit=nearest;
            for(size_t axis=0;axis<3&&exit>=entry;++axis){
                if(std::abs(direction[axis])<1.e-6f){
                    if(p[axis]<lower[axis]||p[axis]>upper[axis])exit=-1.f;
                }else{
                    const auto a=(lower[axis]-p[axis])/direction[axis];
                    const auto z=(upper[axis]-p[axis])/direction[axis];
                    entry=std::max(entry,std::min(a,z));exit=std::min(exit,std::max(a,z));
                }
            }
            if(exit>=entry&&entry<nearest){nearest=entry;result=&joint;}
        }
    }
    return result;
}
size_t BuildingSession::JointCount(JointKind kind)const{
    size_t count=0;
    for(const auto& sector:sectors_)
        for(const auto& joint:sector.joints)
            if(joint.kind==kind&&!joint.approved)++count;
    return count;
}
void BuildingSession::Repair(uint64_t id,JointKind kind,PatchQuality quality,uint64_t revision){
    if(Busy()||unsaved_)return;
    if(revision!=plan_.revision){status_="CIBLE PERIMEE";return;}
    const auto index=size_t(kind);
    if(index>=plan_.jointRepairs.size()||plan_.jointRepairs[index]){
        status_="JOINT CANNE";return;
    }
    bool found=false;
    for(const auto& sector:sectors_)
        for(const auto& joint:sector.joints)
            found|=joint.id==id&&joint.kind==kind&&!joint.approved;
    if(!found){status_="CIBLE PERIMEE";return;}
    status_="PREPARATION";
    job_=std::async(std::launch::async,[d=definition_,p=plan_,index,quality]() mutable{
        p.jointRepairs[index]=uint8_t(quality);
        ++p.revision;
        auto sectors=Build(d,p);
        return Candidate{std::move(p),std::move(sectors),"JOINTS CANNES",true};
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
    if(!candidate_){StartBreach();return;}
    auto& next=*candidate_;
    if(!next.changed){status_=next.status;candidate_.reset();return;}
    auto feet=Local(head);feet.y-=EyeHeight;
    for(const auto& sector:next.sectors){
        if(next.onlyRemoves)break;
        if(std::abs(definition_.Elevation(sector.level)-feet.y)>4.f)continue;
        for(const auto& part:sector.parts){
            const auto old=resident_.find(part.id);
            if((old==resident_.end()||old->second.part!=part)&&Blocks(feet,EyeHeight+.18f,part,definition_.xScale)){
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
    uint64_t id=(definition_.id<<32)|0xff000000u;
    for(unsigned bank=0;bank<lifts_.size();++bank){
        const auto& lift=lifts_[bank];
        const auto r=Shafts[bank];
        const float y=lift.height,mid=(r.left+r.right)*.5f;
        auto add=[&](Box box,Material material){movingParts_.push_back({++id,box,material,true});};
        add({{r.left+.135f,y-.16f,r.front+.04f},{r.right-.135f,y,r.back-.135f}},Material::Floor);
        add({{r.left+.14f,y,r.back-.25f},{r.right-.14f,y+2.34f,r.back-.14f}},Material::Metal);
        add({{r.left+.14f,y+2.34f,r.front+.10f},{r.right-.14f,y+2.43f,r.back-.14f}},Material::Lamp);
        for(int level=-1;level<definition_.floors;++level){
            if(!LiftLanding(definition_,bank,level))continue;
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
           lift.phase||!LiftLanding(definition_,i,level))continue;
        if(In(p.x,p.z,r)&&p.z>=r.front+.36f&&lift.stop==level&&lift.door>.8f){
            for(int n=level+direction;n>=-1&&n<definition_.floors;n+=direction)
                if(LiftLanding(definition_,i,n)){
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
        for(const auto& part:sector.parts)if(Blocks(feet,height,part,definition_.xScale))return false;
    }
    for(const auto& part:movingParts_)if(Blocks(feet,height,part,definition_.xScale))return false;
    return true;
}
float BuildingSession::Support(Point p,float bottom,float top)const{
    float best=-std::numeric_limits<float>::infinity();
    // Only query the island near grade. The triangle extents include the shore,
    // the parvis and the gaps between lots, even beyond the nearest letter.
    if(bottom<=.40f&&top>=-.10f){
        const auto position=Scene(p);
        for(const auto& t:ground_){
            const auto ab=Sub(t[1],t[0]),ac=Sub(t[2],t[0]),ap=Sub(position,t[0]);
            const float area=ab.x*ac.z-ab.z*ac.x;
            const float u=(ap.x*ac.z-ap.z*ac.x)/area;
            const float v=(ab.x*ap.z-ab.z*ap.x)/area;
            if(u<-.00001f||v<-.00001f||u+v>1.00001f)continue;
            const float worldY=t[0].y+u*ab.y+v*ac.y;
            const float localY=(worldY-definition_.origin.y)/SpectacleInteriorUnitsPerMeter;
            if(localY>=bottom&&localY<=top)best=std::max(best,localY);
        }
    }
    auto check=[&](const Part& part){
        const float y=part.bounds.hi.y;
        if(part.solid&&y>=bottom&&y<=top&&y>best&&HorizontalOverlap(p,part.bounds,definition_.xScale))best=y;
    };
    for(const auto& sector:sectors_){
        if(top<definition_.Elevation(sector.level)-.64f||bottom>definition_.Elevation(sector.level+1)+.7f)continue;
        for(const auto& part:sector.parts)check(part);
    }
    for(const auto& part:movingParts_)check(part);
    return best;
}
void BuildingSession::ContinueWalk(const BuildingSession* previous){
    grounded_=previous&&previous->grounded_;
    fallSpeed_=previous?previous->fallSpeed_:0.f;
    riding_=-1;
}
XMFLOAT3 BuildingSession::Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset){
    const auto a=Local(previous),b=Local(proposed);
    if(reset){
        ContinueWalk(nullptr);
        if(!gravity)return proposed;
        const float foot=b.y-EyeHeight;
        const float floor=Support(b,foot-.35f,foot+.23f);
        if(!std::isfinite(floor))return proposed;
        grounded_=true;
        return Scene({b.x,floor+EyeHeight,b.z});
    }
    if(!gravity){fallSpeed_=0;grounded_=false;riding_=-1;return proposed;}
    Point p{a.x,a.y-EyeHeight,a.z};
    if(riding_>=0)p.y=lifts_[size_t(riding_)].height;
    const float dt=float(std::clamp(elapsed,0.,.08));
    const float dx=b.x-a.x,dz=b.z-a.z;
    const int steps=std::clamp(int(std::ceil(std::hypot(dx,dz)/.10f)),1,64);
    for(int n=0;n<steps;++n){
        auto move=[&](float x,float z){
            Point trial{p.x+x,p.y,p.z+z};
            if(grounded_){
                const float level=Support(trial,p.y-.19f,p.y+.23f);
                if(std::isfinite(level)){
                    Point supported{trial.x,level,trial.z};
                    if(Clear(supported,EyeHeight+.13f)){p=supported;return;}
                }
            }
            if(Clear(trial,EyeHeight+.13f))p=trial;
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
    const auto bands=GlyphBands(definition_,floor);
    if(std::none_of(bands.begin(),bands.end(),[&](Rect band){return In(p.x,p.z,band);}))return {};
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
    for(unsigned wing=0;wing<2;++wing)
        if(In(p.x,p.z,BathroomBounds(definition_,wing)))return {floor,BathroomTarget,true};
    for(const auto& r:l->rooms)
        if(In(p.x,p.z,r.bounds)&&std::any_of(bands.begin(),bands.end(),[&](Rect band){
            return In(p.x,p.z,band)&&(std::min(r.bounds.right,band.right)-std::max(r.bounds.left,band.left))*definition_.xScale>=1.65f;
        }))return {floor,r.id,true};
    if(definition_.letter=='O'&&definition_.HasVoid(floor)&&In(p.x,p.z,definition_.atrium)&&!(floor==15&&p.z>=4.02f&&p.z<=6.02f))
        return {floor,UINT64_MAX-3,true};
    return {floor,0,true};
}
const char* BuildingSession::RoomLabel(Target t)const{
    if(!t.inside)return "DEHORS";
    if(t.room==UINT64_MAX)return "ESCALIER";
    if(t.room==UINT64_MAX-1||t.room==UINT64_MAX-2)return "ASCENSEUR";
    if(t.room==UINT64_MAX-3)return "VIDE";
    if(t.room==BathroomTarget)return "SALLE DE BAIN";
    if(t.room)for(const auto& r:plan_.At(t.level)->rooms)if(r.id==t.room)return Label(r.use);
    return "CIRCULATION";
}
float BuildingSession::LotDistance(XMFLOAT3 head)const{
    const auto p=Local(head);
    const auto x=(p.x-std::clamp(p.x,0.f,definition_.width))*definition_.xScale;
    const auto z=p.z-std::clamp(p.z,0.f,definition_.depth);
    return x*x+z*z;
}
std::optional<BuildingSession::Hit> BuildingSession::Trace(XMFLOAT3 from,XMFLOAT3 to)const{
    const auto a=Local(from),b=Local(to);
    if(std::max(a.x,b.x)<-1.f||std::min(a.x,b.x)>definition_.width+1.f||
       std::max(a.z,b.z)<-1.f||std::min(a.z,b.z)>definition_.depth+1.f)return {};
    const std::array point{a.x,a.y,a.z},direction{b.x-a.x,b.y-a.y,b.z-a.z};
    std::optional<Hit> result;
    float closest=1.f;
    auto test=[&](const Part& part){
        if(!part.solid&&part.material!=Material::LampOff&&part.material!=Material::Lamp&&
           part.material!=Material::LampAmber&&part.material!=Material::LampRed)return;
        const auto [lo,hi]=part.bounds;
        const std::array lower{lo.x,lo.y,lo.z},upper{hi.x,hi.y,hi.z};
        float enter=0.f,leave=closest;
        for(unsigned i=0;i<3;++i){
            if(std::abs(direction[i])<1.e-6f){
                if(point[i]<lower[i]||point[i]>upper[i])return;
                continue;
            }
            const auto l=(lower[i]-point[i])/direction[i],r=(upper[i]-point[i])/direction[i];
            enter=std::max(enter,std::min(l,r));
            leave=std::min(leave,std::max(l,r));
            if(enter>leave)return;
        }
        if(enter<closest){closest=enter;result=Hit{enter,part};}
    };
    for(const auto& sector:sectors_){
        if(std::min(a.y,b.y)>definition_.Elevation(sector.level+1)+.7f||
           std::max(a.y,b.y)<definition_.Elevation(sector.level)-.63f)continue;
        for(const auto& part:sector.parts)test(part);
    }
    for(const auto& part:movingParts_)test(part);
    return result;
}
void BuildingSession::Breach(const Hit& hit,XMFLOAT3 position){
    const auto& part=hit.part;
    if(plan_.damage.size()+breaches_.size()>=MaxDamageRecords||breaches_.size()>=32||
       (part.id&0xff000000u)!=0u)return;
    const auto source=part.source?part.source:part.id;
    const auto original=part.source?part.original:part.bounds;
    if(std::count_if(plan_.damage.begin(),plan_.damage.end(),[&](const Damage& d){
        return d.part==source&&d.original==original;
    })>=6)return;
    Box hole=original;
    if(part.material!=Material::Glass){
        const auto c=Local(position);
        const std::array spans{(original.hi.x-original.lo.x)*definition_.xScale,
            original.hi.y-original.lo.y,original.hi.z-original.lo.z};
        const auto axis=unsigned(std::min_element(spans.begin(),spans.end())-spans.begin());
        constexpr float half=.81f;
        if(axis!=0){hole.lo.x=c.x-half/definition_.xScale;hole.hi.x=c.x+half/definition_.xScale;}
        if(axis!=1){hole.lo.y=c.y-1.35f;hole.hi.y=c.y+.85f;}
        if(axis!=2){hole.lo.z=c.z-half;hole.hi.z=c.z+half;}
    }
    breaches_.push_back({source,original,hole});
    StartBreach();
}
void BuildingSession::StartBreach(){
    if(breaches_.empty()||Busy())return;
    auto cuts=std::exchange(breaches_,{});
    job_=std::async(std::launch::async,[d=definition_,p=plan_,cuts=std::move(cuts)]()mutable{
        const auto before=p.damage.size();
        for(const auto& cut:cuts){
            const auto count=std::count_if(p.damage.begin(),p.damage.end(),[&](const Damage& old){
                return old.part==cut.part&&old.original==cut.original;
            });
            const bool duplicate=std::any_of(p.damage.begin(),p.damage.end(),[&](const Damage& old){
                return old.part==cut.part&&old.original==cut.original&&old.opening==cut.opening;
            });
            if(!duplicate&&count<6&&p.damage.size()<MaxDamageRecords)p.damage.push_back(cut);
        }
        if(p.damage.size()==before)return Candidate{std::move(p),{},"BRECHE EXISTANTE",false};
        ++p.revision;
        auto sectors=Build(d,p);
        return Candidate{std::move(p),std::move(sectors),"BRECHE PUBLIEE",true,true};
    });
}
bool BuildingSession::Projectile(const WorldRegistry& world,uint32_t renderId,XMFLOAT3 position){
    const auto point=Local(position);
    for(const auto& sector:sectors_){
        if(point.y<definition_.Elevation(sector.level)-.62f||
           point.y>definition_.Elevation(sector.level+1)+.70f)continue;
        for(const auto& part:sector.parts){
            const auto& b=part.bounds;
            constexpr float tolerance=.025f;
            if(point.x<b.lo.x-tolerance||point.x>b.hi.x+tolerance||
               point.y<b.lo.y-tolerance||point.y>b.hi.y+tolerance||
               point.z<b.lo.z-tolerance||point.z>b.hi.z+tolerance)continue;
            const auto live=resident_.find(part.id);
            if(live==resident_.end()||world.RenderId(live->second.handle)!=renderId)continue;
            Breach(Hit{0.f,part},position);
            return true;
        }
    }
    return false;
}

Pose BuildingSession::Spawn()const{
    return SolarLookAt(Scene({7.75f,EyeHeight,-3.1f}),Scene({7.75f,EyeHeight,3.8f}));
}
}
