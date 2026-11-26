#pragma once
#include "SolarSystem.h"
#include <cwchar>
#include <iterator>
#include <cmath>
#include <array>

namespace OASIS {
enum class SessionMode { Default, Cosmos, Forest, Hall, Loop, Sun, Walk, Prism, Target };
inline SessionMode LaunchSessionMode() noexcept {
    wchar_t value[16]{};
    const DWORD n=GetEnvironmentVariableW(L"OASIS_SESSION_MODE",value,DWORD(std::size(value)));
    if(!n||n>=std::size(value))return SessionMode::Default;
    if(!std::wcscmp(value,L"cosmos"))return SessionMode::Cosmos;
    if(!std::wcscmp(value,L"foret"))return SessionMode::Forest;
    if(!std::wcscmp(value,L"hall")||!std::wcscmp(value,L"bureau"))return SessionMode::Hall;
    if(!std::wcscmp(value,L"loop"))return SessionMode::Loop;
    if(!std::wcscmp(value,L"walk"))return SessionMode::Walk;
    if(!std::wcscmp(value,L"sun"))return SessionMode::Sun;
    if(!std::wcscmp(value,L"prism"))return SessionMode::Prism;
    if(!std::wcscmp(value,L"target"))return SessionMode::Target;
    return SessionMode::Default;
}
inline constexpr double SessionTourSeconds=24.;
inline constexpr double SessionWalkKmh=5.;
inline constexpr double SessionForwardDistanceMeters=16./double(SpectacleInteriorUnitsPerMeter);
inline constexpr double SessionWalkSeconds=SessionForwardDistanceMeters/(SessionWalkKmh/3.6);
inline constexpr bool IsSessionForwardTour(SessionMode mode)noexcept{
    return mode==SessionMode::Loop||mode==SessionMode::Walk;
}
inline constexpr double SessionForwardDuration(SessionMode mode)noexcept{
    return mode==SessionMode::Walk?SessionWalkSeconds:SessionTourSeconds;
}
// Comparison windows only: the former membrane intersections of the
// identical native path. No navigation, projection or scene effect.
inline unsigned SessionTourPhase(double seconds,double duration=SessionTourSeconds)noexcept{
    const double x=-8.+16.*std::clamp(seconds/duration,0.,1.);
    constexpr double side2=.18*.18+.42*.42;
    constexpr double entryRadius=SpectacleRadius-.004;
    constexpr double exitRadius=SpectacleRadius+.004*SpectacleInteriorUnitsPerMeter;
    return x<-std::sqrt(entryRadius*entryRadius-side2)?0u:
           x<std::sqrt(exitRadius*exitRadius-side2)?1u:2u;
}
// PRISM: one central elevated corridor, +X travel, +Z building.
// v2: 28 blocks, seven profiles x four WALK/FLY and building/forest combinations.
// inline constexpr unsigned PrismBlocks=28u;
inline constexpr unsigned PrismBlocks=24u;
inline constexpr double PrismWarmup=6.,PrismBlockSeconds=3.;
inline constexpr double PrismDuration=PrismWarmup+PrismBlocks*PrismBlockSeconds;
inline constexpr float PrismHeight=.82f,PrismCorridorZ=5.08f;
// Lower-middle S floors occupy the eastern 40% of their width. Move the
// short walking segment to face that inhabited band, not the glyph void.
inline constexpr double PrismWalkOffsetMeters=16.;
inline int PrismBlock(double elapsed)noexcept{
    if(elapsed<PrismWarmup)return -1;
    return std::min(int(PrismBlocks),int((elapsed-PrismWarmup)/PrismBlockSeconds));
}
inline Pose PrismPose(double elapsed)noexcept{
    const int block=PrismBlock(elapsed);
    const unsigned b=unsigned(std::clamp(block,0,int(PrismBlocks)-1));
    // v2: const double speed=(b%4u>=2u?90.:5.)/3.6;
    const double speed=5./3.6;
    const double local=block<0?0.:std::clamp(elapsed-PrismWarmup-double(b)*PrismBlockSeconds,0.,PrismBlockSeconds);
    // v2: const double center=b%4u<2u?PrismWalkOffsetMeters:0.;
    const double center=PrismWalkOffsetMeters;
    const float x=float((center+(local-PrismBlockSeconds*.5)*speed)*SpectacleInteriorUnitsPerMeter);
    const XMFLOAT3 eye{x,PrismHeight,PrismCorridorZ};
    // v2: const XMFLOAT3 target{x+1.f,PrismHeight-.38f,PrismCorridorZ+(b%2u?-1.f:1.f)};
    const XMFLOAT3 target{x+1.f,PrismHeight,PrismCorridorZ};
    return SolarLookAt(eye,target);
}
// Retain original checkpoint positions 1, 2, 3 and 6.
inline constexpr std::array<double,4> SessionTourCaptureTimes{0.,4.,8.,20.};
inline Pose SessionTourPose(double seconds,double solarTime) noexcept {
    // Front facade of OASIS campus is around z=5.24. At z=5.08 the
    // camera passes about 4.7 physical metres in front of the glazing.
    // All waypoints rotate and orbit with OASIS.
    const float t=float(std::clamp(seconds/SessionTourSeconds,0.,1.)*12.);
    const float x=t<2.5f?std::lerp(-8.f,-3.2f,t/2.5f):
        t<9.5f?std::lerp(-3.2f,3.2f,(t-2.5f)/7.f):
                std::lerp(3.2f,8.f,(t-9.5f)/2.5f);
    const XMFLOAT3 eye{x,.82f,5.08f};
    // Look towards the visible facade when close, towards the island otherwise.
    const XMFLOAT3 target{std::clamp(x,-2.7f,2.7f),.75f,5.55f};
    (void)solarTime;
    return SolarLookAt(eye,target);
}
// Loop only: keep the original endpoints and duration, look forward.
inline Pose SessionForwardTourPose(double seconds,double solarTime,double duration=SessionTourSeconds) noexcept {
    const float t=float(std::clamp(seconds/duration,0.,1.));
    const float x=std::lerp(-8.f,8.f,t);
    const XMFLOAT3 eye{x,.82f,5.08f},target{x+1.f,eye.y,eye.z};
    (void)solarTime;
    return SolarLookAt(eye,target);
}
}
