#include "AtmosphereDomain.h"
#include "GpuFrameResources.h"
#include "MeshSolidPublication.h"
#include "Assets.h"
#include "Core.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
namespace lab {
namespace {
using Microsoft::WRL::ComPtr;
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE K,class T>struct alignas(void*) Part{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=K;T data{};};
struct RasterPipeline {
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
struct alignas(16) FogGpu {
    uint32_t resources[4];
    float centerTime[4],extent[4],distribution[4],optical[4],controls[4];
    float wake[64][4];
    float windPosition[4],windDirection[4],transport[4],structure[4];
};
static_assert(sizeof(FogGpu)==1184);
bool Vector(LabVec3V1 v){return std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
}
struct AtmosphereDomain::Impl {
    GpuDomainServices* api{};ComPtr<ID3D12Device2> device;
    ComPtr<ID3D12PipelineState> densityPipeline,integratePipeline,compositePipeline,shadowPipeline,proxyPipeline;
    uint32_t pipelineViews{};
    struct Wake{LabVec3V1 end{},start{};float time{},strength{};};
    struct Client {
        uint64_t owner{},lastFrame{};LabFogSettingsV1 settings{};LabFogInputV1 input{};LabFogWindV1 wind{sizeof(LabFogWindV1),0,{},{0,0,-1},0,.3f,4.f,.12f};
        LabFogTransportV1 transport{sizeof(LabFogTransportV1),1,0,1,1,0};
        std::array<Wake,32> wake{};uint32_t cursor{};double lastEmission[2]{},lastTime{},simulationTime{};bool densityValid{},updateDensity{};uint32_t densityIndex{};
        std::array<GpuResourceLease,2> density;GpuResourceLease fog,endpoints;std::array<GpuResourceLease,3> upload{};std::array<FogGpu*,3> mapped{};
        uint32_t width{},height{},views{},quality{UINT32_MAX};
        std::array<D3D12_BARRIER_LAYOUT,2> densityLayout{D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON};D3D12_BARRIER_LAYOUT fogLayout{D3D12_BARRIER_LAYOUT_COMMON},endpointLayout{D3D12_BARRIER_LAYOUT_COMMON};
    };
    struct Stage {
        Impl* self{};Client* client{};D3D12_GPU_VIRTUAL_ADDRESS frame{};D3D12_CPU_DESCRIPTOR_HANDLE color{};
        GpuResourceLease density;
        uint32_t constants[4]{},width{},height{},views{},fullWidth{},fullHeight{},grid{};bool recorded{};
    };
    struct ShadowStage{Impl* self{};uint32_t constants[4]{};D3D12_GPU_VIRTUAL_ADDRESS frame{};D3D12_CPU_DESCRIPTOR_HANDLE color{};uint32_t width{},height{},count{LAB_POPULATION_SHADOW_MAX_V1};bool proxy{};};
    struct ShadowView {ComPtr<ID3D12Resource> resource;GpuResourceHandle srv{};uint64_t lastFrame{};};
    std::array<Client,8> clients{};std::array<Stage,8> stages{};std::array<ShadowStage,32> shadowStages{},proxyStages{};
    std::vector<ShadowView> shadowViews;
    ~Impl(){for(auto& c:clients)Retire(c);if(api)for(auto& v:shadowViews)api->ReleaseDescriptor(v.srv);}
    void Retire(Client& c){for(uint32_t i=0;i<3;i++)if(c.upload[i]&&c.mapped[i])c.upload[i]->resource->Unmap(0,nullptr);c={};}
    Client* Find(uint64_t owner,bool create){for(auto& c:clients)if(c.owner==owner)return &c;if(create)for(auto& c:clients)if(!c.owner){c.owner=owner;return &c;}return nullptr;}
    void Initialize(GpuDomainServices& services){
        api=&services;Hr(api->Device()->QueryInterface(IID_PPV_ARGS(&device)),"Atmosphere device2");
        auto make=[&](const wchar_t* entry,ComPtr<ID3D12PipelineState>& out){auto bytes=ReadBytes(AssetPath((std::wstring(L"shaders/")+entry+L".cso").c_str()));D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=api->BindlessRootSignature();d.CS={bytes.data(),bytes.size()};Hr(api->Device()->CreateComputePipelineState(&d,IID_PPV_ARGS(&out)),"Atmosphere compute pipeline");};
        make(L"FogDensity",densityPipeline);make(L"FogIntegrate",integratePipeline);
    }
    void Pipelines(uint32_t views){
        if(pipelineViews==views)return;std::array<D3D12_VIEW_INSTANCE_LOCATION,4> locations{};if(!views||views>4)Fail("Atmosphere view count");for(uint32_t i=0;i<views;i++)locations[i]={0,i};
        auto make=[&](const wchar_t* vertex,const wchar_t* pixel,uint32_t blend,ComPtr<ID3D12PipelineState>& out){
            auto vs=ReadBytes(AssetPath((std::wstring(L"shaders/")+vertex+L".cso").c_str())),ps=ReadBytes(AssetPath((std::wstring(L"shaders/")+pixel+L".cso").c_str()));
            RasterPipeline p{};p.root.data=api->BindlessRootSignature();p.vs.data={vs.data(),vs.size()};p.ps.data={ps.data(),ps.size()};p.mask.data=UINT_MAX;
            p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;
            p.depth.data.DepthEnable=FALSE;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
            p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.samples.data={1,0};p.views.data={views,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};
            auto& b=p.blend.data.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=blend==1?D3D12_BLEND_ZERO:blend==2?D3D12_BLEND_SRC_ALPHA:D3D12_BLEND_ONE;b.DestBlend=blend==1?D3D12_BLEND_SRC_COLOR:blend==2?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_SRC_ALPHA;b.BlendOp=D3D12_BLEND_OP_ADD;
            b.SrcBlendAlpha=D3D12_BLEND_ZERO;b.DestBlendAlpha=D3D12_BLEND_ONE;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
            D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(p),&p};out.Reset();Hr(device->CreatePipelineState(&desc,IID_PPV_ARGS(&out)),"Atmosphere raster pipeline");
        };
        make(L"FogVS",L"FogComposite",0,compositePipeline);make(L"FlyShadowVS",L"FlyShadowPS",1,shadowPipeline);make(L"FlyProxyVS",L"FlyProxyPS",2,proxyPipeline);pipelineViews=views;
    }
    void Allocate(Client& c,const GpuFrameContext& ctx){
        uint32_t divisor=c.settings.quality==2?2:4,w=(ctx.width+divisor-1)/divisor,h=(ctx.height+divisor-1)/divisor;
        if(c.width!=w||c.height!=h||c.views!=ctx.viewCount||c.quality!=c.settings.quality){
            GpuTextureDesc t{};t.width=w;t.height=h;t.depthOrArraySize=ctx.viewCount;t.format=DXGI_FORMAT_R16G16B16A16_FLOAT;t.name=L"Atmosphere.Integral";c.fog=api->CreateManagedTexture2D(t);
            t.format=DXGI_FORMAT_R32_FLOAT;t.name=L"Atmosphere.Endpoints";c.endpoints=api->CreateManagedTexture2D(t);
            if(c.quality!=c.settings.quality||!c.density[0]){
                uint32_t resolution=c.settings.quality==0?32:c.settings.quality==1?64:96;t.width=t.depthOrArraySize=resolution;t.height=resolution/2;t.format=DXGI_FORMAT_R16_FLOAT;
                for(auto& grid:c.density){t.name=L"Atmosphere.PersistentDensity";grid=api->CreateManagedTexture3D(t);if(!grid)Fail("Atmosphere density allocation");}
                c.densityValid=false;c.densityIndex=0;c.densityLayout={D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON};
            }
            if(!c.fog||!c.endpoints)Fail("Atmosphere volume allocation");
            c.width=w;c.height=h;c.views=ctx.viewCount;c.quality=c.settings.quality;c.fogLayout=c.endpointLayout=D3D12_BARRIER_LAYOUT_COMMON;
        }
        if(!c.upload[0])for(uint32_t i=0;i<3;i++){
            GpuBufferDesc d{};d.bytes=sizeof(FogGpu);d.stride=sizeof(FogGpu);d.heap=D3D12_HEAP_TYPE_UPLOAD;d.uav=false;d.name=L"Atmosphere.Settings";c.upload[i]=api->CreateManagedBuffer(d);if(!c.upload[i])Fail("Atmosphere settings allocation");void* p{};D3D12_RANGE empty{0,0};Hr(c.upload[i]->resource->Map(0,&empty,&p),"Atmosphere settings map");c.mapped[i]=static_cast<FogGpu*>(p);
        }
    }
    void StageVolume(Client& c,const GpuFrameContext& ctx){
        if(ctx.uploadSlot>=3)Fail("Atmosphere upload slot");Allocate(c,ctx);
        const auto& s=c.settings;double displayNow=ctx.timeSeconds;
        double dt=c.lastTime>0?std::clamp(displayNow-c.lastTime,0.,.033):0.;
        if(c.wind.paused)dt=0;
        if(c.lastFrame!=ctx.frameId)c.simulationTime+=dt;
        double now=c.simulationTime;c.updateDensity=!c.densityValid||dt>0;
        uint32_t target=c.updateDensity?1-c.densityIndex:c.densityIndex;
        if(c.lastTime>0&&(displayNow<c.lastTime||displayNow-c.lastTime>.25)){c.wake={};c.lastEmission[0]=c.lastEmission[1]=0;}
        if(c.lastFrame!=ctx.frameId){
            if(dt>0)for(uint32_t h=0;h<2;h++)if(c.input.enabled&&c.input.hand[h].tracked&&now-c.lastEmission[h]>.045){
                auto hand=c.input.hand[h];float speed=std::sqrt(hand.velocity.x*hand.velocity.x+hand.velocity.y*hand.velocity.y+hand.velocity.z*hand.velocity.z);
                if(speed>.2f){c.wake[c.cursor++%32]={hand.position,hand.previous,float(now),std::min(3.f,speed*.55f)};c.lastEmission[h]=now;}
            }c.lastFrame=ctx.frameId;c.lastTime=displayNow;
        }
        auto& gpu=*c.mapped[ctx.uploadSlot];gpu={};gpu.resources[0]=api->DescriptorIndex(c.density[target]->srv);gpu.resources[1]=api->DescriptorIndex(c.density[target]->Uav());gpu.resources[2]=api->DescriptorIndex(c.endpoints->Uav());gpu.resources[3]=api->DescriptorIndex(c.density[c.densityIndex]->srv);
        const float center[]{s.center.x,s.center.y,s.center.z,float(now)},extent[]{s.halfExtent.x,s.halfExtent.y,s.halfExtent.z,0},distribution[]{s.density,s.height,s.falloff,s.scattering},optical[]{s.anisotropy,s.absorption,s.turbulence,s.evolution},controls[]{s.handResponse,s.lightIntensity,float(s.quality),float(s.lightIndex)};
        std::copy_n(center,4,gpu.centerTime);std::copy_n(extent,4,gpu.extent);std::copy_n(distribution,4,gpu.distribution);std::copy_n(optical,4,gpu.optical);std::copy_n(controls,4,gpu.controls);
        gpu.windPosition[0]=c.wind.position.x;gpu.windPosition[1]=c.wind.position.y;gpu.windPosition[2]=c.wind.position.z;gpu.windPosition[3]=c.wind.radius;
        gpu.windDirection[0]=c.wind.direction.x;gpu.windDirection[1]=c.wind.direction.y;gpu.windDirection[2]=c.wind.direction.z;gpu.windDirection[3]=c.wind.speed;
        gpu.structure[0]=c.transport.contrast;gpu.structure[1]=c.transport.frequency;gpu.structure[2]=float(c.transport.enabled);gpu.structure[3]=c.transport.sourceRate;
        gpu.transport[0]=float(dt);gpu.transport[1]=c.densityValid?0.f:1.f;gpu.transport[2]=c.wind.length;gpu.transport[3]=c.wind.dissipation;
        for(uint32_t i=0;i<32;i++){auto w=c.wake[i];float end[]{w.end.x,w.end.y,w.end.z,w.time},start[]{w.start.x,w.start.y,w.start.z,w.strength};std::copy_n(end,4,gpu.wake[i*2]);std::copy_n(start,4,gpu.wake[i*2+1]);}
    }
    void Bind(ID3D12GraphicsCommandList7* l,D3D12_GPU_VIRTUAL_ADDRESS frame,const uint32_t* constants,bool compute){
        ID3D12DescriptorHeap* heaps[]{api->BindlessResourceHeap(),api->BindlessSamplerHeap()};l->SetDescriptorHeaps(2,heaps);
        if(compute){l->SetComputeRootSignature(api->BindlessRootSignature());l->SetComputeRootConstantBufferView(0,frame);l->SetComputeRoot32BitConstants(1,4,constants,0);}
        else{l->SetGraphicsRootSignature(api->BindlessRootSignature());l->SetGraphicsRootConstantBufferView(0,frame);l->SetGraphicsRoot32BitConstants(1,4,constants,0);}
    }
    static void Density(void* context,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(context);s.self->Bind(l,s.frame,s.constants,true);l->SetPipelineState(s.self->densityPipeline.Get());auto d=s.density->desc;l->Dispatch((UINT(d.Width)+3)/4,(d.Height+3)/4,(d.DepthOrArraySize+3)/4);}
    static void Integrate(void* context,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(context);s.self->Bind(l,s.frame,s.constants,true);l->SetPipelineState(s.self->integratePipeline.Get());l->Dispatch((s.width+7)/8,(s.height+7)/8,s.views);}
    static void Viewport(ID3D12GraphicsCommandList7* l,uint32_t w,uint32_t h,D3D12_CPU_DESCRIPTOR_HANDLE color){D3D12_VIEWPORT v{0,0,float(w),float(h),0,1};D3D12_RECT rect{0,0,LONG(w),LONG(h)};l->RSSetViewports(1,&v);l->RSSetScissorRects(1,&rect);l->OMSetRenderTargets(1,&color,FALSE,nullptr);l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);}
    static void Composite(void* context,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<Stage*>(context);uint32_t constants[]{s.constants[0],s.self->api->DescriptorIndex(s.client->fog->srv),s.constants[2],s.self->api->DescriptorIndex(s.client->endpoints->srv)};s.self->Bind(l,s.frame,constants,false);l->SetPipelineState(s.self->compositePipeline.Get());Viewport(l,s.fullWidth,s.fullHeight,s.color);l->DrawInstanced(3,1,0,0);s.recorded=true;}
    static void Shadow(void* context,ID3D12GraphicsCommandList7* l){auto& s=*static_cast<ShadowStage*>(context);s.self->Bind(l,s.frame,s.constants,false);l->SetPipelineState((s.proxy?s.self->proxyPipeline:s.self->shadowPipeline).Get());Viewport(l,s.width,s.height,s.color);l->DrawInstanced(6,s.count,0,0);}
    uint32_t ShadowSrv(ID3D12Resource* resource,uint64_t frame){
        for(auto& v:shadowViews)if(v.resource.Get()==resource){v.lastFrame=frame;return api->DescriptorIndex(v.srv);}
        ShadowView v{};v.resource=resource;v.srv=api->CreateBufferSrv(resource,uint32_t(resource->GetDesc().Width/64),64);v.lastFrame=frame;uint32_t index=api->DescriptorIndex(v.srv);shadowViews.push_back(std::move(v));return index;
    }
    void Build(GpuPassBuilder& b,const GpuFrameContext& ctx){
        if(!api)return;Pipelines(ctx.viewCount);const auto& color=ctx.Require(GpuFrameResourceId::ResolvedColor),depth=ctx.Require(GpuFrameResourceId::SceneDepth);
        uint32_t depthIndex=api->DescriptorIndex(depth.srv);
        for(uint32_t ordinal=0;ordinal<shadowStages.size();ordinal++){
            auto* publication=ctx.FindPublication(GpuDomainId::MeshSolid,ordinal);if(!publication)break;auto* proxy=publication->Find(LAB_POPULATION_PROXY_BINDING_V1);
            if(proxy&&!(proxy->flags&1)){
                auto& p=proxyStages[ordinal];p={};p.self=this;p.proxy=true;p.count=uint32_t(proxy->bytes/64);p.frame=ctx.frameConstants;p.color=color.rtv;p.width=ctx.width;p.height=ctx.height;p.constants[0]=ShadowSrv(proxy->resource.Get(),ctx.frameId);p.constants[1]=uint32_t(proxy->offset/64);p.constants[2]=depthIndex;
                b.BeginPass("Population.Coverage",GpuPassQueue::Direct,{&p,&Shadow});
                b.Buffer(proxy->resource.Get(),GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                b.Texture(depth.resource,GpuPassResourceAccess::Read,depth.initialLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                b.Texture(color.resource,GpuPassResourceAccess::ReadWrite,color.initialLayout,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);b.EndPass();
            }
            // A collective field replaces the sampled floor decals for this publication;
            // the area-preserving body coverage above remains visible.
            if(std::find(ctx.lightOcclusionPublications.begin(),ctx.lightOcclusionPublications.end(),publication)!=ctx.lightOcclusionPublications.end())continue;
            auto* shadow=publication->Find(LAB_POPULATION_SHADOW_BINDING_V1);if(!shadow)continue;
            auto& s=shadowStages[ordinal];s={};s.self=this;s.frame=ctx.frameConstants;s.color=color.rtv;s.width=ctx.width;s.height=ctx.height;s.constants[0]=ShadowSrv(shadow->resource.Get(),ctx.frameId);s.constants[1]=uint32_t(shadow->offset/64);s.constants[2]=depthIndex;
            b.BeginPass("Population.Shadows",GpuPassQueue::Direct,{&s,&Shadow});
            b.Buffer(shadow->resource.Get(),GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(depth.resource,GpuPassResourceAccess::Read,depth.initialLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(color.resource,GpuPassResourceAccess::ReadWrite,color.initialLayout,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);b.EndPass();
        }
        for(auto it=shadowViews.begin();it!=shadowViews.end();)if(ctx.frameId>it->lastFrame+6){api->ReleaseDescriptor(it->srv);it=shadowViews.erase(it);}else ++it;
        for(uint32_t i=0;i<clients.size();i++){
            auto& c=clients[i];if(!c.owner||!c.settings.enabled)continue;StageVolume(c,ctx);auto& s=stages[i];s={};s.self=this;s.client=&c;s.frame=ctx.frameConstants;s.color=color.rtv;s.width=c.width;s.height=c.height;s.views=ctx.viewCount;s.fullWidth=ctx.width;s.fullHeight=ctx.height;
            uint32_t grid=c.updateDensity?1-c.densityIndex:c.densityIndex;s.grid=grid;s.density=c.density[grid];
            s.constants[0]=api->DescriptorIndex(c.upload[ctx.uploadSlot]->srv);s.constants[1]=api->DescriptorIndex(c.fog->Uav());s.constants[2]=depthIndex;
            if(c.updateDensity){b.BeginPass("Atmosphere.Advection",GpuPassQueue::Direct,{&s,&Density});
            b.Buffer(c.upload[ctx.uploadSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(c.densityValid)b.Texture(c.density[c.densityIndex]->resource,GpuPassResourceAccess::Read,c.densityLayout[c.densityIndex],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(c.density[grid]->resource,GpuPassResourceAccess::Write,c.densityLayout[grid],D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);b.EndPass();
            }
            b.BeginPass("Atmosphere.Integrate",GpuPassQueue::Direct,{&s,&Integrate});
            b.Buffer(c.upload[ctx.uploadSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(c.density[grid]->resource,GpuPassResourceAccess::Read,c.densityLayout[grid],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(depth.resource,GpuPassResourceAccess::Read,depth.initialLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(c.fog->resource,GpuPassResourceAccess::Write,c.fogLayout,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            b.Texture(c.endpoints->resource,GpuPassResourceAccess::Write,c.endpointLayout,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(const auto* fields=ctx.resources->Find(GpuFrameResourceId::PopulationOpticalFields))b.Buffer(fields->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(const auto& field:ctx.lightOcclusionTextures)b.Texture(field->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            const auto& light=ctx.Require(GpuFrameResourceId::Lighting);b.Buffer(light.resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(const auto* tlas=ctx.resources->Find(GpuFrameResourceId::TLAS))b.Buffer(tlas->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
            b.EndPass();
            b.BeginPass("Atmosphere.Composite",GpuPassQueue::Direct,{&s,&Composite});
            b.Texture(c.fog->resource,GpuPassResourceAccess::Read,c.fogLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(c.endpoints->resource,GpuPassResourceAccess::Read,c.endpointLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(depth.resource,GpuPassResourceAccess::Read,depth.initialLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            b.Texture(color.resource,GpuPassResourceAccess::ReadWrite,color.initialLayout,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);b.EndPass();

        }
    }
};
void AtmosphereDomain::EndFrame(){
    // BuildPasses may be abandoned on residency failure. Recording completes
    // before EndFrame; commit only a submitted density/integration/composite.
    for(auto& s:impl_->stages){if(s.recorded&&s.client&&s.client->owner){auto& c=*s.client;c.densityLayout[s.grid]=c.fogLayout=c.endpointLayout=D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;c.densityIndex=s.grid;c.densityValid=true;}s={};}
}
AtmosphereDomain::AtmosphereDomain():impl_(std::make_unique<Impl>()){}
AtmosphereDomain::~AtmosphereDomain()=default;
uint32_t AtmosphereDomain::SetSettings(uint64_t owner,const LabFogSettingsV1& s){
    if(!owner||s.structSize<sizeof(s)||s.enabled>1||s.quality>2||s.lightIndex>=MaxLights||!Vector(s.center)||!Vector(s.halfExtent)||s.halfExtent.x<=0||s.halfExtent.y<=0||s.halfExtent.z<=0||s.halfExtent.x>64||s.halfExtent.y>64||s.halfExtent.z>64)return 0;
    const float values[]{s.density,s.height,s.falloff,s.scattering,s.absorption,s.turbulence,s.evolution,s.handResponse,s.lightIntensity};for(float x:values)if(!std::isfinite(x)||x<0||x>16)return 0;
    if(s.density>1||s.turbulence>1||!std::isfinite(s.anisotropy)||std::abs(s.anisotropy)>.8f)return 0;
    auto* c=impl_->Find(owner,true);if(!c)return s.enabled?0:1;c->settings=s;if(!s.enabled){c->wake={};c->input={};}return 1;
}
uint32_t AtmosphereDomain::SetInput(uint64_t owner,const LabFogInputV1& input){
    if(!owner||input.structSize<sizeof(input)||input.enabled>1)return 0;for(auto& h:input.hand)if(!Vector(h.position)||!Vector(h.previous)||!Vector(h.velocity))return 0;
    auto* c=impl_->Find(owner,false);if(!c)return 1;
    auto sanitized=input;for(auto& h:sanitized.hand){float speed=h.velocity.x*h.velocity.x+h.velocity.y*h.velocity.y+h.velocity.z*h.velocity.z;float dx=h.position.x-h.previous.x,dy=h.position.y-h.previous.y,dz=h.position.z-h.previous.z;if(speed>400||dx*dx+dy*dy+dz*dz>.25f)h.tracked=0;}
    c->input=sanitized;return 1;
}
uint32_t AtmosphereDomain::SetWind(uint64_t owner,const LabFogWindV1& w){
    if(!owner||w.structSize<sizeof(w)||w.paused>1||!Vector(w.position)||!Vector(w.direction)||!std::isfinite(w.speed)||w.speed<0||w.speed>20||!std::isfinite(w.radius)||w.radius<.05f||w.radius>4||!std::isfinite(w.length)||w.length<.1f||w.length>16||!std::isfinite(w.dissipation)||w.dissipation<0||w.dissipation>8)return 0;
    float length=w.direction.x*w.direction.x+w.direction.y*w.direction.y+w.direction.z*w.direction.z;if(length<.99f||length>1.01f)return 0;
    auto* c=impl_->Find(owner,true);if(!c)return 0;c->wind=w;return 1;
}
uint32_t AtmosphereDomain::SetTransport(uint64_t owner,const LabFogTransportV1& s){
    if(!owner||s.structSize<sizeof(s)||s.enabled>1||!std::isfinite(s.contrast)||s.contrast<0||s.contrast>1||!std::isfinite(s.frequency)||s.frequency<.1f||s.frequency>2||!std::isfinite(s.sourceRate)||s.sourceRate<0||s.sourceRate>8)return 0;
    auto* c=impl_->Find(owner,true);if(!c)return 0;c->transport=s;return 1;
}
void AtmosphereDomain::Clear(uint64_t owner){if(auto* c=impl_->Find(owner,false))impl_->Retire(*c);}
void AtmosphereDomain::Initialize(GpuDomainServices& s){impl_->Initialize(s);}
void AtmosphereDomain::BuildPasses(GpuPassBuilder& b,const GpuFrameContext& ctx){impl_->Build(b,ctx);}
}
