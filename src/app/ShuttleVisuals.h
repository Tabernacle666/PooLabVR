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
    RenderObject mapDot_,speedDust_;
    std::array<uint32_t,3> mapMaterials_{};
    std::array<RenderObject,2> flame_{};
    static constexpr size_t WakeLevels=8;
    std::array<std::array<uint32_t,WakeLevels>,3> wakeMaterials_{};
    std::vector<ShuttleWake::Triangle> canopy_;
    struct Z31Part { RenderObject object; uint32_t poweredMaterial{Invalid}; };
    std::vector<Z31Part> z31Hull_;
    std::vector<ShuttleWake::Triangle> z31Canopy_;
    void BuildWakeMaterials(SceneData&);
    void Append(RenderScene&,const FrameInput&,const ShuttlePilot&,const VrInputActions&) const;
public:
    explicit ShuttleAssets(SceneData&);
    std::span<const ShuttleWake::Triangle> Canopy(uint32_t craft=1) const noexcept {
        if(craft==2u&&!z31Canopy_.empty())return z31Canopy_;
        return canopy_;
    }
    struct Contribution {
        RenderContributions base;
        const ShuttleAssets& assets;
        const ShuttlePilot& pilot;
        RenderContributions Bind();
    };
};
}
