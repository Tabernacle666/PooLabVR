#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <array>

namespace OASIS {
namespace Understory {
enum class Family : uint8_t { Shrub,Fern,Herb,Grass,Sapling };
enum class Leaf : uint8_t { Oval,Lance,Lobed,Heart,Compound,Needle };
enum class Accent : uint8_t { None,Yellow,White,Pink,RedFruit,BlueFruit,Purple,Orange,Copper,Seed };
struct Species {
    const char* name; Family family; Leaf leaf; Accent accent;
    float minHeight,maxHeight,spread,leafLength,leafWidth;
    unsigned branches,leafPairs,green;
};
inline constexpr std::array<Species,44> SpeciesList{{
    {"Diervilla lonicera",Family::Shrub,Leaf::Lance,Accent::Yellow,.40f,.80f,.65f,.14f,.032f,5,4,1},
    {"Vaccinium angustifolium",Family::Shrub,Leaf::Oval,Accent::BlueFruit,.30f,.50f,.85f,.055f,.017f,6,5,2},
    {"Rhododendron groenlandicum",Family::Shrub,Leaf::Lance,Accent::White,.50f,1.00f,.55f,.095f,.019f,5,5,3},
    {"Rubus idaeus",Family::Shrub,Leaf::Compound,Accent::RedFruit,.60f,1.20f,.58f,.19f,.040f,4,4,0},
    {"Ribes glandulosum",Family::Shrub,Leaf::Lobed,Accent::RedFruit,.50f,1.00f,.82f,.12f,.052f,5,4,4},
    {"Lonicera canadensis",Family::Shrub,Leaf::Oval,Accent::White,.70f,1.20f,.57f,.14f,.035f,4,4,1},
    {"Corylus cornuta",Family::Shrub,Leaf::Heart,Accent::None,.60f,1.20f,.68f,.17f,.065f,5,4,4},
    {"Viburnum edule",Family::Shrub,Leaf::Lobed,Accent::White,.60f,1.20f,.62f,.14f,.058f,5,4,5},
    {"Dryopteris carthusiana",Family::Fern,Leaf::Lance,Accent::None,.30f,.60f,.92f,.29f,.040f,6,8,2},
    {"Athyrium filix-femina",Family::Fern,Leaf::Compound,Accent::None,.60f,1.10f,.67f,.32f,.030f,6,11,0},
    {"Pteridium aquilinum",Family::Fern,Leaf::Compound,Accent::None,.60f,1.20f,.82f,.38f,.050f,3,9,4},
    {"Matteuccia struthiopteris",Family::Fern,Leaf::Lance,Accent::None,.70f,1.20f,.42f,.30f,.036f,8,10,1},
    {"Osmundastrum cinnamomeum",Family::Fern,Leaf::Lance,Accent::Copper,.80f,1.20f,.52f,.32f,.045f,6,9,3},
    {"Aralia nudicaulis",Family::Herb,Leaf::Compound,Accent::White,.30f,.50f,.95f,.19f,.043f,3,3,2},
    {"Aralia hispida",Family::Herb,Leaf::Compound,Accent::White,.30f,.90f,.65f,.19f,.037f,3,3,1},
    {"Eurybia macrophylla",Family::Herb,Leaf::Heart,Accent::Purple,.40f,.90f,.61f,.21f,.081f,3,4,4},
    {"Solidago flexicaulis",Family::Herb,Leaf::Lance,Accent::Yellow,.40f,1.00f,.38f,.14f,.035f,3,5,2},
    {"Polygonatum pubescens",Family::Herb,Leaf::Oval,Accent::White,.30f,.90f,.62f,.16f,.040f,2,6,1},
    {"Maianthemum racemosum",Family::Herb,Leaf::Oval,Accent::White,.30f,.90f,.48f,.17f,.042f,2,6,5},
    {"Actaea rubra",Family::Herb,Leaf::Lobed,Accent::RedFruit,.40f,.80f,.65f,.13f,.055f,3,4,3},
    {"Chamaenerion angustifolium",Family::Herb,Leaf::Lance,Accent::Pink,.60f,1.20f,.30f,.13f,.015f,2,7,2},
    {"Impatiens capensis",Family::Herb,Leaf::Oval,Accent::Orange,.60f,1.20f,.55f,.14f,.037f,4,4,0},
    {"Geum macrophyllum",Family::Herb,Leaf::Lobed,Accent::Yellow,.40f,.90f,.48f,.13f,.046f,3,4,4},
    {"Calamagrostis canadensis",Family::Grass,Leaf::Lance,Accent::Seed,.75f,1.20f,.52f,.50f,.011f,9,3,6},
    {"Carex arctata",Family::Grass,Leaf::Lance,Accent::Seed,.30f,.60f,.82f,.36f,.013f,11,3,5},
    {"Elymus canadensis",Family::Grass,Leaf::Lance,Accent::Seed,.60f,1.20f,.53f,.51f,.017f,7,3,6},
    {"Abies balsamea juvenile",Family::Sapling,Leaf::Needle,Accent::None,.40f,1.20f,.72f,.055f,.007f,5,5,3},
    {"Picea glauca juvenile",Family::Sapling,Leaf::Needle,Accent::None,.40f,1.20f,.57f,.045f,.005f,5,6,5},
    {"Acer saccharum juvenile",Family::Sapling,Leaf::Lobed,Accent::None,.50f,1.20f,.60f,.19f,.072f,4,4,4},
    {"Betula papyrifera juvenile",Family::Sapling,Leaf::Heart,Accent::None,.50f,1.20f,.45f,.13f,.042f,4,4,0},
    {"Vaccinium myrtilloides",Family::Shrub,Leaf::Oval,Accent::BlueFruit,.30f,.60f,.85f,.055f,.021f,7,6,3},
    {"Kalmia angustifolia",Family::Shrub,Leaf::Lance,Accent::Pink,.40f,.90f,.75f,.085f,.022f,7,6,1},
    {"Spiraea alba",Family::Shrub,Leaf::Lance,Accent::White,.60f,1.20f,.70f,.10f,.028f,7,6,4},
    {"Polystichum acrostichoides",Family::Fern,Leaf::Lance,Accent::None,.35f,.70f,.90f,.25f,.035f,8,13,3},
    {"Osmunda regalis",Family::Fern,Leaf::Compound,Accent::Copper,.65f,1.20f,.78f,.31f,.048f,7,11,1},
    {"Dennstaedtia punctilobula",Family::Fern,Leaf::Compound,Accent::None,.40f,.90f,.86f,.28f,.041f,7,12,4},
    {"Clintonia borealis",Family::Herb,Leaf::Oval,Accent::BlueFruit,.30f,.55f,.68f,.23f,.084f,3,3,3},
    {"Trillium erectum",Family::Herb,Leaf::Oval,Accent::Purple,.30f,.50f,.80f,.19f,.080f,3,2,4},
    {"Sanguinaria canadensis",Family::Herb,Leaf::Lobed,Accent::White,.30f,.45f,.80f,.18f,.090f,3,2,2},
    {"Eutrochium maculatum",Family::Herb,Leaf::Lance,Accent::Pink,.75f,1.20f,.57f,.19f,.048f,4,6,1},
    {"Carex crinita",Family::Grass,Leaf::Lance,Accent::Seed,.45f,.90f,.88f,.48f,.015f,13,3,5},
    {"Glyceria striata",Family::Grass,Leaf::Lance,Accent::Seed,.35f,.80f,.73f,.41f,.012f,12,3,4},
    {"Tsuga canadensis juvenile",Family::Sapling,Leaf::Needle,Accent::None,.40f,1.20f,.86f,.040f,.007f,6,6,3},
    {"Sorbus americana juvenile",Family::Sapling,Leaf::Compound,Accent::None,.50f,1.20f,.70f,.21f,.042f,5,5,1}
}};
inline constexpr unsigned Variants=8u;
inline constexpr size_t ModelCount=SpeciesList.size()*Variants;
inline constexpr unsigned Individuals(unsigned species){return SpeciesList[species].family==Family::Sapling?3u:6u;}
}
std::array<WorldObject,Understory::ModelCount> BuildUnderstorySources(SceneData& scene,const std::array<bool,Understory::ModelCount>& used);
}
