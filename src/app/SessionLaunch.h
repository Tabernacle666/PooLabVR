#pragma once
#include "SolarSystem.h"
#include <cwchar>

namespace OASIS {
enum class SessionMode { Default, Cosmos, Forest, Hall, Loop };
inline SessionMode LaunchSessionMode() noexcept {
    wchar_t value[16]{};
    const DWORD n=GetEnvironmentVariableW(L"OASIS_SESSION_MODE",value,DWORD(std::size(value)));
    if(!n||n>=std::size(value))return SessionMode::Default;
    if(!std::wcscmp(value,L"cosmos"))return SessionMode::Cosmos;
    if(!std::wcscmp(value,L"foret"))return SessionMode::Forest;
    if(!std::wcscmp(value,L"hall"))return SessionMode::Hall;
    if(!std::wcscmp(value,L"loop"))return SessionMode::Loop;
    return SessionMode::Default;
}
inline constexpr double SessionTourSeconds=12.;
inline Pose SessionTourPose(double seconds,double solarTime) noexcept {
    // Front facade of OASIS campus is around z=5.24. At z=5.08 the
    // camera passes about 4.7 physical metres in front of the glazing.
    // All waypoints rotate and orbit with OASIS.
    const float t=float(std::clamp(seconds/SessionTourSeconds,0.,1.));
    const float x=-8.f+16.f*t;
    const XMFLOAT3 eye{x,.82f,5.08f};
    // Look towards the visible facade when close, towards the island otherwise.
    const XMFLOAT3 target{std::clamp(x,-2.7f,2.7f),.75f,5.55f};
    const auto body=SolarBodyToCosmos(0u,SolarBody(0u,solarTime));
    return SolarLookAt(TransformPoint(eye,body),TransformPoint(target,body));
}
}
