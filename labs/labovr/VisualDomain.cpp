#include "VisualDomain.h"
#include "VisualAssets.h"
#include "Graphics.h"
#include "GpuFrameResources.h"
#include "ClientAssets.h"
#include "Core.h"
#include <fstream>
#include <numeric>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdio>
namespace lab {
static_assert(ConsoleSpec::MaxTextures==LABOVR_VISUAL_MAX_TEXTURES_V1);
static_assert(ConsoleSpec::MaxMaterials==LABOVR_VISUAL_MAX_MATERIALS_V1);
static_assert(ConsoleSpec::MaxMeshes==LABOVR_VISUAL_MAX_MESHES_V1);
static_assert(ConsoleSpec::MaxObjects==LABOVR_VISUAL_MAX_OBJECTS_V1);
static_assert(ConsoleSpec::MaxLights==LABOVR_VISUAL_MAX_LIGHTS_V1);
static_assert(ConsoleSpec::MaxVisualStreams==LABOVR_VISUAL_MAX_STREAMS_V1);
static_assert(ConsoleSpec::MaxVisualEffects==LABOVR_VISUAL_MAX_EFFECTS_V1);
static_assert(ConsoleSpec::MaxVisualCaptures==LABOVR_VISUAL_MAX_CAPTURES_V1);
static_assert(ConsoleSpec::MaxDynamicVerticesPerFrame==LABOVR_VISUAL_MAX_DYNAMIC_VERTICES_V1);
static_assert(ConsoleSpec::MaxCaptureWidth==LABOVR_VISUAL_MAX_CAPTURE_WIDTH_V1);
static_assert(ConsoleSpec::MaxCaptureHeight==LABOVR_VISUAL_MAX_CAPTURE_HEIGHT_V1);
static_assert(ConsoleSpec::MaxTextureEdge==LABOVR_VISUAL_MAX_TEXTURE_EDGE_V1);
static_assert(ConsoleSpec::MaxHeightParallaxSteps==LABOVR_VISUAL_MAX_HEIGHT_PARALLAX_STEPS_V1);
static_assert(ConsoleSpec::MaxHeightTessellation==LABOVR_VISUAL_MAX_HEIGHT_TESSELLATION_V1);
static_assert(ConsoleSpec::FrameSlots==LABOVR_VISUAL_FRAME_SLOTS_V1);
static_assert(ConsoleSpec::MaxViews==LABOVR_VISUAL_MAX_VIEWS_V1);
static_assert(ConsoleSpec::MaxWorldStaticGpuBytes==LABOVR_VISUAL_MAX_STATIC_GPU_BYTES_V1);
static_assert(ConsoleSpec::MaxWorldDynamicGpuBytes==LABOVR_VISUAL_MAX_DYNAMIC_GPU_BYTES_V1);

using Microsoft::WRL::ComPtr;
namespace {
constexpr uint32_t Groups=1+ConsoleSpec::MaxVisualCaptures;
constexpr uint32_t DrawCapacity=2*ConsoleSpec::MaxObjects+ConsoleSpec::MaxVisualStreams+ConsoleSpec::MaxVisualEffects;
constexpr uint32_t PassCapacity=Groups*(6+ConsoleSpec::MaxVisualEffects*4)+ConsoleSpec::MaxVisualCaptures+1;
constexpr uint32_t OverlayFlag=1u<<0,AlphaFlag=1u<<1;
struct VisualDrawGpu {
    XMFLOAT4 positionEmission{},rotation{0,0,0,1},scale{1,1,1,0},tint{1,1,1,1};
    XMFLOAT4 materialParams{},detailParams{},advancedParams{},deform{},extra{};
    XMUINT4 texture0{Invalid,Invalid,Invalid,Invalid},texture1{Invalid,Invalid,0,0};
    XMFLOAT4 effect0{},effect1{};
};
static_assert(sizeof(VisualDrawGpu)==13*16);
struct VisualLightGpu {XMFLOAT4 positionRadius{},colorPower{};};
struct VisualCamera {
    XMFLOAT4X4 vp[ConsoleSpec::MaxViews],inverseVP[ConsoleSpec::MaxViews],previousVP[ConsoleSpec::MaxViews],shadowVP[24];
    XMFLOAT4 eye[ConsoleSpec::MaxViews];XMUINT4 resourceAbi,dimensions,outputDimensions,counts,topology;XMFLOAT4 timing;
};
template<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type,class T>struct alignas(void*) Subobject {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type=Type;T data{};};
struct Pipeline {
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE,ID3D12RootSignature*> root;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,D3D12_SHADER_BYTECODE> vs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS,D3D12_SHADER_BYTECODE> hs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS,D3D12_SHADER_BYTECODE> ds;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS,D3D12_SHADER_BYTECODE> gs;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS,D3D12_SHADER_BYTECODE> ps;
    Subobject<D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT,D3D12_INPUT_LAYOUT_DESC> input;
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
std::vector<std::byte> VisualShader(const wchar_t* entry){auto path=AssetPath((std::wstring(L"shaders/")+entry+L".cso").c_str());auto bytes=ReadBytes(path);if(bytes.empty())Fail("Missing Core visual shader");return bytes;}
Pose ToPose(const LabPoseV1& p){return {{p.position.x,p.position.y,p.position.z},{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w}};}
LabPoseV1 FromPose(const Pose& p){return {{p.p.x,p.p.y,p.p.z},{p.q.x,p.q.y,p.q.z,p.q.w}};}
bool Finite(float x){return std::isfinite(x);}
void ValidatePose(const LabPoseV1& p,const LabVec3V1& s){float norm=p.orientation.x*p.orientation.x+p.orientation.y*p.orientation.y+p.orientation.z*p.orientation.z+p.orientation.w*p.orientation.w;if(!Finite(p.position.x)||!Finite(p.position.y)||!Finite(p.position.z)||!Finite(norm)||norm<.0001f||!Finite(s.x)||!Finite(s.y)||!Finite(s.z)||s.x==0||s.y==0||s.z==0)Fail("Invalid visual pose");}
}
struct VisualDomain::Impl {
    struct Upload {GpuResourceLease lease;void* mapped{};void Reset(){if(mapped&&lease)lease->resource->Unmap(0,nullptr);mapped=nullptr;lease={};}~Upload(){Reset();}};
    struct DynamicSlot {Upload staging;GpuResourceLease gpu;uint32_t vertexCapacity{};};
    struct IndexSlot {Upload staging;GpuResourceLease gpu;uint32_t indexCapacity{};};
    struct Mesh {GpuResourceLease vb,ib;uint32_t count{},sceneMesh{Invalid};bool valid{};};
    struct Object {LabVisualObjectV1 desc{};LabVisualObjectStateV1 state{};bool standard{};};
    struct ObjectLookup {LabU32 id{Invalid};uint32_t index{Invalid};};
    struct PhaseResources {std::vector<uint32_t> meshes,textures;bool dynamicStream{},sortedStream{};};
    struct SortedTriangle {float depth;uint32_t triangle;};
    struct Stream {LabVisualStreamV1 desc{};bool valid{};};
    struct Effect {LabVisualEffectV1 desc{};LabVisualEffectStateV1 state{};bool valid{};};
    struct BloomBuffers {
        GpuResourceLease a,b;ComPtr<ID3D12DescriptorHeap> rtvA,rtvB;uint32_t width{},height{},layers{};
    };
    struct Capture {
        LabVisualCaptureV1 desc{};bool valid{};BloomBuffers bloom;GpuResourceLease color,depth,snapshot;ComPtr<ID3D12DescriptorHeap> rtvs,dsvs;
        uint64_t native{},frameId{};bool failed{},pending{};LabVisualCaptureRequestV1 request{};
    };
    struct Draw {VisualProgram program{};uint32_t phase{},mesh{Invalid},first{},count{},firstIndex{},data{};bool stream{},indexed{};};
    struct Group {
        std::array<Draw,DrawCapacity> draws{};uint32_t count{},indexCount{};XMFLOAT3 eye{};uint32_t capture{Invalid};
        std::array<Upload,ConsoleSpec::FrameSlots> drawTable,camera;
        std::array<IndexSlot,ConsoleSpec::FrameSlots> indices;
    };
    struct Pass {Impl* self{};Group* group{};uint32_t phase{},slot{},viewCount{},width{},height{};uint64_t camera{};ID3D12Resource *color{},*depth{};D3D12_CPU_DESCRIPTOR_HANDLE rtv{},dsv{};GpuResourceLease snapshot;VisualProgram bloomProgram{};uint32_t drawIndex{},sourceDescriptor{Invalid},bloomFlags{};bool clear{},copy{},indexCopy{},offscreen{};};
    Graphics& graphics;GpuDomainServices* services{};ComPtr<ID3D12Device2> device2;
    // PSOs are fixed Core catalog entries. Index 0 is mono, 1..MaxViews are XR variants.
    std::array<std::array<std::array<ComPtr<ID3D12PipelineState>,ConsoleSpec::MaxViews+1>,2>,uint32_t(VisualProgram::Count)> psos;
    std::array<Mesh,ConsoleSpec::MaxMeshes> meshes;
    std::array<GpuResourceLease,ConsoleSpec::MaxTextures> textures;
    std::array<bool,ConsoleSpec::MaxTextures> textureDeclared{};
    std::array<bool,ConsoleSpec::MaxMeshes> meshDeclared{};
    std::array<LabVisualMaterialV1,ConsoleSpec::MaxMaterials> materials{};
    std::array<bool,ConsoleSpec::MaxMaterials> materialValid{};
    std::array<Object,ConsoleSpec::MaxObjects> objects{};uint32_t objectCount{};
    std::vector<ObjectLookup> objectLookup;uint32_t objectLookupMask{};
    std::array<uint32_t,ConsoleSpec::MaxLights> lightObject{};
    PhaseResources dependencies[2][uint32_t(VisualPhase::Count)];
    std::array<Stream,ConsoleSpec::MaxVisualStreams> streams{};
    std::array<Effect,ConsoleSpec::MaxVisualEffects> effects{};
    std::array<Capture,ConsoleSpec::MaxVisualCaptures> captures{};
    std::array<LabVisualLightV1,ConsoleSpec::MaxLights> lights{};uint32_t lightCount{};
    std::array<Upload,ConsoleSpec::FrameSlots> resourceTable,lightTable;
    std::array<DynamicSlot,ConsoleSpec::FrameSlots> dynamicSlots;
    uint32_t activeGroupCount{},drawCapacity{},textureSpan{},lightCapacity{};
    std::array<Group,Groups> groups;
    std::array<Pass,PassCapacity> passes;uint32_t passCount{};
    std::vector<SortedTriangle> triangleScratch;
    std::array<uint32_t,ConsoleSpec::MaxVisualStreams> streamOffsets{};
    GpuResourceLease mainSnapshot;BloomBuffers mainBloom;Mesh orbMesh;
    std::array<std::array<std::array<std::array<char,64>,4>,ConsoleSpec::MaxVisualEffects>,Groups> bloomNames{};
    LabVisualBatchV1 batch{};Pose mainCamera{};uint64_t frameId{},generation{},staticBytes{},dynamicBytes{},copyBytes{};
    bool configured{},needsSnapshot{};
    explicit Impl(Graphics& g):graphics(g){
        static constexpr const char* stages[]={"Extract","BlurHorizontal","BlurVertical","Composite"};
        for(uint32_t group=0;group<Groups;++group)for(uint32_t effect=0;effect<ConsoleSpec::MaxVisualEffects;++effect)for(uint32_t stage=0;stage<4;++stage)
            std::snprintf(bloomNames[group][effect][stage].data(),64,"Visual.Group%u.Bloom%u.%s",group,effect,stages[stage]);
    }
    void Charge(const GpuResourceLease& r,bool dynamicAllocation=false){if(!r)Fail("Visual GPU allocation refused");auto& total=dynamicAllocation?dynamicBytes:staticBytes;auto limit=dynamicAllocation?ConsoleSpec::MaxWorldDynamicGpuBytes:ConsoleSpec::MaxWorldStaticGpuBytes;if(r->allocationBytes>limit-total)Fail("Visual GPU budget exceeded");total+=r->allocationBytes;}
    void Allocate(Upload& out,uint64_t bytes,uint32_t stride,bool srv,const wchar_t* name){GpuBufferDesc d{};d.bytes=bytes;d.stride=stride;d.heap=D3D12_HEAP_TYPE_UPLOAD;d.srv=srv;d.uav=false;d.name=name;out.lease=services->CreateManagedBuffer(d);Charge(out.lease,true);D3D12_RANGE empty{};Hr(out.lease->resource->Map(0,&empty,&out.mapped),"Map visual upload");}
    void ReleaseDynamic(GpuResourceLease& resource){if(resource){dynamicBytes-=resource->allocationBytes;resource={};}}
    void ReleaseDynamicUpload(Upload& upload){if(upload.lease)dynamicBytes-=upload.lease->allocationBytes;upload.Reset();}
    void EnsureDynamicSlot(uint32_t slot,uint32_t requiredVertices){
        if(requiredVertices>ConsoleSpec::MaxDynamicVerticesPerFrame)Fail("Visual dynamic vertex capacity");
        auto& current=dynamicSlots[slot];if(requiredVertices<=current.vertexCapacity)return;
        uint32_t capacity=std::min(std::max(4096u,std::bit_ceil(requiredVertices)),ConsoleSpec::MaxDynamicVerticesPerFrame);
        ReleaseDynamicUpload(current.staging);ReleaseDynamic(current.gpu);current.vertexCapacity=0;
        uint64_t bytes=uint64_t(capacity)*sizeof(LabVisualStreamVertexV1);
        Allocate(current.staging,bytes,sizeof(LabVisualStreamVertexV1),false,L"Visual.AggregateStaging");
        GpuBufferDesc d{};d.bytes=bytes;d.stride=sizeof(LabVisualStreamVertexV1);d.heap=D3D12_HEAP_TYPE_DEFAULT;d.srv=d.uav=false;d.name=L"Visual.AggregateDefault";
        current.gpu=services->CreateManagedBuffer(d);Charge(current.gpu,true);current.vertexCapacity=capacity;
    }
    uint32_t SortedVertexRequirement(uint32_t visibilityMask)const{
        uint32_t total=0;for(uint32_t i=0;i<batch.streamCount;++i){const auto& frame=batch.streams[i];const auto& stream=streams[frame.stream].desc;
            if(stream.sortMode==LABOVR_VISUAL_SORT_BACK_TO_FRONT_V1&&(stream.visibilityMask&visibilityMask)){
                if(frame.vertexCount>ConsoleSpec::MaxDynamicVerticesPerFrame-total)Fail("Visual sort index capacity");total+=frame.vertexCount;}}
        return total;
    }
    void EnsureIndexSlot(Group& group,uint32_t slot,uint32_t requiredIndices){
        if(requiredIndices>ConsoleSpec::MaxDynamicVerticesPerFrame)Fail("Visual sort index capacity");
        auto& current=group.indices[slot];if(requiredIndices<=current.indexCapacity)return;
        uint32_t capacity=std::min(std::max(1024u,std::bit_ceil(requiredIndices)),ConsoleSpec::MaxDynamicVerticesPerFrame);
        ReleaseDynamicUpload(current.staging);ReleaseDynamic(current.gpu);current.indexCapacity=0;
        uint64_t bytes=uint64_t(capacity)*sizeof(uint32_t);Allocate(current.staging,bytes,sizeof(uint32_t),false,L"Visual.SortedIndexStaging");
        GpuBufferDesc d{};d.bytes=bytes;d.stride=sizeof(uint32_t);d.heap=D3D12_HEAP_TYPE_DEFAULT;d.srv=d.uav=false;d.name=L"Visual.SortedIndexDefault";
        current.gpu=services->CreateManagedBuffer(d);Charge(current.gpu,true);current.indexCapacity=capacity;
    }
    uint32_t FindObject(LabU32 id)const{
        if(id==Invalid||objectLookup.empty())return Invalid;uint32_t hash=id*2654435761u;
        for(uint32_t n=0;n<objectLookup.size();++n){const auto& item=objectLookup[(hash+n)&objectLookupMask];if(item.id==Invalid)return Invalid;if(item.id==id)return item.index;}
        return Invalid;
    }
    void BuildObjectLookup(){
        uint32_t capacity=std::bit_ceil(std::max(2u,objectCount*2u));objectLookup.assign(capacity,{Invalid,Invalid});objectLookupMask=capacity-1;
        for(uint32_t i=0;i<objectCount;++i){uint32_t hash=objects[i].desc.id*2654435761u;for(uint32_t n=0;n<capacity;++n){auto& item=objectLookup[(hash+n)&objectLookupMask];if(item.id==Invalid){item={objects[i].desc.id,i};break;}if(item.id==objects[i].desc.id)Fail("Duplicate visual object ID");}}
    }
    void BuildDependencies(){
        auto add=[&](uint32_t visibility,uint32_t phase,uint32_t mesh,uint32_t material,bool stream,bool sorted){
            for(uint32_t view=0;view<2;++view)if(visibility&(view?LABOVR_VISUAL_CAPTURE_V1:LABOVR_VISUAL_MAIN_V1)){
                auto& resources=dependencies[view][phase];if(mesh!=Invalid)resources.meshes.push_back(mesh);resources.dynamicStream|=stream;resources.sortedStream|=sorted;
                if(material!=Invalid){const auto& m=materials[material];for(auto id:{m.albedoTexture,m.normalTexture,m.ormTexture,m.heightTexture,m.detailTexture})if(id!=Invalid&&textures[id])resources.textures.push_back(id);}
            }
        };
        for(uint32_t i=0;i<objectCount;++i){const auto& object=objects[i];if(object.standard)continue;auto model=materials[object.desc.material].model;
            add(object.desc.visibilityMask,model==LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1?1u:0u,object.desc.mesh,object.desc.material,false,false);
            if(model==LABOVR_VISUAL_MATERIAL_HEIGHT_PBR_V1)add(object.desc.visibilityMask,uint32_t(VisualPhase::SpecialDepth),object.desc.mesh,object.desc.material,false,false);}
        for(const auto& stream:streams)if(stream.valid){const auto& material=materials[stream.desc.material];uint32_t phase=material.model==LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1?1u:(material.flags&OverlayFlag)?2u:0u;
            add(stream.desc.visibilityMask,phase,Invalid,stream.desc.material,true,stream.desc.sortMode==LABOVR_VISUAL_SORT_BACK_TO_FRONT_V1);}
        for(const auto& effect:effects)if(effect.valid&&effect.desc.type==LABOVR_VISUAL_EFFECT_LIQUID_ORB_V1)add(effect.desc.visibilityMask,0,ConsoleSpec::MaxMeshes,Invalid,false,false);
        for(auto& view:dependencies)for(auto& phase:view){std::sort(phase.meshes.begin(),phase.meshes.end());phase.meshes.erase(std::unique(phase.meshes.begin(),phase.meshes.end()),phase.meshes.end());std::sort(phase.textures.begin(),phase.textures.end());phase.textures.erase(std::unique(phase.textures.begin(),phase.textures.end()),phase.textures.end());}
    }
    Mesh UploadMesh(const VisualMeshData& cpu){Mesh m;m.valid=true;m.count=uint32_t(cpu.indices.size());m.vb=services->UploadStaticBuffer(std::as_bytes(std::span(cpu.vertices)),sizeof(VisualMeshVertex),L"Visual.MeshVertices");m.ib=services->UploadStaticBuffer(std::as_bytes(std::span(cpu.indices)),4,L"Visual.MeshIndices");Charge(m.vb);Charge(m.ib);return m;}
    void Pipelines(){
        static const D3D12_INPUT_ELEMENT_DESC meshInput[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"TANGENT",0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,36,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        static const D3D12_INPUT_ELEMENT_DESC streamInput[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        static const D3D12_INPUT_ELEMENT_DESC pointInput[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"COLOR",0,DXGI_FORMAT_R32G32B32A32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1}};
        for(uint32_t program=0;program<uint32_t(VisualProgram::Count);++program)for(uint32_t stream=0;stream<2;++stream){
            auto kind=VisualProgram(program);if(stream&&kind!=VisualProgram::Gel)continue;
            const wchar_t *vs{},*hs{},*ds{},*gs{},*ps{};D3D12_PRIMITIVE_TOPOLOGY_TYPE topo=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            bool inputStream=false,noInput=false,alpha=false,writeDepth=false,post=false;
            switch(kind){
            case VisualProgram::HeightPbr:case VisualProgram::HeightPbrOpaque:case VisualProgram::HeightPbrDepth:
                vs=L"VisualHeightVS";hs=L"VisualHeightHS";ds=L"VisualHeightDS";ps=kind==VisualProgram::HeightPbrDepth?nullptr:L"VisualHeightPS";topo=D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;writeDepth=kind!=VisualProgram::HeightPbr;break;
            case VisualProgram::Gel:vs=stream?L"VisualGelStreamVS":L"VisualGelMeshVS";ps=L"VisualGelPS";inputStream=stream!=0;alpha=true;break;
            case VisualProgram::StreamTriangles:vs=L"VisualStreamVS";ps=L"VisualStreamPS";inputStream=true;alpha=true;writeDepth=true;break;
            case VisualProgram::StreamLines:vs=L"VisualStreamVS";ps=L"VisualStreamPS";inputStream=true;alpha=true;topo=D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;break;
            case VisualProgram::StreamPoints:vs=L"VisualPointVS";ps=L"VisualPointPS";inputStream=true;alpha=true;break;
            case VisualProgram::Cosmos:vs=L"VisualCosmosVS";ps=L"VisualCosmosPS";noInput=true;break;
            case VisualProgram::LiquidOrb:vs=L"VisualOrbVS";ps=L"VisualOrbPS";alpha=true;writeDepth=true;break;
            case VisualProgram::BloomExtract:vs=L"VisualBloomVS";ps=L"VisualBloomExtractPS";noInput=true;post=true;break;
            case VisualProgram::BloomBlur:vs=L"VisualBloomVS";ps=L"VisualBloomBlurPS";noInput=true;post=true;break;
            case VisualProgram::BloomComposite:vs=L"VisualBloomVS";ps=L"VisualBloomCompositePS";noInput=true;post=true;alpha=true;break;
            case VisualProgram::Count:continue;
            }
            auto v=VisualShader(vs),p=ps?VisualShader(ps):std::vector<std::byte>{};auto h=hs?VisualShader(hs):std::vector<std::byte>{},d=ds?VisualShader(ds):std::vector<std::byte>{},g=gs?VisualShader(gs):std::vector<std::byte>{};
            for(uint32_t views=0;views<=ConsoleSpec::MaxViews;++views){Pipeline pipe{};pipe.root.data=services->BindlessRootSignature();pipe.vs.data={v.data(),v.size()};pipe.hs.data={h.data(),h.size()};pipe.ds.data={d.data(),d.size()};pipe.gs.data={g.data(),g.size()};pipe.ps.data=ps?D3D12_SHADER_BYTECODE{p.data(),p.size()}:D3D12_SHADER_BYTECODE{};
                if(!noInput)pipe.input.data={kind==VisualProgram::StreamPoints?pointInput:inputStream?streamInput:meshInput,inputStream?2u:4u};
                auto& blend=pipe.blend.data.RenderTarget[0];blend.BlendEnable=alpha;blend.SrcBlend=post?D3D12_BLEND_ONE:alpha?D3D12_BLEND_SRC_ALPHA:D3D12_BLEND_ONE;blend.DestBlend=post?D3D12_BLEND_ONE:alpha?D3D12_BLEND_INV_SRC_ALPHA:D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=blend.DestBlend;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
                pipe.mask.data=UINT_MAX;pipe.raster.data.FillMode=D3D12_FILL_MODE_SOLID;pipe.raster.data.CullMode=D3D12_CULL_MODE_NONE;pipe.raster.data.DepthClipEnable=TRUE;
                auto& depth=pipe.depth.data;depth.DepthEnable=!post;depth.DepthWriteMask=writeDepth?D3D12_DEPTH_WRITE_MASK_ALL:D3D12_DEPTH_WRITE_MASK_ZERO;depth.DepthFunc=kind==VisualProgram::HeightPbr?D3D12_COMPARISON_FUNC_EQUAL:D3D12_COMPARISON_FUNC_GREATER_EQUAL;depth.StencilReadMask=depth.StencilWriteMask=255;depth.FrontFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};depth.BackFace=depth.FrontFace;
                pipe.topology.data=topo;pipe.formats.data.NumRenderTargets=kind==VisualProgram::HeightPbrDepth?0u:1u;pipe.formats.data.RTFormats[0]=views||kind==VisualProgram::BloomExtract||kind==VisualProgram::BloomBlur?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_R8G8B8A8_UNORM;if(kind==VisualProgram::HeightPbrDepth)pipe.formats.data.RTFormats[0]=DXGI_FORMAT_UNKNOWN;pipe.depthFormat.data=post?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_D32_FLOAT;pipe.samples.data={1,0};
                std::array<D3D12_VIEW_INSTANCE_LOCATION,ConsoleSpec::MaxViews> locations{};for(uint32_t i=0;i<views;++i)locations[i]={0,i};if(views)pipe.views.data={views,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};
                D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(pipe),&pipe};Hr(device2->CreatePipelineState(&desc,IID_PPV_ARGS(&psos[program][stream][views])),"Core visual PSO");
            }
        }
    }
    VisualDrawGpu Material(uint32_t id,uint32_t slot)const{const auto& m=materials[id];VisualDrawGpu d;std::memcpy(&d.tint,m.tint,16);d.materialParams={m.uvScale,m.normalScale,m.roughnessBias,m.heightScale};d.detailParams={m.detailScale,m.detailNormalScale,float(m.parallaxMinSteps),float(m.parallaxMaxSteps)};d.advancedParams={m.wetness,m.emission,m.refractionStrength,m.thickness};d.extra.x=m.tessFactor;d.texture0={m.albedoTexture,m.normalTexture,m.ormTexture,m.heightTexture};d.texture1={m.detailTexture,services->DescriptorIndex(lightTable[slot].lease->srv),lightCount,0};return d;}
    void Add(Group& group,uint32_t slot,Draw draw,const VisualDrawGpu& data){if(group.count>=drawCapacity)Fail("Visual draw capacity");draw.data=group.count;static_cast<VisualDrawGpu*>(group.drawTable[slot].mapped)[group.count]=data;group.draws[group.count++]=draw;}
    void SortStream(Group& group,uint32_t slot,const LabVisualStreamFrameV1& in,Draw& draw){
        uint32_t triangles=in.vertexCount/3;if(triangleScratch.size()<triangles)triangleScratch.resize(triangles);
        for(uint32_t i=0;i<triangles;++i){LabVisualStreamVertexV1 vertices[3];std::memcpy(vertices,reinterpret_cast<const std::byte*>(in.vertices)+size_t(i)*3*sizeof(LabVisualStreamVertexV1),sizeof(vertices));
            XMFLOAT3 center{(vertices[0].position[0]+vertices[1].position[0]+vertices[2].position[0])/3,(vertices[0].position[1]+vertices[1].position[1]+vertices[2].position[1])/3,(vertices[0].position[2]+vertices[1].position[2]+vertices[2].position[2])/3};auto delta=Sub(center,group.eye);triangleScratch[i]={Dot(delta,delta),i};}
        std::sort(triangleScratch.begin(),triangleScratch.begin()+triangles,[](const auto& a,const auto& b){return a.depth==b.depth?a.triangle<b.triangle:a.depth>b.depth;});
        if(in.vertexCount>ConsoleSpec::MaxDynamicVerticesPerFrame-group.indexCount)Fail("Visual sort index capacity");
        draw.indexed=true;draw.firstIndex=group.indexCount;auto* indices=static_cast<uint32_t*>(group.indices[slot].staging.mapped)+group.indexCount;
        for(uint32_t t=0;t<triangles;++t)for(uint32_t k=0;k<3;++k)indices[t*3+k]=triangleScratch[t].triangle*3+k;
        group.indexCount+=in.vertexCount;
    }
    void MakeGroup(Group& group,uint32_t slot,bool capture){
        group.count=group.indexCount=0;uint32_t mask=capture?LABOVR_VISUAL_CAPTURE_V1:LABOVR_VISUAL_MAIN_V1;
        for(auto& e:effects)if(e.valid&&e.state.enabled&&(e.desc.visibilityMask&mask)&&e.desc.type==LABOVR_VISUAL_EFFECT_COSMOS_V1){VisualDrawGpu d;std::memcpy(&d.effect0,e.state.values,16);std::memcpy(&d.effect1,e.state.values+4,16);Add(group,slot,{VisualProgram::Cosmos,0,Invalid,0,3},d);}
        for(uint32_t i=0;i<objectCount;++i){auto& o=objects[i];if(!o.state.visible||!(o.desc.visibilityMask&mask)||o.standard)continue;auto& m=materials[o.desc.material];VisualDrawGpu d=Material(o.desc.material,slot);auto pose=ToPose(o.state.current);d.positionEmission={pose.p.x,pose.p.y,pose.p.z,o.desc.emissionScale};d.rotation=pose.q;d.scale={o.state.scale.x,o.state.scale.y,o.state.scale.z,0};d.deform={o.desc.deformAmplitude,o.desc.deformFrequency,o.desc.deformPhase,float(o.desc.deformMode)};
            bool gel=m.model==LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1;Draw draw{gel?VisualProgram::Gel:m.model==LABOVR_VISUAL_MATERIAL_HEIGHT_PBR_V1?VisualProgram::HeightPbr:VisualProgram::HeightPbrOpaque,gel?1u:0u,o.desc.mesh,0,meshes[o.desc.mesh].count,0,0,false,true};Add(group,slot,draw,d);
            if(m.model==LABOVR_VISUAL_MATERIAL_HEIGHT_PBR_V1){if(group.count>=drawCapacity)Fail("Visual depth draw capacity");Draw depthDraw=group.draws[group.count-1];depthDraw.program=VisualProgram::HeightPbrDepth;depthDraw.phase=uint32_t(VisualPhase::SpecialDepth);group.draws[group.count++]=depthDraw;}
        }
        for(uint32_t i=0;i<batch.streamCount;++i){const auto& in=batch.streams[i];const auto& stream=streams[in.stream].desc;if(!in.vertexCount||!(stream.visibilityMask&mask))continue;auto& m=materials[stream.material];VisualDrawGpu d=Material(stream.material,slot);d.extra.y=in.pointSizePixels>0?in.pointSizePixels:stream.pointSizePixels;d.deform.y=m.detailScale;
            bool gel=m.model==LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1;auto program=gel?VisualProgram::Gel:stream.primitive==LABOVR_VISUAL_STREAM_LINES_V1?VisualProgram::StreamLines:stream.primitive==LABOVR_VISUAL_STREAM_POINTS_V1?VisualProgram::StreamPoints:VisualProgram::StreamTriangles;
            uint32_t phase=gel?1u:(m.flags&OverlayFlag)?2u:0u;Draw draw{program,phase,Invalid,streamOffsets[in.stream],in.vertexCount,0,0,true,false};if(stream.sortMode==LABOVR_VISUAL_SORT_BACK_TO_FRONT_V1)SortStream(group,slot,in,draw);Add(group,slot,draw,d);}
        for(auto& e:effects)if(e.valid&&e.state.enabled&&(e.desc.visibilityMask&mask)&&e.desc.type!=LABOVR_VISUAL_EFFECT_COSMOS_V1){VisualDrawGpu d;std::memcpy(&d.effect0,e.state.values,16);std::memcpy(&d.effect1,e.state.values+4,16);d.positionEmission={e.state.position.x,e.state.position.y,e.state.position.z,1};d.scale={e.state.radius*2,e.state.radius*2,e.state.radius*2,0};bool bloom=e.desc.type==LABOVR_VISUAL_EFFECT_BLOOM_V1;Add(group,slot,{bloom?VisualProgram::BloomComposite:VisualProgram::LiquidOrb,bloom?3u:0u,Invalid,0,bloom?3u:orbMesh.count,0,0,false,!bloom},d);}
    }
    GpuResourceLease Snapshot(ID3D12Resource* source,GpuResourceLease& cache){auto d=source->GetDesc();if(cache&&cache->desc.Width==d.Width&&cache->desc.Height==d.Height&&cache->desc.DepthOrArraySize==d.DepthOrArraySize)return cache;GpuTextureDesc td{};td.width=uint32_t(d.Width);td.height=d.Height;td.depthOrArraySize=d.DepthOrArraySize;td.format=d.Format;td.srv=true;td.uav=false;td.name=L"Visual.SceneSnapshot";if(cache)staticBytes-=cache->allocationBytes;cache=services->CreateManagedTexture2D(td);Charge(cache);return cache;}
    void EnsureBloom(ID3D12Resource* target,BloomBuffers& bloom){
        auto source=target->GetDesc();uint32_t width=std::max(1u,uint32_t(source.Width)/2),height=std::max(1u,source.Height/2),layers=source.DepthOrArraySize;
        if(bloom.a&&bloom.width==width&&bloom.height==height&&bloom.layers==layers)return;
        if(bloom.a)staticBytes-=bloom.a->allocationBytes;if(bloom.b)staticBytes-=bloom.b->allocationBytes;bloom={};
        GpuTextureDesc d{};d.width=width;d.height=height;d.depthOrArraySize=uint16_t(layers);d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;d.srv=true;d.uav=false;
        d.name=L"Visual.BloomA";bloom.a=services->CreateManagedTexture2D(d);Charge(bloom.a);d.name=L"Visual.BloomB";bloom.b=services->CreateManagedTexture2D(d);Charge(bloom.b);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.NumDescriptors=1;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        Hr(services->Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bloom.rtvA)),"Bloom RTV A");Hr(services->Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&bloom.rtvB)),"Bloom RTV B");
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=d.format;
        if(layers>1){rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;rtv.Texture2DArray.ArraySize=layers;}else rtv.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        services->Device()->CreateRenderTargetView(bloom.a->resource,&rtv,bloom.rtvA->GetCPUDescriptorHandleForHeapStart());services->Device()->CreateRenderTargetView(bloom.b->resource,&rtv,bloom.rtvB->GetCPUDescriptorHandleForHeapStart());
        bloom.width=width;bloom.height=height;bloom.layers=layers;
    }
    static void RecordBloom(void* c,ID3D12GraphicsCommandList7* commands){
        auto& p=*static_cast<Pass*>(c);auto& s=*p.self;
        ID3D12DescriptorHeap* heaps[]={s.services->BindlessResourceHeap(),s.services->BindlessSamplerHeap()};commands->SetDescriptorHeaps(2,heaps);commands->SetGraphicsRootSignature(s.services->BindlessRootSignature());commands->SetGraphicsRootConstantBufferView(0,p.camera);
        D3D12_VIEWPORT viewport{0,0,float(p.width),float(p.height),0,1};D3D12_RECT rect{0,0,LONG(p.width),LONG(p.height)};commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&rect);commands->OMSetRenderTargets(1,&p.rtv,FALSE,nullptr);
        commands->SetPipelineState(s.psos[uint32_t(p.bloomProgram)][0][p.offscreen?0:p.viewCount].Get());
        uint32_t roots[]={p.drawIndex,s.services->DescriptorIndex(p.group->drawTable[p.slot].lease->srv),p.sourceDescriptor,s.services->DescriptorIndex(s.resourceTable[p.slot].lease->srv),p.bloomFlags};commands->SetGraphicsRoot32BitConstants(1,5,roots,0);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);commands->IASetVertexBuffers(0,0,nullptr);commands->DrawInstanced(3,1,0,0);
    }
    static void RecordCopy(void* c,ID3D12GraphicsCommandList7* commands){auto& p=*static_cast<Pass*>(c);auto& s=*p.self;if(p.copy)commands->CopyResource(p.snapshot->resource,p.color);else if(p.indexCopy){auto& indices=p.group->indices[p.slot];commands->CopyBufferRegion(indices.gpu->resource,0,indices.staging.lease->resource,0,uint64_t(p.group->indexCount)*sizeof(uint32_t));}else commands->CopyBufferRegion(s.dynamicSlots[p.slot].gpu->resource,0,s.dynamicSlots[p.slot].staging.lease->resource,0,s.copyBytes);}
    static void RecordDraws(void* c,ID3D12GraphicsCommandList7* commands){auto& p=*static_cast<Pass*>(c);auto& s=*p.self;auto& group=*p.group;
        ID3D12DescriptorHeap* heaps[]={s.services->BindlessResourceHeap(),s.services->BindlessSamplerHeap()};commands->SetDescriptorHeaps(2,heaps);commands->SetGraphicsRootSignature(s.services->BindlessRootSignature());commands->SetGraphicsRootConstantBufferView(0,p.camera);
        D3D12_VIEWPORT viewport{0,0,float(p.width),float(p.height),0,1};D3D12_RECT rect{0,0,LONG(p.width),LONG(p.height)};commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&rect);commands->OMSetRenderTargets(p.phase==uint32_t(VisualPhase::SpecialDepth)?0u:1u,p.phase==uint32_t(VisualPhase::SpecialDepth)?nullptr:&p.rtv,FALSE,&p.dsv);
        if(p.clear){float black[]={0,0,0,1};commands->ClearRenderTargetView(p.rtv,black,0,nullptr);commands->ClearDepthStencilView(p.dsv,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);}
        for(uint32_t i=0;i<group.count;++i){const auto& draw=group.draws[i];if(draw.phase!=p.phase)continue;commands->SetPipelineState(s.psos[uint32_t(draw.program)][draw.stream&&draw.program==VisualProgram::Gel?1:0][p.offscreen?0:p.viewCount].Get());
            uint32_t roots[]={draw.data,s.services->DescriptorIndex(group.drawTable[p.slot].lease->srv),p.snapshot?s.services->DescriptorIndex(p.snapshot->srv):Invalid,s.services->DescriptorIndex(s.resourceTable[p.slot].lease->srv),p.snapshot&&p.snapshot->desc.DepthOrArraySize==1?4u:0u};commands->SetGraphicsRoot32BitConstants(1,5,roots,0);
            D3D_PRIMITIVE_TOPOLOGY topology=(draw.program==VisualProgram::HeightPbr||draw.program==VisualProgram::HeightPbrDepth||draw.program==VisualProgram::HeightPbrOpaque)?D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:draw.program==VisualProgram::StreamLines?D3D_PRIMITIVE_TOPOLOGY_LINELIST:D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;commands->IASetPrimitiveTopology(topology);
            auto* mesh=draw.mesh!=Invalid?&s.meshes[draw.mesh]:draw.program==VisualProgram::LiquidOrb?&s.orbMesh:nullptr;
            GpuResourceLease vb=draw.stream?s.dynamicSlots[p.slot].gpu:mesh?mesh->vb:GpuResourceLease{};if(vb){D3D12_VERTEX_BUFFER_VIEW v{vb->resource->GetGPUVirtualAddress(),UINT(vb->desc.Width),draw.stream?uint32_t(sizeof(LabVisualStreamVertexV1)):uint32_t(sizeof(VisualMeshVertex))};commands->IASetVertexBuffers(0,1,&v);}else commands->IASetVertexBuffers(0,0,nullptr);
            if(draw.indexed){auto ib=draw.stream?group.indices[p.slot].gpu:mesh->ib;D3D12_INDEX_BUFFER_VIEW v{ib->resource->GetGPUVirtualAddress(),UINT(ib->desc.Width),DXGI_FORMAT_R32_UINT};commands->IASetIndexBuffer(&v);commands->DrawIndexedInstanced(draw.count,1,draw.firstIndex,draw.stream?INT(draw.first):0,0);}else if(draw.program==VisualProgram::StreamPoints)commands->DrawInstanced(6,draw.count,0,draw.first);else commands->DrawInstanced(draw.count,1,draw.first,0);
        }
    }
    Pass& NewPass(){if(passCount==passes.size())Fail("Visual pass capacity");auto& p=passes[passCount++];p={};p.self=this;return p;}
    void BuildGroup(GpuPassBuilder& b,const GpuFrameContext& frame,uint32_t index,const LabVisualCaptureRequestV1* request){auto& group=groups[index];auto* color=frame.resources->Find(GpuFrameResourceId::ResolvedColor);auto* depth=frame.resources->Find(GpuFrameResourceId::SceneDepth);if(!request&&(!color||!depth))return;
        Capture* capture=request?&captures[request->capture]:nullptr;ID3D12Resource* cr=capture?capture->color->resource:color->resource,*dr=capture?capture->depth->resource:depth->resource;
        if(cr->GetDesc().SampleDesc.Count!=1||dr->GetDesc().SampleDesc.Count!=1)Fail("Visual renderer requires single-sample scene resources");
        uint32_t slot=frame.uploadSlot,width=capture?capture->desc.width:frame.width,height=capture?capture->desc.height:frame.height;uint64_t camera=frame.frameConstants;
        group.eye=mainCamera.p;group.capture=request?request->capture:Invalid;
        if(request){VisualCamera c{};auto pose=ToPose(request->pose);group.eye=pose.p;auto view=XMMatrixInverse(nullptr,Matrix(pose));auto projection=XMMatrixPerspectiveFovLH(request->verticalFov,float(width)/height,request->farZ,request->nearZ);auto vp=view*projection;XMStoreFloat4x4(&c.vp[0],vp);XMStoreFloat4x4(&c.inverseVP[0],XMMatrixInverse(nullptr,vp));c.previousVP[0]=c.vp[0];c.eye[0]={pose.p.x,pose.p.y,pose.p.z,1};c.dimensions={width,height,1,0};c.outputDimensions={width,height,width,height};c.topology={1,1,0,0};c.timing={request->nearZ,request->farZ,float(frame.timeSeconds),3};std::memcpy(group.camera[slot].mapped,&c,sizeof(c));camera=group.camera[slot].lease->resource->GetGPUVirtualAddress();}
        EnsureIndexSlot(group,slot,SortedVertexRequirement(capture?LABOVR_VISUAL_CAPTURE_V1:LABOVR_VISUAL_MAIN_V1));
        MakeGroup(group,slot,capture!=nullptr);GpuResourceLease snapshot;if(needsSnapshot)snapshot=Snapshot(cr,capture?capture->snapshot:mainSnapshot);
        auto texture=[&](ID3D12Resource* r,GpuPassResourceAccess access,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS barriers){b.Texture(r,access,D3D12_BARRIER_LAYOUT_COMMON,layout,sync,barriers);};
        static constexpr const char* names[Groups][9]={
            {"Visual.Main.Opaque","Visual.Main.Refractive","Visual.Main.Overlay","Visual.Main.Post","Visual.Main.BeforeRefraction","Visual.Main.BeforePost","Visual.Main.Readback","Visual.Main.SortedIndexUpload","Visual.Main.HeightDepth"},
            {"Visual.Capture0.Opaque","Visual.Capture0.Refractive","Visual.Capture0.Overlay","Visual.Capture0.Post","Visual.Capture0.BeforeRefraction","Visual.Capture0.BeforePost","Visual.Capture0.Readback","Visual.Capture0.SortedIndexUpload","Visual.Capture0.HeightDepth"},
            {"Visual.Capture1.Opaque","Visual.Capture1.Refractive","Visual.Capture1.Overlay","Visual.Capture1.Post","Visual.Capture1.BeforeRefraction","Visual.Capture1.BeforePost","Visual.Capture1.Readback","Visual.Capture1.SortedIndexUpload","Visual.Capture1.HeightDepth"}};
        if(group.indexCount){auto& p=NewPass();p.group=&group;p.slot=slot;p.indexCopy=true;
            b.BeginPass(names[index][7],GpuPassQueue::Copy,{&p,RecordCopy});b.Buffer(group.indices[slot].staging.lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);b.Buffer(group.indices[slot].gpu->resource,GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);b.EndPass();}
        bool hasHeightDepth=false;for(uint32_t i=0;i<group.count;++i)hasHeightDepth|=group.draws[i].phase==uint32_t(VisualPhase::SpecialDepth);
        const uint32_t phases[]={uint32_t(VisualPhase::SpecialDepth),0,1,2};
        for(uint32_t phase:phases){bool present=capture&&phase==0;for(uint32_t i=0;i<group.count;++i)present|=group.draws[i].phase==phase;if(!present)continue;
            if(snapshot&&phase==1){auto& p=NewPass();p.color=cr;p.snapshot=snapshot;p.copy=true;b.BeginPass(names[index][phase==1?4:5],GpuPassQueue::Direct,{&p,RecordCopy});texture(cr,GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);texture(snapshot->resource,GpuPassResourceAccess::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);b.EndPass();}
            auto& p=NewPass();p.group=&group;p.phase=phase;p.slot=slot;p.viewCount=frame.viewCount;p.width=width;p.height=height;p.camera=camera;p.color=cr;p.depth=dr;p.snapshot=snapshot;p.offscreen=capture!=nullptr;p.clear=capture&&(phase==uint32_t(VisualPhase::SpecialDepth)||(!hasHeightDepth&&phase==0));p.rtv=capture?capture->rtvs->GetCPUDescriptorHandleForHeapStart():color->rtv;p.dsv=capture?capture->dsvs->GetCPUDescriptorHandleForHeapStart():depth->dsv;
            b.BeginPass(names[index][phase==uint32_t(VisualPhase::SpecialDepth)?8:phase],GpuPassQueue::Direct,{&p,RecordDraws});if(phase!=uint32_t(VisualPhase::SpecialDepth)||p.clear)texture(cr,GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);texture(dr,GpuPassResourceAccess::ReadWrite,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
            for(auto* u:{&group.drawTable[slot],&resourceTable[slot],&lightTable[slot]})b.Buffer(u->lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(capture)b.Buffer(group.camera[slot].lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);
            const auto& resources=dependencies[capture?1:0][phase];
            if(resources.dynamicStream&&copyBytes)b.Buffer(dynamicSlots[slot].gpu->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_VERTEX_BUFFER);
            if(resources.sortedStream&&group.indexCount)b.Buffer(group.indices[slot].gpu->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_INDEX_INPUT,D3D12_BARRIER_ACCESS_INDEX_BUFFER);
            for(uint32_t id:resources.meshes){const auto& mesh=id==ConsoleSpec::MaxMeshes?orbMesh:meshes[id];b.Buffer(mesh.vb->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_VERTEX_SHADING,D3D12_BARRIER_ACCESS_VERTEX_BUFFER);b.Buffer(mesh.ib->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_INDEX_INPUT,D3D12_BARRIER_ACCESS_INDEX_BUFFER);}
            for(uint32_t id:resources.textures)texture(textures[id]->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(snapshot&&phase==1)texture(snapshot->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);b.EndPass();
        }
        uint32_t bloomOrdinal=0;
        for(uint32_t drawIndex=0;drawIndex<group.count;++drawIndex){const auto& draw=group.draws[drawIndex];if(draw.program!=VisualProgram::BloomComposite)continue;
            auto& bloom=capture?capture->bloom:mainBloom;EnsureBloom(cr,bloom);
            ID3D12Resource* sources[]={cr,bloom.a->resource,bloom.b->resource,bloom.a->resource};
            ID3D12Resource* targets[]={bloom.a->resource,bloom.b->resource,bloom.a->resource,cr};
            uint32_t descriptors[]={services->DescriptorIndex(capture?capture->color->srv:color->srv),services->DescriptorIndex(bloom.a->srv),services->DescriptorIndex(bloom.b->srv),services->DescriptorIndex(bloom.a->srv)};
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[]={bloom.rtvA->GetCPUDescriptorHandleForHeapStart(),bloom.rtvB->GetCPUDescriptorHandleForHeapStart(),bloom.rtvA->GetCPUDescriptorHandleForHeapStart(),capture?capture->rtvs->GetCPUDescriptorHandleForHeapStart():color->rtv};
            VisualProgram programs[]={VisualProgram::BloomExtract,VisualProgram::BloomBlur,VisualProgram::BloomBlur,VisualProgram::BloomComposite};
            for(uint32_t stage=0;stage<4;++stage){auto& p=NewPass();p.group=&group;p.slot=slot;p.camera=camera;p.viewCount=frame.viewCount;p.offscreen=capture!=nullptr;p.drawIndex=draw.data;p.bloomProgram=programs[stage];p.sourceDescriptor=descriptors[stage];p.bloomFlags=(sources[stage]->GetDesc().DepthOrArraySize==1?4u:0u)|(stage==2?8u:0u);p.width=stage==3?width:bloom.width;p.height=stage==3?height:bloom.height;p.rtv=rtvs[stage];
                b.BeginPass(bloomNames[index][bloomOrdinal][stage].data(),GpuPassQueue::Direct,{&p,RecordBloom});
                texture(sources[stage],GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                texture(targets[stage],stage==3?GpuPassResourceAccess::ReadWrite:GpuPassResourceAccess::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
                b.Buffer(group.drawTable[slot].lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                if(capture)b.Buffer(group.camera[slot].lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_CONSTANT_BUFFER);b.EndPass();
            }
            ++bloomOrdinal;
        }
        if(capture&&!capture->native){void* context{};capture->frameId=frame.frameId;capture->failed=false;capture->native=graphics.RequestTextureReadback(cr,0,frame.frameId,&context);capture->pending=false;if(!capture->native){capture->failed=true;return;}b.BeginPass(names[index][6],GpuPassQueue::Direct,{context,Graphics::RecordTextureReadback});texture(cr,GpuPassResourceAccess::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);b.EndPass();}
    }
};
VisualDomain::VisualDomain(Graphics& g):impl_(std::make_unique<Impl>(g)){}
VisualDomain::~VisualDomain(){Clear();}
void VisualDomain::Initialize(GpuDomainServices& services){auto& s=*impl_;if(s.services==&services)return;if(s.services)Fail("Visual GPU services cannot change");s.services=&services;Hr(services.Device()->QueryInterface(IID_PPV_ARGS(&s.device2)),"Visual D3D12 device2");s.Pipelines();}
void VisualDomain::Clear()noexcept{
    auto& s=*impl_;s.configured=false;s.needsSnapshot=false;s.batch={};s.passCount=0;
    for(auto& capture:s.captures){if(capture.native)s.graphics.ReleaseTextureReadback(capture.native);capture={};}
    for(auto& mesh:s.meshes)mesh={};for(auto& texture:s.textures)texture={};for(auto& material:s.materials)material={};s.materialValid={};s.textureDeclared={};s.meshDeclared={};
    for(auto& o:s.objects)o={};s.objectCount=0;s.streams={};s.effects={};s.lightCount=0;s.orbMesh={};s.mainSnapshot={};s.mainBloom={};
    for(auto& group:s.groups){group.count=group.indexCount=0;for(uint32_t slot=0;slot<ConsoleSpec::FrameSlots;++slot){
        s.ReleaseDynamicUpload(group.drawTable[slot]);s.ReleaseDynamicUpload(group.camera[slot]);
        s.ReleaseDynamicUpload(group.indices[slot].staging);s.ReleaseDynamic(group.indices[slot].gpu);group.indices[slot].indexCapacity=0;}}
    for(uint32_t slot=0;slot<ConsoleSpec::FrameSlots;++slot){s.ReleaseDynamicUpload(s.dynamicSlots[slot].staging);s.ReleaseDynamic(s.dynamicSlots[slot].gpu);s.dynamicSlots[slot].vertexCapacity=0;s.ReleaseDynamicUpload(s.resourceTable[slot]);s.ReleaseDynamicUpload(s.lightTable[slot]);}
    s.triangleScratch.clear();s.objectLookup.clear();s.objectLookupMask=0;s.lightObject.fill(Invalid);for(auto& view:s.dependencies)for(auto& phase:view){phase.meshes.clear();phase.textures.clear();phase.dynamicStream=phase.sortedStream=false;}s.activeGroupCount=s.drawCapacity=s.textureSpan=s.lightCapacity=0;s.staticBytes=s.dynamicBytes=0;
}
bool VisualDomain::HasPackage()const noexcept{return impl_->configured;}
SceneData VisualDomain::Configure(const std::filesystem::path& root,uint64_t generation,const LabVisualPackageV1& p){
    Clear();auto& s=*impl_;if(!s.services||p.version!=1||p.structSize<sizeof(p))Fail("Visual package ABI");
    if(p.textureCount>ConsoleSpec::MaxTextures||p.meshCount>ConsoleSpec::MaxMeshes||p.materialCount>ConsoleSpec::MaxMaterials||p.objectCount>ConsoleSpec::MaxObjects||p.lightCount>ConsoleSpec::MaxLights||p.streamCount>ConsoleSpec::MaxVisualStreams||p.effectCount>ConsoleSpec::MaxVisualEffects||p.captureCount>ConsoleSpec::MaxVisualCaptures)Fail("Visual package capacity");
    if((p.textureCount&&!p.textures)||(p.meshCount&&!p.meshes)||(p.materialCount&&!p.materials)||(p.objectCount&&!p.objects)||(p.lightCount&&!p.lights)||(p.streamCount&&!p.streams)||(p.effectCount&&!p.effects)||(p.captureCount&&!p.captures))Fail("Visual package arrays");
    s.activeGroupCount=1+p.captureCount;s.drawCapacity=std::max(1u,2*p.objectCount+p.streamCount+p.effectCount);s.textureSpan=1;s.lightCapacity=std::max(1u,p.lightCount);
    for(uint32_t i=0;i<p.textureCount;++i){if(p.textures[i].id>=ConsoleSpec::MaxTextures)Fail("Visual texture ID");s.textureSpan=std::max(s.textureSpan,p.textures[i].id+1);}
    s.generation=generation;SceneData scene=CreateClientAssetSet();scene.materials.clear();scene.textures.clear();scene.objects.clear();scene.lights.clear();
    std::array<uint32_t,ConsoleSpec::MaxTextures> textureRemap;textureRemap.fill(Invalid);
    std::array<bool,ConsoleSpec::MaxTextures> standardTexture{},specialTexture{};
    for(uint32_t i=0;i<p.materialCount;++i){const auto& m=p.materials[i];if(m.structSize<sizeof(m)||m.id>=ConsoleSpec::MaxMaterials||s.materialValid[m.id]||m.model>LABOVR_VISUAL_MATERIAL_EMISSIVE_V1)Fail("Invalid visual material");s.materials[m.id]=m;s.materialValid[m.id]=true;auto& out=s.materials[m.id];
        const float parameters[]={m.uvScale,m.normalScale,m.roughnessBias,m.heightScale,m.detailScale,m.detailNormalScale,m.wetness,m.emission,m.refractionStrength,m.thickness,m.tessFactor};for(float value:parameters)if(!Finite(value))Fail("Invalid visual material value");for(float value:m.tint)if(!Finite(value))Fail("Invalid visual tint");
        out.tessFactor=std::clamp(out.tessFactor,1.f,float(ConsoleSpec::MaxHeightTessellation));out.parallaxMaxSteps=std::clamp(out.parallaxMaxSteps,1u,ConsoleSpec::MaxHeightParallaxSteps);out.parallaxMinSteps=std::clamp(out.parallaxMinSteps,1u,out.parallaxMaxSteps);if(out.uvScale<=0)out.uvScale=1;
        auto& usage=m.model==LABOVR_VISUAL_MATERIAL_PBR_V1?standardTexture:specialTexture;
        for(auto id:{m.albedoTexture,m.normalTexture,m.ormTexture,m.heightTexture,m.detailTexture})if(id!=Invalid){if(id>=ConsoleSpec::MaxTextures)Fail("Visual material texture ID");usage[id]=true;}
        s.needsSnapshot|=m.model==LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1;
    }
    for(uint32_t i=0;i<p.textureCount;++i){const auto& t=p.textures[i];if(t.structSize<sizeof(t)||t.id>=ConsoleSpec::MaxTextures||s.textureDeclared[t.id]||t.use>LABOVR_VISUAL_TEXTURE_LINEAR_V1)Fail("Invalid visual texture");s.textureDeclared[t.id]=true;if(!standardTexture[t.id]&&!specialTexture[t.id])continue;auto cpu=LoadVisualImage(ResolveVisualAsset(root,t.pathUtf8),t.use);GpuTextureDesc d{};d.width=cpu.width;d.height=cpu.height;d.format=cpu.format;d.srv=true;d.uav=false;d.name=L"Visual.Texture";std::vector<GpuSubresourceUpload> uploads;d.mips=cpu.mips;
        for(uint32_t mip=0;mip<cpu.mips;++mip){uint32_t width=std::max(1u,cpu.width>>mip),height=std::max(1u,cpu.height>>mip);bool bc=cpu.format>=DXGI_FORMAT_BC1_TYPELESS&&cpu.format<=DXGI_FORMAT_BC5_SNORM||cpu.format>=DXGI_FORMAT_BC6H_TYPELESS&&cpu.format<=DXGI_FORMAT_BC7_UNORM_SRGB;uint32_t blockBytes=cpu.format>=DXGI_FORMAT_BC1_TYPELESS&&cpu.format<=DXGI_FORMAT_BC1_UNORM_SRGB||cpu.format>=DXGI_FORMAT_BC4_TYPELESS&&cpu.format<=DXGI_FORMAT_BC4_SNORM?8:16;uint64_t pitch=bc?uint64_t((width+3)/4)*blockBytes:uint64_t(width)*(cpu.format==DXGI_FORMAT_R16_UNORM?2:4);uint64_t slice=pitch*(bc?(height+3)/4:height);if(mip>=cpu.offsets.size()||cpu.offsets[mip]>cpu.bytes.size()||slice>cpu.bytes.size()-cpu.offsets[mip])Fail("Visual texture mip");uploads.push_back({cpu.bytes.data()+cpu.offsets[mip],pitch,slice});}
        if(specialTexture[t.id]){s.textures[t.id]=s.services->UploadStaticTexture2D(d,uploads);s.Charge(s.textures[t.id]);}if(standardTexture[t.id]){textureRemap[t.id]=uint32_t(scene.textures.size());scene.textures.push_back(std::move(cpu));}}
    for(uint32_t i=0;i<p.materialCount;++i){auto& m=s.materials[p.materials[i].id];for(auto id:{m.albedoTexture,m.normalTexture,m.ormTexture,m.heightTexture,m.detailTexture})if(id!=Invalid&&(id>=ConsoleSpec::MaxTextures||!s.textureDeclared[id]))Fail("Visual material missing texture");}
    scene.materials.resize(ConsoleSpec::MaxMaterials); // IDs remain stable for GPU compute publications.
    for(uint32_t i=0;i<p.materialCount;++i){const auto& m=s.materials[p.materials[i].id];auto& out=scene.materials[m.id];out.colorRoughness={m.tint[0],m.tint[1],m.tint[2],std::clamp(m.roughnessBias+.5f,.025f,1.f)};out.emissionMetallic={m.tint[0]*m.emission,m.tint[1]*m.emission,m.tint[2]*m.emission,0};out.alpha=m.tint[3];out.alphaMode=m.tint[3]<1||m.flags&AlphaFlag?2:0;out.doubleSided=1;out.unlit=(m.flags&4u)!=0;out.emissionMetallic.w=float(m.flags>>16)/65535.f;out.normalScale=m.normalScale;out.albedo=m.albedoTexture==Invalid?Invalid:textureRemap[m.albedoTexture];out.normal=m.normalTexture==Invalid?Invalid:textureRemap[m.normalTexture];out.metalRough=m.ormTexture==Invalid?Invalid:textureRemap[m.ormTexture];}
    // Keep only the declared ID span; the fixed maximum remains a capacity, not an active count.
    uint32_t materialSpan=0;for(uint32_t i=0;i<p.materialCount;++i)materialSpan=std::max(materialSpan,p.materials[i].id+1);scene.materials.resize(materialSpan);
    std::array<bool,ConsoleSpec::MaxMeshes> standardMesh{},specialMesh{};
    for(uint32_t i=0;i<p.objectCount;++i){const auto& object=p.objects[i];
        if(object.structSize<sizeof(object)||object.mesh>=ConsoleSpec::MaxMeshes||object.material>=ConsoleSpec::MaxMaterials||!s.materialValid[object.material]||object.updateMask&~7u||object.id==Invalid)Fail("Invalid visual object declaration");
        (s.materials[object.material].model==LABOVR_VISUAL_MATERIAL_PBR_V1?standardMesh:specialMesh)[object.mesh]=true;
    }
    for(uint32_t i=0;i<p.meshCount;++i){const auto& mesh=p.meshes[i];
        if(mesh.structSize<sizeof(mesh)||mesh.id>=ConsoleSpec::MaxMeshes||s.meshDeclared[mesh.id]||mesh.source>LABOVR_VISUAL_MESH_MEMORY_V1)Fail("Invalid visual mesh");
        s.meshDeclared[mesh.id]=true;if(!standardMesh[mesh.id]&&!specialMesh[mesh.id])continue;
        VisualMeshData cpu;
        if(mesh.source==LABOVR_VISUAL_MESH_MEMORY_V1){
            const auto* data=mesh.data;
            if(!data||data->structSize<sizeof(*data)||!data->vertexCount||!data->indexCount||data->indexCount%3||!data->vertices||!data->indices||
                uint64_t(data->vertexCount)*sizeof(VisualMeshVertex)+uint64_t(data->indexCount)*sizeof(uint32_t)>ConsoleSpec::MaxWorldStaticGpuBytes)Fail("Invalid visual memory mesh");
            cpu.vertices.reserve(data->vertexCount);cpu.indices.reserve(data->indexCount);
            for(uint32_t j=0;j<data->vertexCount;++j){const auto& v=data->vertices[j];
                if(!Finite(v.position.x)||!Finite(v.position.y)||!Finite(v.position.z)||!Finite(v.normal.x)||!Finite(v.normal.y)||!Finite(v.normal.z)||!Finite(v.tangent.x)||!Finite(v.tangent.y)||!Finite(v.tangent.z)||!Finite(v.uv[0])||!Finite(v.uv[1]))Fail("Nonfinite visual memory vertex");
                cpu.vertices.push_back({v.position.x,v.position.y,v.position.z,v.normal.x,v.normal.y,v.normal.z,v.tangent.x,v.tangent.y,v.tangent.z,v.uv[0],v.uv[1]});
            }
            for(uint32_t j=0;j<data->indexCount;++j){if(data->indices[j]>=data->vertexCount)Fail("Visual memory mesh index");cpu.indices.push_back(data->indices[j]);}
        }else cpu=mesh.source==LABOVR_VISUAL_MESH_FILE_V1?LoadVisualObj(ResolveVisualAsset(root,mesh.pathUtf8)):VisualPrimitive(mesh.source);
        if(specialMesh[mesh.id])s.meshes[mesh.id]=s.UploadMesh(cpu);
        if(standardMesh[mesh.id])s.meshes[mesh.id].sceneMesh=(mesh.source==LABOVR_VISUAL_MESH_FILE_V1||mesh.source==LABOVR_VISUAL_MESH_MEMORY_V1)?AppendVisualMesh(scene.meshes,cpu):mesh.source-1;
    }
    for(uint32_t i=0;i<p.objectCount;++i){const auto& in=p.objects[i];
        if(!s.meshDeclared[in.mesh]||in.deformMode>LABOVR_VISUAL_DEFORM_ORGANIC_V1||!in.visibilityMask||(in.visibilityMask&~3u))Fail("Invalid visual object");
        ValidatePose(in.pose,in.scale);
        bool standard=s.materials[in.material].model==LABOVR_VISUAL_MATERIAL_PBR_V1;
        if(standard&&in.updateMask==LABOVR_VISUAL_OBJECT_STATIC_V1){
            RenderObject object;object.previous=object.current=ToPose(in.pose);object.scale={in.scale.x,in.scale.y,in.scale.z};object.id=in.id;object.mesh=s.meshes[in.mesh].sceneMesh;object.material=in.material;
            const auto& material=s.materials[in.material];if(material.tint[3]<1||(material.flags&AlphaFlag))object.flags|=AlphaBlendObject;if(material.flags&LABOVR_VISUAL_MATERIAL_UNLIT_V1)object.flags|=NoShadow|NoRayTracing;
            scene.objects.push_back(object);
        }else{auto& object=s.objects[s.objectCount++];object.desc=in;object.state={sizeof(LabVisualObjectStateV1),in.id,1,0,in.pose,in.pose,in.scale};object.standard=standard;}
    }
    std::vector<LabU32> declaredIds;declaredIds.reserve(p.objectCount);for(uint32_t i=0;i<p.objectCount;++i)declaredIds.push_back(p.objects[i].id);std::sort(declaredIds.begin(),declaredIds.end());if(std::adjacent_find(declaredIds.begin(),declaredIds.end())!=declaredIds.end())Fail("Duplicate visual object ID");
    s.BuildObjectLookup();
    s.lightCount=p.lightCount;for(uint32_t i=0;i<p.lightCount;++i){auto l=p.lights[i];if(l.structSize<sizeof(l)||!Finite(l.power)||!Finite(l.radius)||l.radius<0||(l.radius==0&&l.power>0))Fail("Invalid visual light");s.lights[i]=l;s.lightObject[i]=s.FindObject(l.id);SceneLight out;out.position={l.position.x,l.position.y,l.position.z};out.radius=l.radius;out.color={l.color.x,l.color.y,l.color.z};out.power=l.power;out.direction={l.direction.x,l.direction.y,l.direction.z};out.outerCos=l.outerCos;out.innerCos=l.innerCos;out.id=l.id;out.flags=l.flags;scene.lights.push_back(out);}
    for(uint32_t i=0;i<p.streamCount;++i){const auto& in=p.streams[i];if(in.structSize<sizeof(in)||in.id>=ConsoleSpec::MaxVisualStreams||s.streams[in.id].valid||in.primitive>LABOVR_VISUAL_STREAM_POINTS_V1||in.material>=ConsoleSpec::MaxMaterials||!s.materialValid[in.material]||!in.capacity||in.capacity>ConsoleSpec::MaxDynamicVerticesPerFrame||in.sortMode>LABOVR_VISUAL_SORT_BACK_TO_FRONT_V1||(in.sortMode&&in.primitive!=LABOVR_VISUAL_STREAM_TRIANGLES_V1))Fail("Invalid visual stream");s.streams[in.id]={in,true};}
    bool orb=false;for(uint32_t i=0;i<p.effectCount;++i){const auto& in=p.effects[i];if(in.structSize<sizeof(in)||in.id>=ConsoleSpec::MaxVisualEffects||s.effects[in.id].valid||in.type>LABOVR_VISUAL_EFFECT_BLOOM_V1)Fail("Invalid visual effect");auto& e=s.effects[in.id];e.desc=in;e.valid=true;e.state={};e.state.structSize=sizeof(e.state);e.state.effect=in.id;e.state.enabled=1;e.state.radius=1;std::memcpy(e.state.values,in.values,sizeof(in.values));orb|=in.type==LABOVR_VISUAL_EFFECT_LIQUID_ORB_V1;}
    s.BuildDependencies();
    if(orb)s.orbMesh=s.UploadMesh(VisualPrimitive(LABOVR_VISUAL_MESH_SPHERE_V1));
    for(uint32_t i=0;i<p.captureCount;++i){const auto& in=p.captures[i];if(in.structSize<sizeof(in)||in.id>=ConsoleSpec::MaxVisualCaptures||s.captures[in.id].valid||!in.width||!in.height||in.width>ConsoleSpec::MaxCaptureWidth||in.height>ConsoleSpec::MaxCaptureHeight)Fail("Invalid visual capture");auto& c=s.captures[in.id];c.desc=in;c.valid=true;GpuTextureDesc d{};d.width=in.width;d.height=in.height;d.format=DXGI_FORMAT_R8G8B8A8_UNORM;d.flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;d.srv=true;d.uav=false;d.name=L"Visual.CaptureColor";c.color=s.services->CreateManagedTexture2D(d);s.Charge(c.color);d.format=DXGI_FORMAT_D32_FLOAT;d.flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;d.srv=false;d.name=L"Visual.CaptureDepth";c.depth=s.services->CreateManagedTexture2D(d);s.Charge(c.depth);D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.NumDescriptors=1;heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;Hr(s.services->Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&c.rtvs)),"Visual capture RTV");heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;Hr(s.services->Device()->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&c.dsvs)),"Visual capture DSV");s.services->Device()->CreateRenderTargetView(c.color->resource,nullptr,c.rtvs->GetCPUDescriptorHandleForHeapStart());s.services->Device()->CreateDepthStencilView(c.depth->resource,nullptr,c.dsvs->GetCPUDescriptorHandleForHeapStart());if(s.needsSnapshot)s.Snapshot(c.color->resource,c.snapshot);}
    for(uint32_t slot=0;slot<ConsoleSpec::FrameSlots;++slot){
        s.Allocate(s.resourceTable[slot],s.textureSpan*sizeof(XMUINT2),sizeof(XMUINT2),true,L"Visual.ResourceTable");
        auto* table=static_cast<XMUINT2*>(s.resourceTable[slot].mapped);for(uint32_t i=0;i<s.textureSpan;++i)table[i]=s.textures[i]?XMUINT2{s.services->DescriptorIndex(s.textures[i]->srv),s.services->DescriptorGeneration(s.textures[i]->srv)}:XMUINT2{Invalid,0};
        s.Allocate(s.lightTable[slot],s.lightCapacity*sizeof(VisualLightGpu),sizeof(VisualLightGpu),true,L"Visual.Lights");
        for(uint32_t i=0;i<s.activeGroupCount;++i){auto& group=s.groups[i];s.Allocate(group.drawTable[slot],s.drawCapacity*sizeof(VisualDrawGpu),sizeof(VisualDrawGpu),true,L"Visual.DrawTable");
            if(i>=1)s.Allocate(group.camera[slot],(sizeof(VisualCamera)+255ull)&~255ull,4,false,L"Visual.CaptureCamera");}
    }
    if(p.externalScenePathUtf8&&*p.externalScenePathUtf8){auto path=ResolveVisualAsset(root,p.externalScenePathUtf8);auto external=path.extension()==L".lvscene"?LoadCookedScene(path.wstring()):LoadSceneGLTF(path.wstring());AppendClientScene(scene,external);}
    if(scene.textures.size()>ConsoleSpec::MaxTextures||scene.materials.size()>ConsoleSpec::MaxMaterials||scene.meshes.ranges.size()>ConsoleSpec::MaxMeshes||scene.objects.size()+s.objectCount>ConsoleSpec::MaxObjects||scene.lights.size()>ConsoleSpec::MaxLights)Fail("Combined visual scene capacity");
    uint64_t mainBytes=scene.meshes.vertices.size()*sizeof(Vertex)+scene.meshes.indices.size()*4+scene.meshes.remap.size()*4+scene.meshes.meshlets.size()*sizeof(Meshlet)+scene.meshes.triangles.size()*12;for(const auto& texture:scene.textures)mainBytes+=texture.bytes.size();if(mainBytes>ConsoleSpec::MaxWorldStaticGpuBytes-s.staticBytes)Fail("Combined visual static GPU capacity");s.staticBytes+=mainBytes;
    s.configured=true;return scene;
}
void VisualDomain::BorrowBatch(const LabVisualBatchV1& batch,uint64_t frameId,const Pose& camera){auto& s=*impl_;if(!s.configured)return;s.batch=batch;s.frameId=frameId;s.mainCamera=camera;
    for(uint32_t i=0;i<batch.objectStateCount;++i){const auto& state=batch.objectStates[i];if(state.structSize<sizeof(state))Fail("Visual object state ABI");ValidatePose(state.current,state.scale);
        uint32_t index=s.FindObject(state.id);if(index==Invalid)Fail("Visual object state ID");auto& object=s.objects[index];if(!object.desc.updateMask)Fail("Immutable visual object state");
        if(object.desc.updateMask&LABOVR_VISUAL_OBJECT_TRANSFORM_V1){object.state.previous=state.previous;object.state.current=state.current;}
        if(object.desc.updateMask&LABOVR_VISUAL_OBJECT_SCALE_V1)object.state.scale=state.scale;
        if(object.desc.updateMask&LABOVR_VISUAL_OBJECT_VISIBILITY_V1)object.state.visible=state.visible;
    }
    std::array<bool,ConsoleSpec::MaxVisualStreams> streamSeen{};uint64_t total=0;
    for(uint32_t i=0;i<batch.streamCount;++i){const auto& in=batch.streams[i];if(in.structSize<sizeof(in)||in.stream>=ConsoleSpec::MaxVisualStreams||!s.streams[in.stream].valid||streamSeen[in.stream]||(in.vertexCount&&!in.vertices)||in.vertexCount>s.streams[in.stream].desc.capacity)Fail("Invalid visual stream frame");streamSeen[in.stream]=true;auto primitive=s.streams[in.stream].desc.primitive;if((primitive==LABOVR_VISUAL_STREAM_TRIANGLES_V1&&in.vertexCount%3)||(primitive==LABOVR_VISUAL_STREAM_LINES_V1&&in.vertexCount%2))Fail("Visual stream primitive alignment");total+=in.vertexCount;if(total>ConsoleSpec::MaxDynamicVerticesPerFrame)Fail("Visual dynamic vertex capacity");}
    for(uint32_t i=0;i<batch.effectCount;++i){const auto& in=batch.effects[i];if(in.structSize<sizeof(in)||in.effect>=ConsoleSpec::MaxVisualEffects||!s.effects[in.effect].valid||!Finite(in.radius))Fail("Invalid visual effect state");s.effects[in.effect].state=in;}
    std::array<bool,ConsoleSpec::MaxVisualCaptures> captureSeen{};for(uint32_t i=0;i<batch.captureCount;++i){const auto& in=batch.captures[i];if(in.structSize<sizeof(in)||in.capture>=ConsoleSpec::MaxVisualCaptures||!s.captures[in.capture].valid||captureSeen[in.capture]||!Finite(in.verticalFov)||in.verticalFov<=0||in.verticalFov>=XM_PI||!Finite(in.nearZ)||!Finite(in.farZ)||in.nearZ<=0||in.farZ<=in.nearZ)Fail("Invalid visual capture request");captureSeen[in.capture]=true;ValidatePose(in.pose,{1,1,1});if(in.enabled){s.captures[in.capture].request=in;s.captures[in.capture].pending=true;}}
}
void VisualDomain::AppendStandard(RenderScene& scene,uint32_t meshBase,uint32_t materialBase)const{
    const auto& s=*impl_;
    for(uint32_t i=0;i<s.objectCount;++i){
        const auto& o=s.objects[i];if(!o.standard||!o.state.visible||!(o.desc.visibilityMask&LABOVR_VISUAL_MAIN_V1))continue;
        RenderObject out;out.previous=ToPose(o.state.previous);out.current=ToPose(o.state.current);out.scale={o.state.scale.x,o.state.scale.y,o.state.scale.z};
        out.id=o.desc.id;out.mesh=meshBase+s.meshes[o.desc.mesh].sceneMesh;out.material=materialBase+o.desc.material;
        out.flags=0;
        if(o.desc.updateMask&(LABOVR_VISUAL_OBJECT_TRANSFORM_V1|LABOVR_VISUAL_OBJECT_SCALE_V1|LABOVR_VISUAL_OBJECT_VISIBILITY_V1))
            out.flags|=DynamicObject;
        const auto& material=s.materials[o.desc.material];
        if(material.tint[3]<1||(material.flags&AlphaFlag))out.flags|=AlphaBlendObject;
        if(material.flags&LABOVR_VISUAL_MATERIAL_UNLIT_V1)out.flags|=NoShadow|NoRayTracing;
        scene.objects.push_back(out);++scene.count;
    }
}
void VisualDomain::BuildPasses(GpuPassBuilder& b,const GpuFrameContext& f){auto& s=*impl_;s.passCount=0;if(!s.configured||!f.resources||f.uploadSlot>=ConsoleSpec::FrameSlots||f.viewCount>ConsoleSpec::MaxViews||s.frameId!=f.frameId)return;uint32_t slot=f.uploadSlot,totalVertices=0;s.copyBytes=0;
    for(uint32_t i=0;i<s.batch.streamCount;++i){uint32_t count=s.batch.streams[i].vertexCount;if(count>ConsoleSpec::MaxDynamicVerticesPerFrame-totalVertices)Fail("Visual dynamic vertex capacity");totalVertices+=count;}
    if(totalVertices)s.EnsureDynamicSlot(slot,totalVertices);
    uint32_t offset=0;for(uint32_t i=0;i<s.batch.streamCount;++i){const auto& in=s.batch.streams[i];s.streamOffsets[in.stream]=offset;if(in.vertexCount)std::memcpy(static_cast<LabVisualStreamVertexV1*>(s.dynamicSlots[slot].staging.mapped)+offset,in.vertices,size_t(in.vertexCount)*sizeof(LabVisualStreamVertexV1));offset+=in.vertexCount;}
    s.copyBytes=uint64_t(totalVertices)*sizeof(LabVisualStreamVertexV1);if(s.copyBytes){auto& pass=s.NewPass();pass.slot=slot;b.BeginPass("Visual.DynamicUpload",GpuPassQueue::Copy,{&pass,Impl::RecordCopy});b.Buffer(s.dynamicSlots[slot].staging.lease->resource,GpuPassResourceAccess::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);b.Buffer(s.dynamicSlots[slot].gpu->resource,GpuPassResourceAccess::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);b.EndPass();}
    auto* lights=static_cast<VisualLightGpu*>(s.lightTable[slot].mapped);for(uint32_t i=0;i<s.lightCount;++i){auto in=s.lights[i];if(s.lightObject[i]!=Invalid){const auto& object=s.objects[s.lightObject[i]];in.position=object.state.current.position;if(!object.state.visible)in.power=0;}for(uint32_t j=0;j<s.batch.lightModifierCount;++j){const auto& m=s.batch.lightModifiers[j];if(m.structSize>=sizeof(m)&&(in.flags&m.matchFlags)==m.matchFlags)in.power*=m.powerScale;}lights[i]={{in.position.x,in.position.y,in.position.z,in.radius},{in.color.x,in.color.y,in.color.z,in.power}};}
    // Declare every visual node into the Core plan before its execution batches compile.
    s.BuildGroup(b,f,0,nullptr);uint32_t group=1;for(auto& capture:s.captures)if(capture.valid&&capture.pending&&!capture.native)s.BuildGroup(b,f,group++,&capture.request);
}
LabU32 VisualDomain::PollCapture(LabVisualCaptureHandleV1 id,LabVisualCaptureResultV1* out){auto& s=*impl_;if(!out||out->structSize<sizeof(*out)||id>=s.captures.size()||!s.captures[id].valid)return 0;auto& c=s.captures[id];*out={};out->structSize=sizeof(*out);out->frameId=c.frameId;if(c.failed){out->status=LABOVR_VISUAL_CAPTURE_FAILED_V1;return 1;}if(!c.native)return 1;Graphics::TextureReadbackResult result{};out->status=s.graphics.PollTextureReadback(c.native,result);if(out->status==LABOVR_VISUAL_CAPTURE_READY_V1){out->width=result.width;out->height=result.height;out->rowPitch=result.rowPitch;out->tightRowBytes=result.tightRowBytes;out->byteCount=result.byteCount;out->bytes=result.bytes;}return 1;}
void VisualDomain::ReleaseCapture(LabVisualCaptureHandleV1 id){auto& s=*impl_;if(id>=s.captures.size()||!s.captures[id].valid)return;auto& c=s.captures[id];if(c.native)s.graphics.ReleaseTextureReadback(c.native);c.native=0;c.failed=c.pending=false;c.frameId=0;}
}
