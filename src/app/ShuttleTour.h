#pragma once
#include "SolarSystem.h"

namespace OASIS::ShuttleTour {

inline constexpr double StandbySeconds=4.,FlightSeconds=8.;
// Keep the 4 s warmup and 8 s tour, but approach OASIS directly.
inline constexpr double ApproachEnd=3.8,AtOasis=5.8;
inline constexpr XMFLOAT3 OasisGate{-1.15f,.92f,1.63f};

inline float Blend(double elapsed,double begin,double end) noexcept {
    const float t=float(std::clamp((elapsed-begin)/(end-begin),0.,1.));
    return t*t*(3.f-2.f*t);
}

inline XMFLOAT3 Interpolate(XMFLOAT3 a,XMFLOAT3 b,float t) noexcept {
    return Add(a,Scale(Sub(b,a),t));
}

inline Pose View(double elapsed,double solarTime) noexcept {
    // All waypoints are expressed in OASIS' authored coordinates and follow
    // its solar orbit and spin; the Moon and its former orbit are not needed.
    const auto oasis=SolarBodyToCosmos(0u,SolarBody(0u,solarTime));
    const XMFLOAT3 distant{-1.8f,4.2f,-17.f};
    const XMFLOAT3 approach{-1.7f,2.65f,-4.9f};
    const XMFLOAT3 gate=OasisGate;
    // Measured in the island mesh: land before the tree with logo behind it.
    const XMFLOAT3 seat{-1.15f,.080f,3.64f};
    const XMFLOAT3 landmark{-.64f,.54f,5.50f};

    XMFLOAT3 local{};
    if(elapsed<=ApproachEnd){
        local=Interpolate(distant,approach,Blend(elapsed,0.,ApproachEnd));
    }else if(elapsed<=AtOasis){
        local=Interpolate(approach,gate,Blend(elapsed,ApproachEnd,AtOasis));
    }else{
        local=Interpolate(gate,seat,Blend(elapsed,AtOasis,FlightSeconds));
    }
    // Look at the island during approach, then the selected landmark
    // on entry and landing. Both camera and target follow the rotating body.
    const auto look=Interpolate(SpectacleCenter,landmark,
                                Blend(elapsed,ApproachEnd,AtOasis));
    return SolarLookAt(TransformPoint(local,oasis),TransformPoint(look,oasis));
}

}
