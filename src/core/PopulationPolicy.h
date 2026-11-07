#pragma once
namespace lab {
// A scene can keep evolving while its visual layer is hidden. Visibility here
// indicates whether this scene contains the local observer, not occlusion culling.
struct PopulationExecutionPolicy { bool simulate; bool observerPresent; };
constexpr PopulationExecutionPolicy PopulationPolicy(bool allocated,bool enabled,bool layerVisible) noexcept {
    return {allocated&&enabled,layerVisible};
}
}
