#pragma once
#include "UrbanLayout.h"
#include <array>
#include <cstdint>
#include <vector>

namespace lab::urban {
// One weather clock and one precipitation source for initial/reloaded tiles.
inline constexpr double WinterSnowfallMetersPerSecond=.00000016;
inline constexpr double WinterAirTemperatureC=-1.5;

// Persistent weather state: snow and standing water are metres of water
// equivalent; wetness is a dimensionless surface-film coverage.
class SurfaceTile {
public:
    static constexpr unsigned Resolution=96;
    static constexpr unsigned CellCount=Resolution*Resolution;
    struct Sample {float snow{},water{},wetness{},height{},heat{};};
    struct Appearance {
        float snowCoverage{},puddle{},wetFilm{},runoff{};
    };
    struct WeatherCell {float snow{},water{},wetness{};};
    struct WeatherSnapshot {
        std::vector<WeatherCell> cells;
        double discharged{};
    };
    explicit SurfaceTile(Tile tile);
    void Advance(double elapsedSeconds,double snowfallMetersPerSecond,double airTemperatureC);
    const Sample& At(unsigned x,unsigned z) const { return values_[z*Resolution+x]; }
    Sample& At(unsigned x,unsigned z) { return values_[z*Resolution+x]; }
    Sample AtWorld(Point world) const;
    Appearance AppearanceAt(Point world) const;
    WeatherSnapshot SaveWeather() const;
    void RestoreWeather(const WeatherSnapshot& snapshot);
    double StoredWater() const;
    double DischargedWater() const {return discharged_; }
    Tile Key() const {return key_;}
private:
    Tile key_{};
    std::array<Sample,CellCount> values_{};
    std::array<bool,CellCount> drains_{};
    // Static topology: each cell has a deterministic, bounded downstream
    // neighbour towards its nearest road drain. No per-frame pathfinding.
    std::array<uint16_t,CellCount> downstream_{};
    double discharged_{};
};
} // namespace lab::urban
