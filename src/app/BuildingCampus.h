#pragma once
#include "BuildingSession.h"
#include "LaserSweep.h"
#include "Graphics.h"
#include "SolarSystem.h"
#include <memory>

namespace OASIS {
class BuildingCampus {
    std::array<std::unique_ptr<BuildingSession>,5> letters_;
    WorldRegistry* world_{};
    BuildingSession* walking_{};
    std::array<uint64_t,5> revisions_{};
    uint64_t revision_{1};
    std::vector<SceneLight> lights_;
    std::vector<TerrainTriangle> ground_;

    void UpdateLights(){
        lights_.clear();
        for(const auto& letter:letters_)letter->AppendLights(lights_);
    }

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
            revisions_[i]=letters_[i]->PublicationRevision();
        }
    }
    void SetGround(std::vector<TerrainTriangle> ground){
        ground_=std::move(ground);
        for(const auto& letter:letters_)letter->SetGround(ground_);
    }
    void Install(SceneData& scene,WorldRegistry& world,const Mg2Worktop* worktop=nullptr){
        world_=&world;
        TextureData carpet{};
        carpet.width=carpet.height=32;
        carpet.mips=6;
        carpet.normal=true;
        carpet.format=DXGI_FORMAT_R8G8B8A8_UNORM;
        // Keep the fibre highlight stable as a projected texel becomes smaller.
        for(unsigned mip=0;mip<carpet.mips;++mip){
            const unsigned side=carpet.width>>mip;
            carpet.offsets.push_back(carpet.bytes.size());
            for(unsigned y=0;y<side;++y)for(unsigned x=0;x<side;++x){
                const float phase=(float(x)+.5f)*6.2831853f/float(side);
                const float row=(float(y)+.5f)*6.2831853f/float(side);
                const float strength=mip<3?1.f/float(1u<<mip):0.f;
                const auto fiber=Unit({strength*(.33f*std::sin(3.f*phase+row)+.22f*std::sin(5.f*row)),
                    strength*(.38f*std::cos(4.f*row-phase)+.18f*std::sin(7.f*phase)),1.f});
                for(float channel:{fiber.x,fiber.y,fiber.z})
                    carpet.bytes.push_back(std::byte(uint8_t(std::lround((channel*.5f+.5f)*255.f))));
                carpet.bytes.push_back(std::byte{255});
            }
        }
        const auto carpetNormal=uint32_t(scene.textures.size());
        scene.textures.push_back(std::move(carpet));
        for(const auto& letter:letters_)letter->Install(scene,world,carpetNormal,worktop);
        UpdateLights();
    }
    void Tick(WorldRegistry& world,XMFLOAT3 head,float dt){
        bool changed=false;
        for(size_t i=0;i<letters_.size();++i){
            auto& letter=*letters_[i];
            letter.Tick(world,head,dt);
            letter.StreamWorktops(world,head);
            if(revisions_[i]!=letter.PublicationRevision()){
                revisions_[i]=letter.PublicationRevision();changed=true;
            }
        }
        if(changed){
            UpdateLights();
            ++revision_;
        }
    }
    uint64_t Revision()const{return revision_;}
    size_t ResidentRooms()const{
        size_t count=0;
        for(const auto& letter:letters_)count+=letter->ResidentRooms();
        return count;
    }
    size_t ResidentWorktops()const{
        size_t count=0;
        for(const auto& letter:letters_)count+=letter->ResidentWorktops();
        return count;
    }
    size_t ResidentKeyboards()const{
        size_t count=0;
        for(const auto& letter:letters_)count+=letter->ResidentKeyboards();
        return count;
    }
    std::span<const SceneLight> Lights()const{return lights_;}
    BuildingSession& Pilot()const{return *letters_.front();}
    const BuildingSession& Letter(size_t index)const{return *letters_.at(index);}
    const auto& Letters()const{return letters_;}
    BuildingSession& Select(XMFLOAT3 position){return Nearest(position);}
    void RequestLift(XMFLOAT3 p,int direction){Nearest(p).RequestLift(p,direction);}
    XMFLOAT3 Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset){
        auto& next=Nearest(proposed);
        if(world_)next.PrepareRooms(*world_,reset?proposed:previous,proposed);
        if(walking_!=&next){
            next.ContinueWalk(walking_);
            walking_=&next;
        }
        return next.Move(previous,proposed,elapsed,gravity,reset);
    }
    void Projectiles(std::span<const BuildingImpact> hits){
        for(const auto& hit:hits){
            if(hit.geometryRevision!=revision_){
                // Recheck only the part of the segment known to have reached
                // the campus. Geometry beyond the original impact still needs
                // a full world query.
                const auto end=Add(hit.position,Scale(hit.direction,1.e-5f));
                TraceBuilding(hit.origin,end,true);
                continue;
            }
            for(auto& letter:letters_)
                if(letter->LotDistance(hit.position)<.05f&&
                   letter->Projectile(hit.renderId,hit.position))break;
        }
    }
    bool Laser(LaserRay& ray,double solarTime,bool cut){
        const auto body=solarTime>=0.?SolarBodyToCosmos(0,SolarBody(0,solarTime)):XMMatrixIdentity();
        const auto toLocal=XMMatrixInverse(nullptr,body);
        const auto a=TransformPoint(ray.origin,toLocal),b=TransformPoint(ray.end,toLocal);
        if(auto hit=TraceBuilding(a,b,cut)){
            ray.end=TransformPoint(*hit,body);
            return true;
        }
        return false;
    }
private:
    std::optional<XMFLOAT3> TraceBuilding(XMFLOAT3 a,XMFLOAT3 b,bool cut){
        std::optional<BuildingSession::Hit> nearest;
        BuildingSession* owner=nullptr;
        for(auto& letter:letters_){
            auto hit=letter->Trace(a,b);
            if(hit&&(!nearest||hit->fraction<nearest->fraction||
               (hit->fraction==nearest->fraction&&hit->part.id<nearest->part.id))){
                nearest=hit;
                owner=letter.get();
            }
        }
        if(!nearest)return {};
        const auto position=Add(a,Scale(Sub(b,a),nearest->fraction));
        if(cut)owner->Breach(*nearest,position);
        return position;
    }
};
}
