#include "GraphicsInternal.h"
#include "WorldRegistry.h"
#include <utility>
#include <algorithm>
#include <cstdio>
#include <atomic>
namespace OASIS {
void DiagnosticLine(const char*) noexcept;
namespace {
void DumpRemovedDevice(ID3D12Device* device,HRESULT observed=S_OK) noexcept {
    if(!device)return;
    const HRESULT reason=device->GetDeviceRemovedReason();
    if(SUCCEEDED(reason)&&observed!=DXGI_ERROR_DEVICE_REMOVED&&observed!=DXGI_ERROR_DEVICE_RESET&&observed!=DXGI_ERROR_DEVICE_HUNG)return;
    static std::atomic<bool> reported{};
    if(reported.exchange(true))return;
    char line[1024];
    std::snprintf(line,sizeof(line),"dred_begin reason=0x%08lx observed=0x%08lx\r\n",static_cast<unsigned long>(reason),static_cast<unsigned long>(observed));
    DiagnosticLine(line);
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if(FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))){
        DiagnosticLine("dred_unavailable\r\n");return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if(SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))){
        unsigned nodes=0;
        for(auto* n=crumbs.pHeadAutoBreadcrumbNode;n&&nodes<256;n=n->pNext,++nodes){
            const UINT done=n->pLastBreadcrumbValue?*n->pLastBreadcrumbValue:0;
            if(n->pLastBreadcrumbValue&&done>=n->BreadcrumbCount)continue;
            std::snprintf(line,sizeof(line),
                "dred_list queue=%.120s list=%.220s completed=%u total=%u has_counter=%u\r\n",
                n->pCommandQueueDebugNameA?n->pCommandQueueDebugNameA:"?",
                n->pCommandListDebugNameA?n->pCommandListDebugNameA:"?",
                done,n->BreadcrumbCount,unsigned(n->pLastBreadcrumbValue!=nullptr));
            DiagnosticLine(line);
            if(n->pCommandHistory){
                const UINT pivot=std::min(done,n->BreadcrumbCount);
                const UINT first=pivot>6?pivot-6:0;
                const UINT last=std::min(n->BreadcrumbCount,pivot+6);
                for(UINT i=first;i<last;++i){
                    std::snprintf(line,sizeof(line),"dred_op index=%u operation=%u completed=%u\r\n",
                        i,unsigned(n->pCommandHistory[i]),unsigned(i<done));
                    DiagnosticLine(line);
                }
            }
        }
        std::snprintf(line,sizeof(line),"dred_nodes_examined=%u\r\n",nodes);DiagnosticLine(line);
    }else DiagnosticLine("dred_breadcrumbs_unavailable\r\n");
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if(SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))){
        std::snprintf(line,sizeof(line),"dred_page_fault_va=0x%llx\r\n",
            static_cast<unsigned long long>(fault.PageFaultVA));DiagnosticLine(line);
        auto allocations=[&](const D3D12_DRED_ALLOCATION_NODE* node,const char* kind){
            for(unsigned i=0;node&&i<32;++i,node=node->pNext){
                std::snprintf(line,sizeof(line),"dred_allocation kind=%s type=%u name=%.240s\r\n",
                    kind,unsigned(node->AllocationType),node->ObjectNameA?node->ObjectNameA:"?");
                DiagnosticLine(line);
            }
        };
        allocations(fault.pHeadExistingAllocationNode,"existing");
        allocations(fault.pHeadRecentFreedAllocationNode,"freed");
    }else DiagnosticLine("dred_page_fault_unavailable\r\n");
    DiagnosticLine("dred_end\r\n");
}
}
void GraphicsCheckHr(ID3D12Device* device,HRESULT hr,const char* phase){
    if(FAILED(hr)){
        char line[384];std::snprintf(line,sizeof(line),"gpu_failure phase=%s hr=0x%08lx\r\n",phase,static_cast<unsigned long>(hr));
        DiagnosticLine(line);DumpRemovedDevice(device,hr);
    }
    Hr(hr,phase);
}
namespace graphics_internal {
thread_local TextureDependencies textureDependencies;
}

