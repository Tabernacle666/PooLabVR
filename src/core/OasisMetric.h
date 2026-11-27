#pragma once

namespace OASIS {
// Native OASIS world, tracking, terrain and GPU world positions are metres.
// No portal or camera-dependent scale belongs to this reference frame.
inline constexpr float OasisSceneUnitsPerMeter=1.f;
inline constexpr float OasisMetersToScene(float meters) noexcept {return meters*OasisSceneUnitsPerMeter;}
inline constexpr double OasisMetersToScene(double meters) noexcept {return meters*double(OasisSceneUnitsPerMeter);}
inline constexpr float OasisSceneToMeters(float units) noexcept {return units/OasisSceneUnitsPerMeter;}
inline constexpr double OasisSceneToMeters(double units) noexcept {return units/double(OasisSceneUnitsPerMeter);}
}
