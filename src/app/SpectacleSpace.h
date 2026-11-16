#pragma once
#include "Core.h"
#include <algorithm>
#include <cmath>

namespace OASIS {
inline const XMFLOAT3 SpectacleCenter{0.f,1.f,5.5f};
inline constexpr float SpectacleRadius=3.8f;

// Restrict artificial translation only; never alter physical headset tracking.
inline XMFLOAT3 ConstrainSpectacleMotion(XMFLOAT3 position,XMFLOAT3 motion) noexcept {
    const XMFLOAT3 offset=Sub(position,SpectacleCenter);
    const float a=Dot(motion,motion),c=Dot(offset,offset)-SpectacleRadius*SpectacleRadius;
    if(a<1.e-12f)return motion;
    const float projection=Dot(offset,motion);
    if(c<0.f)return projection<0.f?XMFLOAT3{}:motion;
    if(projection>=0.f)return motion;
    const float discriminant=projection*projection-a*c;
    if(discriminant<0.f)return motion;
    const float entry=(-projection-std::sqrt(discriminant))/a;
    if(entry<0.f||entry>1.f)return motion;
    const float fraction=std::max(0.f,entry-0.001f/std::sqrt(a));
    return Scale(motion,fraction);
}
}
