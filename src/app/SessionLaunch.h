#pragma once
#include "SolarSystem.h"
#include "OasisMetric.h"
#include <cwchar>
#include <iterator>
#include <cmath>
#include <array>

namespace OASIS {
enum class SessionMode { Default, Cosmos, Forest, Hall, Loop, Sun, Walk, Prism, Target, Lod };
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
    if(!std::wcscmp(value,L"lod"))return SessionMode::Lod;
    if(!std::wcscmp(value,L"target"))return SessionMode::Target;
    return SessionMode::Default;
}
inline constexpr double SessionTourSeconds=24.;
inline constexpr double SessionWalkKmh=5.;
inline constexpr double SessionForwardDistanceMeters=466.0194169363705;
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
    const double x=-233.0097084681852+466.0194169363705*std::clamp(seconds/duration,0.,1.);
    constexpr double side2=177.1326228343985;
    constexpr double entryRadius=SpectacleRadius-0.1165048542340926;
    constexpr double exitRadius=SpectacleRadius+.004*OasisSceneUnitsPerMeter;
    return x<-std::sqrt(entryRadius*entryRadius-side2)?0u:
           x<std::sqrt(exitRadius*exitRadius-side2)?1u:2u;
}
// PRISM: one central elevated corridor, +X travel, +Z building.
// v2: 28 blocks, seven profiles x four WALK/FLY and building/forest combinations.
// inline constexpr unsigned PrismBlocks=28u;
inline constexpr unsigned PrismBlocks=24u;
inline constexpr double PrismWarmup=6.,PrismBlockSeconds=3.;
inline constexpr double PrismDuration=PrismWarmup+PrismBlocks*PrismBlockSeconds;
inline constexpr float PrismHeight=23.8834951f,PrismCorridorZ=147.961165f;
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
    const float x=float((center+(local-PrismBlockSeconds*.5)*speed)*OasisSceneUnitsPerMeter);
    const XMFLOAT3 eye{x,PrismHeight,PrismCorridorZ};
    // v2: const XMFLOAT3 target{x+1.f,PrismHeight-.38f,PrismCorridorZ+(b%2u?-1.f:1.f)};
    const XMFLOAT3 target{x+1.f,PrismHeight,PrismCorridorZ};
    return SolarLookAt(eye,target);
}
// LOD-00: five identical passes; three short walking windows and a pause.
inline constexpr unsigned LodExtents=20u,LodBlocks=5u,LodSegments=4u;
inline constexpr double LodWarmup=6.,LodSegmentSeconds=4.,LodBlockSeconds=LodSegments*LodSegmentSeconds;
inline constexpr double LodDuration=LodWarmup+LodBlocks*LodBlockSeconds;
static_assert(LodDuration<=90.);
inline int LodBlock(double elapsed)noexcept{
    return elapsed<LodWarmup?-1:std::min(int(LodBlocks),int((elapsed-LodWarmup)/LodBlockSeconds));
}
inline unsigned LodSegment(double elapsed)noexcept{
    const int b=LodBlock(elapsed);
    if(b<0)return 0u;
    if(b>=int(LodBlocks))return LodSegments-1u;
    return std::min(LodSegments-1u,unsigned((elapsed-LodWarmup-double(b)*LodBlockSeconds)/LodSegmentSeconds));
}
inline Pose LodPose(double elapsed,const XMFLOAT4& extent)noexcept{
    const int block=LodBlock(elapsed);
    const unsigned segment=LodSegment(elapsed);
    const double local=block<0?0.:block>=int(LodBlocks)?LodSegmentSeconds:
        std::clamp(elapsed-LodWarmup-double(block)*LodBlockSeconds-double(segment)*LodSegmentSeconds,0.,LodSegmentSeconds);
    constexpr float x=float(PrismWalkOffsetMeters),nearZ=PrismCorridorZ;
    constexpr float walk=float(SessionWalkKmh/3.6),span=walk*float(LodSegmentSeconds);
    const float dx=x-extent.x;
    const float edge=extent.y-std::sqrt(std::max(0.f,extent.w*extent.w-dx*dx))+5.f;
    const float middle=std::lerp(edge,nearZ-span,.5f);
    const float z=segment==0?edge+float(local)*walk:segment==1?middle+float(local)*walk:
        segment==2?nearZ-span+float(local)*walk:nearZ;
    const float y=segment==0?std::max(PrismHeight,extent.z+35.f):
        segment==1?std::max(PrismHeight,extent.z+24.f):PrismHeight;
    return SolarLookAt({x,y,z},{x,21.8446602f,161.650485f});
}
// Retain original checkpoint positions 1, 2, 3 and 6.
inline constexpr std::array<double,4> SessionTourCaptureTimes{0.,4.,8.,20.};
inline Pose SessionTourPose(double seconds,double solarTime) noexcept {
    // Front facade of OASIS campus is around z=5.24. At z=5.08 the
    // camera passes about 4.7 physical metres in front of the glazing.
    // All waypoints rotate and orbit with OASIS.
    const float t=float(std::clamp(seconds/SessionTourSeconds,0.,1.)*12.);
    const float x=t<2.5f?std::lerp(-233.009708f,-93.2038834f,t/2.5f):
        t<9.5f?std::lerp(-93.2038834f,93.2038834f,(t-2.5f)/7.f):
                std::lerp(93.2038834f,233.009708f,(t-9.5f)/2.5f);
    const XMFLOAT3 eye{x,23.8834951f,147.961165f};
    // Look towards the visible facade when close, towards the island otherwise.
    const XMFLOAT3 target{std::clamp(x,-78.6407766f,78.6407766f),21.8446602f,161.650485f};
    (void)solarTime;
    return SolarLookAt(eye,target);
}
// Loop only: keep the original endpoints and duration, look forward.
inline Pose SessionForwardTourPose(double seconds,double solarTime,double duration=SessionTourSeconds) noexcept {
    const float t=float(std::clamp(seconds/duration,0.,1.));
    const float x=std::lerp(-233.009708f,233.009708f,t);
    const XMFLOAT3 eye{x,23.8834951f,147.961165f},target{x+1.f,eye.y,eye.z};
    (void)solarTime;
    return SolarLookAt(eye,target);
}
}
