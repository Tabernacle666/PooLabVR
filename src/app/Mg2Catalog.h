#pragma once
#include "Assets.h"
#include "SphereGeometry.h"
#include "WorldRegistry.h"
#include "building/BuildingPlan.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OASIS {
struct Mg2ArchitecturalPalette {
    std::array<uint32_t,size_t(Buildings::Material::Count)> material{};
    size_t sources{},mapped{};
};
struct Mg2Worktop {
    static constexpr size_t RoomProfileCount=27u;
    enum class PropKind {Keyboard,Equipment,Accessory};
    struct Prop {
        std::vector<WorldObject> parts;
        XMFLOAT3 offset,lower,upper;
        PropKind kind;
        float chance{1.f};
        size_t sourceEntry{};
    };
    struct Layout {
        std::vector<WorldObject> parts;
        size_t keyboards{};
    };
    enum class RoomMount : uint8_t { Floor, Wall, Countertop, Service };
    struct RoomProp {
        std::vector<WorldObject> parts;
        XMFLOAT3 lower{},upper{};
        float nx{},nz{},chance{1.f},yawDeg{},mountHeight{};
        RoomMount mount{RoomMount::Floor};
        bool providesCountertop{};
    };
    std::array<std::vector<Prop>,size_t(Buildings::MountSurface::Count)> surfaces;
    std::array<std::vector<RoomProp>,RoomProfileCount> roomProfiles;
    enum HeroSlot : size_t {
        Extinguisher=0,CeilingFixture,Door,Desk,OfficeChair,
        ServerRack,Toilet,Sink,Mirror,Soap,ServicePipe,CableBundle,
        SysadminMonitor,SysadminKeyboard,SysadminMouse,HeroCount
    };
    // Authoring front is declared in placement_recipe_v2.json. The GLB
    // importer negates Z, so "front" is explicitly in ENGINE coordinates.
    struct HeroVisual {
        std::vector<WorldObject> parts;
        XMFLOAT3 lower{},upper{};
        XMFLOAT2 engineFront{0.f,-1.f};
    };
    std::array<HeroVisual,HeroCount> heroes;
    std::vector<WorldObject> rooftopHvac;
    // The fixed portal does not rotate with the open door leaf.
    std::vector<WorldObject> doorFrameParts;
    // Vertex-derived height of the actual rigid desktop surface.
    float deskSurfaceY{.78f};
    float tabletopClearance{.004f};
    float networkChaseFaceGap{.29f},networkCableBaseHeight{.14f};
    float seatScreenGap{.23f};
    float extinguisherWallGap{.15f},extinguisherEndGap{.61f},extinguisherHeight{.52f};
    float corridorExtinguisherBaseOffset{.04f},corridorTrafficZ{5.03f};
    bool doorSwingAwayFromStairs{true};
    // Desk placement is inferred from the authoritative desktop recipe,
    // not from a guessed room-world Z convention.
    float screenSideZ{1.f};
    float seatDistance{.82f},seatJitterX{.24f},seatJitterZ{.15f},seatClearance{.45f};
    std::vector<WorldObject> unlitFixture;
    // Each poster ID has precisely the authored paper+frame geometry; only
    // selected IDs are instantiated, while all 100 reuse five texture atlases.
    std::array<std::vector<WorldObject>,100> posterParts;
    Layout Fit(Buildings::MountSurface,float width,float depth,uint64_t stableHost)const;
    bool Has(Buildings::MountSurface surface)const{return !surfaces[size_t(surface)].empty();}
};

class Mg2Catalog {
    struct Entry {
        std::string key;
        std::filesystem::path path;
        uint64_t triangles{};
        XMFLOAT3 lower{},upper{};
        std::optional<SceneData> scene;
        bool realPbr{};
        bool unavailable{};
        XMFLOAT3 runtimeLower{},runtimeUpper{}; // baked glTF vertices
    };
    struct Placement {size_t entry{};XMFLOAT3 offset{};float jitter{};float chance{1.f};};
    struct ForestRow {
        std::filesystem::path recipe;
        std::vector<Placement> additional;
    };
    struct ForestInstance {size_t entry{};Pose pose;float scale{};uint32_t cell{};};
    std::vector<Entry> entries_;
    std::array<std::vector<Placement>,size_t(Buildings::MountSurface::Count)> worktops_;
    struct RoomPlacement {
        size_t entry{};
        float nx{},nz{},chance{1.f},yawDeg{},mountHeight{};
        Mg2Worktop::RoomMount mount{Mg2Worktop::RoomMount::Floor};
    };
    std::array<std::vector<RoomPlacement>,Mg2Worktop::RoomProfileCount> roomPlacements_;
    std::vector<ForestRow> forestRows_;
    std::vector<ForestInstance> forest_;
    size_t plantedCells_{};
    std::filesystem::path directory_;
    uint64_t packRevision_{};
    double prepareMs_{};
    bool geometryPrepared_{};
    bool degraded_{};
    size_t EntryIndex(std::string_view key)const;
    void PrepareEntry(size_t index);
    bool TryPrepareEntry(size_t index);
    std::vector<WorldObject> AppendParts(SceneData& scene,size_t index)const;
public:
    explicit Mg2Catalog(const std::filesystem::path& directory,bool deferGeometry=false);
    void PrepareGeometry();
    void ReleasePreparedGeometry() noexcept;
    bool PrepareForest(const BuildingSiteTerrain& terrain,std::span<const BuildingFootprint> sites,unsigned treeDensity=2u);
    std::optional<Pose> ForestSpawn()const{
        if(forest_.empty())return std::nullopt;
        const auto& root=forest_[forest_.size()/2].pose.p;
        const auto eye=Add(root,{0.f,1.65f*SpectacleInteriorUnitsPerMeter,-2.f*SpectacleInteriorUnitsPerMeter});
        Pose pose{eye};
        return pose;
    }
    void InstallForest(SceneData& scene,WorldRegistry& world)const;
    void WriteSummary()const;
    bool Degraded()const{return degraded_||std::any_of(entries_.begin(),entries_.end(),[](const Entry& e){return e.unavailable;});}
    Mg2ArchitecturalPalette AppendArchitecturalMaterials(SceneData& scene);
    Mg2Worktop AppendWorktop(SceneData& scene);
};
}
