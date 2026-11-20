#pragma once
#include "Core.h"

namespace OASIS {
enum class RightHandTool : uint32_t { Basalt, Scoria, Empty };

struct LaserRay { XMFLOAT3 origin{},end{}; };
struct LaserAfterimage { LaserRay ray; float strength{}; };
inline constexpr uint32_t LaserAfterimageCount=6;
inline constexpr float LaserReach=24.f;

inline float LaserReachToMembrane(XMFLOAT3 origin,XMFLOAT3 direction,XMFLOAT3 center,float radius) noexcept {
    const auto offset=Sub(origin,center);
    const float along=Dot(offset,direction);
    const auto nearest=Sub(offset,Scale(direction,along));
    const float chordSquared=radius*radius-Dot(nearest,nearest);
    return chordSquared>0.f?std::max(0.f,-along+std::sqrt(chordSquared)):0.f;
}

struct LaserSweep {
    static constexpr uint32_t MaxPlanes=28; // Convex hull of up to 16 corners.
    XMFLOAT3 center{},lower{},upper{};
    uint32_t count{};
    std::array<XMFLOAT4,MaxPlanes> planes{};
    // XYZ is the scan axis in each face plane, W and limits are its U range.
    std::array<XMFLOAT4,MaxPlanes> axes{};
    std::array<XMFLOAT4,MaxPlanes> limits{};
};
LaserSweep MakeLaserSweep(const LaserRay& previous,const LaserRay& current,float unitsPerMeter=1.f);
}
