#pragma once

namespace OASIS {
// Permanent OASIS metric frame. Scene coordinates are a fixed storage/render
// representation, never a function of a sphere, camera, or travel state.
// Preserve the authored geometry and the physical size of every metre.
inline constexpr float OasisSceneUnitsPerMeter=0.034333333f;
inline constexpr float OasisMetersToScene(float meters) noexcept {return meters*OasisSceneUnitsPerMeter;}
inline constexpr double OasisMetersToScene(double meters) noexcept {return meters*double(OasisSceneUnitsPerMeter);}
inline constexpr float OasisSceneToMeters(float units) noexcept {return units/OasisSceneUnitsPerMeter;}
inline constexpr double OasisSceneToMeters(double units) noexcept {return units/double(OasisSceneUnitsPerMeter);}
}
