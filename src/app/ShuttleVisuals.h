#pragma once

#include "Assets.h"
#include "ShuttleWake.h"
#include "RenderContributions.h"
#include "shuttle/ShuttleLayout.h"

namespace OASIS {
class ShuttlePilot;
struct VrInputActions;

class ShuttleAssets {
    std::array<std::vector<RenderObject>,ShuttleLayout::ModuleCount> modules_;
    RenderObject mapDot_;
    std::array<uint32_t,3> mapMaterials_{};
    std::array<RenderObject,2> flame_{};
    static constexpr size_t WakeLevels=8;
    std::array<std::array<uint32_t,WakeLevels>,3> wakeMaterials_{};
    std::vector<ShuttleWake::Triangle> canopy_;
    void BuildWakeMaterials(SceneData&);
    void Append(RenderScene&,const FrameInput&,const ShuttlePilot&,const VrInputActions&) const;
public:
    explicit ShuttleAssets(SceneData&);
    std::span<const ShuttleWake::Triangle> Canopy() const noexcept {return canopy_;}
    struct Contribution {
        RenderContributions base;
        const ShuttleAssets& assets;
        const ShuttlePilot& pilot;
        RenderContributions Bind();
    };
};
}
