#pragma once
#include "building/BuildingPlan.h"
#include "Assets.h"
#include "WorldRegistry.h"
#include <future>
#include <memory>
#include <deque>
#include <optional>
#include <unordered_map>

namespace OASIS {
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
    std::unordered_map<uint64_t,Resident> resident_;
    std::unordered_map<uint32_t,uint64_t> partByRenderId_;
    std::future<Candidate> job_;
    std::future<bool> saving_;
    std::optional<Candidate> candidate_;
    std::deque<Buildings::Damage> breaches_,activeBreaches_;
    std::string status_,savedStatus_;
    uint64_t geometryRevision_{1};
    bool unsaved_{};
    std::array<Lift,2> lifts_{};
    int riding_{-1},lastLevel_{};
    float fallSpeed_{};
    bool grounded_{};
    uint32_t mesh_{},paneMesh_{};
    std::array<uint32_t,size_t(Buildings::Material::Count)> materials_{};
    std::vector<Buildings::Part> movingParts_;
    std::span<const TerrainTriangle> ground_;

    Point Local(XMFLOAT3 p)const;
    WorldObject Visual(const Buildings::Part&,int)const;
    void Publish(WorldRegistry&,std::span<const Buildings::Sector>,bool replace=false);
    void AdvanceLifts(WorldRegistry&,float);
    void PublishMoving(WorldRegistry&);
    void StartBreach();
    bool Clear(Point,float)const;
    float Support(Point,float,float)const;
public:
    BuildingSession(const std::filesystem::path& profile,std::filesystem::path save);
    void Install(SceneData&,WorldRegistry&,uint32_t carpetNormal);
    void SetGround(std::span<const TerrainTriangle> ground){ground_=ground;}
    void ContinueWalk(const BuildingSession* previous);
    void AppendLights(std::vector<SceneLight>&)const;
    void Tick(WorldRegistry&,XMFLOAT3 head,float dt);
    void Request(Buildings::Command);
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
    bool Projectile(uint32_t renderId,XMFLOAT3 position);
    uint64_t Revision()const{return plan_.revision;}
    uint64_t GeometryRevision()const{return geometryRevision_;}
    const Buildings::Definition& Definition()const{return definition_;}
    const Buildings::Plan& Plan()const{return plan_;}
    const char* RoomLabel(Target)const;
    const std::string& Status()const{return status_;}
    bool Busy()const{return job_.valid()||candidate_||saving_.valid();}
    bool Grounded()const{return grounded_;}
};
}
