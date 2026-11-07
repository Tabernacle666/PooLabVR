#pragma once
#include "GpuPublication.h"
#include "../cartridge_api/LaboVRAtmosphere.h"
namespace lab {
class AtmosphereDomain final:public GpuDomain {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    AtmosphereDomain();~AtmosphereDomain() override;
    uint32_t SetSettings(uint64_t,const LabFogSettingsV1&);
    uint32_t SetInput(uint64_t,const LabFogInputV1&);
    uint32_t SetWind(uint64_t,const LabFogWindV1&);
    uint32_t SetTransport(uint64_t,const LabFogTransportV1&);
    void Clear(uint64_t);
    GpuWorldDomain Identity()const noexcept override{return LAB_ATMOSPHERE_DOMAIN_V1;}
    const char* Name()const noexcept override{return "Atmosphere";}
    int PresentationOrder()const noexcept override{return 90;}
    void Initialize(GpuDomainServices&) override;
    void EndFrame() override;
    void BuildPasses(GpuPassBuilder&,const GpuFrameContext&) override;
};
}
