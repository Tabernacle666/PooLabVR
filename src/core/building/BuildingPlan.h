#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace OASIS::Buildings {
struct Point {float x{},y{},z{}; bool operator==(const Point&)const=default;};
struct Box {Point lo,hi; bool operator==(const Box&)const=default;};
struct Rect {float left{},front{},right{},back{}; bool operator==(const Rect&)const=default;};
enum class JointKind : uint8_t { PartitionFacade, StairReturn, Count };
enum class PatchQuality : uint8_t { Minimum=1, Balanced, Detailed };
enum class ServiceKind : uint8_t { Chase, Plenum, Chamber, Grille, Air, Data, Power, Fluid, Drain, ColdWater, HotWater };

enum class Scope : uint8_t { Room, Floor, Building };
enum class Action : uint8_t { Regenerate, Approve, Unlock };
enum class Use : uint8_t { Office, Meeting, Reception, Research, Service, Machine, Penthouse };
enum class Material : uint8_t {
    Concrete, Metal, Wall, Floor, Glass, Wood, Lamp, Safety, Cable,
    Ceiling, BlackCeiling, Sage, Sand, Mist, Lilac, Cherry, Moulding,
    BlueCarpet, GreyCarpet, VioletCarpet, Ceramic, DrainPipe, WaterPipe,
    LampAmber, LampRed, LampOff, Count
};
enum class DamageKind : uint8_t { Explosion, Perforation, Impact, Sawing, Fire, Water, Electricity, Count };

struct Definition {
    uint64_t id{},seed{};
    Point origin;
    float height{},width{},depth{};
    int floors{},voidStart{},voidEnd{};
    Rect atrium;
    float slab{},plenum{};
    uint32_t mergeEvery{},receptionEvery{};
    std::array<uint32_t,4> roomWeights{};
    float xScale{1.f};
    char letter{'O'};
    float Elevation(int level)const;
    bool HasVoid(int level)const{return voidStart>0&&level>=voidStart&&level<=voidEnd;}
};

struct Room {
    uint64_t id{};
    Rect bounds;
    uint32_t variant{};
    Use use{};
    bool approved{};
    bool operator==(const Room&)const=default;
};

struct Level {
    int number{};
    uint32_t variant{};
    bool approved{};
    std::vector<Room> rooms;
    bool operator==(const Level&)const=default;
};

struct Target {
    int level{-2};
    uint64_t room{};
    bool inside{};
    bool operator==(const Target&)const=default;
};

inline constexpr size_t MaxDamageRecords=1024;

struct Damage {
    uint64_t part{};
    Box original,opening;
};

struct Plan {
    uint64_t revision{1};
    uint32_t variant{};
    bool approved{};
    std::vector<Level> levels;
    std::array<uint8_t,size_t(JointKind::Count)> jointRepairs{};
    std::vector<Damage> damage;
    const Level* At(int level)const;
    bool Locked(Scope scope,Target target)const;
};

struct Part {
    uint64_t id{};
    Box bounds;
    Material material{};
    bool solid{};
    uint64_t source{};
    Box original{};
    bool operator==(const Part&)const=default;
};

struct Joint {
    uint64_t id{};
    uint64_t room{};
    JointKind kind{};
    std::array<Box,2> sources;
    Box seam;
    bool approved{};
};

// Hosts are stable solid IDs. Endpoints resolve to other service nodes in the
// same definition, even when either sector is absent from the render cache.
struct ServiceNode {
    uint64_t id{},host{},previous{};
    ServiceKind kind{};
    Box bounds;
};
struct Mount {
    uint64_t host{},room{};
    Rect support;
    float height{};
};

struct RoomLight {
    uint64_t host{};
    Box room;
    Point position;
    Material lens{};
};

struct Sector {
    int level{};
    std::vector<Part> parts;
    std::vector<Joint> joints;
    std::vector<ServiceNode> services;
    std::vector<Mount> mounts;
    std::vector<RoomLight> lights;
};

struct Command {
    Target target;
    Scope scope{};
    Action action{};
    int step{1};
    uint64_t revision{};
};

Definition ReadDefinition(const std::filesystem::path&);
Plan Generate(const Definition&);
Plan Load(const std::filesystem::path&,const Definition&);
bool Save(const std::filesystem::path&,const Definition&,const Plan&);
bool Apply(const Definition&,Plan&,const Command&,std::string&);
std::vector<Sector> Build(const Definition&,const Plan&);
const char* Label(Use);
inline constexpr Rect StairOpening{.7f,7.0f,5.85f,11.7f};
inline constexpr std::array<Rect,2> Shafts{{{6.1f,6.1f,8.4f,8.6f},{29.4f,6.1f,31.7f,8.6f}}};
inline constexpr float EastStairOffset=22.1f;
Rect StairAt(float offset);
bool Serves(unsigned lift,int level,int floors);
Rect BathroomBounds(const Definition&,unsigned wing);
std::array<Rect,2> GlyphBands(const Definition&,int level);
inline constexpr uint64_t BathroomTarget=UINT64_MAX-4;
}
