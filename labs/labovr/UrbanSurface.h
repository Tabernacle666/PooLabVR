#pragma once
#include "UrbanLayout.h"
#include <array>

namespace lab::urban {
// Persistent coarse surface state; all quantities are meters of water equivalent.
class SurfaceTile {
public:
    static constexpr unsigned Resolution=24;
    struct Sample {float snow{},water{},wetness{},height{},heat{};};
    explicit SurfaceTile(Tile tile);
    void Advance(double elapsedSeconds,double snowfallMetersPerSecond,double airTemperatureC);
    const Sample& At(unsigned x,unsigned z) const { return values_[z*Resolution+x]; }
    Sample& At(unsigned x,unsigned z) { return values_[z*Resolution+x]; }
    double StoredWater() const;
    double DischargedWater() const {return discharged_; }
    Tile Key() const {return key_;}
private:
    Tile key_{};
    std::array<Sample,Resolution*Resolution> values_{};
    std::array<bool,Resolution*Resolution> drains_{};
    double discharged_{};
};
} // namespace lab::urban
