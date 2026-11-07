#include "TerminalDomain.h"
#include <cmath>
#include "GpuFrameResources.h"
#include "Assets.h"
#include "Core.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <vector>
#include <wrl/client.h>

namespace lab {
namespace {
using Microsoft::WRL::ComPtr;
inline constexpr GpuWorldDomain TerminalDomainId=LAB_TERMINAL_DOMAIN_V1; // TERM, Core-owned
inline constexpr uint32_t BufferedFrames=3;

struct alignas(16) MenuLineGpu {
    float geometry[4]{};
    float surface[4]{};
    uint32_t chars[24]{};
    uint32_t meta[4]{};
    float right[4]{1,0,0,.0039f},up[4]{0,1,0,.04f},normal[4]{0,0,1,0};
};
static_assert(sizeof(MenuLineGpu)==192);

template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class T>
struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=Type;T data{};};
struct MenuPipeline {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND,D3D12_BLEND_DESC> blend;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK,UINT> mask;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER,D3D12_RASTERIZER_DESC> raster;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL,D3D12_DEPTH_STENCIL_DESC> depth;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,D3D12_PRIMITIVE_TOPOLOGY_TYPE> topology;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS,D3D12_RT_FORMAT_ARRAY> formats;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT,DXGI_FORMAT> depthFormat;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC,DXGI_SAMPLE_DESC> samples;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING,D3D12_VIEW_INSTANCING_DESC> views;
};

LabVec3V1 Rotate(LabQuatV1 q,LabVec3V1 v){
    LabVec3V1 t{2*(q.y*v.z-q.z*v.y),2*(q.z*v.x-q.x*v.z),2*(q.x*v.y-q.y*v.x)};
    return {v.x+q.w*t.x+q.y*t.z-q.z*t.y,v.y+q.w*t.y+q.z*t.x-q.x*t.z,v.z+q.w*t.z+q.x*t.y-q.y*t.x};
}
float Dot(LabVec3V1 a,LabVec3V1 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
bool Finite(float v){return std::isfinite(v)&&std::abs(v)<=100000;}
bool Ascii(const char* s,size_t n){for(size_t i=0;i<n;i++)if(uint8_t(s[i])>127)return false;return true;}
std::array<uint8_t,7> Glyph(char c){
    switch(c){
    case 'A':return {14,17,17,31,17,17,17};case 'B':return {30,17,17,30,17,17,30};case 'C':return {15,16,16,16,16,16,15};case 'D':return {30,17,17,17,17,17,30};
    case 'E':return {31,16,16,30,16,16,31};case 'F':return {31,16,16,30,16,16,16};case 'G':return {15,16,16,23,17,17,15};case 'H':return {17,17,17,31,17,17,17};
    case 'I':return {31,4,4,4,4,4,31};case 'J':return {7,2,2,2,18,18,12};case 'K':return {17,18,20,24,20,18,17};case 'L':return {16,16,16,16,16,16,31};
    case 'M':return {17,27,21,21,17,17,17};case 'N':return {17,25,21,19,17,17,17};case 'O':return {14,17,17,17,17,17,14};case 'P':return {30,17,17,30,16,16,16};
    case 'Q':return {14,17,17,17,21,18,13};case 'R':return {30,17,17,30,20,18,17};case 'S':return {15,16,16,14,1,1,30};case 'T':return {31,4,4,4,4,4,4};
    case 'U':return {17,17,17,17,17,17,14};case 'V':return {17,17,17,17,17,10,4};case 'W':return {17,17,17,21,21,21,10};case 'X':return {17,17,10,4,10,17,17};
    case 'Y':return {17,17,10,4,4,4,4};case 'Z':return {31,1,2,4,8,16,31};case '0':return {14,17,19,21,25,17,14};case '1':return {4,12,4,4,4,4,14};
    case '2':return {14,17,1,2,4,8,31};case '3':return {30,1,1,14,1,1,30};case '4':return {2,6,10,18,31,2,2};case '5':return {31,16,16,30,1,1,30};
    case '6':return {14,16,16,30,17,17,14};case '7':return {31,1,2,4,8,8,8};case '8':return {14,17,17,14,17,17,14};case '9':return {14,17,17,15,1,1,14};
    case '\\':return {16,8,8,4,2,2,1};case '_':return {0,0,0,0,0,0,31};case '=':return {0,0,31,0,31,0,0};case '.':return {0,0,0,0,0,12,12};case '+':return {0,4,4,31,4,4,0};case '/':return {1,2,2,4,8,8,16};case '<':return {2,4,8,16,8,4,2};case '>':return {8,4,2,1,2,4,8};case ':':return {0,12,12,0,12,12,0};case '-':return {0,0,0,31,0,0,0};default:return {};
    }
}
}

struct TerminalDomain::Impl {
    GpuDomainServices* services{};
    ComPtr<ID3D12Device2> device2;
    struct Client {uint64_t owner{};LabTerminalFrameV1 frame{};LabTerminalFrameV2 panel{};std::vector<LabTerminalNodeV2> nodes;std::vector<LabTerminalElementV2> elements;};
    std::array<Client,LAB_TERMINAL_MAX_CLIENTS_V1> clients;
    GpuResourceLease atlas;
    std::array<GpuResourceLease,BufferedFrames> lines{};
    std::array<MenuLineGpu*,BufferedFrames> mapped{};
    ComPtr<ID3D12PipelineState> pipeline;
    uint32_t pipelineViews{},pipelineSamples{};
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants{};
    D3D12_CPU_DESCRIPTOR_HANDLE colorRtv{},depthDsv{};
    uint32_t width{},height{},lineCount{},lineDescriptor{},atlasDescriptor{},depthDescriptor{};
    bool useDepth{},suppressed{};

