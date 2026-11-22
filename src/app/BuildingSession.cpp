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
namespace {
constexpr float GroundTileWidth=.125f;
int GroundCell(float coordinate){return int(std::floor(coordinate/GroundTileWidth));}
uint64_t GroundKey(int x,int z){return (uint64_t(uint32_t(x))<<32u)|uint32_t(z);}
}

void BuildingGround::Set(std::span<const TerrainTriangle> triangles){
    tiles_.clear();
    craters_=0;
    for(const auto& tri:triangles){
        const auto& [a,b,c]=tri;
        const int left=GroundCell(std::min({a.x,b.x,c.x})),right=GroundCell(std::max({a.x,b.x,c.x}));
        const int front=GroundCell(std::min({a.z,b.z,c.z})),back=GroundCell(std::max({a.z,b.z,c.z}));
        for(int z=front;z<=back;++z)for(int x=left;x<=right;++x)
            tiles_[GroundKey(x,z)].triangles.push_back(tri);
    }
}

void BuildingGround::ResetCraters(){
    if(!craters_)return;
    for(auto& [_,tile]:tiles_)tile.craters.clear();
    craters_=0;
}

void BuildingGround::AddCrater(XMFLOAT3 center,float radius){
    if(!std::isfinite(center.x)||!std::isfinite(center.y)||!std::isfinite(center.z)||
       !std::isfinite(radius)||radius<=0.f)return;
    const int left=GroundCell(center.x-radius),right=GroundCell(center.x+radius);
    const int front=GroundCell(center.z-radius),back=GroundCell(center.z+radius);
    for(int z=front;z<=back;++z)for(int x=left;x<=right;++x)
        if(auto tile=tiles_.find(GroundKey(x,z));tile!=tiles_.end())
            tile->second.craters.push_back({center,radius});
    ++craters_;
}

