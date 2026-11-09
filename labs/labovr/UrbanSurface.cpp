#include "UrbanSurface.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

namespace lab::urban {
namespace {
constexpr float CellMeters=float(BlockMeters)/SurfaceTile::Resolution;
}

SurfaceTile::SurfaceTile(Tile tile):key_(tile) {
    const auto origin=Origin(tile);
    const auto features=GenerateTile(tile);
    std::vector<Point> drainPoints;
    for(const auto& feature:features)if(feature.kind==Kind::Drain)
        drainPoints.push_back(feature.position);
    for(unsigned z=0;z<Resolution;++z)for(unsigned x=0;x<Resolution;++x) {
        const Point p{origin.x+(x+.5)*CellMeters,origin.z+(z+.5)*CellMeters};
        auto& s=At(x,z);
        s.height=static_cast<float>(GroundHeight(p));
        s.heat=static_cast<float>(DrainHeat(p,features)+LampHeat(p,features)*.35);
        const double mask=.5+.5*Perlin(p.x/17.,p.z/17.,WorldSeed^777);
        s.snow=static_cast<float>(.001+.012*std::clamp(mask,0.,1.));
        s.wetness=.1f;
        for(const auto& drain:drainPoints){
            const double dx=drain.x-p.x,dz=drain.z-p.z;
            if(dx*dx+dz*dz<2.0*2.0){
                drains_[z*Resolution+x]=true;
                break;
            }
        }
    }

    // Multi-source Dijkstra, run once when a tile is constructed. Routes
    // follow gutter drains, prefer downhill grade and remain stable as
    // puddle depth changes. Every non-drain cell stores its next neighbour.
    using Node=std::pair<double,uint16_t>;
    std::priority_queue<Node,std::vector<Node>,std::greater<Node>> queue;
    std::array<double,CellCount> cost;
    cost.fill(std::numeric_limits<double>::infinity());
    for(unsigned i=0;i<CellCount;++i){
        downstream_[i]=uint16_t(i);
        if(drains_[i]){
            cost[i]=0;
            queue.emplace(0.0,uint16_t(i));
        }
    }
    while(!queue.empty()){
        const auto [distance,index]=queue.top();queue.pop();
        if(distance>cost[index])continue;
        const unsigned x=index%Resolution,z=index/Resolution;
        const auto visit=[&](unsigned neighbor){
            // A path that climbs a road crest is more expensive than a
            // level or downhill route, but can still reach the next drain.
            const double rise=std::max(0.,double(values_[index].height)-
                                         double(values_[neighbor].height));
            const double candidate=distance+CellMeters*(1.+rise*24.);
            if(candidate>=cost[neighbor])return;
            cost[neighbor]=candidate;
            downstream_[neighbor]=index;
            queue.emplace(candidate,uint16_t(neighbor));
        };
        if(x>0)visit(index-1);
        if(x+1<Resolution)visit(index+1);
        if(z>0)visit(index-Resolution);
        if(z+1<Resolution)visit(index+Resolution);
    }
}

SurfaceTile::Sample SurfaceTile::AtWorld(Point world) const{
    const Point origin=Origin(key_);
    const double u=(world.x-origin.x)/CellMeters-.5;
    const double v=(world.z-origin.z)/CellMeters-.5;
    const int x0=std::clamp(int(std::floor(u)),0,int(Resolution)-1);
    const int z0=std::clamp(int(std::floor(v)),0,int(Resolution)-1);
    const int x1=std::min(x0+1,int(Resolution)-1);
    const int z1=std::min(z0+1,int(Resolution)-1);
    const float fx=float(std::clamp(u-double(x0),0.,1.));
    const float fz=float(std::clamp(v-double(z0),0.,1.));
    const Sample& a=At(unsigned(x0),unsigned(z0));
    const Sample& b=At(unsigned(x1),unsigned(z0));
    const Sample& c=At(unsigned(x0),unsigned(z1));
    const Sample& d=At(unsigned(x1),unsigned(z1));
    const auto blend=[&](float Sample::*field){
        const float ab=std::lerp(a.*field,b.*field,fx);
        const float cd=std::lerp(c.*field,d.*field,fx);
        return std::lerp(ab,cd,fz);
    };
    return {blend(&Sample::snow),blend(&Sample::water),
            blend(&Sample::wetness),blend(&Sample::height),
            blend(&Sample::heat)};
}

SurfaceTile::Appearance SurfaceTile::AppearanceAt(Point p) const{
    const auto s=AtWorld(p);
    // One continuous weather-to-material mapping shared by snow meshes,
    // standing puddles, moving gutter runoff and damp asphalt films.
    return {
        std::clamp(s.snow/.012f,0.f,2.4f),
        std::clamp(s.water*120.f+s.wetness*.32f,.035f,1.2f),
        std::clamp(s.wetness*.55f+s.water*40.f+s.heat*.095f,0.f,1.f),
        std::clamp(s.water*80.f+s.wetness*.5f+s.heat*.09f,.1f,1.f)
    };
}

void SurfaceTile::Advance(double elapsedSeconds,double snowfall,double temperature) {
    if(!(elapsedSeconds>0))return;
    snowfall=std::clamp(snowfall,0.0,.002);
    while(elapsedSeconds>0) {
        const double dt=std::min(elapsedSeconds,1.0);
        elapsedSeconds-=dt;
        std::array<float,CellCount> transfers{};

        // Climate update is independent of travel direction and cell order.
        // Melt moves snow mass into the same persistent water reservoir.
        for(unsigned i=0;i<CellCount;++i){
            auto& s=values_[i];
            s.snow+=float(snowfall*dt);
            const double meltRate=std::max(0.,temperature+double(s.heat))*.000009;
            const float melt=std::min(s.snow,float(meltRate*dt));
            s.snow-=melt;
            s.water+=melt;
            // Surface film is a visual coverage, not an additional water
            // mass to double count in the conservation budget.
            s.wetness=std::clamp(s.wetness+20.f*melt-float(.003*dt),0.f,1.f);
        }
        // Two-phase transport: all outgoing fluxes read the same previous
        // water snapshot. No neighbour gets a head start from loop order.
        for(unsigned i=0;i<CellCount;++i){
            const auto& s=values_[i];
            const float available=s.water;
            if(available<=0)continue;
            if(drains_[i]){
                const float out=std::min(available,float(.045*dt));
                transfers[i]-=out;
                discharged_+=double(out);
                continue;
            }
            const unsigned next=downstream_[i];
            if(next==i)continue;
            const float grade=s.height-values_[next].height;
            const float fraction=std::min(1.f,float(dt)*
                std::clamp(.22f+std::max(0.f,grade)*3.f,.08f,.65f));
            const float flow=available*fraction;
            transfers[i]-=flow;
            transfers[next]+=flow;
        }
        for(unsigned i=0;i<CellCount;++i){
            auto& s=values_[i];
            s.water=std::max(0.f,s.water+transfers[i]);
            s.wetness=std::max(s.wetness,
                               std::min(1.f,s.water*120.f));
        }
    }
}

double SurfaceTile::StoredWater() const{
    double total=0;
    for(const auto& s:values_)total+=s.snow+s.water;
    return total;
}
} // namespace lab::urban