    ~Impl(){
        for(uint32_t i=0;i<BufferedFrames;i++)if(lines[i]&&mapped[i])lines[i]->resource->Unmap(0,nullptr);
    }
    void Initialize(GpuDomainServices& api){
        services=&api;Hr(api.Device()->QueryInterface(IID_PPV_ARGS(&device2)),"HomeMenu D3D12 device2");
        GpuBufferDesc atlasDesc;atlasDesc.bytes=128ull*7ull*sizeof(uint32_t);atlasDesc.stride=sizeof(uint32_t);atlasDesc.heap=D3D12_HEAP_TYPE_UPLOAD;atlasDesc.uav=false;atlasDesc.name=L"HomeMenu.GlyphAtlas5x7";
        atlas=api.CreateManagedBuffer(atlasDesc);if(!atlas)Fail("HomeMenu glyph atlas allocation");
        std::array<uint32_t,128*7> bits{};for(uint32_t c=0;c<128;c++){char uc=char(c);if(uc>='a'&&uc<='z')uc=char(uc-'a'+'A');auto rows=Glyph(uc);for(uint32_t row=0;row<7;row++)bits[c*7+row]=rows[row];}
        void* target{};D3D12_RANGE empty{0,0};Hr(atlas->resource->Map(0,&empty,&target),"HomeMenu atlas map");std::memcpy(target,bits.data(),sizeof(bits));atlas->resource->Unmap(0,nullptr);atlasDescriptor=api.DescriptorIndex(atlas->srv);
        for(uint32_t i=0;i<BufferedFrames;i++){
            GpuBufferDesc d;d.bytes=((LAB_TERMINAL_MAX_LINES_V1+LAB_TERMINAL_MAX_ELEMENTS_V2)*LAB_TERMINAL_MAX_CLIENTS_V1)*sizeof(MenuLineGpu);d.stride=sizeof(MenuLineGpu);d.heap=D3D12_HEAP_TYPE_UPLOAD;d.uav=false;d.name=L"HomeMenu.Lines";
            lines[i]=api.CreateManagedBuffer(d);if(!lines[i])Fail("HomeMenu line buffer allocation");void* p{};Hr(lines[i]->resource->Map(0,&empty,&p),"HomeMenu line map");mapped[i]=static_cast<MenuLineGpu*>(p);
        }
    }
    void EnsurePipeline(uint32_t views,uint32_t samples,bool depth){
        if(pipeline&&pipelineViews==views&&pipelineSamples==samples&&useDepth==depth)return;
        auto vs=ReadBytes(AssetPath(L"shaders/HomeMenuVS.cso")),ps=ReadBytes(AssetPath(L"shaders/HomeMenuPS.cso"));
        std::array<D3D12_VIEW_INSTANCE_LOCATION,4> locations{};if(views>locations.size())Fail("HomeMenu view count");for(uint32_t i=0;i<views;i++)locations[i]={0,i};
        D3D12_VIEW_INSTANCING_DESC viewDesc{views,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};
        MenuPipeline p{};p.root.data=services->BindlessRootSignature();p.vs.data={vs.data(),vs.size()};p.ps.data={ps.data(),ps.size()};p.mask.data=UINT_MAX;
        p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;
        p.depth.data.DepthEnable=depth;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.depthFormat.data=depth?DXGI_FORMAT_D32_FLOAT:DXGI_FORMAT_UNKNOWN;p.samples.data={samples,0};p.views.data=viewDesc;
        auto& b=p.blend.data.RenderTarget[0];b.BlendEnable=TRUE;b.LogicOpEnable=FALSE;b.SrcBlend=D3D12_BLEND_SRC_ALPHA;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D12_BLEND_OP_ADD;b.SrcBlendAlpha=D3D12_BLEND_ONE;b.DestBlendAlpha=D3D12_BLEND_ZERO;b.BlendOpAlpha=D3D12_BLEND_OP_ADD;b.LogicOp=D3D12_LOGIC_OP_NOOP;b.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(p),&p};pipeline.Reset();Hr(device2->CreatePipelineState(&desc,IID_PPV_ARGS(&pipeline)),"HomeMenu pipeline");pipeline->SetName(L"HomeMenu");pipelineViews=views;pipelineSamples=samples;useDepth=depth;
    }
    void Stage(uint32_t slot){
        if(slot>=BufferedFrames)Fail("Terminal upload slot");auto* out=mapped[slot];lineCount=0;
        for(const auto& client:clients){
        for(uint32_t row=0;row<client.frame.lineCount;row++){
            const auto& source=client.frame.lines[row];auto& line=out[lineCount++];line={};
            line.geometry[0]=source.center.y;line.geometry[1]=source.halfWidth;line.geometry[2]=source.halfHeight;line.geometry[3]=source.center.z;
            line.surface[0]=source.center.x;line.surface[1]=source.curveDepth;line.surface[2]=source.curveCenterX;line.surface[3]=source.curveHalfWidth;
            uint32_t length=0;for(;length<32&&source.text[length];length++)line.chars[length>>2]|=uint32_t(uint8_t(source.text[length]))<<((length&3)*8);
            line.meta[0]=length;line.meta[1]=source.hovered?1u:0u;line.right[3]=(source.flags&1)?.0039f:.010f;
            line.meta[2]=(source.flags&1u)|(source.contourIndex<<8)|(source.contourCount<<16);
            std::memcpy(&line.meta[3],&client.frame.timeSeconds,sizeof(float));
        }
        auto right=Rotate(client.panel.pose.orientation,{1,0,0}),up=Rotate(client.panel.pose.orientation,{0,1,0}),normal=Rotate(client.panel.pose.orientation,{0,0,1});
        for(const auto& source:client.elements){
            auto& line=out[lineCount++];line={};auto p=client.panel.pose.position;
            line.surface[0]=p.x+right.x*source.x+up.x*source.y;line.geometry[0]=p.y+right.y*source.x+up.y*source.y;line.geometry[3]=p.z+right.z*source.x+up.z*source.y;
            line.geometry[1]=source.halfWidth;line.geometry[2]=source.halfHeight;
            line.right[0]=right.x;line.right[1]=right.y;line.right[2]=right.z;line.right[3]=source.pixelMeters;
            line.up[0]=up.x;line.up[1]=up.y;line.up[2]=up.z;line.up[3]=source.backgroundAlpha;
            line.normal[0]=normal.x;line.normal[1]=normal.y;line.normal[2]=normal.z;
            uint32_t length=0;for(;length<LAB_TERMINAL_TEXT_BYTES_V2&&source.text[length];length++)line.chars[length>>2]|=uint32_t(uint8_t(source.text[length]))<<((length&3)*8);
            line.meta[0]=length;line.meta[1]=(source.flags&LAB_TERMINAL_HOVERED_V2?1u:0u)|(source.flags&LAB_TERMINAL_SELECTED_V2?2u:0u);
            line.meta[2]=1u|2u|(client.panel.theme==LAB_TERMINAL_THEME_AMBER_V2?4u:0u)|(source.flags&LAB_TERMINAL_DISABLED_V2?8u:0u)|(source.flags&LAB_TERMINAL_BOTH_FACES_V2?16u:0u);
            line.normal[3]=float(source.id%97)*.017f;std::memcpy(&line.meta[3],&client.panel.timeSeconds,sizeof(float));
        }
        }
        lineDescriptor=services->DescriptorIndex(lines[slot]->srv);
    }
    static void Record(void* context,ID3D12GraphicsCommandList7* list){
        auto& s=*static_cast<Impl*>(context);if(!s.lineCount)return;ID3D12DescriptorHeap* heaps[]{s.services->BindlessResourceHeap(),s.services->BindlessSamplerHeap()};list->SetDescriptorHeaps(2,heaps);list->SetGraphicsRootSignature(s.services->BindlessRootSignature());list->SetGraphicsRootConstantBufferView(0,s.frameConstants);uint32_t constants[4]{s.lineDescriptor,s.atlasDescriptor,s.depthDescriptor,0};list->SetGraphicsRoot32BitConstants(1,4,constants,0);list->SetPipelineState(s.pipeline.Get());D3D12_VIEWPORT viewport{0,0,float(s.width),float(s.height),0,1};D3D12_RECT scissor{0,0,LONG(s.width),LONG(s.height)};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->OMSetRenderTargets(1,&s.colorRtv,FALSE,s.useDepth?&s.depthDsv:nullptr);list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(96,s.lineCount,0,0);
    }
    void Build(GpuPassBuilder& builder,const GpuFrameContext& ctx){
        if(!services||suppressed)return;Stage(ctx.uploadSlot);if(!lineCount)return;const auto& color=ctx.Require(GpuFrameResourceId::ResolvedColor);if(!color.rtv.ptr)Fail("HomeMenu requires rasterizable ResolvedColor");
        auto colorDesc=color.resource->GetDesc();EnsurePipeline(ctx.viewCount,colorDesc.SampleDesc.Count,false);frameConstants=ctx.frameConstants;colorRtv=color.rtv;depthDsv={};width=ctx.width;height=ctx.height;
        builder.BeginPass("Terminal.Raster",GpuPassQueue::Direct,{this,&Record});
        builder.Buffer(atlas->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        builder.Buffer(lines[ctx.uploadSlot]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        builder.Texture(color.resource,GpuPassResourceAccess::ReadWrite,color.initialLayout,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
        const auto& depth=ctx.Require(GpuFrameResourceId::SceneDepth);depthDescriptor=services->DescriptorIndex(depth.srv);
        builder.Texture(depth.resource,GpuPassResourceAccess::Read,depth.initialLayout,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        builder.EndPass();
    }
};

TerminalDomain::TerminalDomain():impl_(std::make_unique<Impl>()){}
TerminalDomain::~TerminalDomain()=default;
uint32_t TerminalDomain::Publish(uint64_t owner,const LabTerminalFrameV1& frame){
    if(!owner||frame.structSize<sizeof(frame)||frame.lineCount>LAB_TERMINAL_MAX_LINES_V1||!std::isfinite(frame.timeSeconds))return 0;
    for(uint32_t i=0;i<frame.lineCount;i++){
        const auto& l=frame.lines[i];const float numbers[]{l.center.x,l.center.y,l.center.z,l.halfWidth,l.halfHeight,l.curveDepth,l.curveCenterX,l.curveHalfWidth};
        for(float v:numbers)if(!std::isfinite(v)||std::abs(v)>100000)return 0;
        if(l.halfWidth<=0||l.halfHeight<=0||l.curveDepth<0||l.curveHalfWidth<=0||l.flags>1||l.contourIndex>=LAB_TERMINAL_MAX_LINES_V1||l.contourCount>LAB_TERMINAL_MAX_LINES_V1||((l.flags&1)&&(!l.contourCount||l.contourIndex>=l.contourCount)))return 0;
        for(char ch:l.text)if(uint8_t(ch)>127)return 0;
    }
    Impl::Client* free{};for(auto& client:impl_->clients){if(client.owner==owner){client.frame=frame;if(!frame.lineCount&&client.elements.empty())client={};return 1;}if(!client.owner&&!free)free=&client;}
    if(!frame.lineCount)return 1;if(!free)return 0;free->owner=owner;free->frame=frame;return 1;
}
uint32_t TerminalDomain::Publish(uint64_t owner,const LabTerminalFrameV2& f){
    if(!owner||f.structSize<sizeof(f)||f.nodeCount>LAB_TERMINAL_MAX_NODES_V2||f.elementCount>LAB_TERMINAL_MAX_ELEMENTS_V2||f.theme>1||!std::isfinite(f.timeSeconds)||(f.nodeCount&&!f.nodes)||(f.elementCount&&!f.elements))return 0;
    const auto& p=f.pose;float q=p.orientation.x*p.orientation.x+p.orientation.y*p.orientation.y+p.orientation.z*p.orientation.z+p.orientation.w*p.orientation.w;
    if(!Finite(p.position.x)||!Finite(p.position.y)||!Finite(p.position.z)||!std::isfinite(q)||std::abs(q-1)>.002f)return 0;
    for(uint32_t i=0;i<f.nodeCount;i++){
        const auto& n=f.nodes[i];if(!n.id||n.capabilities>7||n.flags>3||!Ascii(n.label,sizeof(n.label)))return 0;
        for(uint32_t j=0;j<i;j++)if(f.nodes[j].id==n.id)return 0;
        auto parent=n.parentId;uint32_t depth=0;
        while(parent){if(parent==n.id||++depth>f.nodeCount)return 0;const LabTerminalNodeV2* found{};for(uint32_t j=0;j<f.nodeCount;j++)if(f.nodes[j].id==parent){found=&f.nodes[j];break;}if(!found)return 0;parent=found->parentId;}
    }
    for(uint32_t i=0;i<f.elementCount;i++){
        const auto& e=f.elements[i];if(!e.id||e.kind>LAB_TERMINAL_LABEL_V2||e.flags>31||!Ascii(e.text,sizeof(e.text)))return 0;
        const float values[]{e.x,e.y,e.halfWidth,e.halfHeight,e.pixelMeters,e.backgroundAlpha};for(float v:values)if(!Finite(v))return 0;
        if(e.halfWidth<=0||e.halfHeight<=0||e.pixelMeters<=0||e.backgroundAlpha<0||e.backgroundAlpha>1)return 0;
        for(uint32_t j=0;j<i;j++)if(f.elements[j].id==e.id)return 0;
        if(e.nodeId){bool found=false;for(uint32_t j=0;j<f.nodeCount;j++)found|=f.nodes[j].id==e.nodeId;if(!found)return 0;}
    }
    Impl::Client* slot{};for(auto& c:impl_->clients)if(c.owner==owner){slot=&c;break;}else if(!c.owner&&!slot)slot=&c;
    if(!slot)return f.elementCount?0:1;
    slot->owner=owner;slot->panel=f;slot->panel.nodes=nullptr;slot->panel.elements=nullptr;
    slot->nodes.clear();slot->elements.clear();if(f.nodeCount)slot->nodes.assign(f.nodes,f.nodes+f.nodeCount);if(f.elementCount)slot->elements.assign(f.elements,f.elements+f.elementCount);
    if(slot->elements.empty()&&!slot->frame.lineCount)*slot={};return 1;
}
uint32_t TerminalDomain::HitTest(uint64_t owner,const LabTerminalHitQueryV2& q,LabTerminalHitV2& out)const{
    if(q.structSize<sizeof(q)||out.structSize<sizeof(out)||q.mode>1)return 0;out={sizeof(out)};
    const float values[]{q.origin.x,q.origin.y,q.origin.z,q.direction.x,q.direction.y,q.direction.z,q.maxDistance,q.touchRadius};for(float v:values)if(!Finite(v))return 0;
    if(q.maxDistance<0||q.touchRadius<0||q.touchRadius>.15f)return 0;
    if(q.mode==LAB_TERMINAL_HIT_RAY_V2&&std::abs(Dot(q.direction,q.direction)-1)>.01f)return 0;
    for(const auto& c:impl_->clients)if(c.owner==owner){
        auto inv=c.panel.pose.orientation;inv.x=-inv.x;inv.y=-inv.y;inv.z=-inv.z;
        auto p=c.panel.pose.position;auto o=Rotate(inv,{q.origin.x-p.x,q.origin.y-p.y,q.origin.z-p.z}),d=Rotate(inv,q.direction);
        float t=0;if(q.mode==LAB_TERMINAL_HIT_TOUCH_V2){if(std::abs(o.z)>q.touchRadius)return 1;}
        else{if(std::abs(d.z)<1e-6f)return 1;t=-o.z/d.z;if(t<=0||t>q.maxDistance)return 1;}
        float x=o.x+d.x*t,y=o.y+d.y*t;
        // Reverse order matches raster stacking. Labels/group panels never steal hits.
        for(auto it=c.elements.rbegin();it!=c.elements.rend();++it){const auto& e=*it;
            if(!(e.flags&LAB_TERMINAL_INTERACTIVE_V2)||(e.flags&LAB_TERMINAL_DISABLED_V2)||(o.z>0&&!(e.flags&LAB_TERMINAL_BOTH_FACES_V2)))continue;
            if(std::abs(x-e.x)<=e.halfWidth&&std::abs(y-e.y)<=e.halfHeight){out.hit=1;out.elementId=e.id;out.nodeId=e.nodeId;out.x=x-e.x;out.y=y-e.y;out.distance=q.mode?std::abs(o.z):t;return 1;}
        }return 1;
    }return 1;
}
void TerminalDomain::SetSuppressed(bool value){impl_->suppressed=value;}
void TerminalDomain::Clear(uint64_t owner){for(auto& client:impl_->clients)if(client.owner==owner)client={};}
GpuWorldDomain TerminalDomain::Identity()const noexcept{return TerminalDomainId;}
const char* TerminalDomain::Name()const noexcept{return "Terminal";}
void TerminalDomain::Initialize(GpuDomainServices& services){impl_->Initialize(services);}
void TerminalDomain::BuildPasses(GpuPassBuilder& builder,const GpuFrameContext& context){impl_->Build(builder,context);}
}
