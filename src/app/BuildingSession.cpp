#include "BuildingSession.h"
#include "Mg2Catalog.h"
#include "building/BuildingMaterials.h"
#include "SphereLod.h"
#include "SolarSystem.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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
std::vector<Damage> DamageOn(const Plan& plan,int level){
    std::vector<Damage> damage;
    for(const auto& record:plan.damage)
        if(DamageLevel(record)==level)damage.push_back(record);
    return damage;
}
void BuildLevels(const Definition& definition,const Plan& plan,int first,int end,
                 std::vector<Sector>& sectors,std::vector<std::shared_ptr<const Sector>>& sources){
    for(int level=first;level<end;++level){
        auto source=std::make_shared<Sector>(BuildSector(definition,plan,level));
        auto sector=*source;
        ApplyDamage(definition,sector,DamageOn(plan,level));
        BuildContactIndex(sector);
        sectors.push_back(std::move(sector));
        sources.push_back(std::move(source));
    }
}
template<typename BoundsTest,typename PartTest>
bool VisitParts(const Sector& sector,BoundsTest&& testBounds,PartTest&& testPart){
    if(sector.contactNodes.empty())return false;
    const auto visit=[&](auto&& self,uint32_t index)->bool{
        const auto& node=sector.contactNodes[index];
        if(!testBounds(node.bounds))return false;
        if(!node.count)return self(self,index+1)||self(self,node.right);
        for(uint32_t i=0;i<node.count;++i)
            if(testPart(sector.parts[sector.contactParts[node.first+i]]))return true;
        return false;
    };
    return visit(visit,0);
}

