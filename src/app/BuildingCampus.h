#pragma once
#include "BuildingSession.h"
#include "LaserSweep.h"
#include "Graphics.h"
#include "SolarSystem.h"
#include <memory>

namespace OASIS {
class BuildingCampus {
    std::array<std::unique_ptr<BuildingSession>,5> letters_;
    BuildingSession* walking_{};
    std::array<uint64_t,5> revisions_{};
    uint64_t revision_{1};

    BuildingSession& Nearest(XMFLOAT3 p)const{
        BuildingSession* best=letters_.front().get();
        for(const auto& letter:letters_)
            if(letter->LotDistance(p)<best->LotDistance(p))best=letter.get();
        return *best;
    }
public:
    BuildingCampus(const std::filesystem::path& directory,const std::filesystem::path& save){
        constexpr std::array files{"pilot","a","s-west","i","s-east"};
        for(size_t i=0;i<files.size();++i){
            const auto path=save.empty()?save:save/(i==0?"building.json":std::string(files[i])+".json");
            letters_[i]=std::make_unique<BuildingSession>(directory/(std::string(files[i])+".json"),path);
            revisions_[i]=letters_[i]->Revision();
        }
    }
    void Install(SceneData& scene,WorldRegistry& world){
        for(const auto& letter:letters_)letter->Install(scene,world);
    }
    void Tick(WorldRegistry& world,XMFLOAT3 head,float dt){
        for(size_t i=0;i<letters_.size();++i){
            auto& letter=*letters_[i];
            letter.Tick(world,head,dt);
            if(revisions_[i]!=letter.Revision()){revisions_[i]=letter.Revision();++revision_;}
        }
    }
    uint64_t Revision()const{return revision_;}
    BuildingSession& Pilot()const{return *letters_.front();}
    const BuildingSession& Letter(size_t index)const{return *letters_.at(index);}
    const auto& Letters()const{return letters_;}
    BuildingSession& Select(XMFLOAT3 position){return Nearest(position);}
    void RequestLift(XMFLOAT3 p,int direction){Nearest(p).RequestLift(p,direction);}
    XMFLOAT3 Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset){
        auto& next=Nearest(proposed);
        const bool changed=walking_!=&next;
        walking_=&next;
        return next.Move(previous,proposed,elapsed,gravity,reset||changed);
    }
    void Projectiles(const WorldRegistry& world,std::span<const BuildingImpact> hits){
        for(const auto& hit:hits)
            for(auto& letter:letters_)
                if(letter->LotDistance(hit.position)<.05f&&
                   letter->Projectile(world,hit.renderId,hit.position,hit.direction))break;
    }
    // Both hit tests and published collision use the same per-letter solids.
    bool Laser(LaserRay& ray,double solarTime,bool cut){
        const auto body=solarTime>=0.?SolarBodyToCosmos(0,SolarBody(0,solarTime)):XMMatrixIdentity();
        const auto toLocal=XMMatrixInverse(nullptr,body);
        const auto a=TransformPoint(ray.origin,toLocal),b=TransformPoint(ray.end,toLocal);
        std::optional<BuildingSession::Hit> nearest;
        BuildingSession* owner=nullptr;
        for(auto& letter:letters_){
            auto hit=letter->Trace(a,b);
            if(hit&&(!nearest||hit->fraction<nearest->fraction)){nearest=hit;owner=letter.get();}
        }
        if(!nearest)return false;
        const auto position=Add(a,Scale(Sub(b,a),nearest->fraction));
        ray.end=TransformPoint(position,body);
        if(cut)owner->Breach(*nearest,position);
        return true;
    }
};
}
