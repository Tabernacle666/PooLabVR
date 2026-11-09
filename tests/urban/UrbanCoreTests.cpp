#include "../../labs/labovr/UrbanLayout.h"
#include "../../labs/labovr/UrbanSurface.h"
#include <cassert>
#include <cmath>
#include <set>
#include <utility>
#include <iostream>
using namespace lab::urban;
int main() {
    assert(GridOffsetX==-16.0&&GridOffsetZ==13.0);
    assert((TileAt({0.,0.})==Tile{0,-1})); // The laboratory is off the road.
    assert((TileAt({-0.01,-120.01})==Tile{0,-2}));
    assert((TileAt({120.,120.})==Tile{1,0}));
    for(int x=-3;x<=3;++x)for(int z=-3;z<=3;++z) {
        const Tile tile{x,z};
        assert(TileAt(Origin(tile))==tile);
        assert((TileAt({Origin(tile).x-.001,Origin(tile).z-.001})==
                Tile{x-1,z-1}));
    }
    for(int x=-3;x<=3;++x)for(int z=-3;z<=3;++z) {
        const Tile t{x,z};
        const auto a=GenerateTile(t), b=GenerateTile(t);
        assert(a.size()==b.size());
        std::set<uint64_t> ids;
        for(unsigned i=0;i<a.size();++i) {
            assert(a[i].id==b[i].id);
            assert(ids.insert(a[i].id).second);
        }
        assert(std::abs(GroundHeight({120.0*x,120.0*z}) -
                        GroundHeight({120.0*x,120.0*z}))<1e-12);
        const auto next=GenerateTile({x+1,z});
        std::vector<Feature> lampsA,lampsB;
        for(const auto& f:a)if(f.kind==Kind::Lamp&&f.axis==Axis::East)lampsA.push_back(f);
        for(const auto& f:next)if(f.kind==Kind::Lamp&&f.axis==Axis::East)lampsB.push_back(f);
        assert(lampsA.size()==6&&lampsB.size()==6);
        assert(std::abs(lampsB.front().position.x-lampsA.back().position.x-20.)<1e-9);
        assert(lampsA.back().side==-lampsB.front().side);
        // Across neighbours, no identities may be duplicated.
        for(const auto& f:next)assert(!ids.contains(f.id));
    }
    const auto features=GenerateTile({-1,-1});
    uint64_t lamp=0;
    for(const auto& f:features)if(f.kind==Kind::Lamp){lamp=f.id;break;}
    assert(lamp);
    const auto flashes=FlashEvents(lamp,0.,300.);
    assert(flashes.size()>=7);
    for(const auto& f:flashes) {
        assert(f.ends>f.begins);
        assert(LampOff(lamp,(f.begins+f.ends)*.5));
        // A giant 300-second frame must still report each audio trigger.
    }
    SurfaceTile s({0,0});
    const double before=s.StoredWater()+s.DischargedWater();
    s.Advance(10.,0.,0.);
    const double after=s.StoredWater()+s.DischargedWater();
    assert(std::abs(before-after)<1e-4);
    s.Advance(1.,.00001,0.);
    assert(s.StoredWater()+s.DischargedWater()>before);
    // Weather is a function of elapsed time, not the render frame rate.
    SurfaceTile whole({0,0}),partitioned({0,0});
    whole.Advance(12.,WinterSnowfallMetersPerSecond,WinterAirTemperatureC);
    for(int i=0;i<12;++i)
        partitioned.Advance(1.,WinterSnowfallMetersPerSecond,WinterAirTemperatureC);
    for(unsigned z=0;z<SurfaceTile::Resolution;++z)
        for(unsigned x=0;x<SurfaceTile::Resolution;++x){
            const auto& a=whole.At(x,z);
            const auto& b=partitioned.At(x,z);
            assert(std::abs(a.snow-b.snow)<1e-7f);
            assert(std::abs(a.water-b.water)<1e-7f);
            assert(std::abs(a.wetness-b.wetness)<1e-7f);
        }
    // Bilinear weather sampling is continuous and drives every surface
    // effect from the same field, including the runoff near a warm drain.
    const auto origin=Origin({0,0});
    const Point p{origin.x+10.,origin.z+6.};
    const auto sample=whole.AtWorld(p);
    const auto appearance=whole.AppearanceAt(p);
    assert(std::isfinite(sample.snow)&&std::isfinite(sample.water));
    assert(appearance.snowCoverage>=0&&appearance.snowCoverage<=2.4f);
    assert(appearance.wetFilm>=0&&appearance.wetFilm<=1.f);
    assert(appearance.runoff>=.1f&&appearance.runoff<=1.f);
    // The directed water field actually discharges a local reservoir.
    SurfaceTile runoff({0,0});
    runoff.At(8,4).water+=.025f;
    const double initial=runoff.StoredWater()+runoff.DischargedWater();
    runoff.Advance(15.,0.,0.);
    const double final=runoff.StoredWater()+runoff.DischargedWater();
    assert(std::abs(initial-final)<1e-3);
    assert(runoff.DischargedWater()>.001);
    std::cout<<"urban invariants: PASS\n";
}
