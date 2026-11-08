#pragma once
#include <cstdint>
#include <vector>

namespace lab::urban {

// One tile owns its northeast intersection, the eastbound road segment,
// the northbound road segment and the northeast block interior. Ownership
// never depends on which neighbouring tiles are currently resident.
inline constexpr double BlockMeters = 120.0;
inline constexpr double RoadHalfWidth = 6.0;
inline constexpr double SidewalkWidth = 2.0;
inline constexpr double WallSetback = 2.0;
inline constexpr double LampPitch = 20.0;
inline constexpr uint64_t WorldSeed = 0x504f4f4c41425652ull;

struct Tile { int64_t x{}, z{}; bool operator==(const Tile&) const = default; };
struct Point { double x{}, z{}; };
enum class Kind : uint8_t {
    Intersection, RoadEast, RoadNorth, Sidewalk, Wall,
    Lamp, Drain, Deciduous, Conifer
};
enum class Axis : uint8_t { East, North, None };
struct Feature {
    uint64_t id{};
    Kind kind{};
    Axis axis{Axis::None};
    Point position{};
    // 0 for non-sided objects, -1 and +1 relative to the street axis.
    int side{};
    double extent{};
    double height{};
};
Tile TileAt(Point world);
Point Origin(Tile tile);
uint64_t Identity(Kind kind, Tile tile, uint32_t ordinal = 0);
uint64_t Hash(uint64_t value);
double Perlin(double x, double z, uint64_t seed = WorldSeed);
double GroundHeight(Point position);
std::vector<Feature> GenerateTile(Tile tile);

// Atmospheric/surface sources are geographical, not loaded-object pointers.
double DrainHeat(Point position, const std::vector<Feature>& sources);
double LampHeat(Point position, const std::vector<Feature>& sources);

struct Flash {
    uint64_t id{};
    double begins{}, ends{};
};
// Query by elapsed time, not a frame-sampled boolean: even a 20 ms outage
// produces one event when an XR frame skips over the complete outage.
std::vector<Flash> FlashEvents(uint64_t lampId, double fromSeconds, double toSeconds);
bool LampOff(uint64_t lampId, double seconds);

} // namespace lab::urban
