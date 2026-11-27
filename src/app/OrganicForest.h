#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <array>
#include <vector>

namespace OASIS {
// Opaque woody geometry plus fixed 3D cutout sprigs with shared OMM atlases.
// Recipe-derived placement envelope, in model metres, independent of GPU LOD.
struct OrganicTreeEnvelope { float radius{},low{},high{}; };
std::vector<OrganicTreeEnvelope> BuildOrganicTreeEnvelopes(unsigned variantsPerSpecies);
struct OrganicTreeSource {
    std::vector<WorldObject> parts;
    uint64_t triangles{};
};
std::vector<OrganicTreeSource> BuildOrganicTreeSources(SceneData& scene,unsigned variantsPerSpecies=1u);
}
