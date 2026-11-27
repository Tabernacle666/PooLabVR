#pragma once
#include "BuildingSession.h"
#include "TerrainTerritory.h"
#include "OasisMetric.h"
#include "LaserSweep.h"
#include "Graphics.h"
#include "SolarSystem.h"
#include <memory>
#include <algorithm>
#include <cmath>

namespace OASIS {
class BuildingCampus {
    std::array<std::unique_ptr<BuildingSession>,5> letters_;
    WorldRegistry* world_{};
    BuildingSession* walking_{};
    std::array<uint64_t,5> revisions_{};
    uint64_t revision_{1};
    std::vector<SceneLight> lights_;
    float beaconElapsed_{};
    uint32_t beaconStep_{};
    BuildingGround ground_;
    TerrainTerritory terrain_;
    BuildingSession* laserOwner_{};
    uint64_t laserSource_{};
    XMFLOAT3 laserPosition_{};

    void UpdateLights(){
        lights_.clear();
        for(const auto& letter:letters_)letter->AppendLights(lights_,beaconStep_);
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
    void SetGround(std::span<const TerrainTriangle> ground){
        ground_.Set(ground);
        for(const auto& letter:letters_)letter->SetGround(ground_);
    }
    void Install(SceneData& scene,WorldRegistry& world,const Mg2Worktop* worktop=nullptr,const Mg2ArchitecturalPalette* pbr=nullptr){
        world_=&world;
        terrain_.Install(scene,world);
        ground_.ResetCraters();
        EndLaser();
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
        scene.buildingOcclusionBoxes.clear();
        scene.buildingOcclusionBoxes.reserve(letters_.size());
        for(const auto& letter:letters_){
            const auto& d=letter->Definition();
            const auto appendBox=[&](float left,float right,int first,int end){
                const auto lo=letter->Scene({left,d.Elevation(first),0.f});
                const auto hi=letter->Scene({right,d.Elevation(end),d.depth});
                scene.buildingOcclusionBoxes.push_back({{lo.x,lo.y,lo.z,0.f},{hi.x,hi.y,hi.z,0.f}});
            };
            if(d.letter=='S'){
                // Follow the actual S silhouette; merge consecutive identical
                // structural bands into a single box. No door/window detail.
                int first=-1;
                auto band=Buildings::GlyphBands(d,first)[0];
                for(int level=0;level<d.floors;++level){
                    const auto current=Buildings::GlyphBands(d,level)[0];
                    if(current==band)continue;
                    appendBox(band.left,band.right,first,level);
                    first=level;
                    band=current;
                }
                appendBox(band.left,band.right,first,d.floors);
            }else{
                appendBox(0.f,d.width,-1,d.floors);
            }
            letter->Install(scene,world,carpetNormal,worktop,pbr);
        }
        UpdateLights();
    }
    void Tick(WorldRegistry& world,const Pose& head,float dt,float farMeters){
        terrain_.Update(world,head.p,farMeters);
        bool changed=false;
        const auto forward=Unit(TransformDirection({0.f,0.f,1.f},Matrix(head)));
        for(size_t i=0;i<letters_.size();++i){
            auto& letter=*letters_[i];
            letter.Tick(world,head.p,dt,forward,farMeters);
            letter.StreamWorktops(world,head.p);
            if(revisions_[i]!=letter.PublicationRevision()){
                revisions_[i]=letter.PublicationRevision();changed=true;
            }
        }
        beaconElapsed_+=std::max(0.f,dt);
        bool beaconChanged=false;
        if(beaconElapsed_>=.28f){
            beaconElapsed_=std::fmod(beaconElapsed_,.28f);
            beaconStep_=(beaconStep_+1u)%4u;
            beaconChanged=true;
        }
        if(changed||beaconChanged)UpdateLights();
        // No world revision for light-only animation: avoids recapturing OASIS.
        if(changed)++revision_;
    }
    void AtmosphereContext(FrameInput& input){
        input.forestExterior=false;
        for(const auto& letter:letters_)
            if(letter->Locate(input.head.p).inside)return;
        if(const auto height=ground_.Height(input.head.p)){
            input.forestGroundY=*height;
            input.forestExterior=true;
        }
    }
    uint64_t Revision()const{return revision_;}
    size_t GroundCraterCount()const{return ground_.CraterCount();}
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
            if(hit.renderId==BuildingImpact::GroundCraterHit){
                ground_.AddCrater(hit.position,hit.origin.x);
                continue;
            }
            if(hit.geometryRevision!=revision_){
                // Recheck only the part of the segment known to have reached
                // the campus. Geometry beyond the original impact still needs
                // a full world query.
                const auto end=Add(hit.position,Scale(hit.direction,0.000291262136f));
                auto [owner,nearest]=ClosestPart(hit.origin,end);
                if(nearest)owner->Breach(*nearest,Add(hit.origin,Scale(Sub(end,hit.origin),nearest->fraction)));
                continue;
            }
            for(auto& letter:letters_)
                if(letter->LotDistance(hit.position)<.05f&&
                   letter->Projectile(hit.renderId,hit.position))break;
        }
    }
    void EndLaser(){laserOwner_=nullptr;}
    bool Laser(LaserRay& ray,double solarTime){
        const auto body=solarTime>=0.?SolarBodyToCosmos(0,SolarBody(0,solarTime)):XMMatrixIdentity();
        const auto toLocal=XMMatrixInverse(nullptr,body);
        const auto a=TransformPoint(ray.origin,toLocal),b=TransformPoint(ray.end,toLocal);
        auto [owner,hit]=ClosestPart(a,b);
        if(!hit){EndLaser();return false;}
        const auto position=Add(a,Scale(Sub(b,a),hit->fraction));
        const auto source=hit->part.source?hit->part.source:hit->part.id;
        const bool continuous=owner==laserOwner_&&source==laserSource_;
        if(!continuous||Length(Sub(position,laserPosition_))>=.006f*OasisSceneUnitsPerMeter){
            owner->Saw(*hit,continuous?laserPosition_:position,position);
            laserOwner_=owner;
            laserSource_=source;
            laserPosition_=position;
        }
        ray.end=TransformPoint(position,body);
        return true;
    }
private:
    std::pair<BuildingSession*,std::optional<BuildingSession::Hit>> ClosestPart(XMFLOAT3 a,XMFLOAT3 b){
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
        return {owner,nearest};
    }
};
}
