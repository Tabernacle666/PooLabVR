#pragma once

// Généré par fabrication/profil_cpp.py à partir des deux profils JSON.
#include "PropulsionVaisseau.h"
#include "TableauDeBord.h"

namespace OASIS::Vaisseaux::Lunaire {

inline constexpr PropulsionProfile Propulsion{
    {1650, 12800, 0.3, 0.24, 0.12, -0.349065850399, 0.349065850399, 2.18166156499, PowerCurve{{{0, 0, 0}, {0.25, 395, 28000}, {0.5, 1420, 140000}, {0.75, 2900, 320000}, {1, 4800, 560000}}}},
    {450, 3850, 0.8, 1.1, -0.418879020479, 0.418879020479, 2.09439510239, 0.36, 1, 960, PowerCurve{{{0, 0, 0}, {0.25, 228, 4780}, {0.5, 912, 38250}, {0.75, 2053, 129100}, {1, 3650, 306000}}}}
};

inline constexpr std::array<Mount, 8> Mounts{{
    {"R_AR_G", Organ::Reactor, Side::Left, {-1.55, 1.32, 0}, 1},
    {"R_AV_G", Organ::Reactor, Side::Left, {1.55, 1.32, 0}, -1},
    {"R_AR_D", Organ::Reactor, Side::Right, {-1.55, -1.32, 0}, 1},
    {"R_AV_D", Organ::Reactor, Side::Right, {1.55, -1.32, 0}, -1},
    {"H_AR_G", Organ::Fan, Side::Left, {-0.76, 1.32, -0.23}, 1},
    {"H_AV_G", Organ::Fan, Side::Left, {0.76, 1.32, -0.23}, -1},
    {"H_AR_D", Organ::Fan, Side::Right, {-0.76, -1.32, -0.23}, -1},
    {"H_AV_D", Organ::Fan, Side::Right, {0.76, -1.32, -0.23}, 1},
}};

inline constexpr double DryKg = 1200;
inline constexpr double FuelCapacityKg = 120;
inline constexpr Vec3 DryCentreMetres{0, 0, 0.027};
inline constexpr Vec3 FuelCentreMetres{-0.2, 0, -0.27};

inline constexpr double MapPeriodSeconds = 0.5;
inline constexpr std::array<int, 2> MapPagePixels{256, 128};
inline constexpr std::array<Theme, 5> Themes{{
    {"Phosphore", {0.012000f, 0.041000f, 0.023000f}},
    {"Ambre", {0.067000f, 0.027000f, 0.010000f}},
    {"Cyan", {0.008000f, 0.039000f, 0.053000f}},
    {"Ardoise", {0.022000f, 0.026000f, 0.050000f}},
    {"Prune", {0.048000f, 0.015000f, 0.050000f}},
}};

} // namespace OASIS::Vaisseaux::Lunaire
