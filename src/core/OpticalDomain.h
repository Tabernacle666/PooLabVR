#pragma once
#include "GpuFrameResources.h"
#include "../cartridge_api/LaboVROptical.h"
namespace lab {
class OpticalDomain final:public GpuDomain {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    OpticalDomain();~OpticalDomain() override;
    uint32_t SetSettings(uint64_t,const LabPopulationOpticalSettingsV1&);
    void Clear(uint64_t);
    GpuWorldDomain Identity()const noexcept override{return LAB_POPULATION_OPTICAL_DOMAIN_V1;}
    const char* Name()const noexcept override{return "PopulationOptics";}
    int PresentationOrder()const noexcept override{return 95;}
    void Initialize(GpuDomainServices&) override;
    // Called after publication/light patch producers, before surface lighting.
    void BuildFields(GpuPassBuilder&,GpuFrameContext&,GpuFrameResources&);
    void BuildPasses(GpuPassBuilder&,const GpuFrameContext&) override;
    void EndFrame() override;
    uint32_t FieldCount()const noexcept;
    uint32_t FieldDescriptor()const noexcept;
};
}
