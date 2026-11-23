#pragma once
#include "building/BuildingPlan.h"
#include "Assets.h"
#include "WorldRegistry.h"
#include <future>
#include <memory>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace OASIS {
struct Mg2Worktop;
struct Mg2ArchitecturalPalette;
class BuildingGround {
    struct Crater {XMFLOAT3 center;float radius;};
    struct Tile {
        std::vector<TerrainTriangle> triangles;
        std::vector<Crater> craters;
    };
    std::unordered_map<uint64_t,Tile> tiles_;
    size_t craters_{};
public:
    void Set(std::span<const TerrainTriangle>);
    void AddCrater(XMFLOAT3,float radius);
    void ResetCraters();
    std::optional<float> Height(XMFLOAT3)const;
    size_t CraterCount()const{return craters_;}
};
class BuildingSession {
    using Point=Buildings::Point;
    using Box=Buildings::Box;
    using Target=Buildings::Target;
    struct Resident {Buildings::Part part;uint64_t handle;};
    struct Candidate {
        Buildings::Plan plan;
        std::vector<Buildings::Sector> sectors;
        std::string status;
        bool changed{},onlyRemoves{};
        std::vector<std::shared_ptr<const Buildings::Sector>> sources;
    };
    struct Lift {
        int stop{},destination{};
        float height{};
        float door{};
        uint32_t phase{};
    };
    Buildings::Definition definition_;
    std::filesystem::path savePath_;
    Buildings::Plan plan_;
    std::vector<std::shared_ptr<const Buildings::Sector>> sectors_,sources_;
    std::unordered_set<uint64_t> residentRooms_;
    std::unordered_set<uint64_t> heroDeskProxyIds_;
    std::optional<Point> roomViewer_;
    // Last rendered head direction drives visible-window room residency.
    // Nearby rooms remain resident independently of the camera.
    std::optional<XMFLOAT3> roomForward_;
    float roomViewFarMeters_{};
    std::array<int,2> roomDestinations_{};
    uint64_t roomGeometryRevision_{};
    std::unordered_map<uint64_t,Resident> resident_;
    std::unordered_map<uint32_t,uint64_t> partByRenderId_;
    struct WorktopResident {
        Buildings::Mount mount;
        int level;
        std::vector<uint64_t> handles;
        size_t keyboards{};
    };
    const Mg2Worktop* worktop_{};
    std::array<std::array<uint32_t,4>,size_t(Buildings::Material::Count)> wearMaterials_{};
    std::array<std::vector<uint32_t>,3> whitePaintVariants_{};
    struct RoomDecorResident {std::vector<uint64_t> handles;};
    std::unordered_map<uint64_t,RoomDecorResident> roomDecor_;
    std::unordered_set<uint64_t> forceRoomDecor_;
    std::unordered_set<uint64_t> placementWarnings_;
    std::optional<Point> roomDecorViewer_;
    uint64_t roomDecorRevision_{};
    uint64_t roomDecorGeometryRevision_{};
    void StreamRoomDecor(WorldRegistry&,XMFLOAT3 head);
    std::unordered_map<uint64_t,WorktopResident> worktops_;
    std::optional<Point> worktopViewer_;
    uint64_t worktopRevision_{};
    std::future<Candidate> job_;
    std::future<bool> saving_;
    std::optional<Candidate> candidate_;
    std::deque<Buildings::Damage> breaches_,activeBreaches_;
    std::string status_,savedStatus_;
    uint64_t geometryRevision_{1};
    uint64_t publicationRevision_{1};
    bool unsaved_{};
    std::array<Lift,2> lifts_{};
    int riding_{-1},lastLevel_{};
    float fallSpeed_{};
    bool grounded_{};
    uint32_t mesh_{},paneMesh_{};
    std::array<uint32_t,size_t(Buildings::Material::Count)> materials_{};
    std::array<uint32_t,4> paneMaterials_{};
    std::vector<Buildings::Part> movingParts_;
    const BuildingGround* ground_{};

    Point Local(XMFLOAT3 p)const;
    WorldObject Visual(const Buildings::Part&,int)const;
    void Publish(WorldRegistry&,std::span<const Buildings::Sector* const>,bool replace=false);
    void AdvanceLifts(WorldRegistry&,float);
    void PublishMoving(WorldRegistry&);
    void StartBreach();
    void QueueBreach(Buildings::Damage);
    std::optional<Buildings::Mount> WorktopSupport(const Buildings::Mount&,int level)const;
    bool Clear(Point,float)const;
    float Support(Point,float,float)const;
    bool RoomResident(const Buildings::Part& part)const{return !part.room||residentRooms_.contains(part.room);}
public:
    BuildingSession(const std::filesystem::path& profile,std::filesystem::path save);
    void Install(SceneData&,WorldRegistry&,uint32_t carpetNormal,const Mg2Worktop* worktop=nullptr,const Mg2ArchitecturalPalette* pbr=nullptr);
    void StreamWorktops(WorldRegistry&,XMFLOAT3 head);
    void PrepareRooms(WorldRegistry&,XMFLOAT3 from,XMFLOAT3 to,
                      std::optional<XMFLOAT3> forward={},float farMeters=0.f);
    std::optional<Pose> WorktopSpawn()const;
    size_t ResidentWorktops()const{return worktops_.size();}
    size_t ResidentRoomDecor()const{return roomDecor_.size();}
    size_t ResidentKeyboards()const;
    void SetGround(const BuildingGround& ground){ground_=&ground;}
    void ContinueWalk(const BuildingSession* previous);
    void AppendLights(std::vector<SceneLight>&,uint32_t beaconStep=0u)const;
    void Tick(WorldRegistry&,XMFLOAT3 head,float dt,XMFLOAT3 forward,float farMeters);
    void Request(Buildings::Command);
    // On-demand semantic re-dressing of one room (or corridor/bathroom).
    // Does not rewrite wall geometry, damage, or any other room's content.
    void FixRoom(Buildings::Target target);
    const Buildings::Joint* Inspect(const Pose&)const;
    size_t JointCount(Buildings::JointKind)const;
    void Repair(uint64_t joint,Buildings::JointKind,Buildings::PatchQuality,uint64_t revision);
    void RequestLift(XMFLOAT3 head,int direction);
    XMFLOAT3 Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset);
    Target Locate(XMFLOAT3);
    XMFLOAT3 Scene(Point)const;
    Pose Spawn()const;
    float LotDistance(XMFLOAT3)const;
    struct Hit {float fraction{};Buildings::Part part;};
    std::optional<Hit> Trace(XMFLOAT3 from,XMFLOAT3 to)const;
    void Breach(const Hit&,XMFLOAT3);
    void Saw(const Hit&,XMFLOAT3 from,XMFLOAT3 to);
    bool Projectile(uint32_t renderId,XMFLOAT3 position);
    uint64_t Revision()const{return plan_.revision;}
    uint64_t GeometryRevision()const{return geometryRevision_;}
    uint64_t PublicationRevision()const{return publicationRevision_;}
    size_t ResidentRooms()const{return residentRooms_.size();}
    const Buildings::Definition& Definition()const{return definition_;}
    const Buildings::Plan& Plan()const{return plan_;}
    const char* RoomLabel(Target)const;
    const std::string& Status()const{return status_;}
    bool Busy()const{return job_.valid()||candidate_||saving_.valid();}
    bool Grounded()const{return grounded_;}
};
}
