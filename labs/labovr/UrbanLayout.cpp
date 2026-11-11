#include "UrbanLayout.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace lab::urban {
namespace {
double Fraction(double v) { return v - std::floor(v); }
double Fade(double t) { return t*t*t*(t*(t*6.0-15.0)+10.0); }
double Mix(double a,double b,double t) { return a+(b-a)*t; }
uint64_t Pack(int64_t a) { return static_cast<uint64_t>(a); }
double Gradient(int64_t x,int64_t z,double dx,double dz,uint64_t seed) {
    static constexpr std::array<std::array<double,2>,8> g{{
        {{1,0}},{{0,1}},{{-1,0}},{{0,-1}},
        {{.7071067811865476,.7071067811865476}},
        {{-.7071067811865476,.7071067811865476}},
        {{-.7071067811865476,-.7071067811865476}},
        {{.7071067811865476,-.7071067811865476}}
    }};
    const auto h=Hash(Hash(Pack(x)^seed)^Hash(Pack(z)+0x9e3779b97f4a7c15ull));
    const auto& v=g[h&7u];
    return v[0]*dx+v[1]*dz;
}
int Side(int64_t globalIndex) { return (static_cast<uint64_t>(globalIndex)&1u)?-1:1; }
} // namespace

uint64_t Hash(uint64_t value) {
    value+=0x9e3779b97f4a7c15ull;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ull;
    value=(value^(value>>27))*0x94d049bb133111ebull;
    return value^(value>>31);
}
Tile TileAt(Point p) {
    return {static_cast<int64_t>(std::floor((p.x-GridOffsetX)/BlockMeters)),
            static_cast<int64_t>(std::floor((p.z-GridOffsetZ)/BlockMeters))};
}
Point Origin(Tile tile) {
    return {GridOffsetX+double(tile.x)*BlockMeters,
            GridOffsetZ+double(tile.z)*BlockMeters};
}
uint64_t Identity(Kind kind,Tile tile,uint32_t ordinal) {
    auto h=Hash(WorldSeed^static_cast<uint64_t>(kind));
    h=Hash(h^Pack(tile.x));
    h=Hash(h^Pack(tile.z));
    return Hash(h^ordinal);
}
double Perlin(double x,double z,uint64_t seed) {
    const double fx=std::floor(x), fz=std::floor(z);
    const auto i=static_cast<int64_t>(fx), k=static_cast<int64_t>(fz);
    const double u=Fraction(x),v=Fraction(z);
    const double a=Gradient(i,k,u,v,seed);
    const double b=Gradient(i+1,k,u-1,v,seed);
    const double c=Gradient(i,k+1,u,v-1,seed);
    const double d=Gradient(i+1,k+1,u-1,v-1,seed);
    return Mix(Mix(a,b,Fade(u)),Mix(c,d,Fade(u)),Fade(v));
}
double GroundHeight(Point p) {
    // Exact counterpart in UrbanMeshes.py; continuous grade and tile-periodic relief.
    const double x=p.x-GridOffsetX,z=p.z-GridOffsetZ;
    constexpr double k=6.283185307179586476925286766559/BlockMeters;
    return .0025*(x+z)
          +.085*(1.-std::cos(k*x))+.055*(1.-std::cos(k*z))
          +.025*(1.-std::cos(4.*k*x))+.018*(1.-std::cos(4.*k*z))
          +.006*std::sin(8.*k*x)*std::sin(8.*k*z);
}
std::vector<Feature> GenerateTile(Tile tile) {
    std::vector<Feature> out;
    out.reserve(96);
    const auto o=Origin(tile);
    auto add=[&](Kind kind,Axis axis,Point position,int side,double extent,
                 double height,uint32_t ordinal) {
        out.push_back({Identity(kind,tile,ordinal),kind,axis,position,side,extent,height});
    };
    add(Kind::Intersection,Axis::None,o,0,2*RoadHalfWidth,GroundHeight(o),0);
    add(Kind::RoadEast,Axis::East,{o.x+BlockMeters*.5,o.z},0,
        BlockMeters-2*RoadHalfWidth,GroundHeight({o.x+60,o.z}),0);
    add(Kind::RoadNorth,Axis::North,{o.x,o.z+BlockMeters*.5},0,
        BlockMeters-2*RoadHalfWidth,GroundHeight({o.x,o.z+60}),0);

    // Sidewalks at +/- 7 m. No object extends into intersection corners.
    for(uint32_t i=0;i<2;++i) {
        const int side=i?-1:1;
        add(Kind::Sidewalk,Axis::East,{o.x+60,o.z+side*7},side,108,
            GroundHeight({o.x+60,o.z+side*7}),i);
        add(Kind::Sidewalk,Axis::North,{o.x+side*7,o.z+60},side,108,
            GroundHeight({o.x+side*7,o.z+60}),2+i);
    }
    // Lamp phase is global: x*6+k or z*6+k, including negative indices.
    // Offsets 10,30,...110 leave exactly 20 m across tile boundaries.
    for(int k=0;k<6;++k) {
        const double d=10.0+LampPitch*k;
        const int eastSide=Side(tile.x*6+k);
        const int northSide=Side(tile.z*6+k);
        add(Kind::Lamp,Axis::East,{o.x+d,o.z+eastSide*8.8},
            eastSide,0,6.8,static_cast<uint32_t>(k));
        add(Kind::Lamp,Axis::North,{o.x+northSide*8.8,o.z+d},
            northSide,0,6.8,static_cast<uint32_t>(k+6));
        if((k&1)==0) {
            add(Kind::Drain,Axis::East,{o.x+d,o.z+eastSide*5.65},
                eastSide,.55,GroundHeight({o.x+d,o.z+eastSide*5.65}),static_cast<uint32_t>(k));
            add(Kind::Drain,Axis::North,{o.x+northSide*5.65,o.z+d},
                northSide,.55,GroundHeight({o.x+northSide*5.65,o.z+d}),static_cast<uint32_t>(k+6));
        }
    }
    // Inner-block wall: 2 m beyond the outer edge of the 2 m sidewalk.
    // Four faces belong to the interior owner, never to adjacent street tiles.
    const double inset=RoadHalfWidth+SidewalkWidth+WallSetback;
    const double mid=BlockMeters*.5;
    for(uint32_t i=0;i<4;++i) {
        const bool alongX=i<2;
        const double coord=(i&1)?BlockMeters-inset:inset;
        Point p=alongX?Point{o.x+mid,o.z+coord}:Point{o.x+coord,o.z+mid};
        add(Kind::Wall,alongX?Axis::East:Axis::North,p,
            (i&1)?1:-1,BlockMeters-2*inset,2.43,i);
    }
    for(uint32_t i=0;i<46;++i) {
        // Dense enough to read as a treeline, with varying stable silhouettes.
        const auto seed=Identity(Kind::Deciduous,tile,i);
        const double x=inset+9.0+(double(Hash(seed)&0xffffu)/65535.0)*(BlockMeters-2*inset-18);
        const double z=inset+9.0+(double(Hash(seed>>1)&0xffffu)/65535.0)*(BlockMeters-2*inset-18);
        const bool pine=(i%3)==0;
        const Point p{o.x+x,o.z+z};
        add(pine?Kind::Conifer:Kind::Deciduous,Axis::None,p,0,
            2.0+double((seed>>17)&7u)*.34,10.5+double((seed>>24)&15u)*.48,i);
    }
    return out;
}
double DrainHeat(Point position,const std::vector<Feature>& sources) {
    double heat=0;
    for(const auto& f:sources)if(f.kind==Kind::Drain) {
        const double dx=position.x-f.position.x,dz=position.z-f.position.z;
        heat+=5.0*std::exp(-(dx*dx+dz*dz)/(2*2.7*2.7));
    }
    return heat;
}
double LampHeat(Point position,const std::vector<Feature>& sources) {
    double heat=0;
    for(const auto& f:sources)if(f.kind==Kind::Lamp) {
        const double dx=position.x-f.position.x,dz=position.z-f.position.z;
        heat+=1.7*std::exp(-(dx*dx+dz*dz)/(2*4.2*4.2));
    }
    return heat;
}
std::vector<Flash> FlashEvents(uint64_t id,double from,double to) {
    if(!(to>from)||to<0) return {};
    const uint64_t h=Hash(id^0x47514c49544348ull);
    const double period=17.0+double(h%1700)/100.0; // 17 .. 33.99 s
    const double phase=double((h>>16)%1000)/1000.0*period;
    const int64_t first=static_cast<int64_t>(std::floor((from-phase)/period))-1;
    const int64_t last=static_cast<int64_t>(std::ceil((to-phase)/period))+1;
    std::vector<Flash> events;
    for(int64_t k=std::max<int64_t>(0,first);k<=last;++k) {
        const double start=phase+double(k)*period;
        const double duration=.035+double(Hash(h^uint64_t(k))%160)/1000.0;
        // Start inside (from,to] is the single audio event trigger.
        if(start>from&&start<=to)events.push_back({Hash(id^uint64_t(k)),start,start+duration});
    }
    return events;
}
bool LampOff(uint64_t id,double seconds) {
    if(seconds<0)return false;
    const uint64_t h=Hash(id^0x47514c49544348ull);
    const double period=17.0+double(h%1700)/100.0;
    const double phase=double((h>>16)%1000)/1000.0*period;
    const double cycle=std::floor((seconds-phase)/period);
    if(cycle<0)return false;
    const double start=phase+cycle*period;
    const double duration=.035+double(Hash(h^uint64_t(cycle))%160)/1000.0;
    return seconds>=start&&seconds<start+duration;
}
} // namespace lab::urban
