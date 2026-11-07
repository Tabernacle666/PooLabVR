#pragma once
#include "RenderContributions.h"
#include <vector>
namespace lab {
class ClientLayers {
    RenderContributions persistent_{},active_{};
    std::vector<uint32_t> materials_;
    static void Append(void*,RenderScene&,const FrameInput&,double);
    static void Light(void*,SceneLight&,double);
public:
    ClientLayers()=default;
    ~ClientLayers()=default;
    void Set(RenderContributions persistent,RenderContributions active={});
    RenderContributions Contributions()const;
};
}
