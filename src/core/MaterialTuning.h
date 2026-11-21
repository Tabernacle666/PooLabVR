#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace OASIS {

// Percentages, except the signed roughness and metallic offsets in points.
enum class MaterialParameter : uint32_t {
    Red,Green,Blue,Gamma,Uv,Roughness,Normal,Metallic,Emission,Opacity,Count
};
using MaterialTuning=std::array<int,std::size_t(MaterialParameter::Count)>;
inline constexpr MaterialTuning DefaultMaterialTuning{100,100,100,100,100,0,100,0,100,100};

}
