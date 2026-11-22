#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OASIS {
struct Mg2Worktop {
    std::vector<WorldObject> parts;
    float halfWidth{},halfDepth{};
};

class Mg2Catalog {
    struct Entry {
        std::string key;
        std::filesystem::path path;
        uint64_t triangles{};
        XMFLOAT3 lower{},upper{};
        std::optional<SceneData> scene;
    };
    struct Placement {size_t entry{};XMFLOAT3 offset{};float jitter{};};
    struct Tree {size_t entry{};Pose pose;float scale{};};
    std::filesystem::path directory_;
    std::vector<Entry> entries_;
    std::vector<Placement> worktop_;
    std::vector<Tree> trees_;
    size_t plantedCells_{};
    uint64_t packRevision_{};
    double prepareMs_{};
    size_t EntryIndex(std::string_view key)const;
    void PrepareEntry(size_t index);
    std::vector<WorldObject> AppendParts(SceneData& scene,size_t index)const;
public:
    explicit Mg2Catalog(const std::filesystem::path& directory);
    bool PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites);
    void InstallForest(SceneData& scene,WorldRegistry& world)const;
    void WriteSummary()const;
    Mg2Worktop AppendWorktop(SceneData& scene)const;
};
}
