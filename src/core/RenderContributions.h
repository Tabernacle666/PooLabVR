#pragma once
#include "Core.h"
#include <span>
namespace OASIS {
class GpuSceneCache;
struct RenderContributions {
    std::span<const uint32_t> streamMaterials{};
    void* context{};
    void (*appendContext)(void*,RenderScene&,const FrameInput&,double){};
    void (*lightContext)(void*,SceneLight&,double){};
    const GpuSceneCache* (*residentTiles)(void*){};
    bool shuttleCrt{};
};
}