void Graphics::RequestCapture(const std::wstring& path){impl_->RequestCapture(path);}
void Graphics::UpdateDlss(int direction){impl_->UpdateDlss(direction);}
bool Graphics::PrepareDlss(DlssPreset preset){return impl_->PrepareDlss(preset);}
bool Graphics::SetDlssScale(float scale){return impl_->SetDlssScale(scale);}
DlssScaleControl Graphics::GetDlssScaleControl()const{
    const auto& dlss=impl_->dlss;
    return {dlss.requested>0.f?dlss.requested:1.f,dlss.minScale,dlss.maxScale,
            dlss.available&&impl_->MsaaSamples==1u};
}
void Graphics::SetDlssDiagnostic(bool enabled){
    if(enabled&&!impl_->dlssDiagnosticEnabled)impl_->dlssDiagnostic={};
    impl_->dlssDiagnosticEnabled=enabled;
}
std::vector<BuildingImpact> Graphics::DrainBuildingImpacts(){
    std::vector<BuildingImpact> impacts;
    const auto done=impl_->direct.fence->GetCompletedValue();
    if(done==UINT64_MAX)return impacts;
    std::array<Impl::CutFrame*,GpuSceneCache::FrameSlots> ready{};
    size_t count=0;
    for(auto& frame:impl_->cutFrames)
        if(frame.buildingFence&&done>=frame.buildingFence)ready[count++]=&frame;
    std::sort(ready.begin(),ready.begin()+count,[](const auto* a,const auto* b){return a->buildingFrame<b->buildingFrame;});
    for(size_t i=0;i<count;++i){
        auto& frame=*ready[i];
        const D3D12_RANGE read{0,(Impl::CutProjectileCapacity+1u)*sizeof(BuildingImpact)};
        const D3D12_RANGE written{0,0};
        void* memory=nullptr;
        if(FAILED(frame.buildingReadback->Map(0,&read,&memory)))break;
        const auto first=impacts.size();
        const auto* hit=static_cast<const BuildingImpact*>(memory);
        const uint32_t firstSlot=frame.buildingStepped?0u:Impl::CutProjectileCapacity;
        for(uint32_t n=firstSlot;n<=Impl::CutProjectileCapacity;++n)if(hit[n].projectile){
            auto& impact=impacts.emplace_back(hit[n]);
            impact.geometryRevision=frame.buildingGeometryRevision;
        }
        frame.buildingReadback->Unmap(0,&written);
        std::sort(impacts.begin()+first,impacts.end(),[](const BuildingImpact& a,const BuildingImpact& b){
            if(a.hitTime!=b.hitTime)return a.hitTime<b.hitTime;
            return a.projectile<b.projectile;
        });
        frame.buildingFence=0;
    }
    return impacts;
}
void Graphics::SetBuildingRevision(uint64_t revision,std::span<const SceneLight> lights){
    const auto previous=std::exchange(impl_->buildingRevision,revision);
    if(previous==revision){
        // Update blinking light power without rebuilding geometry
        // or allocating per-frame light arrays.
        if(impl_->buildingLights.size()!=lights.size()||
           !std::equal(impl_->buildingLights.begin(),impl_->buildingLights.end(),
              lights.begin(),[](const SceneLight& a,const SceneLight& b){
                  return a.power==b.power;
              }))
            impl_->buildingLights.assign(lights.begin(),lights.end());
        return;
    }
    impl_->buildingLights.assign(lights.begin(),lights.end());
}
void Graphics::EnableSpherePair(const AtmosphereSettings& companion,uint32_t count){
    if(count<SpectaclePairCount||count>SpectacleSphereCount)Fail("Spectacle sphere count");
    impl_->companion=companion;
    impl_->sphereCount=count;
}
void Graphics::SetDiagnosticRayTracing(bool enabled){
    // Benchmark-only: hold the first fully published MG2 selection for
    // both phases. Do not replace it with the ordinary source meshes.
    impl_->diagnosticFreezeMg2=true;
    if(impl_->diagnosticDxrEnabled==enabled)return;
    impl_->diagnosticDxrEnabled=enabled;
    impl_->historyReady=false;
    impl_->roomProbeSlots.clear();impl_->roomProbeCredits=0.;impl_->roomProbeLastSchedule=-1.;
    for(uint32_t i=0;i<impl_->frames.size();++i){
        impl_->frames[i].tlasBuilt=false;
        impl_->rayScene.InvalidateFrame(i);
    }
}
void Graphics::EnableDepthDiagnostics(){impl_->depthDiagnosticEnabled=true;}
Graphics::DepthMode Graphics::RequestedDepthMode()const{
    return impl_->depthBaselineRequested?DepthMode::Baseline:impl_->depthHybridRequested?DepthMode::Hybrid:DepthMode::Selective;
}
Graphics::DepthMode Graphics::ActiveDepthMode()const{
    return impl_->depthHybridActive?DepthMode::Hybrid:impl_->depthSplitActive?DepthMode::Selective:DepthMode::Baseline;
}
void Graphics::EnableSphereBaselineTiming(bool scenePasses){
    impl_->sphereBaselineTiming=true;
    impl_->scenePassTiming=scenePasses;
}
size_t Graphics::ReadSphereGpuTimings(std::array<SphereGpuTimings,12>& output){
    impl_->CollectGpuTimings();
    const auto count=impl_->sphereGpuTimingCount;
    std::copy_n(impl_->sphereGpuTimings.begin(),count,output.begin());
    impl_->sphereGpuTimingCount=0;
    return count;
}
DlssDiagnosticState Graphics::GetDlssDiagnostic()const{
    auto result=impl_->dlssDiagnostic;
    result.requestedScale=impl_->dlss.requested;
    result.initialized=impl_->dlss.initialized;
    result.reconstructed=impl_->dlss.historyReady;
    const auto fence=impl_->dlss.lastUseFence;
    result.fencePending=fence&&impl_->direct.fence->GetCompletedValue()<fence;
    return result;
}
void Graphics::SetDeveloperMode(bool enabled){impl_->developerMode=enabled;}
void Graphics::SetAtmosphere(const AtmosphereSettings& settings){
    const float values[]{settings.center.x,settings.center.y,settings.center.z,settings.radius,settings.color.x,settings.color.y,settings.color.z,settings.density,settings.emissionReferencePower};
    for(float value:values)if(!std::isfinite(value))Fail("Invalid atmosphere setting");
    if(settings.radius<0||settings.density<0||settings.emissionReferencePower<0)Fail("Invalid atmosphere setting");
    for(float value:{settings.forestFog,settings.forestDew,settings.forestInsects})
        if(!std::isfinite(value)||std::abs(value)>1.f)Fail("Invalid forest atmosphere setting");
    for(float value:{settings.forestFogTone,settings.forestFogContrast,settings.forestBarkSpecular})
        if(!std::isfinite(value)||value<0.f||value>1.f)Fail("Invalid forest appearance");
    const auto& film=settings.film;
    const float filmValues[]{film.patternSpeed,film.opacity,film.deformation,film.glitter,
        film.glitterWidth,film.waveStrength,film.waveSpeed,film.waveDuration};
    for(float value:filmValues)if(!std::isfinite(value)||value<0.f)Fail("Invalid membrane setting");
    if(film.waveDuration<=0.f)Fail("Invalid membrane wave duration");
    const auto& current=impl_->atmosphere;
    if(settings.membrane!=current.membrane||settings.radius!=current.radius||
       settings.center.x!=current.center.x||settings.center.y!=current.center.y||
       settings.center.z!=current.center.z)
        impl_->sphereMembranes[0].resetHistory=true;
    impl_->atmosphere=settings;
}
void Graphics::InitializeDesktop(){impl_->InitializeDesktop();}
void Graphics::ConfigureOutput(const OutputSurface& surface,const SceneData& scene,JobSystem& jobs,RenderContributions contributions){impl_->ConfigureOutput(surface,scene,jobs,contributions);}
const char* Graphics::StartupCacheReport() const noexcept{return impl_->arb0Report;}
void Graphics::ReleaseOutput(){impl_->ReleaseOutput();}
bool Graphics::CaptureDone()const{return impl_->capture.recorded||impl_->capture.failed;}
bool Graphics::CaptureFailed()const{return impl_->capture.failed;}
bool Graphics::SaveCapture(){return impl_->SaveCapture();}
bool Graphics::CapturePending()const{
    const auto& capture=impl_->capture;
    return capture.requested||capture.recorded||capture.failed;
}
std::future<bool> Graphics::FinishCaptureAsync(){return impl_->FinishCaptureAsync();}
GraphicsMetrics Graphics::GetMetrics()const{
    const auto budget=impl_->managedMemory.Budget();
    return {impl_->hudFps,budget.usage,budget.budget,impl_->width,impl_->height};
}

