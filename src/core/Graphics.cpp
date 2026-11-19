#include "GraphicsInternal.h"
namespace OASIS {
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
void Graphics::EnableSphereBaselineTiming(){impl_->sphereBaselineTiming=true;}
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
    const auto& film=settings.film;
    const float filmValues[]{film.patternSpeed,film.opacity,film.deformation,film.glitter,
        film.glitterWidth,film.waveStrength,film.waveSpeed,film.waveDuration};
    for(float value:filmValues)if(!std::isfinite(value)||value<0.f)Fail("Invalid membrane setting");
    if(film.waveDuration<=0.f)Fail("Invalid membrane wave duration");
    const auto& current=impl_->atmosphere;
    if(settings.membrane!=current.membrane||settings.radius!=current.radius||
       settings.center.x!=current.center.x||settings.center.y!=current.center.y||
       settings.center.z!=current.center.z)
        impl_->membrane.resetHistory=true;
    impl_->atmosphere=settings;
}
void Graphics::InitializeDesktop(){impl_->InitializeDesktop();}
void Graphics::ConfigureOutput(const OutputSurface& surface,const SceneData& scene,JobSystem& jobs,RenderContributions contributions){impl_->ConfigureOutput(surface,scene,jobs,contributions);}
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

bool Graphics::SetAntialiasing(uint32_t mode){
    if(mode>2)return false;
    impl_->aaMode=mode;return true;
}
bool Graphics::SetMultisampling(uint32_t samples){
    if((samples!=1&&samples!=4)||!impl_->device)return false;
    if(samples==4)for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        if(FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))||!q.NumQualityLevels)return false;
    }
    impl_->requestedSamples=samples;return true;
}
bool Graphics::SetSurfaceSettings(const SurfaceSettings& s){
    if(s.material>65535||(!impl_->surfaceData.empty()&&s.material>=impl_->surfaceData.size()))return false;
    const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(!std::isfinite(v[i])||v[i]<lo[i]||v[i]>hi[i])return false;
    auto& edits=impl_->surfaceEdits;
    auto it=std::find_if(edits.begin(),edits.end(),[&](const auto& edit){return edit.material==s.material;});
    if(it==edits.end())edits.push_back(s);else *it=s;
    impl_->ApplySurfaceEdits();return true;
}
Graphics::Graphics():impl_(std::make_unique<Impl>()){}Graphics::~Graphics()=default;void Graphics::Initialize(const XrGraphicsRequirementsD3D12KHR& r){impl_->Initialize(r);}void Graphics::Configure(const XR& x,const SceneData& assets,JobSystem& jobs,RenderContributions extra){impl_->Configure(x,assets,jobs,extra);}void Graphics::ReplaceScene(const SceneData& assets,RenderContributions extra){impl_->ReplaceScene(assets,extra);}void Graphics::UnloadScene(){impl_->UnloadScene();}bool Graphics::SceneLoaded()const{return impl_->sceneLoaded;}void Graphics::RegisterDomain(std::unique_ptr<GpuDomain> domain){impl_->RegisterDomain(std::shared_ptr<GpuDomain>(std::move(domain)));}void Graphics::RegisterSharedDomain(std::shared_ptr<GpuDomain> domain){impl_->RegisterDomain(std::move(domain));}void Graphics::InitializeDomain(GpuDomain& domain){if(!impl_->domainServicesReady)Fail("GPU domain services not initialized");domain.Initialize(*impl_);}bool Graphics::UnregisterDomain(GpuWorldDomain id){return impl_->UnregisterDomain(id);}void Graphics::AttachStream(GpuStream& stream){if(stream.Domain()==GpuDomainId::Invalid||!impl_->FindDomain(stream.Domain()))Fail("GPU stream domain is not registered");impl_->streams.push_back(&stream);impl_->publications.reserve(impl_->streams.size());}void Graphics::DetachStream(GpuStream& stream){impl_->DetachStream(stream);}void Graphics::ReserveStreams(){impl_->ReserveStreams();}void Graphics::CancelReservedStreams(){impl_->CancelReservedStreams();}bool Graphics::Prepare(const FrameInput& s){
    if(!impl_->developerMode&&!impl_->dlssDiagnosticEnabled)return impl_->Prepare(s);
    const int64_t start=QpcCounter();
    const bool ready=impl_->Prepare(s);
    impl_->cpuPrepareMs=float(QpcSeconds(QpcCounter()-start)*1000.0);
    return ready;
}void Graphics::Render(XRFrame& f,const GpuProducerCoordinator& coordinator){impl_->Render(f,coordinator);}void Graphics::WaitIdle(){impl_->WaitStartup(impl_->direct);impl_->WaitStartup(impl_->compute);impl_->WaitStartup(impl_->copy);}ID3D12Device* Graphics::Device()const{return impl_->device.Get();}ID3D12RootSignature* Graphics::BindlessRootSignature()const{return impl_->root.Get();}ID3D12DescriptorHeap* Graphics::BindlessResourceHeap()const{return impl_->descriptors.Get();}ID3D12DescriptorHeap* Graphics::BindlessSamplerHeap()const{return impl_->samplers.Get();}GpuResourceLease Graphics::CreateManagedBuffer(const GpuBufferDesc& d){return impl_->CreateManagedBuffer(d);}GpuResourceLease Graphics::CreateManagedTexture2D(const GpuTextureDesc& d){return impl_->CreateManagedTexture2D(d);}GpuResourceLease Graphics::CreateManagedTexture3D(const GpuTextureDesc& d){return impl_->CreateManagedTexture3D(d);}GpuResourceLease Graphics::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){return impl_->UploadStaticBuffer(bytes,stride,name);}GpuResourceLease Graphics::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){return impl_->UploadStaticTexture2D(d,subresources);}uint32_t Graphics::DescriptorIndex(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorIndex(h):Invalid;}uint32_t Graphics::DescriptorGeneration(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorGeneration(h):0;}ID3D12CommandQueue* Graphics::Queue()const{return impl_->direct.queue.Get();}
}
