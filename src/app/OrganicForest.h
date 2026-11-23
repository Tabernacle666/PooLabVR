#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <array>
#include <vector>

namespace OASIS {
// All foliage is closed, opaque triangle geometry: no alpha cards,
// camera-facing quads, impostors, or texture-only leaf silhouettes.
struct OrganicTreeSource {
    std::vector<WorldObject> parts;
    uint64_t triangles{};
};
std::array<OrganicTreeSource,3> BuildOrganicTreeSources(SceneData& scene);
}
