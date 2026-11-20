#pragma once
#include "SolarSystem.h"
#include <limits>

namespace OASIS {
inline constexpr float SpectacleInteriorUnitsPerMeter=.103f/3.f;
inline constexpr float SpectacleInteriorFarMeters=260.f;

inline bool InsideSpectacle(XMFLOAT3 position) noexcept {
    const auto offset=Sub(position,SpectacleCenter);
    return Dot(offset,offset)<SpectacleRadius*SpectacleRadius;
}

inline uint32_t NearestSpectacle(XMFLOAT3 position,uint32_t count,double solarTime=-1.) noexcept {
    uint32_t nearest=0;
    float distance=std::numeric_limits<float>::max();
    for(uint32_t i=0;i<count;++i){
        const auto offset=Sub(position,SolarSceneCenter(i,solarTime));
        const float squared=Dot(offset,offset);
        if(squared<distance){nearest=i;distance=squared;}
    }
    return nearest;
}

class SpectaclePassage {
    uint32_t inside_{Invalid},count_{1u};
public:
    void SetCount(uint32_t count) noexcept {count_=count;}
    void Reset(bool inside) noexcept {inside_=inside?0u:Invalid;}
    void Locate(XMFLOAT3 position,double solarTime=-1.) noexcept {
        const auto index=NearestSpectacle(position,count_,solarTime);
        const auto offset=Sub(position,SolarSceneCenter(index,solarTime));
        inside_=Dot(offset,offset)<SpectacleRadius*SpectacleRadius?index:Invalid;
    }
    bool Inside() const noexcept {return inside_!=Invalid;}
    uint32_t Sphere() const noexcept {return inside_;}
    float UnitsPerMeter() const noexcept {return Inside()?SpectacleInteriorUnitsPerMeter:1.f;}

    XMFLOAT3 Move(XMFLOAT3 position,XMFLOAT3 worldAlignedMeters,FrameInput& input,double previousTime=-1.) noexcept {
        // Opposite edges of a thin band prevent the tracker from retriggering
        // at the same surface. Position stays continuous at either edge.
        constexpr double seamMeters=.004;
        input.crossingCount=0;
        const double endTime=input.solarTime;
        double startTime=previousTime>=0.?previousTime:endTime;
        for(auto& crossing:input.crossings){
            const float units=UnitsPerMeter();
            const auto step=Scale(worldAlignedMeters,units);
            if(startTime==endTime&&Dot(step,step)==0.f)break;
            const bool exiting=Inside();
            const double radius=SpectacleRadius+(exiting?1.:-1.)*seamMeters*units;
            double first=2.;
            uint32_t owner=Invalid;
            for(uint32_t index=0;index<count_;++index){
                if(exiting&&index!=inside_)continue;
                const auto center=SolarSceneCenter(index,startTime);
                const auto motion=Sub(SolarSceneCenter(index,endTime),center);
                const auto relative=Sub(step,motion);
                const double dx=relative.x,dy=relative.y,dz=relative.z;
                const double a=dx*dx+dy*dy+dz*dz;
                if(a==0.)continue;
                const auto offset=Sub(position,center);
                const double x=offset.x,y=offset.y,z=offset.z;
                const double b=x*dx+y*dy+z*dz,c=x*x+y*y+z*z-radius*radius;
                const double disc=b*b-a*c;
                const bool across=exiting?(c>0.&&b>0.):(c<0.&&b<0.);
                if(disc<=0.&&!across)continue;
                const double root=across?0.:(-b+(exiting?1.:-1.)*std::sqrt(disc))/a;
                if(root>=0.&&root<=1.&&root<first){first=root;owner=index;}
            }
            if(owner==Invalid){
                position=Add(position,step);
                worldAlignedMeters={};
                break;
            }
            position=Add(position,Scale(step,float(first)));
            worldAlignedMeters=Scale(worldAlignedMeters,float(1.-first));
            inside_=exiting?Invalid:owner;
            startTime=std::lerp(startTime,endTime,first);
            const auto center=SolarSceneCenter(owner,startTime);
            crossing={Add(center,Scale(Unit(Sub(position,center)),SpectacleRadius)),!exiting,owner,startTime};
            ++input.crossingCount;
        }
        input.sceneUnitsPerMeter=UnitsPerMeter();
        input.insideSphere=inside_;
        input.farClipMeters=Inside()?SpectacleInteriorFarMeters:FarZ;
        return Add(position,Scale(worldAlignedMeters,input.sceneUnitsPerMeter));
    }
};
}
