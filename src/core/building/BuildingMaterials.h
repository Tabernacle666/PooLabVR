#pragma once
#include "BuildingPlan.h"

namespace OASIS::Buildings {
using Resistance=std::array<uint8_t,size_t(DamageKind::Count)>;
struct MaterialSpec {
    const char* name;
    std::array<float,3> color;
    float roughness;
    Resistance resistance;
    float textureVariation{};
    std::array<float,3> emission{};
    float metallic{};
};

// Relative resistance from 0 to 100. Laser and saw share DamageKind::Sawing.
inline constexpr Resistance Masonry{77,80,85,75,95,78,72};
inline constexpr Resistance Steel{80,77,79,68,94,88,24};
inline constexpr Resistance Plaster{21,28,33,23,62,18,75};
inline constexpr Resistance Fabric{6,9,18,9,12,28,79};
inline constexpr Resistance Glazing{14,20,13,44,47,98,96};
inline constexpr Resistance Timber{32,38,48,22,22,46,81};
inline constexpr Resistance Porcelain{27,38,32,54,90,98,95};
inline constexpr Resistance Polymer{18,19,34,25,18,93,91};
inline constexpr Resistance Copper{45,42,56,40,86,96,7};
inline constexpr Resistance Fixture{16,19,24,30,45,32,23};

inline constexpr auto MaterialSpecs=std::to_array<MaterialSpec>({
    {"OASIS beton",{.34f,.34f,.32f},.87f,Masonry},
    {"OASIS aluminium",{.13f,.16f,.19f},.47f,Steel,0.f,{},.82f},
    {"OASIS cloison",{.72f,.73f,.70f},.90f,Plaster},
    {"OASIS sol",{.28f,.32f,.36f},.79f,Masonry},
    {"OASIS verre",{.12f,.19f,.21f},.23f,Glazing},
    {"OASIS mobilier",{.41f,.31f,.24f},.68f,Timber},
    {"OASIS diffuseur blanc",{.82f,.84f,.85f},.82f,Fixture,0.f,{.56f,.58f,.58f}},
    {"OASIS extincteur",{.66f,.04f,.025f},.44f,Steel},
    {"OASIS reseaux",{.36f,.23f,.12f},.64f,Polymer},
    {"OASIS plafond blanc casse",{.86f,.84f,.80f},.93f,Plaster,.04f},
    {"OASIS plafond graphite",{.29f,.31f,.33f},.94f,Plaster,.035f},
    {"OASIS mur sauge",{.55f,.63f,.57f},.92f,Plaster},
    {"OASIS mur sable",{.72f,.64f,.54f},.93f,Plaster},
    {"OASIS mur brume",{.60f,.67f,.72f},.93f,Plaster},
    {"OASIS mur lilas",{.67f,.61f,.69f},.93f,Plaster},
    {"OASIS mur rose argile",{.52f,.32f,.34f},.92f,Plaster},
    {"OASIS moulure claire",{.70f,.65f,.54f},.76f,Timber},
    {"OASIS tapis bleu gris",{.24f,.30f,.35f},.97f,Fabric,.09f},
    {"OASIS tapis gris",{.31f,.32f,.33f},.97f,Fabric,.09f},
    {"OASIS tapis bordeaux patine",{.34f,.20f,.24f},.98f,Fabric,.11f},
    {"OASIS ceramique",{.65f,.70f,.68f},.47f,Porcelain},
    {"OASIS conduite egout",{.12f,.13f,.15f},.92f,Polymer},
    {"OASIS conduite eau",{.45f,.22f,.10f},.61f,Copper},
    {"OASIS diffuseur ambre",{.74f,.46f,.20f},.78f,Fixture,0.f,{.80f,.49f,.21f}},
    {"OASIS diffuseur rouge",{.59f,.20f,.18f},.83f,Fixture,0.f,{.65f,.20f,.17f}},
    {"OASIS diffuseur eteint",{.26f,.25f,.24f},.82f,Fixture}
});
static_assert(MaterialSpecs.size()==size_t(Material::Count));

constexpr const MaterialSpec& Describe(Material material){
    return MaterialSpecs[size_t(material)];
}
}
