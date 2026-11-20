#include "LaserSweep.h"

namespace OASIS {
namespace {
XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
XMFLOAT3 Perpendicular(XMFLOAT3 v){
    return Unit(Cross(v,std::abs(v.y)<.8f?XMFLOAT3{0,1,0}:XMFLOAT3{1,0,0}));
}
struct HullPoint {
    double x{},y{},z{};
};
HullPoint HullSub(HullPoint a,HullPoint b){
    return {a.x-b.x,a.y-b.y,a.z-b.z};
}
HullPoint HullScale(HullPoint v,double scale){
    return {v.x*scale,v.y*scale,v.z*scale};
}
HullPoint HullCross(HullPoint a,HullPoint b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
double HullDot(HullPoint a,HullPoint b){
    return a.x*b.x+a.y*b.y+a.z*b.z;
}
}

LaserSweep MakeLaserSweep(const LaserRay& previousRay,const LaserRay& currentRay,float unitsPerMeter){
    const auto anchor=unitsPerMeter==1.f?XMFLOAT3{}:currentRay.origin;
    const auto toMeters=[&](XMFLOAT3 p){return Scale(Sub(p,anchor),1.f/unitsPerMeter);};
    const LaserRay previous{toMeters(previousRay.origin),toMeters(previousRay.end)};
    const LaserRay current{toMeters(currentRay.origin),toMeters(currentRay.end)};
    // A human-scale kerf remains discernible against a full-sized facade.
    const float halfWidth=unitsPerMeter<1.f?.05f:.02f;
    constexpr double planeEpsilon=1.e-8;
    const auto oldDirection=Unit(Sub(previous.end,previous.origin));
    const auto newDirection=Unit(Sub(current.end,current.origin));
    auto forward=Unit(Add(oldDirection,newDirection));
    if(Length(forward)<.5f)forward=newDirection;
    if(Length(forward)<.5f)return {};
    const auto travel=Add(Sub(current.origin,previous.origin),Sub(current.end,previous.end));
    auto normal=Unit(Cross(forward,travel));
    if(Length(Cross(forward,travel))<.001f)normal=Perpendicular(forward);
    const auto sideways=Unit(Cross(normal,forward));

    const std::array<XMFLOAT3,4> endpoints{previous.origin,previous.end,current.origin,current.end};
    LaserSweep sweep;
    sweep.center=Scale(Add(Add(endpoints[0],endpoints[1]),Add(endpoints[2],endpoints[3])),.25f);
    std::array<HullPoint,16> points{};
    size_t count=0;
    for(auto endpoint:endpoints){
        for(float u:{-halfWidth,halfWidth})for(float v:{-halfWidth,halfWidth}){
            const auto across=Add(Scale(sideways,u),Scale(normal,v));
            const auto precise=HullPoint{double(endpoint.x)+across.x,double(endpoint.y)+across.y,double(endpoint.z)+across.z};
            const auto corner=XMFLOAT3{float(precise.x),float(precise.y),float(precise.z)};
            if(!count)sweep.lower=sweep.upper=corner;
            sweep.lower={std::min(sweep.lower.x,corner.x),std::min(sweep.lower.y,corner.y),
                         std::min(sweep.lower.z,corner.z)};
            sweep.upper={std::max(sweep.upper.x,corner.x),std::max(sweep.upper.y,corner.y),
                         std::max(sweep.upper.z,corner.z)};
            points[count++]=HullSub(precise,{sweep.center.x,sweep.center.y,sweep.center.z});
        }
    }

    struct AcceptedPlane {HullPoint normal;double distance;};
    std::array<AcceptedPlane,LaserSweep::MaxPlanes> accepted{};
    auto addPlane=[&](HullPoint normal,double distance){
        for(uint32_t i=0;i<sweep.count;++i){
            const auto& old=accepted[i];
            if(HullDot(normal,old.normal)>1.-1.e-12&&
               std::abs(distance-old.distance)<1.e-7)return true;
        }
        if(sweep.count==LaserSweep::MaxPlanes)return false;
        accepted[sweep.count]={normal,distance};
        const XMFLOAT3 n{float(normal.x),float(normal.y),float(normal.z)};
        const auto sideways=Cross(forward,n);
        const auto axis=Length(sideways)<.05f?Perpendicular(n):Unit(sideways);
        double lower=std::numeric_limits<double>::max(),upper=-lower,support=-lower;
        for(const auto& vertex:points){
            const double u=HullDot({axis.x,axis.y,axis.z},vertex);
            lower=std::min(lower,u);
            upper=std::max(upper,u);
            support=std::max(support,HullDot({n.x,n.y,n.z},vertex));
        }
        // The remaining hull planes trim the final wall. Near-coplanar
        // supporting planes can share fewer corners after merging, so
        // their individual support corners are not safe scan bounds.
        sweep.planes[sweep.count]={n.x,n.y,n.z,float(support)};
        sweep.axes[sweep.count]={axis.x,axis.y,axis.z,float(lower-.002)};
        sweep.limits[sweep.count]={float(upper+.002),0.f,0.f,0.f};
        ++sweep.count;
        return true;
    };

    // The long interior beam has a thin cross-section. Evaluate supports in
    // double precision to keep nearly coplanar corners on a single face.
    for(size_t a=0;a+2<points.size();++a)
        for(size_t b=a+1;b+1<points.size();++b)
            for(size_t c=b+1;c<points.size();++c){
                auto n=HullCross(HullSub(points[b],points[a]),HullSub(points[c],points[a]));
                const double length=std::sqrt(HullDot(n,n));
                if(length<1.e-10)continue;
                n=HullScale(n,1./length);
                const double face=HullDot(n,points[a]);
                double lo=face,hi=face;
                for(const auto& point:points){
                    const auto projected=HullDot(n,point);
                    lo=std::min(lo,projected);
                    hi=std::max(hi,projected);
                    if(lo<face-planeEpsilon&&hi>face+planeEpsilon)break;
                }
                if(hi<=face+planeEpsilon){
                    if(!addPlane(n,hi))return {};
                }else if(lo>=face-planeEpsilon){
                    if(!addPlane(HullScale(n,-1.),-lo))return {};
                }
            }
    if(sweep.count<4)return {};
    // Fit the thin hull in physical metres, then publish its plane distances
    // and scan bounds in the same coordinates as the cut cache.
    sweep.center=Add(anchor,Scale(sweep.center,unitsPerMeter));
    sweep.lower=Add(anchor,Scale(sweep.lower,unitsPerMeter));
    sweep.upper=Add(anchor,Scale(sweep.upper,unitsPerMeter));
    for(uint32_t i=0;i<sweep.count;++i){
        sweep.planes[i].w*=unitsPerMeter;
        sweep.axes[i].w*=unitsPerMeter;
        sweep.limits[i].x*=unitsPerMeter;
    }
    return sweep;
}
}