std::optional<float> BuildingGround::Height(XMFLOAT3 position)const{
    const auto tile=tiles_.find(GroundKey(GroundCell(position.x),GroundCell(position.z)));
    if(tile==tiles_.end())return {};
    std::optional<float> highest;
    for(const auto& [a,b,c]:tile->second.triangles){
        const auto ab=Sub(b,a),ac=Sub(c,a),ap=Sub(position,a);
        const float area=ab.x*ac.z-ab.z*ac.x;
        if(std::abs(area)<1.e-10f)continue;
        const float u=(ap.x*ac.z-ap.z*ac.x)/area,v=(ab.x*ap.z-ab.z*ap.x)/area;
        if(u<-1.e-5f||v<-1.e-5f||u+v>1.00001f)continue;
        float y=a.y+u*ab.y+v*ac.y;
        // Same axial, diagonal and corner clip planes as Cut.LocalBlocks.
        for(const auto& crater:tile->second.craters){
            const auto d=Sub({position.x,y,position.z},crater.center);
            const float x=std::abs(d.x),h=std::abs(d.y),z=std::abs(d.z);
            const float r=crater.radius;
            if(h>r)continue;
            const float diagonal=r*1.41421356f,corner=r*1.73205081f;
            const float edge=std::max({x/r,z/r,(x+z)/diagonal,
                x/(diagonal-h),z/(diagonal-h),(x+z)/(corner-h)});
            // CraterBottom shrinks the flat floor to 60% of its aperture.
            y-=r*.75f*std::clamp((1.f-edge)/.40f,0.f,1.f);
        }
        if(!highest||y>*highest)highest=y;
    }
    return highest;
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
    if(worktop_&&part.room&&!part.facade){
        // Stable canonical host ID + room ID + surface role. Cosmetic ageing does
        // not rewrite the material after a wall is damaged or a room is revisited.
        float chance=0.f;
        switch(part.material){
        case Material::Floor:chance=worktop_->wearLikelihood[0];break;
        case Material::Wall:chance=worktop_->wearLikelihood[2];break;
        case Material::Wood:chance=worktop_->wearLikelihood[3];break;
        case Material::Metal:chance=worktop_->wearLikelihood[5];break;
        case Material::Moulding:chance=worktop_->wearLikelihood[6];break;
        case Material::Ceramic:chance=worktop_->wearLikelihood[7];break;
        case Material::BlueCarpet:
        case Material::GreyCarpet:
        case Material::VioletCarpet:chance=worktop_->wearLikelihood[1];break;
        default:break;
        }
        uint64_t hash=(part.source?part.source:part.id)^
            (part.room*0x9e3779b97f4a7c15ull)^(uint64_t(part.material)<<48);
        hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ull;
        hash^=hash>>27;hash*=0x94d049bb133111ebull;hash^=hash>>31;
        const double selection=double(uint32_t(hash))/4294967296.0;
        if(selection<double(chance))
            o.material=wearMaterials_[size_t(part.material)][1u+uint32_t((hash>>32)%3u)];
    }
    if(part.room&&!part.facade&&o.material==materials_[size_t(part.material)]){
        const std::vector<uint32_t>* shades=nullptr;
        if(part.material==Material::Wall)shades=&whitePaintVariants_[0];
        else if(part.material==Material::Ceiling)shades=&whitePaintVariants_[1];
        else if(part.material==Material::Ceramic)shades=&whitePaintVariants_[2];
        if(shades&&!shades->empty()){
            uint64_t seed=part.room*0x9e3779b97f4a7c15ull^(part.source?part.source:part.id);
            seed^=seed>>30;seed*=0xbf58476d1ce4e5b9ull;
            seed^=seed>>27;seed*=0x94d049bb133111ebull;seed^=seed>>31;
            o.material=(*shades)[size_t(seed%shades->size())];
        }
    }
    if(!part.solid)o.flags|=NoCollision;
    if(part.material==Material::Glass){
        o.flags|=AlphaBlendObject|NoShadow;
        o.mesh=paneMesh_;
        if(b.x-a.x<b.z-a.z){
            std::swap(o.scale.x,o.scale.z);
            o.current.q=o.previous.q={0.f,.70710678f,0.f,.70710678f};
        }
    }
    if(part.paneRoom){
        const size_t lens=part.paneLens==Material::Lamp?1u:part.paneLens==Material::LampAmber?2u:
            part.paneLens==Material::LampRed?3u:0u;
        o.material=paneMaterials_[residentRooms_.contains(part.paneRoom)][lens];
        if(a.z>definition_.depth*.5f)o.current.q=o.previous.q={0.f,1.f,0.f,0.f};
    }
    o.tileLayer=int32_t(definition_.id%16384u)*64+level+2;
    return o;
}
void BuildingSession::Install(SceneData& scene,WorldRegistry& world,uint32_t carpetNormal,const Mg2Worktop* worktop,const Mg2ArchitecturalPalette* pbr){
    resident_.clear();
    partByRenderId_.clear();
    residentRooms_.clear();
    roomViewer_.reset();
    worktop_=worktop;
    roomDecor_.clear();
    roomDecorViewer_.reset();
    roomDecorRevision_=0;
    worktops_.clear();
    worktopViewer_.reset();
    mesh_=AppendBuildingBox(scene);
    paneMesh_=AppendBuildingBox(scene,true);
    for(size_t i=0;i<materials_.size();++i){
        const auto& spec=Describe(Material(i));
        AssetMaterial m{};
        const auto material=Material(i);
        const uint32_t pbrIndex=pbr?pbr->material[i]:Invalid;
        const bool scanned=pbrIndex!=Invalid&&pbrIndex<scene.materials.size();
        if(scanned){
            m=scene.materials[pbrIndex];
            // The scanned albedo replaces the old dark material tint.
            const float rough=m.colorRoughness.w;
            m.colorRoughness={1.f,1.f,1.f,rough};
            if(material==Material::Wall)m.colorRoughness={.93f,.93f,.91f,rough};
            else if(material==Material::Ceiling)m.colorRoughness={.96f,.95f,.93f,rough};
            else if(material>=Material::Sage&&material<=Material::Cherry){
                const float greatest=std::max({spec.color[0],spec.color[1],spec.color[2],.001f});
                const float brightness=.80f*std::sqrt(greatest/.55f);
                m.colorRoughness={std::clamp(brightness*spec.color[0]/greatest,.04f,1.f),
                    std::clamp(brightness*spec.color[1]/greatest,.04f,1.f),
                    std::clamp(brightness*spec.color[2]/greatest,.04f,1.f),rough};
            }else if(material==Material::BlueCarpet)m.colorRoughness={.42f,.55f,.73f,rough};
            else if(material==Material::GreyCarpet)m.colorRoughness={.67f,.68f,.71f,rough};
            else if(material==Material::VioletCarpet)m.colorRoughness={.65f,.38f,.72f,rough};
            // Building box UVs cover each surface once: repeat the real scanned
            // texels rather than stretch one plank across an entire room.
            const float repeat=(material==Material::Wall||material==Material::Ceiling||
                (material>=Material::Sage&&material<=Material::Cherry))?4.f:
                material==Material::Floor||material==Material::Concrete?6.f:2.f;
            for(auto& mapping:m.mapping){
                mapping.offsetScale.z*=repeat;
                mapping.offsetScale.w*=repeat;
            }
        }else{
            m.colorRoughness={spec.color[0],spec.color[1],spec.color[2],spec.roughness};
            m.emissionMetallic={spec.emission[0],spec.emission[1],spec.emission[2],spec.metallic};
            m.macroVariation=spec.textureVariation;
        }
        if(material==Material::BlueCarpet||material==Material::GreyCarpet||material==Material::VioletCarpet){
            const bool deep=material==Material::VioletCarpet;
            if(!scanned){
                m.normal=carpetNormal;
                m.normalScale=deep?.88f:.34f;
                m.mapping[1].offsetScale={0.f,0.f,deep?10.f:20.f,deep?8.f:16.f};
            }
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
    // A small, bounded set of shared wear states per material. Real PBR maps are
    // reused; no new texture or GPU material is created for each individual room.
    for(size_t i=0;i<wearMaterials_.size();++i){
        wearMaterials_[i][0]=materials_[i];
        const auto role=Material(i);
        const bool eligible=role==Material::Floor||role==Material::Concrete||
            role==Material::Wall||role==Material::Wood||role==Material::Metal||
            role==Material::Moulding||role==Material::Ceramic||
            (role>=Material::BlueCarpet&&role<=Material::VioletCarpet);
        for(uint32_t state=1;state<4;++state){
            wearMaterials_[i][state]=materials_[i];
            if(!eligible)continue;
            auto aged=scene.materials[materials_[i]];
            const float fade=(role==Material::Metal?.018f:.012f)*float(state);
            aged.colorRoughness.x*=1.f-fade;
            aged.colorRoughness.y*=1.f-fade*.94f;
            aged.colorRoughness.z*=1.f-fade*.87f;
            aged.colorRoughness.w=std::min(1.f,aged.colorRoughness.w+.027f*float(state));
            aged.macroVariation+=.009f*float(state);
            wearMaterials_[i][state]=uint32_t(scene.materials.size());
            scene.materials.push_back(aged);
            scene.materialNames.emplace_back(std::string(Describe(role).name)+" aged");
        }
    }
    for(auto& variants:whitePaintVariants_)variants.clear();
    if(worktop_&&!worktop_->whiteShades.empty()){
        const std::array roles{Material::Wall,Material::Ceiling,Material::Ceramic};
        for(size_t category=0;category<roles.size();++category){
            for(const auto& shade:worktop_->whiteShades){
                const bool eligible=category==0u?
                    (shade.family==1u||shade.family>=3u):
                    category==1u?(shade.family>=2u&&shade.family<=4u):
                    (shade.family==0u||shade.family==5u);
                if(!eligible)continue;
                auto painted=scene.materials[materials_[size_t(roles[category])]];
                painted.colorRoughness={shade.linear.x,shade.linear.y,shade.linear.z,
                    std::clamp(.5f*(painted.colorRoughness.w+shade.roughness),.04f,1.f)};
                const auto index=uint32_t(scene.materials.size());
                scene.materials.push_back(painted);
                scene.materialNames.emplace_back(std::string(Describe(roles[category]).name)+" white");
                whitePaintVariants_[category].push_back(index);
            }
        }
    }
    constexpr std::array<XMFLOAT4,4> light{{
        {0.f,0.f,0.f,0.f},{1.f,.94f,.84f,0.f},{1.f,.68f,.36f,0.f},{1.f,.055f,.10f,0.f}
    }};
    for(size_t resident=0;resident<paneMaterials_.size();++resident){
        for(size_t lens=0;lens<light.size();++lens){
            auto m=scene.materials[materials_[size_t(Material::Glass)]];
            m.emissionMetallic=light[lens];
            m.macroVariation=resident?1.f:2.f;
            paneMaterials_[resident][lens]=uint32_t(scene.materials.size());
            scene.materials.push_back(m);
            scene.materialNames.emplace_back("Room window");
        }
    }
    for(const auto& sector:sectors_){
        // Relief Ultra captures see the complete structural model at startup.
        for(const auto& room:sector->rooms)residentRooms_.insert(room.id);
        const auto* initial=sector.get();
        Publish(world,std::span(&initial,1));
    }
    PublishMoving(world);
}
void BuildingSession::PrepareRooms(WorldRegistry& world,XMFLOAT3 from,XMFLOAT3 to){
    const auto a=Local(from),b=Local(to);
    std::array<int,2> destinations{};
    for(size_t i=0;i<lifts_.size();++i)
        destinations[i]=lifts_[i].phase?lifts_[i].destination:-2;
    const bool preparingBreach=!breaches_.empty()||!activeBreaches_.empty();
    if(preparingBreach)roomViewer_.reset();
    else{
        const auto nearby=[&](Point p){
            const auto c=*roomViewer_;
            return std::hypot((p.x-c.x)*definition_.xScale,p.y-c.y,p.z-c.z)<.25f;
        };
        if(roomViewer_&&roomGeometryRevision_==geometryRevision_&&roomDestinations_==destinations&&nearby(a)&&nearby(b))
            return;
        roomViewer_=b;
        roomDestinations_=destinations;
        roomGeometryRevision_=geometryRevision_;
    }
    const Box swept{{std::min(a.x,b.x),std::min(a.y,b.y)-EyeHeight,std::min(a.z,b.z)},
                    {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)}};
    constexpr float enter=8.f,leave=11.f,verticalEnter=3.6f,verticalLeave=5.f;
    auto reached=[&](const Box& room,const Box& approach,float horizontal,float vertical){
        const float x=std::max({0.f,room.lo.x-approach.hi.x,approach.lo.x-room.hi.x})*definition_.xScale;
        const float z=std::max({0.f,room.lo.z-approach.hi.z,approach.lo.z-room.hi.z});
        return room.lo.y<=approach.hi.y+vertical&&room.hi.y>=approach.lo.y-vertical&&
               x*x+z*z<=horizontal*horizontal;
    };
    std::unordered_set<uint64_t> requested;
    std::vector<const Sector*> changed;
    for(const auto& sector:sectors_){
        const float floor=definition_.Elevation(sector->level),top=definition_.Elevation(sector->level+1);
        bool sectorChanged=false;
        for(const auto& room:sector->rooms){
            const bool resident=residentRooms_.contains(room.id);
            const auto& r=room.bounds;
            const Box bounds{{r.left,floor,r.front},{r.right,top,r.back}};
            bool needed=reached(bounds,swept,resident?leave:enter,resident?verticalLeave:verticalEnter);
            if(!needed){
                for(size_t i=0;i<lifts_.size();++i){
                    if(destinations[i]!=sector->level)continue;
                    const auto shaft=Shafts[i];
                    const Box landing{{shaft.left,floor,shaft.front},{shaft.right,top,shaft.back}};
                    needed|=reached(bounds,landing,enter,0.f);
                }
            }
            const auto nearOpening=[&](const Damage& damage){return reached(bounds,damage.opening,2.2f,verticalEnter);};
            if(!needed)needed=std::any_of(breaches_.begin(),breaches_.end(),nearOpening)||
                             std::any_of(activeBreaches_.begin(),activeBreaches_.end(),nearOpening);
            if(needed)requested.insert(room.id);
            sectorChanged|=needed!=resident;
        }
        if(sectorChanged)changed.push_back(sector.get());
    }
    if(changed.empty()&&requested.size()==residentRooms_.size())return;
    residentRooms_.swap(requested);
    Publish(world,changed,true);
    ++publicationRevision_;
}
std::optional<Mount> BuildingSession::WorktopSupport(const Mount& mount)const{
    if(!worktop_||!worktop_->Has(mount.surface)||
       (mount.detailPlaceholder&&!resident_.contains(mount.detailPlaceholder)))return {};
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
        if(source.surface!=MountSurface::OfficeDesktop)continue;
        const auto mount=WorktopSupport(source);
        if(!mount)continue;
        const auto room=std::find_if(rooms.begin(),rooms.end(),[&](const Room& r){return r.id==mount->room;});
        if(room==rooms.end())continue;
        const auto& b=mount->support;
        if(worktop_->Fit(mount->surface,(b.right-b.left)*definition_.xScale,b.back-b.front).parts.empty())continue;
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

void BuildingSession::StreamRoomDecor(WorldRegistry& world,XMFLOAT3 head){
    if(!worktop_)return;
    const auto view=Local(head);
    const auto dist2=[&](Point p){
        const float dx=(p.x-view.x)*definition_.xScale,dy=p.y-view.y,dz=p.z-view.z;
        return dx*dx+dy*dy+dz*dz;
    };
    if(roomDecorViewer_&&roomDecorRevision_==publicationRevision_&&
       dist2(*roomDecorViewer_)<.16f)return;
    roomDecorViewer_=view;
    const bool rebuild=roomDecorRevision_!=publicationRevision_;
    roomDecorRevision_=publicationRevision_;
    constexpr float Enter=7.f,Leave=9.f,units=SpectacleInteriorUnitsPerMeter;
    std::unordered_set<uint64_t> keep;
    struct Pending {uint64_t room;size_t first,count;};
    std::vector<Pending> pending;
    std::vector<WorldObject> added;
    if(LotDistance(head)<Leave*Leave)for(const auto& sector:sectors_){
        const float floor=definition_.Elevation(sector->level);
        if(std::abs(view.y-floor)>Leave+3.f)continue;
        for(const auto& room:sector->rooms){
            if(!residentRooms_.contains(room.id))continue;
            const auto& b=room.bounds;
            const Point center{(b.left+b.right)*.5f,floor+.7f,(b.front+b.back)*.5f};
            const auto existing=roomDecor_.find(room.id);
            const float limit=existing==roomDecor_.end()?Enter:Leave;
            if(dist2(center)>=limit*limit)continue;
            if(existing!=roomDecor_.end()&&!rebuild){keep.insert(room.id);continue;}
            uint64_t hash=room.id^(uint64_t(room.variant)<<32)^definition_.seed;
            hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ull;
            hash^=hash>>27;hash*=0x94d049bb133111ebull;hash^=hash>>31;
            size_t profile=size_t(hash%15ull);
            if(room.use==Buildings::Use::Meeting)profile=7u;
            else if(room.use==Buildings::Use::Reception)profile=15u;
            else if(room.use==Buildings::Use::Penthouse)profile=2u;
            else if(room.use==Buildings::Use::Machine)profile=14u;
            else if(room.use==Buildings::Use::Service)profile=4u;
            else if(room.use==Buildings::Use::Research)profile=(hash&1ull)?8u:13u;
            const float roomWidth=(b.right-b.left)*definition_.xScale,roomDepth=b.back-b.front;
            const size_t start=added.size();
            std::vector<Buildings::Rect> occupied;
            // Respect resident desk footprints: decorations do not occupy
            // workstations or the player's clear walking path.
            for(const auto& mount:sector->mounts)if(mount.room==room.id){
                const auto& box=mount.support;
                occupied.push_back({(box.left-center.x)*definition_.xScale,box.front-center.z,
                                    (box.right-center.x)*definition_.xScale,box.back-center.z});
            }
            const auto& catalog=worktop_->roomProfiles[profile];
            constexpr size_t ResidentFurnitureMax=8u; // candidates remain unlimited in source packs
            size_t installed=0;
            for(size_t k=0;k<catalog.size()&&installed<ResidentFurnitureMax;++k){
                const size_t slot=(k+size_t((hash>>32)%catalog.size()))%catalog.size();
                const auto& prop=catalog[slot];
                uint64_t dice=hash^(uint64_t(slot+1)*0x9e3779b97f4a7c15ull);
                dice^=dice>>30;dice*=0xbf58476d1ce4e5b9ull;
                dice^=dice>>27;dice*=0x94d049bb133111ebull;dice^=dice>>31;
                if(double(uint32_t(dice))/4294967296.0>double(prop.chance))continue;
                const float margin=.18f;
                const float radiusX=std::max(std::abs(prop.lower.x),std::abs(prop.upper.x));
                const float radiusZ=std::max(std::abs(prop.lower.z),std::abs(prop.upper.z));
                const float travelX=roomWidth*.5f-radiusX-margin;
                const float travelZ=roomDepth*.5f-radiusZ-margin;
                if(travelX<=0.f||travelZ<=0.f)continue;
                const float x=prop.nx*travelX,z=prop.nz*travelZ;
                const Buildings::Rect footprint{x+prop.lower.x,z+prop.lower.z,
                                                 x+prop.upper.x,z+prop.upper.z};
                if(std::any_of(occupied.begin(),occupied.end(),[&](const auto& other){
                    constexpr float gap=.12f;
                    return footprint.left<other.right+gap&&footprint.right>other.left-gap&&
                           footprint.front<other.back+gap&&footprint.back>other.front-gap;
                }))continue;
                occupied.push_back(footprint);
                ++installed;
                const float localX=center.x+x/definition_.xScale,localZ=center.z+z;
                const auto position=Scene({localX,floor-prop.lower.y+.006f,localZ});
                const float halfYaw=prop.yawDeg*(3.14159265359f/360.f);
                for(auto part:prop.parts){
                    part.current.p=position;
                    part.current.q={0.f,std::sin(halfYaw),0.f,std::cos(halfYaw)};
                    part.previous=part.current;
                    part.scale={units,units,units};
                    part.tileLayer=int32_t(definition_.id%16384u)*64+sector->level+2;
                    added.push_back(part);
                }
            }
            // Attach original OASIS wear geometry only to nearby eligible
            // room hosts; no decal is ever resident outside the room budget.
            auto wear=[&](size_t type,float likelihood,Point anchor,uint64_t salt){
                if(worktop_->wearOverlays[type].empty()||likelihood<=0.f)return;
                uint64_t draw=hash^salt;
                draw^=draw>>30;draw*=0xbf58476d1ce4e5b9ull;
                draw^=draw>>27;draw*=0x94d049bb133111ebull;draw^=draw>>31;
                if(double(uint32_t(draw))/4294967296.0>=double(likelihood))return;
                const auto position=Scene(anchor);
                for(auto part:worktop_->wearOverlays[type]){
                    part.current.p=position;
                    part.current.q={0.f,0.f,0.f,1.f};
                    part.previous=part.current;
                    part.scale={units,units,units};
                    part.tileLayer=int32_t(definition_.id%16384u)*64+sector->level+2;
                    added.push_back(part);
                }
            };
            if(room.use==Buildings::Use::Office||room.use==Buildings::Use::Research)
                wear(0u,worktop_->wearLikelihood[2],
                     {center.x,floor+.82f,b.back-.014f},0x1b4b5e7d28ff0a11ull);
            const bool carpet=std::any_of(sector->parts.begin(),sector->parts.end(),
                [&](const Buildings::Part& part){
                    return part.room==room.id&&
                        (part.material==Material::BlueCarpet||
                         part.material==Material::GreyCarpet||
                         part.material==Material::VioletCarpet);
                });
            const size_t floorOverlay=carpet?2u:1u;
            const float chance=carpet?worktop_->wearLikelihood[1]:worktop_->wearLikelihood[0];
            wear(floorOverlay,chance,{center.x,floor+.008f,center.z},0x3a61edeb7d922733ull);
            // Wall-mounted art is selected from the existing 100 original
            // poster designs. Find an actual opaque resident wall rather than
            // placing a rectangle through windows or in open space.
            if(room.use!=Buildings::Use::Machine&&room.use!=Buildings::Use::Service&&
               size_t((hash>>15)&7ull)!=0u){
                const Buildings::Part* host=nullptr;
                bool alongX=false;
                float span=0.f;
                for(const auto& part:sector->parts){
                    if(part.room!=room.id||part.facade)continue;
                    const auto mat=part.material;
                    if(mat!=Material::Wall&&
                       !(mat>=Material::Sage&&mat<=Material::Cherry))continue;
                    const auto& box=part.bounds;
                    if(box.hi.y-box.lo.y<1.4f)continue;
                    const float sizeX=(box.hi.x-box.lo.x)*definition_.xScale;
                    const float sizeZ=box.hi.z-box.lo.z;
                    const bool faceX=sizeX<.28f&&sizeZ>1.25f;
                    const bool faceZ=sizeZ<.28f&&sizeX>1.25f;
                    if(!faceX&&!faceZ)continue;
                    const float length=faceX?sizeZ:sizeX;
                    if(length<=span)continue;
                    span=length;host=&part;alongX=faceX;
                }
                if(host){
                    const auto& box=host->bounds;
                    Point p{(box.lo.x+box.hi.x)*.5f,
                            std::clamp(floor+1.62f,box.lo.y+.55f,box.hi.y-.55f),
                            (box.lo.z+box.hi.z)*.5f};
                    const bool inward=alongX?center.x>=p.x:center.z>=p.z;
                    const float yaw=alongX?(inward?1.570796327f:-1.570796327f):
                                            (inward?0.f:3.141592654f);
                    if(alongX)p.x+=(inward?.05f:-.05f)/definition_.xScale;
                    else p.z+=inward?.05f:-.05f;
                    const unsigned genre=room.use==Buildings::Use::Reception?2u:
                        room.use==Buildings::Use::Research?4u:
                        room.use==Buildings::Use::Penthouse?0u:
                        unsigned((hash>>23)%5ull);
                    const unsigned count=(room.use==Buildings::Use::Reception||
                        room.use==Buildings::Use::Meeting)&&span>3.2f?2u:1u;
                    for(unsigned j=0;j<count;++j){
                        const unsigned id=5u*unsigned(((hash>>32)+j*7u+
                            uint64_t(sector->level+2)*3u)%20ull)+genre;
                        const auto& poster=worktop_->posterParts[id];
                        if(poster.size()!=2u)continue;
                        Point at=p;
                        if(count==2u){
                            const float side=j==0?-.55f:.55f;
                            if(alongX)at.z+=side;
                            else at.x+=side/definition_.xScale;
                        }
                        const auto position=Scene(at);
                        for(auto part:poster){
                            part.current.p=position;
                            part.current.q={0.f,std::sin(yaw*.5f),0.f,std::cos(yaw*.5f)};
                            part.previous=part.current;
                            part.scale={units,units,units};
                            part.tileLayer=int32_t(definition_.id%16384u)*64+sector->level+2;
                            added.push_back(part);
                        }
                    }
                }
            }
            if(added.size()!=start)pending.push_back({room.id,start,added.size()-start});
        }
    }
    std::vector<uint64_t> removed;
    for(auto it=roomDecor_.begin();it!=roomDecor_.end();){
        if(keep.contains(it->first)){++it;continue;}
        removed.insert(removed.end(),it->second.handles.begin(),it->second.handles.end());
        it=roomDecor_.erase(it);
    }
    if(removed.empty()&&added.empty())return;
    const std::vector<WorldRegistry::Change> changes;
    const auto handles=world.ApplyBatch(changes,removed,added);
    for(const auto& item:pending){
        RoomDecorResident state;
        state.handles.assign(handles.begin()+item.first,handles.begin()+item.first+item.count);
        roomDecor_.emplace(item.room,std::move(state));
    }
}

void BuildingSession::StreamWorktops(WorldRegistry& world,XMFLOAT3 head){
    if(!worktop_)return;
    StreamRoomDecor(world,head);
    const auto view=Local(head);
    const auto distanceSquared=[&](Point point){
        const float x=(view.x-point.x)*definition_.xScale,y=view.y-point.y,z=view.z-point.z;
        return x*x+y*y+z*z;
    };
    if(worktopViewer_&&worktopRevision_==publicationRevision_&&distanceSquared(*worktopViewer_)<.16f)
        return;
    worktopViewer_=view;
    const bool geometryChanged=worktopRevision_!=publicationRevision_;
    worktopRevision_=publicationRevision_;
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
            const auto layout=worktop_->Fit(mount->surface,(b.right-b.left)*definition_.xScale,
                                            b.back-b.front,mount->host^(mount->room<<1u));
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
        if(!resident_.contains(source.liveHost?source.liveHost:source.host))continue;
        const auto [lo,hi]=source.room;
        SceneLight light{};
        light.position=Scene(source.position);
        light.direction=Scale({(hi.x-lo.x)*definition_.xScale*.5f,source.position.y-lo.y,
            (hi.z-lo.z)*.5f},units);
        light.outerCos=(hi.y-source.position.y)*units;
        light.radius=Length(light.direction)*1.40f;
        switch(source.lens){
        case Material::LampRed:light.color={1.f,.44f,.37f};light.power=.018f;break;
        case Material::LampAmber:light.color={1.f,.73f,.46f};light.power=.023f;break;
        default:light.color={1.f,.94f,.84f};light.power=.021f;break;
        }
        light.flags=BuildingLightFlag;
        light.id=0x71000000u+((uint32_t(definition_.id)&0xfffu)<<12u)+
            uint32_t(sector->level+1)*64u+uint32_t(i);
        lights.push_back(light);
    }
}
void BuildingSession::Publish(WorldRegistry& world,std::span<const Sector* const> sectors,bool replace){
    std::unordered_set<uint64_t> keep;
    std::vector<uint64_t> removed;
    std::vector<WorldRegistry::Change> changes;
    std::vector<std::pair<Part,int>> added;
    std::vector<WorldObject> visuals;
    for(const auto* sector:sectors)for(const auto& part:sector->parts){
        if(!RoomResident(part))continue;
        if(!keep.insert(part.id).second)Fail("Duplicate building component identity");
        const auto i=resident_.find(part.id);
        if(i==resident_.end()){
            visuals.push_back(Visual(part,sector->level));
            added.emplace_back(part,sector->level);
        }else if(i->second.part!=part||i->second.paneResident!=residentRooms_.contains(part.paneRoom)){
            const auto& old=i->second.part;
            if((part.id&0xf0000000u)==0x80000000u&&
               (old.source!=part.source||old.bounds!=part.bounds))
                Fail("Overlapping building fragment identities");
            changes.push_back({i->second.handle,Visual(part,sector->level)});
            i->second.part=part;
            i->second.paneResident=residentRooms_.contains(part.paneRoom);
        }
    }
    if(replace)for(const auto* sector:sectors){
        for(const auto& part:sectors_.at(size_t(sector->level+1))->parts){
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
        const auto& part=added[i].first;
        resident_.emplace(part.id,Resident{part,handles[i],residentRooms_.contains(part.paneRoom)});
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
    PrepareRooms(world,head,head);
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
        std::vector<const Sector*> changed;
        changed.reserve(next.sectors.size());
        for(const auto& sector:next.sectors)changed.push_back(&sector);
        Publish(world,changed,true);
        for(size_t i=0;i<next.sectors.size();++i){
            auto& sector=next.sectors[i];
            const auto index=size_t(sector.level+1);
            if(!next.sources.empty())sources_.at(index)=std::move(next.sources[i]);
            sectors_.at(index)=std::make_shared<Sector>(std::move(sector));
        }
        ++geometryRevision_;
        ++publicationRevision_;
        PrepareRooms(world,head,head);
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
            [&](const Part& part){return RoomResident(part)&&Blocks(feet,height,part,definition_.xScale);}))return false;
    for(const auto& part:movingParts_)if(Blocks(feet,height,part,definition_.xScale))return false;
    return true;
}
float BuildingSession::Support(Point p,float bottom,float top)const{
    float best=-std::numeric_limits<float>::infinity();
    // The floor of a crater can be well below the original grade.
    if(ground_&&bottom<=.40f)if(const auto ground=ground_->Height(Scene(p))){
        const float localY=(*ground-definition_.origin.y)/SpectacleInteriorUnitsPerMeter;
        if(localY>=bottom&&localY<=top)best=localY;
    }
    auto check=[&](const Part& part){
        const float y=part.bounds.hi.y;
        if(part.solid&&y>=bottom&&y<=top&&y>best&&RoomResident(part)&&HorizontalOverlap(p,part.bounds,definition_.xScale))best=y;
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
        if(!RoomResident(part))return false;
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
    QueueBreach({source,original,hole});
}
void BuildingSession::Saw(const Hit& hit,XMFLOAT3 from,XMFLOAT3 to){
    const auto& part=hit.part;
    const auto source=part.source?part.source:part.id;
    if((source&0xff000000u)!=0u)return;
    const auto original=part.source?part.original:part.bounds;
    const float xScale=definition_.xScale;
    const std::array spans{(original.hi.x-original.lo.x)*xScale,
        original.hi.y-original.lo.y,original.hi.z-original.lo.z};
    const size_t axis=std::min_element(spans.begin(),spans.end())-spans.begin();
    auto a=Local(from);
    const auto b=Local(to);
    constexpr float half=.031f,grid=.012f;
    float length=std::hypot((b.x-a.x)*xScale,b.y-a.y,b.z-a.z);
    if(length>.85f){a=b;length=0.f;}
    const unsigned steps=std::max(1u,unsigned(std::ceil(length/half)));
    const auto snap=[](float value){return std::round(value/grid)*grid;};
    for(unsigned i=0;i<=steps;++i){
        const float t=float(i)/steps;
        const Point c{snap(std::lerp(a.x,b.x,t)*xScale)/xScale,snap(std::lerp(a.y,b.y,t)),
            snap(std::lerp(a.z,b.z,t))};
        Box hole=original;
        if(axis!=0){hole.lo.x=c.x-half/xScale;hole.hi.x=c.x+half/xScale;}
        if(axis!=1){hole.lo.y=c.y-half;hole.hi.y=c.y+half;}
        if(axis!=2){hole.lo.z=c.z-half;hole.hi.z=c.z+half;}
        QueueBreach({source,original,hole});
    }
}
void BuildingSession::QueueBreach(Damage cut){
    const auto same=[&](const Damage& damage){
        return damage.part==cut.part&&damage.original==cut.original&&damage.opening==cut.opening;
    };
    if(std::any_of(activeBreaches_.begin(),activeBreaches_.end(),same)||
       std::any_of(breaches_.begin(),breaches_.end(),same))return;
    breaches_.push_back(cut);
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
