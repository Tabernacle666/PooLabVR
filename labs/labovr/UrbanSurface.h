#pragma once
#include "UrbanLayout.h"
#include <array>
#include <cstdint>

namespace lab::urban {
inline constexpr double WinterSnowfallMetersPerSecond=.00000016;
inline constexpr double WinterAirTemperatureC=-1.5;

// Absolute-time climate/hydrology: no one-second catch-up or tile weather archive.
class SurfaceTile {
public:
    static constexpr unsigned Resolution=96;
    static constexpr unsigned CellCount=Resolution*Resolution;
    struct Sample {float snow{},water{},wetness{},height{},heat{},runoff{};};
    struct Appearance {float snowCoverage{},puddle{},wetFilm{},runoff{};};
    struct WaterBalance {double stored{},discharged{};};
    explicit SurfaceTile(Tile tile);
    void SetTime(double seconds) noexcept;
    const Sample& At(unsigned x,unsigned z) const {return values_[z*Resolution+x];}
    Sample AtWorld(Point world) const;
    Appearance AppearanceAt(Point world) const;
    WaterBalance Balance() const;
    double StoredWater() const {return Balance().stored;}
    double DischargedWater() const {return Balance().discharged;}
    Tile Key() const {return key_;}
private:
    static constexpr unsigned Rates=8;
    struct Group {
        uint32_t count{};
        double initialSnow{},initialWater{};
        std::array<double,2> snowSin{},snowCos{},rainSin{},rainCos{};
    };
    Tile key_{};
    double seconds_{};
    std::array<Sample,CellCount> values_{};
    std::array<uint8_t,CellCount> snowRates_{},waterRates_{};
    std::array<float,CellCount> drainDistance_{};
    // Linear superposition makes the exact aggregate balance O(64) instead
    // of scanning all 9,216 cells on the renderer's telemetry tick.
    std::array<Group,Rates*Rates> groups_{};
    Sample EvaluateCell(unsigned index)const;
};
} // namespace lab::urban
