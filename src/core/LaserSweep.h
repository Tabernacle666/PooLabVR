#pragma once
#include "Core.h"

namespace OASIS {
enum class RightHandTool : uint32_t { Basalt, Scoria, Empty };

struct LaserRay { XMFLOAT3 origin{},end{}; };
struct LaserAfterimage { LaserRay ray; float strength{}; };
inline constexpr uint32_t LaserAfterimageCount=6;
inline constexpr float LaserReach=24.f;

struct LaserSweep {
    static constexpr uint32_t MaxPlanes=28; // Convex hull of up to 16 corners.
    XMFLOAT3 center{},lower{},upper{};
    uint32_t count{};
    std::array<XMFLOAT4,MaxPlanes> planes{};
    // XYZ is the scan axis in each face plane, W and limits are its U range.
    std::array<XMFLOAT4,MaxPlanes> axes{};
    std::array<XMFLOAT4,MaxPlanes> limits{};
};
LaserSweep MakeLaserSweep(const LaserRay& previous,const LaserRay& current);
}
