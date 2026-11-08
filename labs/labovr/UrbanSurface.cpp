#include "UrbanSurface.h"
#include <algorithm>
#include <cmath>

namespace lab::urban {
SurfaceTile::SurfaceTile(Tile tile):key_(tile) {
    const auto origin=Origin(tile);
    const auto features=GenerateTile(tile);
    for(unsigned z=0;z<Resolution;++z)for(unsigned x=0;x<Resolution;++x) {
        const Point p{origin.x+(x+.5)*BlockMeters/Resolution,
                      origin.z+(z+.5)*BlockMeters/Resolution};
        auto& s=At(x,z);
        s.height=static_cast<float>(GroundHeight(p));
        s.heat=static_cast<float>(DrainHeat(p,features)+LampHeat(p,features)*.35);
        const double mask=.5+.5*Perlin(p.x/17.,p.z/17.,WorldSeed^777);
        s.snow=static_cast<float>(.001+.012*std::clamp(mask,0.,1.));
        s.wetness=.1f;
        for(const auto& f:features)if(f.kind==Kind::Drain) {
            const double dx=f.position.x-p.x,dz=f.position.z-p.z;
            if(dx*dx+dz*dz<3.5*3.5)drains_[z*Resolution+x]=true;
        }
    }
}
void SurfaceTile::Advance(double elapsedSeconds,double snowfall,double temperature) {
    // Explicitly bounded, variable-frame-rate independent substeps. All flux
    // transfers refer to the previous substep so update order is immaterial.
    if(!(elapsedSeconds>0))return;
    elapsedSeconds=std::min(elapsedSeconds,600.0);
    snowfall=std::clamp(snowfall,0.0,.002);
    while(elapsedSeconds>0) {
        const double dt=std::min(elapsedSeconds,1.0);
        elapsedSeconds-=dt;
        std::array<float,Resolution*Resolution> transfer{};
        for(unsigned z=0;z<Resolution;++z)for(unsigned x=0;x<Resolution;++x) {
            const auto i=z*Resolution+x;
            auto& s=values_[i];
            s.snow+=static_cast<float>(snowfall*dt);
            const double meltRate=std::max(0.,temperature+double(s.heat))*.000009;
            const float melt=std::min(s.snow,static_cast<float>(meltRate*dt));
            s.snow-=melt;s.water+=melt;
            // Surface film retains a trace of water even after runoff.
            s.wetness=std::clamp(s.wetness+float(melt*20.0*dt)-float(.003*dt),0.f,1.f);
            if(drains_[i]) {
                const float out=std::min(s.water,float(.045*dt));
                s.water-=out;discharged_+=out;
                continue;
            }
            unsigned best=i;
            float level=s.height+s.water;
            auto consider=[&](unsigned nx,unsigned nz) {
                const unsigned n=nz*Resolution+nx;
                const float other=values_[n].height+values_[n].water;
                if(other<level-.00001f){level=other;best=n;}
            };
            if(x>0)consider(x-1,z);
            if(x+1<Resolution)consider(x+1,z);
            if(z>0)consider(x,z-1);
            if(z+1<Resolution)consider(x,z+1);
            if(best!=i) {
                const float flow=std::min(s.water,
                    std::max(0.f,(s.height+s.water-level)*.12f*float(dt)));
                transfer[i]-=flow;transfer[best]+=flow;
            }
        }
        for(unsigned i=0;i<values_.size();++i) {
            values_[i].water=std::max(0.f,values_[i].water+transfer[i]);
            values_[i].wetness=std::max(values_[i].wetness,
                                        std::min(1.f,values_[i].water*120.f));
        }
    }
}
double SurfaceTile::StoredWater() const {
    double total=0;
    for(const auto& s:values_)total+=s.snow+s.water;
    return total;
}
} // namespace lab::urban
