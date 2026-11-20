#pragma once
#include "SolarSystem.h"

namespace OASIS::ShuttleTour {

inline constexpr double StandbySeconds=4.,FlightSeconds=8.;
inline constexpr double OverMoon=2.6,PastMoon=3.8,AtOasis=5.8;
inline constexpr XMFLOAT3 OasisGate{-1.15f,.92f,1.63f};

inline float Blend(double elapsed,double begin,double end) noexcept {
    const float t=float(std::clamp((elapsed-begin)/(end-begin),0.,1.));
    return t*t*(3.f-2.f*t);
}

inline XMFLOAT3 Interpolate(XMFLOAT3 a,XMFLOAT3 b,float t) noexcept {
    return Add(a,Scale(Sub(b,a),t));
}

inline Pose View(double elapsed,double solarTime) noexcept {
    const auto oasis=SolarBody(0u,solarTime);
    const auto moon=SolarCenter(SolarMoon,solarTime);
    const auto axis=Unit(Sub(moon,oasis.p));
    const auto side=Unit(XMFLOAT3{-axis.z,0.f,axis.x});
    auto lunarPoint=[&](float along,float lateral,float height){
        return Add(Add(moon,Scale(axis,along)),Add(Scale(side,lateral),{0.f,height,0.f}));
    };
    const auto start=lunarPoint(19.f,2.8f,3.2f);
    const auto over=lunarPoint(1.35f,1.1f,2.15f);
    const auto past=lunarPoint(-2.6f,.85f,1.85f);
    const auto body=SolarBodyToCosmos(0u,oasis);
    const auto gate=TransformPoint(OasisGate,body);
    // Measured in the island mesh. The seat is just before the vegetation,
    // with a clear line of sight to the logo at the centre of the island.
    const auto seat=TransformPoint({-1.15f,.080f,3.64f},body);
    const auto landmark=TransformPoint({-.64f,.54f,5.50f},body);
    XMFLOAT3 position{};
    if(elapsed<=OverMoon){
        position=Interpolate(start,over,Blend(elapsed,0.,OverMoon));
    }else if(elapsed<=PastMoon){
        position=Interpolate(over,past,Blend(elapsed,OverMoon,PastMoon));
    }else if(elapsed<=AtOasis){
        // Sweep around the outside so the island is first entered on descent.
        const auto from=Sub(past,oasis.p),to=Sub(gate,oasis.p);
        const float fromYaw=std::atan2(from.x,from.z),toYaw=std::atan2(to.x,to.z);
        const auto t=Blend(elapsed,PastMoon,AtOasis);
        // Keep the same side of the planet for the entire arc, even when
        // the two moving waypoints pass opposite azimuths.
        const double arcStart=solarTime-(elapsed-PastMoon);
        const auto startBody=SolarBody(0u,arcStart);
        const auto startAxis=Unit(Sub(SolarCenter(SolarMoon,arcStart),startBody.p));
        const auto startSide=Unit(XMFLOAT3{-startAxis.z,0.f,startAxis.x});
        const auto startFrom=Add(Scale(startAxis,LunarOrbitRadius-2.6f),Scale(startSide,.85f));
        const auto startTo=Sub(TransformPoint(OasisGate,SolarBodyToCosmos(0u,startBody)),startBody.p);
        const float chosen=std::remainder(std::atan2(startTo.x,startTo.z)-
                                           std::atan2(startFrom.x,startFrom.z),XM_2PI);
        const float sweep=chosen+std::remainder(toYaw-fromYaw-chosen,XM_2PI);
        const float yaw=fromYaw+t*sweep;
        const float radius=std::lerp(std::hypot(from.x,from.z),std::hypot(to.x,to.z),t);
        position=Add(oasis.p,{radius*std::sin(yaw),std::lerp(from.y,to.y,t),radius*std::cos(yaw)});
    }else{
        position=Interpolate(gate,seat,Blend(elapsed,AtOasis,FlightSeconds));
    }
    const auto toOasis=Unit(Sub(oasis.p,position));
    const auto toMoon=Unit(Sub(moon,position));
    const auto approach=Unit(Add(toOasis,toMoon));
    const auto departure=Interpolate(approach,toOasis,Blend(elapsed,1.7,OverMoon));
    const auto direction=Interpolate(departure,Unit(Sub(landmark,position)),
                                      Blend(elapsed,PastMoon,AtOasis));
    return SolarLookAt(position,Add(position,direction));
}

}
