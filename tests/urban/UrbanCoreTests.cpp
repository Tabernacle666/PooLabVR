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
    std::cout<<"urban invariants: PASS\n";
}
