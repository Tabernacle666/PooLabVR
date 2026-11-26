#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <array>
#include <vector>

namespace OASIS {
// Opaque woody geometry plus fixed 3D cutout sprigs with shared OMM atlases.
// The number of planted trees and their spatial distribution are unchanged.
struct OrganicTreeSource {
    std::vector<WorldObject> parts;
    uint64_t triangles{};
};
std::vector<OrganicTreeSource> BuildOrganicTreeSources(SceneData& scene,unsigned variantsPerSpecies=1u);
}
