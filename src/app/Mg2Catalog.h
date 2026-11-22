#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include "building/BuildingPlan.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OASIS {
struct Mg2ArchitecturalPalette {
    std::array<uint32_t,size_t(Buildings::Material::Count)> material{};
    size_t sources{},mapped{};
};
struct Mg2Worktop {
    enum class PropKind {Keyboard,Equipment,Accessory};
    struct Prop {
        std::vector<WorldObject> parts;
        XMFLOAT3 offset,lower,upper;
        PropKind kind;
    };
    struct Layout {
        std::vector<WorldObject> parts;
        size_t keyboards{};
    };
    struct RoomProp {
        std::vector<WorldObject> parts;
        XMFLOAT3 lower{},upper{};
        float nx{},nz{},chance{1.f},yawDeg{};
    };
    std::array<std::vector<Prop>,size_t(Buildings::MountSurface::Count)> surfaces;
    std::array<std::vector<RoomProp>,8> roomProfiles;
    std::array<float,10> wearLikelihood{};
    // Named-node selections from the existing original OASIS wear GLB.
    std::array<std::vector<WorldObject>,3> wearOverlays;
    Layout Fit(Buildings::MountSurface,float width,float depth)const;
    bool Has(Buildings::MountSurface surface)const{return !surfaces[size_t(surface)].empty();}
};

class Mg2Catalog {
    struct Entry {
        std::string key;
        std::filesystem::path path;
        uint64_t triangles{};
        XMFLOAT3 lower{},upper{};
        std::optional<SceneData> scene;
        bool realPbr{};
    };
    struct Placement {size_t entry{};XMFLOAT3 offset{};float jitter{};};
    struct ForestRow {
        std::filesystem::path recipe;
        std::vector<Placement> additional;
    };
    struct ForestInstance {size_t entry{};Pose pose;float scale{};};
    std::vector<Entry> entries_;
    std::array<std::vector<Placement>,size_t(Buildings::MountSurface::Count)> worktops_;
    struct RoomPlacement {size_t entry{};float nx{},nz{},chance{1.f},yawDeg{};};
    std::array<std::vector<RoomPlacement>,8> roomPlacements_;
    std::array<float,10> wearLikelihood_{};
    std::vector<ForestRow> forestRows_;
    std::vector<ForestInstance> forest_;
    size_t plantedCells_{};
    std::filesystem::path directory_;
    uint64_t packRevision_{};
    double prepareMs_{};
    size_t EntryIndex(std::string_view key)const;
    void PrepareEntry(size_t index);
    std::vector<WorldObject> AppendParts(SceneData& scene,size_t index)const;
public:
    explicit Mg2Catalog(const std::filesystem::path& directory);
    bool PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,unsigned treeDensity=2u);
    void InstallForest(SceneData& scene,WorldRegistry& world)const;
    void WriteSummary()const;
    Mg2ArchitecturalPalette AppendArchitecturalMaterials(SceneData& scene);
    Mg2Worktop AppendWorktop(SceneData& scene)const;
};
}
