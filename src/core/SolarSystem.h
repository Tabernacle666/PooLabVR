#pragma once
#include "SphereLod.h"

namespace OASIS {

inline constexpr uint32_t SolarPlanetCount=9u;
inline constexpr uint32_t SolarMoon=SolarPlanetCount,SolarBodyCount=SolarPlanetCount+1u;
inline constexpr uint32_t OasisOrbitSlot=2u;
inline constexpr float SunRadius=2.f,SunClearance=30.f;
inline constexpr float SolarLocalExtension=5.f,SolarOrbitSpacing=34.f;
inline constexpr float FirstSolarOrbit=SunRadius+SunClearance+SpectacleRadius;
inline constexpr float OasisOrbitRadius=FirstSolarOrbit+OasisOrbitSlot*SolarOrbitSpacing;
inline constexpr float SunLightRange=FirstSolarOrbit+(SolarPlanetCount-1u)*SolarOrbitSpacing+60.f;
inline constexpr float SunPower=16000.f*(OasisOrbitRadius/65.f)*(OasisOrbitRadius/65.f);
inline constexpr float LunarOrbitRadius=8.f,LunarRadius=1.32f;
inline constexpr float LunarInteriorUnitsPerMeter=1.f/12.f;
inline constexpr double LunarOrbitPeriod=48.;
inline constexpr XMFLOAT3 SunColor{1.f,.84f,.63f};
inline constexpr uint32_t SolarSeed=0x4f415349u;
inline constexpr double SolarSpinPeriod=120.;
inline constexpr double SolarTurn=6.2831853071795864769;
static_assert(SolarBodyCount<=SpectacleSphereCount);
static_assert(LunarOrbitRadius>SpectacleRadius+LunarRadius);
static_assert(2.f*(SphereLodSettings{}.bands[0].exitDistance+SolarLocalExtension+SpectacleRadius)<SolarOrbitSpacing);

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

inline constexpr float SolarBodyRadius(uint32_t owner) noexcept {
    return owner==SolarMoon?LunarRadius:SpectacleRadius;
}

inline constexpr float SolarInteriorUnitsPerMeter(uint32_t owner) noexcept {
    return owner==SolarMoon?LunarInteriorUnitsPerMeter:SpectacleInteriorUnitsPerMeter;
}

inline float SolarLocalSurfaceRange(const SphereLodSettings& settings,bool keep,uint32_t owner) noexcept {
    const auto& band=settings.bands[0];
    // Leave a gap between adjacent planets' largest native-render intervals.
    const float maxRange=.5f*SolarOrbitSpacing-SpectacleRadius-.1f;
    const float range=std::min(maxRange,(keep?band.exitDistance:band.enterDistance)+SolarLocalExtension);
    return range*(SolarBodyRadius(owner)/SpectacleRadius);
}

inline const auto SolarOrbits=[]{
    // Keep OASIS's orbital travel speed while expanding the orbital rings.
    constexpr double oasisPeriod=240.*OasisOrbitRadius/65.;
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

inline double LunarOrbitAngle(double seconds) noexcept {
    return .85-SolarTurn*std::fmod(seconds,LunarOrbitPeriod)/LunarOrbitPeriod;
}

inline XMFLOAT3 LunarOrbitalVector(double x,double z) noexcept {
    return {float(x),float(z*std::sin(-.24)),float(z*std::cos(-.24))};
}

inline XMFLOAT3 SolarCenter(uint32_t owner,double seconds) noexcept {
    const auto& orbit=SolarOrbits[owner==SolarMoon?0u:owner];
    const double angle=SolarOrbitAngle(orbit,seconds);
    const auto center=SolarOrbitalVector(orbit,orbit.radius*std::cos(angle),orbit.radius*std::sin(angle));
    if(owner!=SolarMoon)return center;
    const double lunarAngle=LunarOrbitAngle(seconds);
    return Add(center,LunarOrbitalVector(LunarOrbitRadius*std::cos(lunarAngle),
        LunarOrbitRadius*std::sin(lunarAngle)));
}

inline Pose SolarBody(uint32_t owner,double seconds) noexcept {
    Pose body{SolarCenter(owner,seconds)};
    if(owner==SolarMoon)return body;
    const auto& orbit=SolarOrbits[owner];
    const float spin=float(orbit.spinPhase+SolarTurn*std::fmod(seconds,SolarSpinPeriod)/SolarSpinPeriod);
    // Keep the axis fixed in the cosmos while the body spins around its local up.
    const auto spinAndAxis=XMMatrixRotationY(spin)*XMMatrixRotationZ(orbit.inclination+.13f)*
        XMMatrixRotationY(float(orbit.node));
    XMStoreFloat4(&body.q,XMQuaternionRotationMatrix(spinAndAxis));
    return body;
}

inline XMFLOAT3 SolarTransportVelocity(uint32_t owner,const Pose& body,double seconds,XMFLOAT3 point) noexcept {
    const auto& orbit=SolarOrbits[owner==SolarMoon?0u:owner];
    const double angle=SolarOrbitAngle(orbit,seconds);
    const double speed=orbit.radius*SolarTurn/orbit.period;
    const auto orbitVelocity=SolarOrbitalVector(orbit,-speed*std::sin(angle),speed*std::cos(angle));
    if(owner==SolarMoon){
        const double lunarAngle=LunarOrbitAngle(seconds),lunarSpeed=LunarOrbitRadius*SolarTurn/LunarOrbitPeriod;
        return Add(orbitVelocity,LunarOrbitalVector(lunarSpeed*std::sin(lunarAngle),
            -lunarSpeed*std::cos(lunarAngle)));
    }
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
    if(owner==SolarMoon)return {.24f,.29f,.37f};
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
    const auto center=SolarCenter(0u,seconds);
    const auto radial=Unit(center);
    const XMFLOAT3 forward{-radial.z,0.f,radial.x};
    const auto offset=Add(Add(Scale(radial,-6.65f),Scale(forward,4.65f)),{0.f,.55f,0.f});
    return SolarLookAt(Add(center,offset),center);
}

inline XMVECTOR SolarInverseRotation(uint32_t owner,double seconds) noexcept {
    if(seconds<0.)return XMQuaternionIdentity();
    const auto body=SolarBody(owner,seconds);
    return XMQuaternionConjugate(XMLoadFloat4(&body.q));
}

}
