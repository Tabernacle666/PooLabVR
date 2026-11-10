#include "GraphicsInternal.h"
namespace lab {
namespace graphics_internal {
thread_local uint64_t traceFrame{};
thread_local TextureDependencies textureDependencies;
}

uint32_t Graphics::SetFogSettings(uint64_t owner,const LabFogSettingsV1& settings){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetSettings(owner,settings);
}
uint32_t Graphics::SetFogWind(uint64_t owner,const LabFogWindV1& wind){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetWind(owner,wind):0;}
uint32_t Graphics::SetFogInput(uint64_t owner,const LabFogInputV1& input){return impl_->atmosphereDomain?impl_->atmosphereDomain->SetInput(owner,input):0;}
void Graphics::ClearAtmosphere(uint64_t owner){if(impl_->atmosphereDomain)impl_->atmosphereDomain->Clear(owner);if(impl_->opticalDomain)impl_->opticalDomain->Clear(owner);}
uint32_t Graphics::SetFogTransport(uint64_t owner,const LabFogTransportV1& s){
    if(!impl_->atmosphereDomain){auto domain=std::make_shared<AtmosphereDomain>();impl_->RegisterDomain(domain);impl_->atmosphereDomain=std::move(domain);}
    return impl_->atmosphereDomain->SetTransport(owner,s);
}
uint32_t Graphics::SetAntialiasing(uint64_t owner,const LabAntialiasingV1& s){
    if(!owner||s.structSize<sizeof(s)||s.mode>2)return 0;for(auto& c:impl_->aaClients)if(c.owner==owner){c.mode=s.mode;impl_->aaMode=s.mode;return 1;}impl_->aaClients.push_back({owner,s.mode});impl_->aaMode=s.mode;return 1;
}
uint32_t Graphics::SetMultisampling(uint64_t owner,const LabMultisamplingV1& s){
    if(!owner||s.structSize<sizeof(s)||(s.samples!=1&&s.samples!=4))return 0;
    if(s.samples==4)for(auto format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,4,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};
        if(FAILED(impl_->device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)))||!q.NumQualityLevels)return 0;
    }
    auto& clients=impl_->msaaClients;auto it=std::find_if(clients.begin(),clients.end(),[&](const auto& c){return c.owner==owner;});
    if(it==clients.end())clients.push_back({owner,s.samples});else it->samples=s.samples;
    impl_->requestedSamples=clients.back().samples;return 1;
}
uint32_t Graphics::SetSurfaceSettings(uint64_t owner,const LabSurfaceSettingsV1& s){
    if(!owner||s.structSize<sizeof(s)||s.material>65535||(!impl_->surfaceData.empty()&&s.material>=impl_->surfaceData.size()))return 0;
    const float v[]{s.color.x,s.color.y,s.color.z,s.roughness,s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};const float lo[]{0,0,0,.04f,0,.1f,0,0},hi[]{1,1,1,1,.3f,300,.15f,1};for(uint32_t i=0;i<8;i++)if(!std::isfinite(v[i])||v[i]<lo[i]||v[i]>hi[i])return 0;
    for(auto& e:impl_->surfaceEdits)if(e.owner==owner&&e.settings.material==s.material){if(memcmp(&e.settings,&s,sizeof(s))){e.settings=s;impl_->ApplySurfaceEdits();}return 1;}impl_->surfaceEdits.push_back({owner,s});impl_->ApplySurfaceEdits();return 1;
}
void Graphics::ClearRendering(uint64_t owner){
    std::erase_if(impl_->msaaClients,[&](const auto& c){return c.owner==owner;});impl_->requestedSamples=impl_->msaaClients.empty()?LAB_MSAA_SAMPLES:impl_->msaaClients.back().samples;
    std::erase_if(impl_->aaClients,[&](const auto& c){return c.owner==owner;});impl_->aaMode=impl_->aaClients.empty()?0:impl_->aaClients.back().mode;
    if(std::erase_if(impl_->surfaceEdits,[&](const auto& c){return c.owner==owner;}))impl_->ApplySurfaceEdits();
}
uint32_t Graphics::SetPopulationOpticalSettings(uint64_t owner,const LabPopulationOpticalSettingsV1& s){return impl_->opticalDomain?impl_->opticalDomain->SetSettings(owner,s):0;}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV1& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::PublishTerminal(uint64_t owner,const LabTerminalFrameV2& frame){
    if(!impl_->terminalDomain){auto domain=std::make_shared<TerminalDomain>();impl_->RegisterDomain(domain);impl_->terminalDomain=std::move(domain);}
    return impl_->terminalDomain->Publish(owner,frame);
}
uint32_t Graphics::HitTestTerminal(uint64_t owner,const LabTerminalHitQueryV2& q,LabTerminalHitV2& hit)const{return impl_->terminalDomain?impl_->terminalDomain->HitTest(owner,q,hit):0;}




