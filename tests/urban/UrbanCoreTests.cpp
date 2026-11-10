#include "../../labs/labovr/UrbanLayout.h"
#include "../../labs/labovr/UrbanSurface.h"
#include "../../labs/labovr/VegetationDetail.h"
#include <cassert>
#include <cmath>
#include <set>
#include <utility>
#include <iostream>
#include <memory>
using namespace lab::urban;
int main() {
    static_assert(TreeDetailAt(0.)==TreeDetail::Close);
    static_assert(TreeDetailAt(30.*30.)==TreeDetail::Close);
    static_assert(TreeDetailAt(30.01*30.01)==TreeDetail::Middle);
    static_assert(TreeDetailAt(80.*80.)==TreeDetail::Middle);
    static_assert(TreeDetailAt(80.01*80.01)==TreeDetail::Distant);
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
    // Nonperiodic city grade: 30 cm per block in either axis.
    for(int i=-2;i<=2;++i)for(int j=-2;j<=2;++j) {
        const Point a{GridOffsetX+120.*i,GridOffsetZ+120.*j};
        assert(std::abs(GroundHeight({a.x+120.,a.z})-GroundHeight(a)-.3)<1e-9);
        assert(std::abs(GroundHeight({a.x,a.z+120.})-GroundHeight(a)-.3)<1e-9);
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
    // Resident eviction/reentry preserves live weather and discharge.
    const auto checkpoint=runoff.SaveWeather();
    auto revived=std::make_unique<SurfaceTile>(Tile{0,0});
    revived->RestoreWeather(checkpoint);
    assert(std::abs(revived->StoredWater()-runoff.StoredWater())<1e-6);
    assert(std::abs(revived->DischargedWater()-runoff.DischargedWater())<1e-6);
    runoff.Advance(5.,WinterSnowfallMetersPerSecond,WinterAirTemperatureC);
    revived->Advance(5.,WinterSnowfallMetersPerSecond,WinterAirTemperatureC);
    assert(std::abs(revived->StoredWater()-runoff.StoredWater())<1e-6);
    assert(std::abs(revived->DischargedWater()-runoff.DischargedWater())<1e-6);
    std::cout<<"urban invariants: PASS\n";
}
