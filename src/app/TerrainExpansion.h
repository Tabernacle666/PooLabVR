#pragma once
#include "ExpansionFlora.h"
#include "BotanicalVariation.h"
#include "AppSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <vector>

namespace OASIS {
// SF8: an extent is a scalar, not an allocated continent. The authored
// campus remains intact; only the complement of its original outline
// can receive new land. All live objects are owned by WorldRegistry.
class TerrainExpansion {
    static constexpr int32_t GroundLayer=67,TreeLayer=68,DetailLayer=69,FaunaLayer=70;
    static constexpr double Side=TerritorialTileMeters;
    static constexpr double DetailReach=45.,FaunaReach=15.;
    struct Point {double x{},z{};};
    using Polygon=std::vector<Point>;
    using Triangle=std::array<Point,3>;
    struct Candidate {GpuTileKey key;double distance;};
    struct Batch {
        std::vector<WorldObject> objects,trees;
        std::vector<uint64_t> ids,treeIds;
        std::vector<uint32_t> assemblies;
        void Add(WorldObject o,uint64_t id){objects.push_back(o);ids.push_back(id);}
    };
    std::vector<Triangle> footprint_;
    std::map<GpuTileKey,std::vector<uint32_t>> footprintBins_;
    // Only the finite authored shoreline has unique geometry. Interior
    // cells are excluded; every other extension cell instances four
    // shared UV-phase meshes (one geometry topology).
    std::map<GpuTileKey,uint32_t> boundary_;
    std::array<uint32_t,4> groundMeshes_{Invalid,Invalid,Invalid,Invalid};
    struct TreeModel {std::array<WorldObject,2> parts;uint32_t assembly{};};
    std::array<std::vector<TreeModel>,3> treeModels_;
    std::array<std::vector<unsigned>,5> understoryFamilies_;
    ExpansionFlora flora_;
    WorldRegistry* world_{};
    Point center_{};
    float groundY_{};
    double initialRadius_{};
    unsigned density_{2};
    uint32_t steps_{},lastSteps_{};
    bool ready_{},selectionValid_{};
    int64_t lastX_{},lastZ_{};
    double lastFar_{};
    std::vector<Candidate> pending_;
    size_t next_{};
    uint32_t material_{Invalid};

