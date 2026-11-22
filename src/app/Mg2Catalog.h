#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <optional>
#include <string>
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
    struct Placement {size_t entry{};XMFLOAT3 offset{};};
    std::vector<Entry> entries_;
    std::vector<Placement> worktop_;
    uint64_t packRevision_{};
    double prepareMs_{};
public:
    explicit Mg2Catalog(const std::filesystem::path& directory);
    void WriteSummary()const;
    Mg2Worktop AppendWorktop(SceneData& scene)const;
};
}
