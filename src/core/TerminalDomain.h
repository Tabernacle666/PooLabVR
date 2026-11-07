#pragma once
#include "GpuPublication.h"
#include <memory>
#include "../cartridge_api/LaboVRTerminal.h"
namespace lab {

class TerminalDomain final : public GpuDomain {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    TerminalDomain();
    ~TerminalDomain() override;
    TerminalDomain(const TerminalDomain&)=delete;
    TerminalDomain& operator=(const TerminalDomain&)=delete;
    uint32_t Publish(uint64_t,const LabTerminalFrameV1&);
    uint32_t Publish(uint64_t,const LabTerminalFrameV2&);
    uint32_t HitTest(uint64_t,const LabTerminalHitQueryV2&,LabTerminalHitV2&)const;
    void SetSuppressed(bool);
    void Clear(uint64_t);
    GpuWorldDomain Identity()const noexcept override;
    const char* Name()const noexcept override;
    int PresentationOrder()const noexcept override{return 100;}
    void Initialize(GpuDomainServices&) override;
    void BuildPasses(GpuPassBuilder&,const GpuFrameContext&) override;
};
}
