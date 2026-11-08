#pragma once
#include "GpuFrameResources.h"
#include "MeshSolidPublication.h"
namespace lab {
namespace GpuDomainId {inline constexpr GpuWorldDomain Snowfall=0x534e4f57u;}
class SnowDomain final : public GpuDomain {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    SnowDomain();~SnowDomain() override;
    GpuWorldDomain Identity()const noexcept override{return GpuDomainId::Snowfall;}
    const char* Name()const noexcept override{return "Snowfall";}
    int PresentationOrder()const noexcept override{return 80;}
    void Initialize(GpuDomainServices&) override;
    void BuildPasses(GpuPassBuilder&,const GpuFrameContext&) override;
    void EndFrame() override;
};
}
