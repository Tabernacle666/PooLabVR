#pragma once
#include "MaterialTuning.h"
#include <filesystem>
#include <span>

namespace OASIS {

struct SceneData;
class MaterialProfiles {
public:
    static constexpr uint32_t SlotCount=17;
    static constexpr uint32_t FieldCount=uint32_t(MaterialParameter::Count);
    void Load(const std::filesystem::path& folder);
    void Install(SceneData&) const;
    bool Save();
    void OverwriteFactory();
    bool Edit(uint32_t slot,uint32_t field,int direction,bool restore);
    int Value(uint32_t slot,uint32_t field) const {return values_[slot][field];}
    std::span<const MaterialTuning> Values() const {return values_;}
    uint64_t Revision() const {return revision_;}

private:
    std::filesystem::path path_,textures_;
    std::array<MaterialTuning,SlotCount> values_{},factory_{};
    uint64_t revision_{};
    bool dirty_{};
};

}
