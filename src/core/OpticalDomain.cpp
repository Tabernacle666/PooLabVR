#include "OpticalDomain.h"
#include "Assets.h"
#include "Core.h"
#include <algorithm>
#include <array>
#include <cmath>
namespace lab {
namespace {
using Microsoft::WRL::ComPtr;
struct alignas(16) FieldGpu {
    uint32_t map[4]{},selection[4]{};float bodyWing[4]{},controls[4]{},history[4]{};uint32_t work[4]{},input[4]{};
};
static_assert(sizeof(FieldGpu)==112);
static_assert(sizeof(LabPopulationOpticalInstanceV1)==64&&offsetof(LabPopulationOpticalInstanceV1,orientation)==16&&offsetof(LabPopulationOpticalInstanceV1,scale)==32);
struct InputGpu {uint32_t descriptor{},first{},count{},reserved{};};
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE K,class T>struct alignas(void*) Part{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=K;T data{};};
struct DebugPipeline {
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> formats;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    Part<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
};
bool Finite(float v){return std::isfinite(v);}
}
struct OpticalDomain::Impl {
    static constexpr uint32_t MaxFields=8,MaxInputs=256;
    GpuDomainServices* api{};ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12PipelineState> clear,splat,prefix,origin,debug;uint32_t debugViews{};
    struct Client {
        uint64_t owner{};LabPopulationOpticalSettingsV1 settings{};uint32_t quality{UINT32_MAX},index{};bool valid{};
        double time{};GpuResourceLease bins,state;std::array<GpuResourceLease,2> field;
        std::array<D3D12_BARRIER_LAYOUT,2> layout{D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON};
    };
    struct Stage {
        Impl* self{};Client* client{};GpuResourceLease output;uint32_t constants[4]{},width{},height{},layers{},target{};D3D12_GPU_VIRTUAL_ADDRESS frame{};bool submitted{};char clearName[64]{},prefixName[64]{},originName[64]{},debugName[64]{};
    };
    struct SplatStage {Stage* field{};uint32_t constants[4]{},count{};char name[64]{};};
    struct DebugStage {Stage* field{};D3D12_CPU_DESCRIPTOR_HANDLE color{};uint32_t width{},height{};};
    struct View {ComPtr<ID3D12Resource> resource;GpuResourceHandle srv{};uint64_t frame{};};
    std::array<Client,MaxFields> clients{};std::array<Stage,MaxFields> stages{};std::array<DebugStage,MaxFields> debugStages{};
    std::array<SplatStage,MaxInputs> splats{};std::vector<View> views;
    std::array<GpuResourceLease,3> tables,inputs;std::array<FieldGpu*,3> tableMapped{};std::array<InputGpu*,3> inputMapped{};
    std::vector<GpuResourceLease> activeTextures;std::vector<const GpuPublication*> activePublications;uint32_t count{},tableIndex{},currentSlot{};
    ~Impl(){if(api)for(auto& v:views)api->ReleaseDescriptor(v.srv);for(uint32_t i=0;i<3;i++){if(tables[i]&&tableMapped[i])tables[i]->resource->Unmap(0,nullptr);if(inputs[i]&&inputMapped[i])inputs[i]->resource->Unmap(0,nullptr);}}
    Client* Find(uint64_t owner,bool create){for(auto& c:clients)if(c.owner==owner)return &c;if(create)for(auto& c:clients)if(!c.owner){c.owner=owner;return &c;}return nullptr;}
    void Initialize(GpuDomainServices& s){
        api=&s;Hr(api->Device()->QueryInterface(IID_PPV_ARGS(&device)),"Optical device2");
        auto make=[&](const wchar_t* name,ComPtr<ID3D12PipelineState>& p){auto bytes=ReadBytes(AssetPath((std::wstring(L"shaders/")+name+L".cso").c_str()));D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=api->BindlessRootSignature();d.CS={bytes.data(),bytes.size()};Hr(api->Device()->CreateComputePipelineState(&d,IID_PPV_ARGS(&p)),"Population optical pipeline");};
        make(L"OpticalClear",clear);make(L"OpticalSplat",splat);make(L"OpticalPrefix",prefix);make(L"OpticalOrigin",origin);
        activeTextures.reserve(MaxFields);activePublications.reserve(MaxInputs);views.reserve(32);
    }
    void AllocateTables(){
        if(tables[0])return;
        for(uint32_t i=0;i<3;i++){
            GpuBufferDesc d{};d.heap=D3D12_HEAP_TYPE_UPLOAD;d.uav=false;d.bytes=sizeof(FieldGpu)*MaxFields;d.stride=sizeof(FieldGpu);d.name=L"Population.OpticalFields";tables[i]=api->CreateManagedBuffer(d);
            d.bytes=sizeof(InputGpu)*MaxInputs;d.stride=sizeof(InputGpu);d.name=L"Population.OpticalInputs";inputs[i]=api->CreateManagedBuffer(d);
            if(!tables[i]||!inputs[i])Fail("Population optical tables allocation");D3D12_RANGE empty{0,0};void* p{};
            Hr(tables[i]->resource->Map(0,&empty,&p),"Population optical table map");tableMapped[i]=static_cast<FieldGpu*>(p);
            Hr(inputs[i]->resource->Map(0,&empty,&p),"Population optical inputs map");inputMapped[i]=static_cast<InputGpu*>(p);
        }
    }
    void Allocate(Client& c,uint32_t w,uint32_t h,uint32_t layers){
        if(c.quality==c.settings.quality&&c.bins)return;
        GpuBufferDesc b{};b.bytes=uint64_t(w)*h*layers*3*8;b.stride=8;b.name=L"Population.OpticalAccumulator64";c.bins=api->CreateManagedBuffer(b);
        b.bytes=16;b.stride=16;b.name=L"Population.OpticalOrigin";c.state=api->CreateManagedBuffer(b);
        GpuTextureDesc t{};t.width=w;t.height=h;t.depthOrArraySize=layers;t.format=DXGI_FORMAT_R32G32B32A32_FLOAT;t.name=L"Population.OpticalCumulative";
        for(auto& f:c.field)f=api->CreateManagedTexture2D(t);
        if(!c.bins||!c.state||!c.field[0]||!c.field[1])Fail("Population optical field allocation");
        c.quality=c.settings.quality;c.valid=false;c.index=0;c.layout={D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON};
    }
    uint32_t Srv(ID3D12Resource* resource,uint64_t frame){
        for(auto& v:views)if(v.resource.Get()==resource){v.frame=frame;return api->DescriptorIndex(v.srv);}
        auto bytes=resource->GetDesc().Width;if(bytes%64||bytes/64>UINT32_MAX)Fail("Population optical input stride");
        View v{};v.resource=resource;v.srv=api->CreateBufferSrv(resource,uint32_t(bytes/64),64);v.frame=frame;uint32_t index=api->DescriptorIndex(v.srv);views.push_back(std::move(v));return index;
    }
    void Bind(ID3D12GraphicsCommandList7* l,Stage& s,const uint32_t* constants){
        ID3D12DescriptorHeap* heaps[]{api->BindlessResourceHeap(),api->BindlessSamplerHeap()};l->SetDescriptorHeaps(2,heaps);l->SetComputeRootSignature(api->BindlessRootSignature());l->SetComputeRootConstantBufferView(0,s.frame);l->SetComputeRoot32BitConstants(1,4,constants,0);
    }
    static void ClearBins(void* p,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(p);s.self->Bind(l,s,s.constants);l->SetPipelineState(s.self->clear.Get());l->Dispatch((s.width*s.height+255)/256,1,1);}
    static void Splat(void* p,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<SplatStage*>(p);s.field->self->Bind(l,*s.field,s.constants);l->SetPipelineState(s.field->self->splat.Get());l->Dispatch((s.count+255)/256,1,1);}
    static void Prefix(void* p,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(p);s.self->Bind(l,s,s.constants);l->SetPipelineState(s.self->prefix.Get());l->Dispatch((s.width+7)/8,(s.height+7)/8,1);}
    static void Origin(void* p,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(p);s.self->Bind(l,s,s.constants);l->SetPipelineState(s.self->origin.Get());l->Dispatch(1,1,1);s.submitted=true;}
    void CommonReads(GpuPassBuilder& b,const GpuFrameContext& ctx){
        b.Buffer(tables[ctx.uploadSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        auto& lights=ctx.Require(GpuFrameResourceId::Lighting);b.Buffer(lights.resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    }
    void BuildFields(GpuPassBuilder& b,GpuFrameContext& ctx,GpuFrameResources& resources){
        count=0;tableIndex=0;activeTextures.clear();activePublications.clear();stages={};if(!api||!ctx.publications)return;
        if(ctx.uploadSlot>=3)Fail("Population optical upload slot");currentSlot=ctx.uploadSlot;AllocateTables();
        std::fill_n(tableMapped[currentSlot],MaxFields,FieldGpu{});uint32_t inputCount=0;
        for(uint32_t i=0;i<MaxFields;i++){
            auto& c=clients[i];if(!c.owner||!c.settings.enabled){c.valid=false;continue;}
            uint32_t first=inputCount;uint64_t particleCount=0;
            for(auto* publication:ctx.publications->Entries()){
                if(publication->sourceOwner!=c.owner||publication->domain!=c.settings.domainId)continue;
                auto* input=publication->Find(c.settings.binding);if(!input||input->flags&1)continue;
                if(input->type!=GpuPublishedResourceType::Buffer||input->offset%64||input->bytes%64||input->bytes/64>1048576||input->offset/64>UINT32_MAX)Fail("Population optical publication range invalid");
                if(!input->bytes)continue;particleCount+=input->bytes/64;if(particleCount>1048576)Fail("Population optical active count exceeds contract");if(inputCount==MaxInputs)Fail("Population optical publication capacity exceeded");
                activePublications.push_back(publication);
                inputMapped[currentSlot][inputCount++]={Srv(input->resource.Get(),ctx.frameId),uint32_t(input->offset/64),uint32_t(input->bytes/64),0};
            }
            if(first==inputCount){c.valid=false;continue;}
            const uint32_t widths[]{96,192,256},layers[]{48,64,96};uint32_t w=widths[c.settings.quality],h=w,n=layers[c.settings.quality];Allocate(c,w,h,n);
            uint32_t target=1-c.index,row=count++;auto& s=stages[row];s={};s.self=this;s.client=&c;s.output=c.field[target];s.target=target;s.width=w;s.height=h;s.layers=n;s.frame=ctx.frameConstants;
            snprintf(s.clearName,sizeof(s.clearName),"Population.OpticalClear.%u",i);snprintf(s.prefixName,sizeof(s.prefixName),"Population.OpticalPrefix.%u",i);snprintf(s.originName,sizeof(s.originName),"Population.OpticalOrigin.%u",i);snprintf(s.debugName,sizeof(s.debugName),"Population.OpticalView.%u",i);
            s.constants[0]=api->DescriptorIndex(tables[currentSlot]->srv);s.constants[1]=row;
            auto& g=tableMapped[currentSlot][row];g={};g.map[0]=api->DescriptorIndex(s.output->srv);g.map[1]=w;g.map[2]=h;g.map[3]=n;
            g.selection[0]=1;g.selection[1]=c.settings.lightIndex;g.selection[2]=c.settings.sourceId;g.selection[3]=c.settings.visualization;
            g.bodyWing[0]=c.settings.bodyHalfExtent.x;g.bodyWing[1]=c.settings.bodyHalfExtent.y;g.bodyWing[2]=c.settings.bodyHalfExtent.z;g.bodyWing[3]=c.settings.wingArea;
            g.controls[0]=c.settings.wingOpacity;g.controls[1]=c.settings.sourceRadius;g.controls[2]=c.settings.coverage;g.controls[3]=c.settings.stabilizationSeconds;
            double dt=c.time>0?std::clamp(ctx.timeSeconds-c.time,0.,.1):0;
            g.history[0]=dt>0&&c.settings.stabilizationSeconds>0?std::min(.65f,std::exp(-float(dt)/c.settings.stabilizationSeconds)):0;
            g.history[1]=.01f;g.history[2]=16;g.history[3]=c.valid?1.f:0.f;
            g.work[0]=api->DescriptorIndex(c.bins->Uav());g.work[1]=api->DescriptorIndex(c.field[c.index]->srv);g.work[2]=api->DescriptorIndex(s.output->Uav());g.work[3]=api->DescriptorIndex(c.state->srv);
            g.input[0]=api->DescriptorIndex(c.state->Uav());g.input[1]=api->DescriptorIndex(c.bins->srv);c.time=ctx.timeSeconds;
            activeTextures.push_back(s.output);
            b.BeginPass(s.clearName,GpuPassQueue::Direct,{&s,&ClearBins});
            b.Buffer(tables[currentSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Buffer(c.bins->resource,GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);b.EndPass();
            uint32_t k=first;
            for(auto* publication:ctx.publications->Entries()){
                if(publication->sourceOwner!=c.owner||publication->domain!=c.settings.domainId)continue;auto* input=publication->Find(c.settings.binding);if(!input||input->flags&1||!input->bytes)continue;
                auto& sp=splats[k];sp={};sp.field=&s;std::copy_n(s.constants,4,sp.constants);sp.constants[2]=api->DescriptorIndex(inputs[currentSlot]->srv);sp.constants[3]=k;sp.count=inputMapped[currentSlot][k].count;++k;
                snprintf(sp.name,sizeof(sp.name),"Population.OpticalSplat.%u.%u",i,k-1);
                b.BeginPass(sp.name,GpuPassQueue::Direct,{&sp,&Splat});CommonReads(b,ctx);
                b.Buffer(inputs[currentSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                b.Buffer(input->resource.Get(),GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                b.Buffer(c.bins->resource,GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);b.EndPass();
            }
            b.BeginPass(s.prefixName,GpuPassQueue::Direct,{&s,&Prefix});CommonReads(b,ctx);
            b.Buffer(c.bins->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Buffer(c.state->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(c.valid)b.Texture(c.field[c.index]->resource,GpuPassResourceAccess::Read,c.layout[c.index],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(s.output->resource,GpuPassResourceAccess::Write,c.layout[target],D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);b.EndPass();
            b.BeginPass(s.originName,GpuPassQueue::Direct,{&s,&Origin});CommonReads(b,ctx);
            // Reading the completed field sequences origin publication after its
            // prefix readers, so the previous light pose cannot be overwritten early.
            b.Texture(s.output->resource,GpuPassResourceAccess::Read,c.layout[target],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Buffer(c.state->resource,GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);b.EndPass();
        }
        for(auto it=views.begin();it!=views.end();)if(ctx.frameId>it->frame+6){api->ReleaseDescriptor(it->srv);it=views.erase(it);}else ++it;
        if(count){tableIndex=api->DescriptorIndex(tables[currentSlot]->srv);resources.Set({GpuFrameResourceId::PopulationOpticalFields,tables[currentSlot]->resource,GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,tables[currentSlot]->srv});}
        ctx.lightOcclusionTextures=activeTextures;ctx.lightOcclusionPublications=activePublications;
    }
    void DebugPso(uint32_t viewsCount){
        if(debugViews==viewsCount)return;std::array<D3D12_VIEW_INSTANCE_LOCATION,4> locations{};if(!viewsCount||viewsCount>4)Fail("Population optical view count");for(uint32_t i=0;i<viewsCount;i++)locations[i]={0,i};
        auto vs=ReadBytes(AssetPath(L"shaders/OpticalDebugVS.cso")),ps=ReadBytes(AssetPath(L"shaders/OpticalDebugPS.cso"));DebugPipeline p{};
        p.root.data=api->BindlessRootSignature();p.vs.data={vs.data(),vs.size()};p.ps.data={ps.data(),ps.size()};p.mask.data=UINT_MAX;
        p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;
        p.depth.data.DepthEnable=FALSE;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.samples.data={1,0};p.views.data={viewsCount,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};
        auto& blend=p.blend.data.RenderTarget[0];blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.BlendOp=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ZERO;blend.DestBlendAlpha=D3D12_BLEND_ONE;blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof(p),&p};debug.Reset();Hr(device->CreatePipelineState(&d,IID_PPV_ARGS(&debug)),"Population optical visualization pipeline");debugViews=viewsCount;
    }
    static void Debug(void* p,ID3D12GraphicsCommandList7* l){
        auto& d=*static_cast<DebugStage*>(p);auto& s=*d.field;auto& self=*s.self;
        ID3D12DescriptorHeap* heaps[]{self.api->BindlessResourceHeap(),self.api->BindlessSamplerHeap()};l->SetDescriptorHeaps(2,heaps);l->SetGraphicsRootSignature(self.api->BindlessRootSignature());l->SetGraphicsRootConstantBufferView(0,s.frame);l->SetGraphicsRoot32BitConstants(1,4,s.constants,0);l->SetPipelineState(self.debug.Get());
        D3D12_VIEWPORT v{0,0,float(d.width),float(d.height),0,1};D3D12_RECT rect{0,0,LONG(d.width),LONG(d.height)};l->RSSetViewports(1,&v);l->RSSetScissorRects(1,&rect);l->OMSetRenderTargets(1,&d.color,FALSE,nullptr);l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);l->DrawInstanced(3,1,0,0);
    }
    void BuildDebug(GpuPassBuilder& b,const GpuFrameContext& ctx){
        for(uint32_t i=0;i<count;i++){
            auto& s=stages[i];if(!s.client->settings.visualization)continue;DebugPso(ctx.viewCount);auto& d=debugStages[i];const auto& color=ctx.Require(GpuFrameResourceId::ResolvedColor);d={&s,color.rtv,ctx.width,ctx.height};
            b.BeginPass(s.debugName,GpuPassQueue::Direct,{&d,&Debug});
            b.Buffer(tables[currentSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Buffer(ctx.Require(GpuFrameResourceId::Lighting).resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(s.output->resource,GpuPassResourceAccess::Read,s.client->layout[s.target],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(color.resource,GpuPassResourceAccess::ReadWrite,color.initialLayout,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);b.EndPass();
        }
    }
};
OpticalDomain::OpticalDomain():impl_(std::make_unique<Impl>()){}
OpticalDomain::~OpticalDomain()=default;
void OpticalDomain::Initialize(GpuDomainServices& api){impl_->Initialize(api);}
uint32_t OpticalDomain::SetSettings(uint64_t owner,const LabPopulationOpticalSettingsV1& s){
    if(!owner||s.structSize<sizeof(s)||s.enabled>1||s.quality>2||s.lightIndex>=MaxLights||!s.domainId||!s.binding||s.visualization>2)return 0;
    const float v[]{s.bodyHalfExtent.x,s.bodyHalfExtent.y,s.bodyHalfExtent.z};for(float a:v)if(!Finite(a)||a<.0001f||a>.05f)return 0;
    if(!Finite(s.wingArea)||s.wingArea<0||s.wingArea>.01f||!Finite(s.wingOpacity)||s.wingOpacity<0||s.wingOpacity>1||!Finite(s.sourceRadius)||s.sourceRadius<0||s.sourceRadius>.1f||!Finite(s.coverage)||s.coverage<.25f||s.coverage>2||!Finite(s.stabilizationSeconds)||s.stabilizationSeconds<0||s.stabilizationSeconds>.08f)return 0;
    auto* c=impl_->Find(owner,true);if(!c)return 0;
    const auto& old=c->settings;
    if(old.enabled!=s.enabled||old.lightIndex!=s.lightIndex||old.sourceId!=s.sourceId||old.domainId!=s.domainId||old.binding!=s.binding||old.coverage!=s.coverage||old.wingArea!=s.wingArea||old.wingOpacity!=s.wingOpacity||old.bodyHalfExtent.x!=s.bodyHalfExtent.x||old.bodyHalfExtent.y!=s.bodyHalfExtent.y||old.bodyHalfExtent.z!=s.bodyHalfExtent.z)c->valid=false;
    c->settings=s;return 1;
}
void OpticalDomain::Clear(uint64_t owner){if(auto* c=impl_->Find(owner,false))*c={};}
void OpticalDomain::BuildFields(GpuPassBuilder& b,GpuFrameContext& c,GpuFrameResources& resources){impl_->BuildFields(b,c,resources);}
void OpticalDomain::BuildPasses(GpuPassBuilder& b,const GpuFrameContext& c){impl_->BuildDebug(b,c);}
void OpticalDomain::EndFrame(){for(auto& s:impl_->stages){if(s.submitted&&s.client){s.client->index=s.target;s.client->layout[s.target]=D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;s.client->valid=true;}s={};}}
uint32_t OpticalDomain::FieldCount()const noexcept{return impl_->count;}
uint32_t OpticalDomain::FieldDescriptor()const noexcept{return impl_->tableIndex;}
}
