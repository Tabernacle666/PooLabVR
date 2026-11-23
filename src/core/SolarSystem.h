#pragma once
#include "SphereGeometry.h"

namespace OASIS {

inline constexpr uint32_t SolarPlanetCount=9u;
inline constexpr uint32_t SolarBodyCount=SolarPlanetCount;
inline constexpr uint32_t OasisOrbitSlot=2u;
inline constexpr float SunRadius=2.f,SunClearance=30.f;
inline constexpr float SolarOrbitSpacing=18.f;
inline constexpr float FirstSolarOrbit=SunRadius+SunClearance+SpectacleRadius;
inline constexpr float OasisOrbitRadius=FirstSolarOrbit+OasisOrbitSlot*SolarOrbitSpacing;
inline constexpr float SunLightRange=FirstSolarOrbit+(SolarPlanetCount-1u)*SolarOrbitSpacing+60.f;
inline constexpr float SunPower=16000.f*(OasisOrbitRadius/65.f)*(OasisOrbitRadius/65.f);
inline constexpr XMFLOAT3 SunColor{1.f,.84f,.63f};
inline constexpr uint32_t SolarSeed=0x4f415349u;
inline constexpr double SolarSpinPeriod=420.;
inline constexpr double SolarTurn=6.2831853071795864769;
static_assert(SolarBodyCount<=SpectacleSphereCount);

struct SolarOrbit {
    float radius,inclination;
    double node,phase,spinPhase,period;
};

inline float SolarRandom(uint32_t& state) noexcept {
    state=1664525u*state+1013904223u;
    return float(state>>8u)/16777216.f;
}

inline constexpr uint32_t SolarOrbitSlot(uint32_t owner) noexcept {
    return owner==0u?OasisOrbitSlot:owner<=OasisOrbitSlot?owner-1u:owner;
}

inline constexpr float SolarBodyRadius([[maybe_unused]] uint32_t owner) noexcept {
    return SpectacleRadius;
}

inline constexpr float SolarInteriorUnitsPerMeter([[maybe_unused]] uint32_t owner) noexcept {
    return SpectacleInteriorUnitsPerMeter;
}

inline const auto SolarOrbits=[]{
    // Keep the bodies easy to intercept at normal shuttle thrust.
    constexpr double oasisPeriod=1800.*OasisOrbitRadius/65.;
    constexpr std::array inclinations{7.f,4.f,0.f,9.f,15.f,20.f,27.f,18.f,33.f};
    std::array<SolarOrbit,SolarPlanetCount> orbits{};
    for(uint32_t owner=0;owner<SolarPlanetCount;++owner){
        const auto slot=SolarOrbitSlot(owner);
        uint32_t seed=SolarSeed+owner*0x9e3779b9u;
        const double node=SolarTurn*SolarRandom(seed),phase=SolarTurn*SolarRandom(seed);
        const double spinPhase=SolarTurn*SolarRandom(seed);
        const float radius=FirstSolarOrbit+float(slot)*SolarOrbitSpacing;
        orbits[owner]={radius,XMConvertToRadians(inclinations[slot]),owner?node:0.,
            owner?phase:0.,owner?spinPhase:0.,oasisPeriod*std::pow(double(radius)/OasisOrbitRadius,1.5)};
    }
    return orbits;
}();

inline double SolarOrbitAngle(const SolarOrbit& orbit,double seconds) noexcept {
    return orbit.phase+SolarTurn*std::fmod(seconds,orbit.period)/orbit.period;
}

inline XMFLOAT3 SolarOrbitalVector(const SolarOrbit& orbit,double x,double z) noexcept {
    const double tiltedZ=z*std::cos(orbit.inclination);
    const double c=std::cos(orbit.node),s=std::sin(orbit.node);
    return {float(x*c-tiltedZ*s),float(z*std::sin(orbit.inclination)),float(x*s+tiltedZ*c)};
}

inline XMFLOAT3 SolarCenter(uint32_t owner,double seconds) noexcept {
    const auto& orbit=SolarOrbits[owner];
    const double angle=SolarOrbitAngle(orbit,seconds);
    return SolarOrbitalVector(orbit,orbit.radius*std::cos(angle),orbit.radius*std::sin(angle));
}

inline Pose SolarBody(uint32_t owner,double seconds) noexcept {
    Pose body{SolarCenter(owner,seconds)};
    const auto& orbit=SolarOrbits[owner];
    const float spin=float(orbit.spinPhase+SolarTurn*std::fmod(seconds,SolarSpinPeriod)/SolarSpinPeriod);
    // Keep the axis fixed in the cosmos while the body spins around its local up.
    const auto spinAndAxis=XMMatrixRotationY(spin)*XMMatrixRotationZ(orbit.inclination+.13f)*
        XMMatrixRotationY(float(orbit.node));
    XMStoreFloat4(&body.q,XMQuaternionRotationMatrix(spinAndAxis));
    return body;
}

inline XMFLOAT3 SolarTransportVelocity(uint32_t owner,const Pose& body,double seconds,XMFLOAT3 point) noexcept {
    const auto& orbit=SolarOrbits[owner];
    const double angle=SolarOrbitAngle(orbit,seconds);
    const double speed=orbit.radius*SolarTurn/orbit.period;
    const auto orbitVelocity=SolarOrbitalVector(orbit,-speed*std::sin(angle),speed*std::cos(angle));
    const auto omega=XMVector3Rotate(XMVectorSet(0.f,float(SolarTurn/SolarSpinPeriod),0.f,0.f),
        XMLoadFloat4(&body.q));
    const auto arm=Sub(point,body.p);
    XMFLOAT3 spinVelocity;
    XMStoreFloat3(&spinVelocity,XMVector3Cross(omega,XMLoadFloat3(&arm)));
    return Add(orbitVelocity,spinVelocity);
}

inline XMMATRIX SolarBodyToCosmos(uint32_t owner,const Pose& body) noexcept {
    const auto center=SpectacleCenters[owner];
    return XMMatrixTranslation(-center.x,-center.y,-center.z)*Matrix(body);
}

inline XMFLOAT3 SolarSceneCenter(uint32_t owner,double seconds) noexcept {
    return seconds>=0.?SolarCenter(owner,seconds):SpectacleCenters[owner];
}

inline constexpr float SolarSceneRadius(uint32_t owner,double seconds) noexcept {
    return seconds>=0.?SolarBodyRadius(owner):SpectacleRadius;
}

inline XMFLOAT3 SolarTerrainColor(uint32_t owner) noexcept {
    constexpr std::array<XMFLOAT3,SolarPlanetCount-1u> colors{{
        {.50f,.25f,.15f},{.28f,.45f,.51f},{.26f,.43f,.25f},{.51f,.29f,.41f},
        {.36f,.41f,.56f},{.53f,.41f,.22f},{.26f,.48f,.42f},{.49f,.47f,.54f}}};
    return colors[owner-1u];
}

inline double SolarElapsed(int64_t displayTime,int64_t& origin) noexcept {
    if(!origin)origin=displayTime;
    return std::max(0.,double(displayTime-origin)*1.e-9);
}

inline Pose SolarLookAt(XMFLOAT3 position,XMFLOAT3 target) noexcept {
    const auto d=Sub(target,position);
    Pose pose{position};
    const float pitch=-std::atan2(d.y,std::hypot(d.x,d.z));
    XMStoreFloat4(&pose.q,XMQuaternionRotationRollPitchYaw(pitch,std::atan2(d.x,d.z),0.f));
    return pose;
}

inline Pose SolarOasisApproach(double seconds) noexcept {
    const auto body=SolarBodyToCosmos(0u,SolarBody(0u,seconds));
    return SolarLookAt(TransformPoint({0.f,1.55f,-2.6f},body),
                       TransformPoint({0.f,1.55f,5.5f},body));
}

inline XMVECTOR SolarInverseRotation(uint32_t owner,double seconds) noexcept {
    if(seconds<0.)return XMQuaternionIdentity();
    const auto body=SolarBody(owner,seconds);
    return XMQuaternionConjugate(XMLoadFloat4(&body.q));
}

}
