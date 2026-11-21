#pragma once
#include "building/BuildingPlan.h"
#include "Assets.h"
#include "WorldRegistry.h"
#include <future>
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
        bool changed{};
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
    std::vector<Buildings::Sector> sectors_;
    std::unordered_map<uint64_t,Resident> resident_;
    std::future<Candidate> job_;
    std::future<bool> saving_;
    std::optional<Candidate> candidate_;
    std::string status_,savedStatus_;
    bool unsaved_{};
    std::array<Lift,2> lifts_{};
    int riding_{-1},lastLevel_{};
    float fallSpeed_{};
    bool grounded_{};
    uint32_t mesh_{};
    std::array<uint32_t,size_t(Buildings::Material::Count)> materials_{};
    std::vector<Buildings::Part> movingParts_;

    Point Local(XMFLOAT3 p)const;
    WorldObject Visual(const Buildings::Part&,int)const;
    void Publish(WorldRegistry&,std::span<const Buildings::Sector>);
    void AdvanceLifts(WorldRegistry&,float);
    void PublishMoving(WorldRegistry&);
    bool Clear(Point,float)const;
    float Support(Point,float,float)const;
public:
    BuildingSession(const std::filesystem::path& profile,std::filesystem::path save);
    void Install(SceneData&,WorldRegistry&);
    void Tick(WorldRegistry&,XMFLOAT3 head,float dt);
    void Request(Buildings::Command);
    void RequestLift(XMFLOAT3 head,int direction);
    XMFLOAT3 Move(XMFLOAT3 previous,XMFLOAT3 proposed,double elapsed,bool gravity,bool reset);
    Target Locate(XMFLOAT3);
    XMFLOAT3 Scene(Point)const;
    Pose Spawn()const;
    const Buildings::Definition& Definition()const{return definition_;}
    const Buildings::Plan& Plan()const{return plan_;}
    const char* RoomLabel(Target)const;
    const std::string& Status()const{return status_;}
    bool Busy()const{return job_.valid()||candidate_||saving_.valid();}
    bool Grounded()const{return grounded_;}
};
}
