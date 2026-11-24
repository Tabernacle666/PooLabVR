#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace OASIS {
struct AssetMaterial;
std::filesystem::path PhysicalMaterialLibraryPath();
std::filesystem::path PhysicalMaterialBindingsPath();
// Bindings are authored per source and, when needed, per node and source slot.
// An empty result means the source/slot has not been classified yet.
std::string PhysicalMaterialBinding(const std::filesystem::path& source,
                                    std::string_view slot,std::string_view node);
void ApplyPhysicalMaterial(AssetMaterial&,std::string_view materialId);
}