RenderingOptions Graphics::GetRenderingOptions()const{
    const auto aa=impl_->requestedSamples==4u?SceneAntialiasing::Msaa4:SceneAntialiasing(impl_->aaMode);
    return {impl_->reflectionMode,aa,impl_->peripheralShading,impl_->solarVisibilityMode};
}
RenderingState Graphics::GetRenderingState()const{
    auto state=impl_->renderingState;
    state.msaa4Supported=impl_->msaa4Supported;
    if(!state.outputWidth){
        state.outputWidth=impl_->width;
        state.outputHeight=impl_->height;
    }
    return state;
}
void Graphics::SetPrismProfile(uint32_t profile){
    if(profile>6u)Fail("Invalid PRISM profile");
    impl_->prismProfile=profile;
}
void Graphics::SetPrismSceneMask(uint32_t mask){
    if(mask>31u||(mask>7u&&(mask&16u)==0u))Fail("Invalid PRISM/LOD scene mask");
    if(impl_->prismSceneMask!=mask)impl_->historyReady=false;
    impl_->prismSceneMask=mask;
}
bool Graphics::SetRenderingOptions(const RenderingOptions& options){
    if(options.reflections>SceneReflections::Ambient||options.antialiasing>SceneAntialiasing::Msaa4||
       options.peripheral>PeripheralShading::FullRate||options.solarVisibility>SceneSolarVisibility::NoShadows)return false;
    const bool msaa=options.antialiasing==SceneAntialiasing::Msaa4;
    if(!SetMultisampling(msaa?4u:1u))return false;
    if(!msaa){
        SetAntialiasing(uint32_t(options.antialiasing));
        impl_->msaa4AllocationFailed=false;
    }
    if(impl_->reflectionMode!=options.reflections)impl_->historyReady=false;
    impl_->reflectionMode=options.reflections;
    impl_->peripheralShading=options.peripheral;
    impl_->solarVisibilityMode=options.solarVisibility;
    return true;
}
bool Graphics::SetAntialiasing(uint32_t mode){
    if(mode>2)return false;
    impl_->aaMode=mode;return true;
}
bool Graphics::SetMultisampling(uint32_t samples){
    if((samples!=1&&samples!=4)||!impl_->device)return false;
    if(samples==4&&!impl_->SupportsMsaa4())return false;
    impl_->requestedSamples=samples;return true;
}
bool Graphics::SetSurfaceSettings(const SurfaceSettings& s){
    if(s.material>65535||(!impl_->surfaceData.empty()&&s.material>=impl_->surfaceData.size()))return false;
    const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(!std::isfinite(v[i])||v[i]<lo[i]||v[i]>hi[i])return false;
    auto& edits=impl_->surfaceEdits;
    auto it=std::find_if(edits.begin(),edits.end(),[&](const auto& edit){return edit.material==s.material;});
    if(it==edits.end())edits.push_back(s);else *it=s;
    impl_->ApplySurfaceEdits(std::span<const uint32_t>(&s.material,1));
    return true;
}
void Graphics::SetMaterialTuning(std::span<const MaterialTuning> tuning){
    auto& saved=impl_->materialTuning;
    if(std::equal(saved.begin(),saved.end(),tuning.begin(),tuning.end()))return;
    std::vector<uint32_t> edited;
    for(const auto& [slot,indices]:impl_->materialSurfacesBySlot){
        const auto& before=slot<saved.size()?saved[slot]:DefaultMaterialTuning;
        const auto& after=slot<tuning.size()?tuning[slot]:DefaultMaterialTuning;
        if(before==after)continue;
        edited.insert(edited.end(),indices.begin(),indices.end());
    }
    saved.assign(tuning.begin(),tuning.end());
    if(!edited.empty()){
        std::sort(edited.begin(),edited.end());
        impl_->ApplySurfaceEdits(edited);
    }
    impl_->historyReady=false;
}
Graphics::Graphics():impl_(std::make_unique<Impl>()){}Graphics::~Graphics(){if(impl_)DumpRemovedDevice(impl_->device.Get());}void Graphics::Initialize(const XrGraphicsRequirementsD3D12KHR& r){impl_->Initialize(r);}void Graphics::Configure(const XR& x,const SceneData& assets,JobSystem& jobs,RenderContributions extra){impl_->Configure(x,assets,jobs,extra);}void Graphics::ReplaceScene(const SceneData& assets,RenderContributions extra){impl_->ReplaceScene(assets,extra);}void Graphics::UnloadScene(){impl_->UnloadScene();}bool Graphics::SceneLoaded()const{return impl_->sceneLoaded;}void Graphics::RegisterDomain(std::unique_ptr<GpuDomain> domain){impl_->RegisterDomain(std::shared_ptr<GpuDomain>(std::move(domain)));}void Graphics::RegisterSharedDomain(std::shared_ptr<GpuDomain> domain){impl_->RegisterDomain(std::move(domain));}void Graphics::InitializeDomain(GpuDomain& domain){if(!impl_->domainServicesReady)Fail("GPU domain services not initialized");domain.Initialize(*impl_);}bool Graphics::UnregisterDomain(GpuWorldDomain id){return impl_->UnregisterDomain(id);}void Graphics::AttachStream(GpuStream& stream){if(stream.Domain()==GpuDomainId::Invalid||!impl_->FindDomain(stream.Domain()))Fail("GPU stream domain is not registered");impl_->streams.push_back(&stream);impl_->publications.reserve(impl_->streams.size());}void Graphics::DetachStream(GpuStream& stream){impl_->DetachStream(stream);}void Graphics::ReserveStreams(){impl_->ReserveStreams();}void Graphics::CancelReservedStreams(){impl_->CancelReservedStreams();}bool Graphics::StageForestGrowth(std::span<const WorldObject> flora,std::span<const uint64_t> floraIds,
    std::span<const WorldObject> trees,std::span<const uint64_t> treeIds){
    return impl_->StageForestGrowth(flora,floraIds,trees,treeIds);
}
void Graphics::CommitForestGrowth(){impl_->CommitForestGrowth();}
bool Graphics::PrepareLoading(const XRFrame& f,std::atomic<uint32_t>& progress,const std::atomic<bool>& cancel){try{return impl_->PrepareLoading(f,progress,cancel);}catch(...){DumpRemovedDevice(impl_->device.Get());throw;}}
bool Graphics::WarmLoadingFrame(XRFrame& f,std::atomic<uint32_t>& progress,const std::atomic<bool>& cancel){
    try{return impl_->WarmLoadingFrame(f,progress,cancel);}catch(...){DumpRemovedDevice(impl_->device.Get());throw;}
}
bool Graphics::Prepare(const FrameInput& s){
    if(!impl_->developerMode&&!impl_->dlssDiagnosticEnabled)return impl_->Prepare(s);
    const int64_t start=QpcCounter();
    const bool ready=impl_->Prepare(s);
    impl_->cpuPrepareMs=float(QpcSeconds(QpcCounter()-start)*1000.0);
    return ready;
}void Graphics::Render(XRFrame& f,const GpuProducerCoordinator& coordinator){try{impl_->Render(f,coordinator);}catch(...){DumpRemovedDevice(impl_->device.Get());throw;}}void Graphics::WaitIdle(){impl_->WaitStartup(impl_->direct);impl_->WaitStartup(impl_->compute);impl_->WaitStartup(impl_->copy);}ID3D12Device* Graphics::Device()const{return impl_->device.Get();}ID3D12RootSignature* Graphics::BindlessRootSignature()const{return impl_->root.Get();}ID3D12DescriptorHeap* Graphics::BindlessResourceHeap()const{return impl_->descriptors.Get();}ID3D12DescriptorHeap* Graphics::BindlessSamplerHeap()const{return impl_->samplers.Get();}GpuResourceLease Graphics::CreateManagedBuffer(const GpuBufferDesc& d){return impl_->CreateManagedBuffer(d);}GpuResourceLease Graphics::CreateManagedTexture2D(const GpuTextureDesc& d){return impl_->CreateManagedTexture2D(d);}GpuResourceLease Graphics::CreateManagedTexture3D(const GpuTextureDesc& d){return impl_->CreateManagedTexture3D(d);}GpuResourceLease Graphics::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){return impl_->UploadStaticBuffer(bytes,stride,name);}GpuResourceLease Graphics::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){return impl_->UploadStaticTexture2D(d,subresources);}uint32_t Graphics::DescriptorIndex(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorIndex(h):Invalid;}uint32_t Graphics::DescriptorGeneration(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorGeneration(h):0;}ID3D12CommandQueue* Graphics::Queue()const{return impl_->direct.queue.Get();}
}
