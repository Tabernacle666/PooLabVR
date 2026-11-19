#pragma once
#include "Core.h"

namespace OASIS {
inline const XMFLOAT3 SpectacleCenter{0.f,1.f,5.5f};
inline constexpr float SpectacleRadius=3.8f;
inline constexpr float SpectacleInteriorUnitsPerMeter=.103f/3.f;
inline constexpr float SpectacleInteriorFarMeters=260.f;

inline bool InsideSpectacle(XMFLOAT3 position) noexcept {
    const auto offset=Sub(position,SpectacleCenter);
    return Dot(offset,offset)<SpectacleRadius*SpectacleRadius;
}

class SpectaclePassage {
    bool inside_{};
public:
    void Reset(bool inside) noexcept {inside_=inside;}
    bool Inside() const noexcept {return inside_;}
    float UnitsPerMeter() const noexcept {return inside_?SpectacleInteriorUnitsPerMeter:1.f;}

    XMFLOAT3 Move(XMFLOAT3 position,XMFLOAT3 worldAlignedMeters,FrameInput& input) noexcept {
        // Opposite edges of a thin band prevent the tracker from retriggering
        // at the same surface. Position stays continuous at either edge.
        constexpr double seamMeters=.004;
        input.crossingCount=0;
        for(auto& crossing:input.crossings){
            const float units=UnitsPerMeter();
            const auto offset=Sub(position,SpectacleCenter);
            const auto step=Scale(worldAlignedMeters,units);
            const double x=offset.x,y=offset.y,z=offset.z;
            const double dx=step.x,dy=step.y,dz=step.z;
            const double a=dx*dx+dy*dy+dz*dz;
            if(a==0.)break;
            const double b=x*dx+y*dy+z*dz;
            const double radius=SpectacleRadius+(inside_?1.:-1.)*seamMeters*units;
            const double c=x*x+y*y+z*z-radius*radius;
            const double discriminant=b*b-a*c;
            // A rounded endpoint may already be across the threshold in the
            // previous mode. Recover at the current point on continued motion.
            const bool across=inside_?(c>0.&&b>0.):(c<0.&&b<0.);
            if(discriminant<=0.&&!across){
                position=Add(position,step);
                worldAlignedMeters={};
                break;
            }
            const double root=across?0.:(-b+(inside_?1.:-1.)*std::sqrt(discriminant))/a;
            if(root<0.||root>1.){
                position=Add(position,step);
                worldAlignedMeters={};
                break;
            }
            position=Add(position,Scale(step,float(root)));
            worldAlignedMeters=Scale(worldAlignedMeters,float(1.-root));
            inside_=!inside_;
            crossing={Add(SpectacleCenter,Scale(Unit(Sub(position,SpectacleCenter)),SpectacleRadius)),
                      inside_};
            ++input.crossingCount;
        }
        input.sceneUnitsPerMeter=UnitsPerMeter();
        input.farClipMeters=inside_?SpectacleInteriorFarMeters:FarZ;
        // A straight segment can cross a sphere at most twice.
        return Add(position,Scale(worldAlignedMeters,input.sceneUnitsPerMeter));
    }
};
}