void Graphics::ClearTerminal(uint64_t owner){if(impl_->terminalDomain)impl_->terminalDomain->Clear(owner);}
Graphics::Graphics():impl_(std::make_unique<Impl>()){impl_->facade=this;}Graphics::~Graphics()=default;void Graphics::Initialize(const XrGraphicsRequirementsD3D12KHR& r,bool gpuValidation){impl_->Initialize(r,gpuValidation);}void Graphics::Configure(const XR& x,const SceneData& assets,JobSystem& jobs,RenderContributions extra){impl_->Configure(x,assets,jobs,extra);}void Graphics::ReplaceScene(const SceneData& assets,RenderContributions extra){impl_->ReplaceScene(assets,extra);}void Graphics::UnloadScene(){impl_->UnloadScene();}bool Graphics::SceneLoaded()const{return impl_->sceneLoaded;}void Graphics::RegisterDomain(std::unique_ptr<GpuDomain> domain){impl_->RegisterDomain(std::shared_ptr<GpuDomain>(std::move(domain)));}void Graphics::RegisterSharedDomain(std::shared_ptr<GpuDomain> domain){impl_->RegisterDomain(std::move(domain));}void Graphics::InitializeDomain(GpuDomain& domain){if(!impl_->domainServicesReady)Fail("GPU domain services not initialized");domain.Initialize(*impl_);}bool Graphics::UnregisterDomain(GpuWorldDomain id){return impl_->UnregisterDomain(id);}void Graphics::AttachStream(GpuStream& stream){if(stream.Domain()==GpuDomainId::Invalid||!impl_->FindDomain(stream.Domain()))Fail("GPU stream domain is not registered");impl_->streams.push_back(&stream);impl_->publications.reserve(impl_->streams.size());}void Graphics::DetachStream(GpuStream& stream){impl_->DetachStream(stream);}void Graphics::ReserveStreams(){impl_->ReserveStreams();}void Graphics::CancelReservedStreams(){impl_->CancelReservedStreams();}bool Graphics::Prepare(const FrameInput& s){return impl_->Prepare(s);}GpuMemoryStats Graphics::MemoryStats(){impl_->CollectManaged();return impl_->MemoryStats();}GpuValidationState Graphics::ValidationState()const{return impl_->validation.State(impl_->device.Get());}void Graphics::Render(XRFrame& f,const std::function<void()>& waitForStreams){impl_->Render(f,waitForStreams);}void Graphics::WaitIdle(){impl_->WaitStartup(impl_->direct);impl_->WaitStartup(impl_->compute);impl_->WaitStartup(impl_->copy);}ID3D12Device* Graphics::Device()const{return impl_->device.Get();}ID3D12RootSignature* Graphics::BindlessRootSignature()const{return impl_->root.Get();}ID3D12DescriptorHeap* Graphics::BindlessResourceHeap()const{return impl_->descriptors.Get();}ID3D12DescriptorHeap* Graphics::BindlessSamplerHeap()const{return impl_->samplers.Get();}GpuResourceLease Graphics::CreateManagedBuffer(const GpuBufferDesc& d){return impl_->CreateManagedBuffer(d);}GpuResourceLease Graphics::CreateManagedTexture2D(const GpuTextureDesc& d){return impl_->CreateManagedTexture2D(d);}GpuResourceLease Graphics::CreateManagedTexture3D(const GpuTextureDesc& d){return impl_->CreateManagedTexture3D(d);}GpuResourceLease Graphics::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){return impl_->UploadStaticBuffer(bytes,stride,name);}GpuResourceLease Graphics::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){return impl_->UploadStaticTexture2D(d,subresources);}uint32_t Graphics::DescriptorIndex(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorIndex(h):Invalid;}uint32_t Graphics::DescriptorGeneration(GpuResourceHandle h)const{return h.Valid()?impl_->DescriptorGeneration(h):0;}ID3D12CommandQueue* Graphics::Queue()const{return impl_->direct.queue.Get();}
}
