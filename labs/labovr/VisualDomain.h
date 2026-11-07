#pragma once
#include "GpuPublication.h"
#include "LaboVRVisualService.h"
#include "Assets.h"
#include <filesystem>
namespace lab {
class Graphics;
inline constexpr GpuWorldDomain VisualDomainId=0x56495331u;
enum class VisualProgram {HeightPbr,HeightPbrDepth,HeightPbrOpaque,Gel,StreamTriangles,StreamLines,StreamPoints,Cosmos,LiquidOrb,BloomExtract,BloomBlur,BloomComposite,Count};
enum class VisualPhase:uint32_t {Opaque,Refractive,Overlay,Post,SpecialDepth,Count};
class VisualDomain final:public GpuDomain {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit VisualDomain(Graphics&);~VisualDomain() override;
    GpuWorldDomain Identity()const noexcept override{return VisualDomainId;}
    const char* Name()const noexcept override{return "Visual";}
    int PresentationOrder()const noexcept override{return 100;}
    void Initialize(GpuDomainServices&) override;
    void BuildPasses(GpuPassBuilder&,const GpuFrameContext&) override;
    SceneData Configure(const std::filesystem::path&,uint64_t,const LabVisualPackageV1&);
    void BorrowBatch(const LabVisualBatchV1&,uint64_t frameId,const Pose& camera);
    void AppendStandard(RenderScene&,uint32_t meshBase,uint32_t materialBase)const;
    void Clear() noexcept;
    LabU32 PollCapture(LabVisualCaptureHandleV1,LabVisualCaptureResultV1*);
    void ReleaseCapture(LabVisualCaptureHandleV1);
    bool HasPackage()const noexcept;
};
}
