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
#include <set>
#include <vector>

namespace OASIS {
// Logical expansion is independent of render route granularity. Full flat
// ground cells may share an exact spatial page; expansion canopy groups
// logical cells in stable owner pages without replacing individual trees.
class TerrainExpansion {
    static constexpr int32_t GroundLayer=67,TreeLayer=ExpansionCanopyTileLayer,DetailLayer=69,FaunaLayer=70;
    static constexpr double Side=TerritorialTileMeters;
    static constexpr double DetailReach=45.,FaunaReach=15.;
    // One full 8x8 page contains 128 source triangles, the MG2 cluster
    // triangle target. This bounds page geometry, NOT territory or view reach.
    static constexpr unsigned GroundPageLevels=3;
    struct Point {double x{},z{};};
    using Polygon=std::vector<Point>;
    using Triangle=std::array<Point,3>;
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
    std::array<uint32_t,GroundPageLevels+1> groundPageMeshes_{};
    // Disjoint cover of the already published logical ground, not a second
    // object registry. The WorldRegistry owns the actual live route records.
    std::array<std::set<GpuTileKey>,GroundPageLevels+1> groundPages_;
    uint64_t logicalGroundCells_{},logicalTrees_{};
    uint64_t logicalCanopyCells_{},canopyOwnerPages_{};
    struct GroundDelta {
        std::map<uint64_t,WorldObject> added;
        std::vector<uint64_t> removed;
    };
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
    std::set<GpuTileKey> localGenerated_;
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
    uint32_t AppendMesh(SceneData& scene,GpuTileKey key,const std::vector<Polygon>& polygons,bool shared,int64_t width=1){
        auto& m=scene.meshes;MeshRange range{};
        range.firstVertex=uint32_t(m.vertices.size());range.firstIndex=uint32_t(m.indices.size());
        range.firstMeshlet=uint32_t(m.meshlets.size());
        const Point c{(double(key.x)+.5*double(width))*Side,(double(key.z)+.5*double(width))*Side};
        auto vertex=[&](Point p,GpuTileKey phase){
            const uint32_t i=uint32_t(m.vertices.size());
            // Four parity variants keep the 4 m soil texture phase continuous
            // across a 10 m tile lattice. Shoreline pieces use absolute UVs.
            const float x=float(p.x-c.x),z=float(p.z-c.z);
            // Retain each original cell's UV phase, even inside a larger page.
            // No reliance on sampler wrap, material transforms or UV scaling.
            const double u=shared?p.x-double(phase.x)*Side+double(phase.x&1)*Side:p.x;
            const double v=shared?p.z-double(phase.z)*Side+double(phase.z&1)*Side:p.z;
            m.vertices.push_back({{x,0.f,z},{0.f,1.f,0.f},{float(u/4.),float(v/4.)},{}});
            m.indices.push_back(i);
        };
        for(const auto& p:polygons)for(size_t i=1;i+1<p.size();++i){
            const double a=Cross2(p[0],p[i],p[i+1]);if(std::abs(a)<1.e-8)continue;
            const auto phase=width==1?key:GroundKey(Cell(p[0].x),Cell(p[0].z));
            vertex(p[0],phase);
            if(a>0.){vertex(p[i+1],phase);vertex(p[i],phase);}
            else {vertex(p[i],phase);vertex(p[i+1],phase);}
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
    static double LocalReach(int32_t layer){return layer==FaunaLayer?FaunaReach:DetailReach;}
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
                const auto& models=treeModels_[BotanicalVariation::TreeSpecies(Random(seed^0x611u))];
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
    static int64_t AlignPage(int64_t coordinate,int64_t width){
        const auto r=coordinate%width;
        return coordinate-(r<0?r+width:r);
    }
    WorldObject GroundPageObject(GpuTileKey key,unsigned level)const{
        const int64_t width=int64_t(1)<<level;
        WorldObject o{};o.mesh=level?groundPageMeshes_[level]:Mesh(key);
        o.material=material_;o.tileLayer=GroundLayer;
        const Point center{(double(key.x)+.5*double(width))*Side,(double(key.z)+.5*double(width))*Side};
        o.current=o.previous=PlantPose(center,0.f);
        o.boundRadius=float(Side*double(width)*.7071067811865476);
        return o;
    }
    void StageGround(GpuTileKey key,GroundDelta& delta){
        if(!groundPages_[0].insert(key).second)Fail("A11 duplicate logical ground cell");
        delta.added.emplace(Identity(key,0),GroundPageObject(key,0));
        for(unsigned level=0;level<GroundPageLevels;++level){
            const int64_t childWidth=int64_t(1)<<level,parentWidth=childWidth*2;
            const auto parent=GroundKey(AlignPage(key.x,parentWidth),AlignPage(key.z,parentWidth));
            const std::array<GpuTileKey,4> children{{parent,
                GroundKey(parent.x+childWidth,parent.z),GroundKey(parent.x,parent.z+childWidth),
                GroundKey(parent.x+childWidth,parent.z+childWidth)}};
            auto& pages=groundPages_[level];
            if(!std::all_of(children.begin(),children.end(),[&](GpuTileKey child){
                // Only full, unclipped cells can become part of a flat page.
                return pages.contains(child)&&(level!=0||!boundary_.contains(child));
            }))break;
            for(const auto child:children){
                pages.erase(child);
                const auto id=Identity(child,level);
                // Cancel routes staged in this growth before allocating IDs.
                // Retire only pages which were published on earlier growths.
                if(!delta.added.erase(id))delta.removed.push_back(id);
            }
            if(!groundPages_[level+1].insert(parent).second)Fail("A11 overlapping ground page");
            delta.added.emplace(Identity(parent,level+1),GroundPageObject(parent,level+1));
            key=parent;
        }
    }
    void PublishGrowth(WorldRegistry& world){
        if(steps_==lastSteps_)return;
        const double radius=Radius(steps_),oldRadius=lastSteps_?Radius(lastSteps_):0.;
        Batch batch;GroundDelta ground;size_t groundCells=0,canopyCells=0;
        const bool treesAvailable=std::any_of(treeModels_.begin(),treeModels_.end(),
            [](const auto& models){return !models.empty();});
        for(int64_t x=Cell(center_.x-radius);x<=Cell(center_.x+radius);++x){
            const double dx=(double(x)+.5)*Side-center_.x;
            if(std::abs(dx)>radius)continue;
            const double zExtent=std::sqrt(std::max(0.,radius*radius-dx*dx));
            const int64_t z0=Cell(center_.z-zExtent),z1=Cell(center_.z+zExtent);
            const auto emit=[&](int64_t first,int64_t last){
                for(int64_t z=first;z<=last;++z){
                    auto key=GroundKey(x,z);const auto p=Center(key);
                    const double distance=std::hypot(p.x-center_.x,p.z-center_.z);
                    if(distance>radius||(lastSteps_&&distance<=oldRadius)||!Eligible(key))continue;
                    StageGround(key,ground);++groundCells;
                    if(treesAvailable){
                        key.layer=TreeLayer;
                        const size_t before=batch.assemblies.size();
                        Generate(key,batch);
                        if(batch.assemblies.size()!=before)++canopyCells;
                    }
                }
            };
            // Visit only the new ring, not the entire previously published disk.
            if(lastSteps_&&std::abs(dx)<oldRadius){
                const double oldZ=std::sqrt(std::max(0.,oldRadius*oldRadius-dx*dx));
                const int64_t innerFirst=Cell(center_.z-oldZ)+1,innerLast=Cell(center_.z+oldZ)-1;
                if(innerFirst<=innerLast){
                    emit(z0,std::min(z1,innerFirst-1));
                    emit(std::max(z0,innerLast+1),z1);
                    continue;
                }
            }
            emit(z0,z1);
        }
        std::vector<WorldObject> groundObjects;
        std::vector<uint64_t> groundIds;
        groundObjects.reserve(ground.added.size());groundIds.reserve(ground.added.size());
        for(const auto& [id,object]:ground.added){groundIds.push_back(id);groundObjects.push_back(object);}
        // Replace the four children and publish the parent in the same registry
        // transaction; render consumers cannot observe an intermediate hole.
        if(!groundIds.empty()||!ground.removed.empty())
            world.ApplyBatch({},ground.removed,groundObjects,groundIds);
        // Check only the owner pages touched by this ring, not the whole world.
        // Use the actual stored float pose, exactly like WorldRegistry::TileOf.
        std::map<GpuTileKey,size_t> canopyDelta;
        for(const auto& object:batch.trees)
            ++canopyDelta[ExpansionCanopyPageKey(Cell(object.current.p.x),Cell(object.current.p.z))];
        const auto& resident=world.Resident().ResidentTiles();
        uint64_t newPages=0,addedRoutes=0;
        for(auto& [key,count]:canopyDelta){
            addedRoutes+=count;
            const auto old=resident.find(key);
            if(old==resident.end())++newPages;
            else count+=old->second.byIdentity.size();
        }
        if(!batch.trees.empty())world.CreateCanopyBatch(batch.trees,batch.assemblies,batch.treeIds);
        for(const auto& [key,count]:canopyDelta){
            const auto current=resident.find(key);
            if(current==resident.end()||current->second.byIdentity.size()!=count)
                Fail("A11.1 canopy page route coverage mismatch");
        }
        if(addedRoutes!=uint64_t(batch.assemblies.size())*2u)
            Fail("A11.1 canopy pair coverage mismatch");
        logicalCanopyCells_+=canopyCells;canopyOwnerPages_+=newPages;
        logicalGroundCells_+=groundCells;logicalTrees_+=batch.assemblies.size();
        char canopyAudit[384];std::snprintf(canopyAudit,sizeof(canopyAudit),
            "mg2_a11_1_canopy steps=%u logical_cells=%llu owner_pages=%llu page_side_m=%.0f changed_pages=%zu new_pages=%llu added_routes=%llu logical_trees=%llu resident_routes=%llu validated=1\r\n",
            steps_,static_cast<unsigned long long>(logicalCanopyCells_),static_cast<unsigned long long>(canopyOwnerPages_),
            Side*double(ExpansionCanopyPageCells),canopyDelta.size(),static_cast<unsigned long long>(newPages),
            static_cast<unsigned long long>(addedRoutes),static_cast<unsigned long long>(logicalTrees_),
            static_cast<unsigned long long>(2u*logicalTrees_));
        DiagnosticLine(canopyAudit);
        uint64_t covered=0,pages=0;
        for(unsigned level=0;level<=GroundPageLevels;++level){
            const uint64_t count=groundPages_[level].size();
            covered+=count*(uint64_t(1)<<(2u*level));pages+=count;
        }
        if(covered!=logicalGroundCells_)Fail("A11 ground page cover mismatch");
        char audit[512];std::snprintf(audit,sizeof(audit),
            "mg2_a11_territory steps=%u logical_ground_cells=%llu covered_cells=%llu ground_routes=%llu pages_10m=%zu pages_20m=%zu pages_40m=%zu pages_80m=%zu routes_added=%zu routes_retired=%zu logical_trees=%llu canopy_routes=%llu\r\n",
            steps_,static_cast<unsigned long long>(logicalGroundCells_),static_cast<unsigned long long>(covered),
            static_cast<unsigned long long>(pages),groundPages_[0].size(),groundPages_[1].size(),groundPages_[2].size(),groundPages_[3].size(),
            groundIds.size(),ground.removed.size(),static_cast<unsigned long long>(logicalTrees_),static_cast<unsigned long long>(2u*logicalTrees_));
        DiagnosticLine(audit);
        char line[256];std::snprintf(line,sizeof(line),
            "sf8_publish steps=%u ground_tiles=%zu trees=%zu world_tiles=%llu radius_m=%.2f area_km2=%.6f\r\n",
            steps_,groundCells,batch.assemblies.size(),static_cast<unsigned long long>(world.TileCount()),
            radius,3.14159265358979323846*radius*radius/1.e6);DiagnosticLine(line);
        lastSteps_=steps_;
    }
    void StreamLocal(WorldRegistry& world,XMFLOAT3 viewer){
        const auto distance=[&](GpuTileKey key){const auto p=Center(key);
            return std::hypot(p.x-double(viewer.x),p.z-double(viewer.z));};
        std::vector<uint64_t> removed;
        const auto& resident=world.Resident().ResidentTiles();
        for(auto it=localGenerated_.begin();it!=localGenerated_.end();){
            if(distance(*it)<=LocalReach(it->layer)+20.){++it;continue;}
            if(const auto tile=resident.find(*it);tile!=resident.end())
                for(const auto& o:tile->second.objects)removed.push_back(o.identity);
            it=localGenerated_.erase(it);
        }
        if(!removed.empty())world.EraseBatch(removed);
        Batch batch;
        for(int64_t x=Cell(viewer.x-DetailReach);x<=Cell(viewer.x+DetailReach);++x)
        for(int64_t z=Cell(viewer.z-DetailReach);z<=Cell(viewer.z+DetailReach);++z){
            auto key=GroundKey(x,z);if(!Eligible(key))continue;
            const double d=distance(key);
            for(int32_t layer=DetailLayer;layer<=FaunaLayer;++layer){
                key.layer=layer;
                if(d>LocalReach(layer)||localGenerated_.contains(key))continue;
                if(layer==DetailLayer&&!flora_.hasFloor)continue;
                if(layer==FaunaLayer&&!flora_.insects)continue;
                Generate(key,batch);localGenerated_.insert(key);
            }
        }
        if(!batch.objects.empty())world.CreateBatch(batch.objects,batch.ids);
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
        world_=&world;ready_=false;selectionValid_=false;lastSteps_=0;localGenerated_.clear();boundary_.clear();
        for(auto& models:treeModels_)models.clear();
        for(auto& models:understoryFamilies_)models.clear();
        groundMeshes_.fill(Invalid);groundPageMeshes_.fill(Invalid);material_=Invalid;
        for(auto& pages:groundPages_)pages.clear();
        logicalGroundCells_=logicalTrees_=0;
        logicalCanopyCells_=canopyOwnerPages_=0;
        if(!scene.solarSystem||footprint_.empty())return;
        for(uint32_t i=0;i<scene.materialNames.size();++i)
            if(scene.materialNames[i]=="Island soil"){material_=i;break;}
        if(material_==Invalid)return;
        for(int64_t z=0;z<2;++z)for(int64_t x=0;x<2;++x){
            const auto key=GroundKey(x,z);
            groundMeshes_[size_t(x+2*z)]=AppendMesh(scene,key,{Square(key)},true);
        }
        // Bake reusable, phase-exact pages before GPU geometry initialization.
        // Each page retains the original two triangles per 10m cell.
        for(unsigned level=1;level<=GroundPageLevels;++level){
            const int64_t width=int64_t(1)<<level;
            std::vector<Polygon> cells;cells.reserve(size_t(width*width));
            for(int64_t z=0;z<width;++z)for(int64_t x=0;x<width;++x)
                cells.push_back(Square(GroundKey(x,z)));
            groundPageMeshes_[level]=AppendMesh(scene,GroundKey(0,0),cells,true,width);
            if(groundPageMeshes_[level]==Invalid)Fail("A11 ground page bake failed");
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
        if(!ready_||steps_==std::numeric_limits<uint32_t>::max())return false;
        // Only the existing packed coordinate identity's representable domain.
        const double reach=Radius(steps_+1u);
        if(std::max(std::abs(center_.x),std::abs(center_.z))+reach+Side>=double(1ll<<22)*Side)return false;
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
    void Tick(WorldRegistry& world,XMFLOAT3 viewer,float){
        if(!ready_||!steps_)return;
        const bool expanded=steps_!=lastSteps_;
        if(expanded)PublishGrowth(world);
        if(!std::isfinite(viewer.x)||!std::isfinite(viewer.z))return;
        const auto x=Cell(viewer.x),z=Cell(viewer.z);
        if(expanded||!selectionValid_||x!=lastX_||z!=lastZ_){
            StreamLocal(world,viewer);selectionValid_=true;lastX_=x;lastZ_=z;
        }
    }
};
}
