#pragma once

#include "Assets.h"
#include "RenderContributions.h"
#include "shuttle/ShuttleLayout.h"

namespace OASIS {
class ShuttlePilot;
struct VrInputActions;

class ShuttleAssets {
    std::array<std::vector<RenderObject>,ShuttleLayout::ModuleCount> modules_;
    RenderObject mapDot_;
    std::array<uint32_t,3> mapMaterials_{};
    void Append(RenderScene&,const FrameInput&,const ShuttlePilot&,const VrInputActions&) const;
public:
    explicit ShuttleAssets(SceneData&);
    struct Contribution {
        RenderContributions base;
        const ShuttleAssets& assets;
        const ShuttlePilot& pilot;
        RenderContributions Bind();
    };
};
}
