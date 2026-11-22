#pragma once
#include "Assets.h"
#include <string>
#include <vector>

namespace OASIS {
class Mg2Catalog {
    struct Entry {
        std::string key;
        SceneData scene;
    };
    std::vector<Entry> entries_;
public:
    explicit Mg2Catalog(const std::filesystem::path& directory);
    void WriteSummary()const;
};
}