bool Overlaps(const Box& a,const Box& b){
    return a.lo.x<=b.hi.x&&a.hi.x>=b.lo.x&&a.lo.y<=b.hi.y&&a.hi.y>=b.lo.y&&
        a.lo.z<=b.hi.z&&a.hi.z>=b.lo.z;
}
bool Blocks(Point feet,float height,const Part& part,float xScale){
    return part.solid&&feet.y+.003f<part.bounds.hi.y&&feet.y+height>part.bounds.lo.y+.002f&&
        HorizontalOverlap(feet,part.bounds,xScale);
}
}
BuildingSession::BuildingSession(const std::filesystem::path& profile,std::filesystem::path save)
    :definition_(ReadDefinition(profile)),savePath_(std::move(save)),
     plan_(Load(savePath_,definition_)){
    std::vector<Sector> initial;
    BuildLevels(definition_,plan_,-1,definition_.floors,initial,sources_);
    for(auto& sector:initial)sectors_.push_back(std::make_shared<Sector>(std::move(sector)));
}
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
    if(part.facade)o.flags|=OASIS_INSTANCE_BUILDING_FACADE;
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
void BuildingSession::Install(SceneData& scene,WorldRegistry& world,uint32_t carpetNormal,const Mg2Worktop* worktop){
    resident_.clear();
    partByRenderId_.clear();
    worktop_=worktop;
    worktops_.clear();
    worktopViewer_.reset();
    mesh_=AppendBuildingBox(scene);
    paneMesh_=AppendBuildingBox(scene,true);
    for(size_t i=0;i<materials_.size();++i){
        const auto& spec=Describe(Material(i));
        AssetMaterial m{};
        m.colorRoughness={spec.color[0],spec.color[1],spec.color[2],spec.roughness};
        m.emissionMetallic={spec.emission[0],spec.emission[1],spec.emission[2],spec.metallic};
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
    for(const auto& sector:sectors_)Publish(world,std::span(sector.get(),1));
    PublishMoving(world);
}
std::optional<Mount> BuildingSession::WorktopSupport(const Mount& mount)const{
    if(!worktop_||!mount.detailPlaceholder||!resident_.contains(mount.detailPlaceholder))return {};
    const auto host=resident_.find(mount.host);
    if(host==resident_.end())return {};
    const auto& bounds=host->second.part.bounds;
    auto supported=mount;
    auto& plate=supported.support;
    plate={std::max(plate.left,bounds.lo.x),std::max(plate.front,bounds.lo.z),
           std::min(plate.right,bounds.hi.x),std::min(plate.back,bounds.hi.z)};
    if(plate.left>=plate.right||plate.front>=plate.back)return {};
    return supported;
}
std::optional<Pose> BuildingSession::WorktopSpawn()const{
    if(!worktop_)return {};
    const auto& ground=*sectors_.at(1);
    const auto& rooms=plan_.At(ground.level)->rooms;
    const float floor=definition_.Elevation(ground.level);
    for(const auto& source:ground.mounts){
        const auto mount=WorktopSupport(source);
        if(!mount)continue;
        const auto room=std::find_if(rooms.begin(),rooms.end(),[&](const Room& r){return r.id==mount->room;});
        if(room==rooms.end())continue;
        const auto& b=mount->support;
        if(worktop_->Fit((b.right-b.left)*definition_.xScale,b.back-b.front).parts.empty())continue;
        const float x=(b.left+b.right)*.5f,z=(b.front+b.back)*.5f;
        for(const float offset:{.94f,.73f}){
            Point feet{x,floor,z-offset};
            if(!In(feet.x,feet.z,room->bounds))continue;
            feet.y=Support(feet,floor-.08f,floor+.06f);
            if(!std::isfinite(feet.y)||!Clear(feet,EyeHeight+.13f))continue;
            const auto eye=Scene({feet.x,feet.y+EyeHeight,feet.z});
            const auto target=Scene({x,mount->height+.37f,z+.20f});
            if(!Trace(eye,target))return SolarLookAt(eye,target);
        }
    }
    return {};
}
void BuildingSession::StreamWorktops(WorldRegistry& world,XMFLOAT3 head){
    if(!worktop_)return;
    const auto view=Local(head);
    const auto distanceSquared=[&](Point point){
        const float x=(view.x-point.x)*definition_.xScale,y=view.y-point.y,z=view.z-point.z;
        return x*x+y*y+z*z;
    };
    if(worktopViewer_&&worktopRevision_==geometryRevision_&&distanceSquared(*worktopViewer_)<.16f)
        return;
    worktopViewer_=view;
    const bool geometryChanged=worktopRevision_!=geometryRevision_;
    worktopRevision_=geometryRevision_;
    struct PendingWorktop {WorktopResident item;size_t first,count;};
    std::unordered_set<uint64_t> keep;
    std::vector<PendingWorktop> pending;
    std::vector<WorldObject> added;
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    constexpr float Enter=7.f,Leave=9.f;
    if(LotDistance(head)<Leave*Leave)for(const auto& sector:sectors_){
        for(const auto& source:sector->mounts){
            const auto mount=WorktopSupport(source);
            if(!mount)continue;
            const auto& b=mount->support;
            const Point center{(b.left+b.right)*.5f,mount->height,(b.front+b.back)*.5f};
            const auto old=worktops_.find(mount->host);
            const float limit=old==worktops_.end()?Enter:Leave;
            if(distanceSquared(center)>=limit*limit)continue;
            if(old!=worktops_.end()&&old->second.mount==*mount){
                keep.insert(mount->host);
                continue;
            }
            const auto layout=worktop_->Fit((b.right-b.left)*definition_.xScale,b.back-b.front);
            if(layout.parts.empty())continue;
            pending.push_back({{*mount,sector->level,{},layout.keyboards},added.size(),layout.parts.size()});
            const auto position=Scene(center);
            for(auto part:layout.parts){
                part.current.p=Add(position,Scale(part.current.p,units));
                part.previous=part.current;
                part.scale={units,units,units};
                part.tileLayer=int32_t(definition_.id%16384u)*64+sector->level+2;
                added.push_back(part);
            }
        }
    }
    std::vector<WorldRegistry::Change> changes;
    std::vector<uint64_t> removed;
    auto showPlaceholder=[&](const WorktopResident& item,bool visible){
        const auto part=resident_.find(item.mount.detailPlaceholder);
        if(part==resident_.end())return;
        auto object=Visual(part->second.part,item.level);
        object.enabled=visible;
        changes.push_back({part->second.handle,object});
    };
    for(auto it=worktops_.begin();it!=worktops_.end();){
        const auto& item=it->second;
        if(keep.contains(it->first)){
            if(geometryChanged)showPlaceholder(item,false);
            ++it;
        }else{
            showPlaceholder(item,true);
            removed.insert(removed.end(),item.handles.begin(),item.handles.end());
            it=worktops_.erase(it);
        }
    }
    for(const auto& entry:pending)showPlaceholder(entry.item,false);
    if(changes.empty()&&removed.empty()&&added.empty())return;
    const auto handles=world.ApplyBatch(changes,removed,added);
    for(auto& entry:pending){
        const auto first=handles.begin()+entry.first;
        entry.item.handles.assign(first,first+entry.count);
        const auto key=entry.item.mount.host;
        worktops_.emplace(key,std::move(entry.item));
    }
}
size_t BuildingSession::ResidentKeyboards()const{
    size_t count=0;
    for(const auto& entry:worktops_)count+=entry.second.keyboards;
    return count;
}
void BuildingSession::AppendLights(std::vector<SceneLight>& lights)const{
    constexpr float units=SpectacleInteriorUnitsPerMeter;
    for(const auto& sector:sectors_)for(size_t i=0;i<sector->lights.size();++i){
        const auto& source=sector->lights[i];
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
            uint32_t(sector->level+1)*64u+uint32_t(i);
        lights.push_back(light);
    }
}
void BuildingSession::Publish(WorldRegistry& world,std::span<const Sector> sectors,bool replace){
    std::unordered_set<uint64_t> keep;
    std::vector<uint64_t> removed;
    std::vector<WorldRegistry::Change> changes;
    std::vector<std::pair<Part,int>> added;
    std::vector<WorldObject> visuals;
    for(const auto& sector:sectors)for(const auto& part:sector.parts){
        if(!keep.insert(part.id).second)Fail("Duplicate building component identity");
        const auto i=resident_.find(part.id);
        if(i==resident_.end()){
            visuals.push_back(Visual(part,sector.level));
            added.emplace_back(part,sector.level);
        }else if(i->second.part!=part){
            const auto& old=i->second.part;
            if((part.id&0xf0000000u)==0x80000000u&&
               (old.source!=part.source||old.bounds!=part.bounds))
                Fail("Overlapping building fragment identities");
            changes.push_back({i->second.handle,Visual(part,sector.level)});
            i->second.part=part;
        }
    }
    if(replace)for(const auto& sector:sectors){
        for(const auto& part:sectors_.at(size_t(sector.level+1))->parts){
            if(keep.contains(part.id))continue;
            const auto i=resident_.find(part.id);
            if(i==resident_.end())continue;
            partByRenderId_.erase(world.RenderId(i->second.handle));
            removed.push_back(i->second.handle);
            resident_.erase(i);
        }
    }
    const auto handles=world.ApplyBatch(changes,removed,visuals);
    for(size_t i=0;i<handles.size();++i){
        resident_.emplace(added[i].first.id,Resident{added[i].first,handles[i]});
        partByRenderId_.emplace(world.RenderId(handles[i]),added[i].first.id);
    }
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
        if(result.changed&&command.action==Action::Regenerate){
            const bool all=command.scope==Scope::Building;
            BuildLevels(d,p,all?-1:command.target.level,all?d.floors:command.target.level+1,
                        result.sectors,result.sources);
        }
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
        if(definition_.Elevation(sector->level)>origin.y+4.f||
           definition_.Elevation(sector->level+1)<origin.y-4.f)continue;
        for(const auto& joint:sector->joints){
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
        for(const auto& joint:sector->joints)
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
        for(const auto& joint:sector->joints)
            found|=joint.id==id&&joint.kind==kind&&!joint.approved;
    if(!found){status_="CIBLE PERIMEE";return;}
    status_="PREPARATION";
    job_=std::async(std::launch::async,[d=definition_,p=plan_,index,quality]() mutable{
        p.jointRepairs[index]=uint8_t(quality);
        ++p.revision;
        Candidate result{std::move(p),{},"JOINTS CANNES",true,false,{}};
        BuildLevels(d,result.plan,-1,d.floors,result.sectors,result.sources);
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
        try{candidate_=job_.get();}
        catch(...){
            breaches_.insert(breaches_.begin(),activeBreaches_.begin(),activeBreaches_.end());
            activeBreaches_.clear();
            status_="GENERATION ECHEC";
            return;
        }
    }
    if(!candidate_){StartBreach();return;}
    auto& next=*candidate_;
    if(!next.changed){status_=next.status;activeBreaches_.clear();candidate_.reset();return;}
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
        Publish(world,next.sectors,true);
        for(size_t i=0;i<next.sectors.size();++i){
            auto& sector=next.sectors[i];
            const auto index=size_t(sector.level+1);
            if(!next.sources.empty())sources_.at(index)=std::move(next.sources[i]);
            sectors_.at(index)=std::make_shared<Sector>(std::move(sector));
        }
        ++geometryRevision_;
    }
    plan_=std::move(next.plan);
    savedStatus_=std::move(next.status);
    activeBreaches_.clear();
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
    const auto x=Radius/definition_.xScale;
    const Box query{{feet.x-x,feet.y,feet.z-Radius},{feet.x+x,feet.y+height,feet.z+Radius}};
    for(const auto& sector:sectors_)
        if(VisitParts(*sector,[&](const Box& b){return Overlaps(b,query);},
            [&](const Part& part){return Blocks(feet,height,part,definition_.xScale);}))return false;
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
    const auto x=Radius/definition_.xScale;
    const Box query{{p.x-x,bottom,p.z-Radius},{p.x+x,top,p.z+Radius}};
    for(const auto& sector:sectors_)
        VisitParts(*sector,[&](const Box& b){return Overlaps(b,query);},
            [&](const Part& part){check(part);return false;});
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
    const auto hitBox=[&](const Box& box)->std::optional<float>{
        const auto [lo,hi]=box;
        const std::array lower{lo.x,lo.y,lo.z},upper{hi.x,hi.y,hi.z};
        float enter=0.f,leave=closest;
        for(unsigned i=0;i<3;++i){
            if(std::abs(direction[i])<1.e-6f){
                if(point[i]<lower[i]||point[i]>upper[i])return {};
                continue;
            }
            const auto l=(lower[i]-point[i])/direction[i],r=(upper[i]-point[i])/direction[i];
            enter=std::max(enter,std::min(l,r));
            leave=std::min(leave,std::max(l,r));
            if(enter>leave)return {};
        }
        return enter;
    };
    const auto test=[&](const Part& part){
        if(!part.solid&&part.material!=Material::LampOff&&part.material!=Material::Lamp&&
           part.material!=Material::LampAmber&&part.material!=Material::LampRed)return false;
        if(auto hit=hitBox(part.bounds);hit&&(*hit<closest||
           (*hit==closest&&result&&part.id<result->part.id))){
            closest=*hit;
            result=Hit{*hit,part};
        }
        return false;
    };
    for(const auto& sector:sectors_)
        VisitParts(*sector,[&](const Box& b){return hitBox(b).has_value();},test);
    for(const auto& part:movingParts_)test(part);
    return result;
}
void BuildingSession::Breach(const Hit& hit,XMFLOAT3 position){
    const auto& part=hit.part;
    const auto source=part.source?part.source:part.id;
    if((source&0xff000000u)!=0u)return;
    const auto original=part.source?part.original:part.bounds;
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
    const auto same=[&](const Damage& damage){
        return damage.part==source&&damage.original==original&&damage.opening==hole;
    };
    if(std::any_of(activeBreaches_.begin(),activeBreaches_.end(),same)||
       std::any_of(breaches_.begin(),breaches_.end(),same))return;
    breaches_.push_back({source,original,hole});
}
void BuildingSession::StartBreach(){
    if(breaches_.empty()||Busy())return;
    job_=std::async(std::launch::async,[d=definition_,p=plan_,cuts=breaches_,
            sources=sources_,published=sectors_]()mutable{
        std::map<int,std::vector<uint64_t>> changed;
        for(const auto& cut:cuts)
            if(RecordDamage(p.damage,cut))changed[DamageLevel(cut)].push_back(cut.part);
        if(changed.empty())return Candidate{std::move(p),{},"BRECHE EXISTANTE",false,false,{}};
        std::vector<Sector> delta;
        delta.reserve(changed.size());
        for(const auto& [level,solids]:changed){
            const auto index=size_t(level+1);
            auto& sector=delta.emplace_back(*sources.at(index));
            ApplyDamage(d,sector,DamageOn(p,level),published.at(index).get(),solids);
            BuildContactIndex(sector);
        }
        ++p.revision;
        return Candidate{std::move(p),std::move(delta),"BRECHE PUBLIEE",true,true,{}};
    });
    activeBreaches_.swap(breaches_);
}
bool BuildingSession::Projectile(uint32_t renderId,XMFLOAT3 position){
    const auto identity=partByRenderId_.find(renderId);
    if(identity==partByRenderId_.end())return false;
    const auto& part=resident_.at(identity->second).part;
    const auto& b=part.bounds;
    const auto point=Local(position);
    constexpr float tolerance=.025f;
    if(point.x<b.lo.x-tolerance||point.x>b.hi.x+tolerance||
       point.y<b.lo.y-tolerance||point.y>b.hi.y+tolerance||
       point.z<b.lo.z-tolerance||point.z>b.hi.z+tolerance)return false;
    Breach(Hit{0.f,part},position);
    return true;
}

Pose BuildingSession::Spawn()const{
    return SolarLookAt(Scene({7.75f,EyeHeight,-3.1f}),Scene({7.75f,EyeHeight,3.8f}));
}
}