    static int64_t Cell(double p){return int64_t(std::floor(p/Side));}
    static Point Center(GpuTileKey k){return {(double(k.x)+.5)*Side,(double(k.z)+.5)*Side};}
    static GpuTileKey GroundKey(int64_t x,int64_t z){return {x,z,GroundLayer};}
    static double Cross2(Point a,Point b,Point p){return (b.x-a.x)*(p.z-a.z)-(b.z-a.z)*(p.x-a.x);}
    static double Area(const Polygon& p){
        double a=0.;for(size_t i=0;i<p.size();++i){
            const auto& u=p[i];const auto& v=p[(i+1)%p.size()];a+=u.x*v.z-u.z*v.x;
        }return .5*std::abs(a);
    }
    static Polygon Clip(const Polygon& p,Point a,Point b,double sign){
        Polygon out;if(p.empty())return out;
        auto previous=p.back();double d=sign*Cross2(a,b,previous);
        for(const auto current:p){
            const double e=sign*Cross2(a,b,current);
            if((d>=0.)!=(e>=0.)){
                const double t=d/(d-e);
                out.push_back({std::lerp(previous.x,current.x,t),std::lerp(previous.z,current.z,t)});
            }
            if(e>=0.)out.push_back(current);
            previous=current;d=e;
        }
        return out;
    }
    static Polygon Square(GpuTileKey key){
        const double x=double(key.x)*Side,z=double(key.z)*Side;
        return {{x,z},{x+Side,z},{x+Side,z+Side},{x,z+Side}};
    }
    static bool InTriangle(Point p,const Triangle& t){
        const double sign=Cross2(t[0],t[1],t[2])>=0.?1.:-1.;
        for(unsigned i=0;i<3;++i)
            if(sign*Cross2(t[i],t[(i+1)%3],p)<-1.e-7)return false;
        return true;
    }
    bool Original(Point p)const{
        const auto it=footprintBins_.find(GroundKey(Cell(p.x),Cell(p.z)));
        if(it==footprintBins_.end())return false;
        for(uint32_t i:it->second)if(InTriangle(p,footprint_[i]))return true;
        return false;
    }
    std::vector<Polygon> Complement(GpuTileKey key,const std::vector<uint32_t>& triangles)const{
        std::vector<Polygon> pieces{Square(key)};
        for(uint32_t id:triangles){
            const auto& t=footprint_[id];
            const double sign=Cross2(t[0],t[1],t[2])>=0.?1.:-1.;
            std::vector<Polygon> next;
            for(auto p:pieces){
                // Subtract the convex source triangle. The outside pieces
                // are disjoint because the remainder is clipped successively.
                for(unsigned i=0;i<3&&!p.empty();++i){
                    auto outside=Clip(p,t[i],t[(i+1)%3],-sign);
                    if(outside.size()>=3&&Area(outside)>1.e-8)next.push_back(std::move(outside));
                    p=Clip(p,t[i],t[(i+1)%3],sign);
                    if(p.size()<3||Area(p)<=1.e-8)p.clear();
                }
            }
            pieces=std::move(next);if(pieces.empty())break;
        }
        return pieces;
    }
    uint32_t AppendMesh(SceneData& scene,GpuTileKey key,const std::vector<Polygon>& polygons,bool shared){
        auto& m=scene.meshes;MeshRange range{};
        range.firstVertex=uint32_t(m.vertices.size());range.firstIndex=uint32_t(m.indices.size());
        range.firstMeshlet=uint32_t(m.meshlets.size());
        const auto c=Center(key);
        auto vertex=[&](Point p){
            const uint32_t i=uint32_t(m.vertices.size());
            // Four parity variants keep the 4 m soil texture phase continuous
            // across a 10 m tile lattice. Shoreline pieces use absolute UVs.
            const float x=float(p.x-c.x),z=float(p.z-c.z);
            const double u=shared?double(x)+Side*.5+double(key.x&1)*Side:p.x;
            const double v=shared?double(z)+Side*.5+double(key.z&1)*Side:p.z;
            m.vertices.push_back({{x,0.f,z},{0.f,1.f,0.f},{float(u/4.),float(v/4.)},{}});
            m.indices.push_back(i);
        };
        for(const auto& p:polygons)for(size_t i=1;i+1<p.size();++i){
            const double a=Cross2(p[0],p[i],p[i+1]);if(std::abs(a)<1.e-8)continue;
            vertex(p[0]);
            if(a>0.){vertex(p[i+1]);vertex(p[i]);}
            else {vertex(p[i]);vertex(p[i+1]);}
        }
        if(m.indices.size()==range.firstIndex)return Invalid;
        FinishRange(m,range);
        const uint32_t id=uint32_t(m.ranges.size());
        m.ranges.push_back(range);m.deformations.emplace_back();m.motions.emplace_back();
        return id;
    }
    uint32_t Mesh(GpuTileKey key)const{
        key.layer=GroundLayer;
        if(const auto i=boundary_.find(key);i!=boundary_.end())return i->second;
        return groundMeshes_[unsigned(key.x&1)|unsigned((key.z&1)<<1)];
    }
    double Radius(uint32_t steps)const{return initialRadius_+Side*double(steps);}
    bool Eligible(GpuTileKey key)const{
        if(!steps_||Mesh(key)==Invalid)return false;
        const auto p=Center(key);
        return std::hypot(p.x-center_.x,p.z-center_.z)<=Radius(steps_);
    }
    static uint64_t Identity(GpuTileKey key,uint32_t ordinal){
        constexpr int64_t limit=1ll<<22;
        if(key.x<-limit||key.x>=limit||key.z<-limit||key.z>=limit||
           key.layer<GroundLayer||key.layer>FaunaLayer||ordinal>=4096u)
            Fail("SF8 identity range");
        return (1ull<<63)|(6ull<<60)|(uint64_t(key.x+limit)<<37)|
            (uint64_t(key.z+limit)<<14)|(uint64_t(key.layer-GroundLayer)<<12)|ordinal;
    }
    static uint32_t Seed(int64_t x,int64_t z,uint32_t salt){
        return uint32_t(x)*0x9e3779b9u^uint32_t(z)*0x85ebca6bu^salt;
    }
    static float Random(uint32_t seed){return BotanicalVariation::Random(seed);}
    bool Owns(GpuTileKey key,Point p)const{
        return Cell(p.x)==key.x&&Cell(p.z)==key.z&&!Original(p);
    }
    Pose PlantPose(Point p,float yaw)const{
        const float a=yaw*.5f;
        return {{float(p.x),groundY_,float(p.z)},{0.f,std::sin(a),0.f,std::cos(a)}};
    }
    template<class Fn> void Lattice(GpuTileKey key,double spacing,float jitter,uint32_t salt,Fn&& fn)const{
        const double x=double(key.x)*Side,z=double(key.z)*Side;
        const int64_t x0=int64_t(std::floor(x/spacing))-1,z0=int64_t(std::floor(z/spacing))-1;
        const int64_t x1=int64_t(std::ceil((x+Side)/spacing))+1,z1=int64_t(std::ceil((z+Side)/spacing))+1;
        for(int64_t iz=z0;iz<=z1;++iz)for(int64_t ix=x0;ix<=x1;++ix){
            const uint32_t seed=Seed(ix,iz,salt);
            const Point p{(double(ix)+.5+jitter*(Random(seed^0x11u)-.5))*spacing,
                          (double(iz)+.5+jitter*(Random(seed^0x22u)-.5))*spacing};
            if(Owns(key,p))fn(p,seed);
        }
    }
    double Reach(int32_t layer,double farMeters)const{
        if(layer==FaunaLayer)return FaunaReach;
        if(layer==DetailLayer)return DetailReach;
        // Terrain and canopy remain visible over the camera's actual reach.
        return std::max(DetailReach,farMeters+15.);
    }
    void Generate(GpuTileKey key,Batch& batch)const{
        const auto c=Center(key);
        if(key.layer==GroundLayer){
            WorldObject o{};o.mesh=Mesh(key);o.material=material_;o.tileLayer=key.layer;
            o.current=o.previous=PlantPose(c,0.f);o.boundRadius=float(Side*.7071067811865476);
            batch.Add(o,Identity(key,0));return;
        }
        uint32_t ordinal=0;
        if(key.layer==TreeLayer){
            const double spacing=5.*std::sqrt(2./double(std::max(1u,density_)));
            Lattice(key,spacing,.70f,0x53463854u,[&](Point p,uint32_t seed){
                const auto& models=treeModels_[std::min(2u,unsigned(Random(seed^0x611u)*3.f))];
                if(models.empty())return;
                const float scale=BotanicalVariation::Tree(Random(seed^0x21au));
                const auto& model=models.at(BotanicalVariation::TreeForm(seed,unsigned(models.size()),scale));
                const auto pose=PlantPose(p,XM_2PI*Random(seed^0x816u));
                for(auto o:model.parts){
                    o.tileLayer=key.layer;o.current=o.previous=pose;o.scale={scale,scale,scale};
                    batch.trees.push_back(o);batch.treeIds.push_back(Identity(key,ordinal++));
                }
                batch.assemblies.push_back(model.assembly);
            });
            return;
        }
        if(key.layer==DetailLayer){
            if(flora_.hasFloor)Lattice(key,1.65,.20f,0x53463846u,[&](Point p,uint32_t seed){
                const float roll=Random(seed^0x33u);
                const unsigned family=roll<.30f?3u:roll<.52f?1u:roll<.70f?2u:0u;
                auto o=flora_.floor[family*2u+unsigned(Random(seed^0x55u)>.5f)];
                const auto& span=BotanicalVariation::Floor[family];
                const float scale=BotanicalVariation::Plant(Random(seed^0x5154u),span[0],span[1]);
                o.tileLayer=key.layer;o.current=o.previous=PlantPose(p,XM_2PI*Random(seed^0x44u));
                o.current.p.y+=.008f;o.previous=o.current;o.scale={1.f,scale,1.f};
                batch.Add(o,Identity(key,ordinal++));
                if(Random(seed^0x66u)<.013f){
                    auto wood=flora_.floor[8];wood.tileLayer=key.layer;wood.current=wood.previous=o.current;
                    const float s=BotanicalVariation::Plant(Random(seed^0x5155u),.50f,3.f);
                    wood.scale={s,s,s};batch.Add(wood,Identity(key,ordinal++));
                }
            });
            Lattice(key,1.65,.40f,0x53463855u,[&](Point p,uint32_t seed){
                if(Random(seed^0x33u)>.91f)return;
                const float roll=Random(seed^0x45u);
                const unsigned family=roll<.27f?0u:roll<.57f?1u:roll<.76f?2u:roll<.93f?3u:4u;
                const auto& models=understoryFamilies_[family];if(models.empty())return;
                const auto model=models[std::min(models.size()-1,size_t(Random(seed^0x56u)*float(models.size())))];
                auto o=flora_.understory[model];const auto& span=BotanicalVariation::Understory[family];
                const float s=BotanicalVariation::Plant(Random(seed^0x5156u),span[0],span[1]);
                o.tileLayer=key.layer;o.current=o.previous=PlantPose(p,XM_2PI*Random(seed^0x78u));
                o.current.p.y+=.008f;o.previous=o.current;o.scale={1.f,s,1.f};
                batch.Add(o,Identity(key,ordinal++));
            });
            return;
        }
        if(key.layer==FaunaLayer&&flora_.insects){
            const auto seed=Seed(key.x,key.z,0x53463849u);
            if(Random(seed)<.40f&&Owns(key,c)){
                auto o=*flora_.insects;o.tileLayer=key.layer;o.current=o.previous=PlantPose(c,0.f);
                o.scale={1.f,1.f,1.f};batch.Add(o,Identity(key,0));
            }
        }
    }
    void Select(WorldRegistry& world,XMFLOAT3 viewer,double farMeters){
        pending_.clear();next_=0;
        const auto& resident=world.Resident().ResidentTiles();
        const auto distance=[&](GpuTileKey k){const auto p=Center(k);
            return std::hypot(p.x-double(viewer.x),p.z-double(viewer.z));};
        std::vector<uint64_t> removed;
        for(const auto& [key,tile]:resident){
            if(key.layer<GroundLayer||key.layer>FaunaLayer)continue;
            if(distance(key)<=Reach(key.layer,farMeters)+20.)continue;
            for(const auto& o:tile.objects)removed.push_back(o.identity);
        }
        if(!removed.empty())world.EraseBatch(removed);
        const double reach=Reach(GroundLayer,farMeters);
        const double radius=Radius(steps_);
        const auto x0=std::max(Cell(viewer.x-reach),Cell(center_.x-radius));
        const auto x1=std::min(Cell(viewer.x+reach),Cell(center_.x+radius));
        const auto z0=std::max(Cell(viewer.z-reach),Cell(center_.z-radius));
        const auto z1=std::min(Cell(viewer.z+reach),Cell(center_.z+radius));
        for(int64_t x=x0;x<=x1;++x)for(int64_t z=z0;z<=z1;++z){
            auto key=GroundKey(x,z);if(!Eligible(key))continue;
            const double d=distance(key);
            for(int32_t layer=GroundLayer;layer<=FaunaLayer;++layer){
                key.layer=layer;
                if(d>Reach(layer,farMeters)||resident.contains(key))continue;
                if(layer==TreeLayer&&treeModels_[0].empty())continue;
                if(layer==DetailLayer&&!flora_.hasFloor)continue;
                if(layer==FaunaLayer&&!flora_.insects)continue;
                pending_.push_back({key,d});
            }
        }
        // Never delay collision coverage near the viewer behind distant
        // botanical work. Equal-distance ground precedes vegetation.
        std::sort(pending_.begin(),pending_.end(),[](const auto& a,const auto& b){
            const bool nearA=a.key.layer==GroundLayer&&a.distance<=DetailReach;
            const bool nearB=b.key.layer==GroundLayer&&b.distance<=DetailReach;
            if(nearA!=nearB)return nearA;
            if(a.distance!=b.distance)return a.distance<b.distance;
            if(a.key.layer!=b.key.layer)return a.key.layer<b.key.layer;
            return a.key<b.key;
        });
    }
public:
    ExpansionFlora& Flora(){return flora_;}
    void Capture(const SceneData& scene,unsigned density){
        density_=std::max(1u,density);footprint_.clear();footprintBins_.clear();steps_=0;
        std::vector<TerrainTriangle> source;
        float top=-std::numeric_limits<float>::max();
        for(const auto& o:scene.objects){
            if(o.material>=scene.materialNames.size()||scene.materialNames[o.material]!="Island soil")continue;
            const auto& r=scene.meshes.ranges.at(o.mesh);
            for(uint32_t i=r.firstIndex;i+2u<r.firstIndex+r.indexCount;i+=3u){
                TerrainTriangle t;
                for(unsigned k=0;k<3;++k){
                    const auto p=scene.meshes.vertices.at(scene.meshes.indices.at(i+k)).position;
                    t[k]=Transform({p.x*o.scale.x,p.y*o.scale.y,p.z*o.scale.z},o.current);
                    top=std::max(top,t[k].y);
                }
                source.push_back(t);
            }
        }
        Point lo{1.e30,1.e30},hi{-1.e30,-1.e30};groundY_=top;
        for(const auto& t:source){
            if(std::any_of(t.begin(),t.end(),[&](const auto& p){return std::abs(p.y-top)>.001f;}))continue;
            Triangle tri{{{t[0].x,t[0].z},{t[1].x,t[1].z},{t[2].x,t[2].z}}};
            if(std::abs(Cross2(tri[0],tri[1],tri[2]))<1.e-8)continue;
            const uint32_t id=uint32_t(footprint_.size());footprint_.push_back(tri);
            const double left=std::min({tri[0].x,tri[1].x,tri[2].x}),right=std::max({tri[0].x,tri[1].x,tri[2].x});
            const double front=std::min({tri[0].z,tri[1].z,tri[2].z}),back=std::max({tri[0].z,tri[1].z,tri[2].z});
            lo.x=std::min(lo.x,left);lo.z=std::min(lo.z,front);hi.x=std::max(hi.x,right);hi.z=std::max(hi.z,back);
            for(int64_t x=Cell(left);x<=Cell(right);++x)for(int64_t z=Cell(front);z<=Cell(back);++z)
                footprintBins_[GroundKey(x,z)].push_back(id);
        }
        center_={(lo.x+hi.x)*.5,(lo.z+hi.z)*.5};initialRadius_=0.;
        for(const auto& t:footprint_)for(const auto p:t)
            initialRadius_=std::max(initialRadius_,std::hypot(p.x-center_.x,p.z-center_.z));
    }
    void Install(SceneData& scene,WorldRegistry& world){
        world_=&world;ready_=false;selectionValid_=false;pending_.clear();next_=0;boundary_.clear();
        for(auto& models:treeModels_)models.clear();
        for(auto& models:understoryFamilies_)models.clear();
        groundMeshes_.fill(Invalid);material_=Invalid;
        if(!scene.solarSystem||footprint_.empty())return;
        for(uint32_t i=0;i<scene.materialNames.size();++i)
            if(scene.materialNames[i]=="Island soil"){material_=i;break;}
        if(material_==Invalid)return;
        for(int64_t z=0;z<2;++z)for(int64_t x=0;x<2;++x){
            const auto key=GroundKey(x,z);
            groundMeshes_[size_t(x+2*z)]=AppendMesh(scene,key,{Square(key)},true);
        }
        for(const auto& [key,triangles]:footprintBins_){
            const auto polygons=Complement(key,triangles);
            double area=0.;for(const auto& p:polygons)area+=Area(p);
            if(area<=1.e-6){boundary_[key]=Invalid;continue;}
            if(area>=Side*Side-1.e-6)continue;
            boundary_[key]=AppendMesh(scene,key,polygons,false);
        }
        scene.meshes.skin.resize(scene.meshes.vertices.size());
        if(!scene.triangleSolids.empty())scene.triangleSolids.resize(scene.meshes.indices.size()/3u,Invalid);
        for(uint32_t i=0;i<scene.canopy.assemblies.size();++i){
            const auto& a=scene.canopy.assemblies[i];if(a.species>=treeModels_.size())continue;
            TreeModel model{};model.assembly=i;
            bool valid=true;
            for(unsigned k=0;k<2;++k){
                if(a.meshes[k]>=scene.meshes.ranges.size()||a.materials[k]>=scene.materials.size()){valid=false;break;}
                auto& o=model.parts[k];o.mesh=a.meshes[k];o.material=a.materials[k];
                o.flags=BuildingObject|NoCollision|OASIS_INSTANCE_FOREST|(k==0?OASIS_INSTANCE_METRIC_BARK:0u);
                const auto& b=scene.meshes.ranges[o.mesh].bounds;
                o.boundRadius=std::max(.001f,Length({b.x,b.y,b.z})+b.w);
            }
            if(valid)treeModels_[a.species].push_back(model);
        }
        for(unsigned i=0;i<flora_.used.size();++i)if(flora_.used[i])
            understoryFamilies_[size_t(Understory::SpeciesList[i/Understory::Variants].family)].push_back(i);
        ready_=true;
    }
    uint32_t Steps()const noexcept{return steps_;}
    double OpticalReach(XMFLOAT3 viewer)const noexcept{
        if(!ready_||!std::isfinite(viewer.x)||!std::isfinite(viewer.y)||!std::isfinite(viewer.z))return 0.;
        // Include the logical extent, full boundary cells and canopy clearance.
        const double horizontal=std::hypot(double(viewer.x)-center_.x,double(viewer.z)-center_.z)+Radius(steps_)+2.*Side;
        return std::hypot(horizontal,std::abs(double(viewer.y)-groundY_)+64.);
    }
    bool Grow(){
        if(!ready_||Radius(steps_)>9990000.)return false;
        ++steps_;
        char line[160];std::snprintf(line,sizeof(line),"sf8_continent radius_m=%.2f extension_m=%.0f steps=%u\r\n",
            Radius(steps_),Side*steps_,steps_);DiagnosticLine(line);
        return true;
    }
    std::optional<float> Height(XMFLOAT3 p)const{
        if(!ready_||!steps_||!world_||!std::isfinite(p.x)||!std::isfinite(p.z))return {};
        const auto key=GroundKey(Cell(p.x),Cell(p.z));
        // Physical support is a property of the logical continent, not of
        // the current render residency or a pending streaming batch.
        if(!Eligible(key)||Original({p.x,p.z}))return {};
        return groundY_;
    }
    void Tick(WorldRegistry& world,XMFLOAT3 viewer,float farMeters){
        if(!ready_||!steps_||!std::isfinite(viewer.x)||!std::isfinite(viewer.z)||!std::isfinite(farMeters))return;
        const auto x=Cell(viewer.x),z=Cell(viewer.z);
        const bool moved=!selectionValid_||x!=lastX_||z!=lastZ_||double(farMeters)!=lastFar_;
        const bool expanded=steps_!=lastSteps_;
        // A radius change entirely beyond the active territory must not
        // rebuild a catalog, a selection, or even the pending local queue.
        const bool intersects=lastSteps_==0||
            std::hypot(double(viewer.x)-center_.x,double(viewer.z)-center_.z)+
            Reach(GroundLayer,farMeters)+20.>=Radius(lastSteps_);
        if(moved||(expanded&&intersects)){
            Select(world,viewer,farMeters);selectionValid_=true;
            lastX_=x;lastZ_=z;lastFar_=farMeters;
        }
        lastSteps_=steps_;
        if(next_>=pending_.size())return;
        Batch batch;unsigned work=0;
        while(next_<pending_.size()){
            const auto& c=pending_[next_];
            const bool urgent=c.key.layer==GroundLayer&&c.distance<=DetailReach;
            if(!urgent&&work>=24u)break;
            ++next_;if(world.Resident().ResidentTiles().contains(c.key))continue;
            Generate(c.key,batch);if(!urgent)++work;
        }
        if(!batch.objects.empty())world.CreateBatch(batch.objects,batch.ids);
        if(!batch.trees.empty())world.CreateCanopyBatch(batch.trees,batch.assemblies,batch.treeIds);
    }
};
}
