#pragma once
#include "Assets.h"
#include "WorldRegistry.h"
#include <array>

namespace OASIS {

// A destructive subtraction removes geometry; it never creates rigid bodies or debris.
class DestructibleScene {
public:
    struct Hit {XMFLOAT3 position{},normal{};float distance{};};
    explicit DestructibleScene(SceneData& scene);
    bool Trace(XMFLOAT3 start,XMFLOAT3 direction,float range,Hit& hit) const;
    bool Carve(XMFLOAT3 center,float radius);
    uint32_t BeamMesh() const noexcept{return beamMesh_;}
    uint32_t BallMesh() const noexcept{return ballMesh_;}
    uint32_t BeamMaterial() const noexcept{return beamMaterial_;}
    uint32_t BallMaterial() const noexcept{return ballMaterial_;}
private:
    struct Triangle {Vertex v[3];};
    struct Part {std::vector<Triangle> triangles;RenderObject object{};};
    SceneData* scene_{};
    std::vector<Part> parts_;
    uint32_t soilMaterial_{},beamMaterial_{},ballMaterial_{},beamMesh_{},ballMesh_{};
    void AddTerrain();
    void Rebuild();
};

class DemolitionControls {
    DestructibleScene& geometry_;
    WorldRegistry& world_;
    WorldObject beam_{},ball_{};
    uint64_t beamId_{},ballId_{};
    XMFLOAT3 position_{},velocity_{};
    double lastTime_{},flightTime_{};
    bool flying_{};
public:
    DemolitionControls(DestructibleScene&,WorldRegistry&);
    // True only when the scene's actual triangle geometry changed.
    bool Tick(const FrameInput&,bool vr);
};
}
