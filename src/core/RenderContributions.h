#pragma once
#include "Core.h"
#include <span>
namespace lab {
class GpuSceneCache;
struct RenderContributions {
    std::span<const uint32_t> streamMaterials{};
    void* context{};
    void (*appendContext)(void*,RenderScene&,const FrameInput&,double){};
    void (*lightContext)(void*,SceneLight&,double){};
    const GpuSceneCache* (*residentTiles)(void*){};
};
}
