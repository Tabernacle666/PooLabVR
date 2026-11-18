#include "GraphicsInternal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace OASIS {
namespace {

enum class OrbitSurface : uint32_t { Metal, Housing, Display, Glass };

struct OrbitVertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    XMFLOAT4 colorRoughness;
    XMFLOAT4 emissionMetallic;
    uint32_t surface{};
};
static_assert(sizeof(OrbitVertex)==68);

struct alignas(256) OrbitConstants {
    XMFLOAT4X4 viewProjection[MaxViews];
    XMFLOAT4 eye[MaxViews];
    XMFLOAT4 metrics;
    XMFLOAT4 display;
    XMFLOAT4 needleDirections;
};
static_assert(MaxViews==4);

XMFLOAT2 NeedleDirection(float percent){
    constexpr float pi=3.14159265f;
    const auto value=std::clamp(percent,0.f,1000.f);
    const auto angle=pi+pi*std::log2(1.f+.08f*value)/std::log2(81.f);
    return {std::cos(angle),std::sin(angle)};
}

OrbitSurface SurfaceFor(std::string_view name){
    if(name=="Orbit Housing")return OrbitSurface::Housing;
    if(name=="Orbit Display")return OrbitSurface::Display;
    if(name=="Orbit Glass")return OrbitSurface::Glass;
    return OrbitSurface::Metal;
}

}

void Graphics::Impl::ConfigurePhosphorOrbit(){
    if(outputSurface.desktop)return;
    const auto asset=LoadSceneGLTF(AssetPath(L"phosphor_orbit.glb"));
    std::vector<OrbitVertex> vertices(asset.meshes.vertices.size());
    std::vector<uint32_t> indices,glassIndices;
    indices.reserve(asset.meshes.indices.size());
    for(const auto& object:asset.objects){
        const auto& range=asset.meshes.ranges[object.mesh];
        const auto& material=asset.materials[object.material];
        const auto surface=SurfaceFor(asset.materialNames[object.material]);
        for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;++i){
            const auto& vertex=asset.meshes.vertices[i];
            vertices[i]={vertex.position,vertex.normal,vertex.uv,material.colorRoughness,
                         material.emissionMetallic,uint32_t(surface)};
        }
        auto& target=surface==OrbitSurface::Glass?glassIndices:indices;
        target.insert(target.end(),asset.meshes.indices.begin()+range.firstIndex,
                      asset.meshes.indices.begin()+range.firstIndex+range.indexCount);
    }
    if(indices.empty()||glassIndices.empty())Fail("Phosphor Orbit geometry");
    phosphorOrbit.opaqueIndexCount=uint32_t(indices.size());
    phosphorOrbit.glassIndexCount=uint32_t(glassIndices.size());
    indices.insert(indices.end(),glassIndices.begin(),glassIndices.end());

    phosphorOrbit.vertices=UploadVector(vertices);
    phosphorOrbit.indices=UploadVector(indices);
    phosphorOrbit.vertexSrv=Srv(phosphorOrbit.vertices.Get(),uint32_t(vertices.size()),sizeof(OrbitVertex));
    phosphorOrbit.indexSrv=Srv(phosphorOrbit.indices.Get(),uint32_t(indices.size()),sizeof(uint32_t));

    const auto vs=Shader(L"PhosphorOrbitVertex"),ps=Shader(L"PhosphorOrbitPixel");
    std::array<D3D12_VIEW_INSTANCE_LOCATION,MaxViews> views{};
    for(uint32_t view=0;view<viewCount;++view)views[view]={0,view};
    PresentPipeline pipeline{};
    pipeline.root.data=root.Get();
    pipeline.vs.data={vs.data(),vs.size()};
    pipeline.ps.data={ps.data(),ps.size()};
    pipeline.mask.data=UINT_MAX;
    pipeline.raster.data.FillMode=D3D12_FILL_MODE_SOLID;
    pipeline.raster.data.CullMode=D3D12_CULL_MODE_NONE;
    pipeline.raster.data.DepthClipEnable=TRUE;
    pipeline.depth.data.DepthEnable=TRUE;
    pipeline.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    pipeline.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    pipeline.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.formats.data.NumRenderTargets=1;
    pipeline.formats.data.RTFormats[0]=colorFormat;
    pipeline.depthFormat.data=DXGI_FORMAT_D32_FLOAT;
    pipeline.samples.data={1,0};
    pipeline.views.data={viewCount,views.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};
    auto& blend=pipeline.blend.data.RenderTarget[0];
    blend.SrcBlend=D3D12_BLEND_ONE;
    blend.DestBlend=D3D12_BLEND_ZERO;
    blend.BlendOp=D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlendAlpha=D3D12_BLEND_ZERO;
    blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    const D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(pipeline),&pipeline};
    const auto hash=Hash(ps,Hash(vs));
    phosphorOrbit.opaquePSO=Pipeline(L"phosphor_orbit",stream,hash);
    pipeline.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
    blend.BlendEnable=TRUE;
    blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
    blend.SrcBlendAlpha=D3D12_BLEND_ZERO;
    blend.DestBlendAlpha=D3D12_BLEND_ONE;
    phosphorOrbit.glassPSO=Pipeline(L"phosphor_orbit_glass",stream,hash);
    BakePhosphorOrbitPlate();
}

void Graphics::Impl::BakePhosphorOrbitPlate(){
    constexpr UINT plateWidth=1200,plateHeight=612;
    constexpr auto format=DXGI_FORMAT_R16G16_FLOAT;
    auto& plate=phosphorOrbit.plate;
    const auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc=Texture(plateWidth,plateHeight,format,1,1,1,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&plate.resource)),"Phosphor Orbit plate");
    phosphorOrbit.plateSrv=TextureSrv(plate.resource.Get(),format);
    phosphorOrbit.plateUav=TextureUav(plate.resource.Get(),format);

    const auto cs=Shader(L"PhosphorOrbitPlate");
    ComputePipeline pipeline{};
    pipeline.root.data=root.Get();
    pipeline.cs.data={cs.data(),cs.size()};
    const D3D12_PIPELINE_STATE_STREAM_DESC stream{sizeof(pipeline),&pipeline};
    phosphorOrbit.platePSO=Pipeline(L"phosphor_orbit_plate",stream,Hash(cs));

    auto* list=startup.list.Get();
    StartupTransition(list,plate,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                      D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
    ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
    list->SetDescriptorHeaps(2,heaps);
    list->SetComputeRootSignature(root.Get());
    const uint32_t draws[]{0u,0u,Physical(phosphorOrbit.plateUav)};
    list->SetComputeRoot32BitConstants(1,3,draws,0);
    list->SetPipelineState(phosphorOrbit.platePSO.Get());
    list->Dispatch((plateWidth+7)/8,(plateHeight+7)/8,1);
    StartupTransition(list,plate,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                      D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING);
}

void Graphics::Impl::ResetPhosphorOrbit(){
    passGraph.Forget(phosphorOrbit.vertices.Get());
    passGraph.Forget(phosphorOrbit.indices.Get());
    if(phosphorOrbit.vertexSrv.Valid())ReleaseDescriptor(phosphorOrbit.vertexSrv);
    if(phosphorOrbit.indexSrv.Valid())ReleaseDescriptor(phosphorOrbit.indexSrv);
    if(phosphorOrbit.plateSrv.Valid())ReleaseDescriptor(phosphorOrbit.plateSrv);
    if(phosphorOrbit.plateUav.Valid())ReleaseDescriptor(phosphorOrbit.plateUav);
    phosphorOrbit={};
}

D3D12_GPU_VIRTUAL_ADDRESS Graphics::Impl::PreparePhosphorOrbit(Frame& frame,const XRFrame& xr){
    if(!developerMode||!phosphorOrbit.grip.tracked||!phosphorOrbit.opaquePSO||!xr.tracked)return 0;
    const auto head=Scale(Add(xr.eyes.front(),xr.eyes[xr.viewCount-1]),.5f);
    const auto wrist=Transform({0.f,.031f,-.072f},phosphorOrbit.grip.pose);
    const auto fromHead=Sub(wrist,head);
    if(Length(fromHead)<.14f)return 0;
    const auto normal=Unit(fromHead);
    const auto headUp=Sub(Transform({0.f,1.f,0.f},xr.input.head),xr.input.head.p);
    const auto z=XMLoadFloat3(&normal);
    const auto right=XMVector3Cross(XMLoadFloat3(&headUp),z);
    if(XMVectorGetX(XMVector3LengthSq(right))<.0025f)return 0;
    const auto x=XMVector3Normalize(right);
    const auto y=XMVector3Cross(z,x);
    const auto position=Add(wrist,Scale(normal,.018f));
    constexpr float metersPerUnit=.034f;
    const XMMATRIX model(XMVectorScale(x,metersPerUnit),
                         XMVectorScale(y,metersPerUnit),
                         XMVectorScale(z,metersPerUnit),
                         XMVectorSet(position.x,position.y,position.z,1.f));
    const auto local=XMMatrixInverse(nullptr,model);
    OrbitConstants constants{};
    for(uint32_t view=0;view<xr.viewCount;++view){
        XMStoreFloat4x4(&constants.viewProjection[view],model*XMLoadFloat4x4(&xr.vp[view]));
        XMStoreFloat4(&constants.eye[view],XMVector3TransformCoord(XMLoadFloat3(&xr.eyes[view]),local));
    }
    constants.metrics=frame.constants->hud;
    const auto& metrics=constants.metrics;
    const auto targetHz=1.e9f/float(xr.displayPeriod);
    const auto renderPercent=metrics.x>=0.f?100.f*metrics.x/targetHz:-1000.f;
    const auto margin=metrics.y>0.f&&metrics.x>0.f?metrics.z:-1000.f;
    const auto renderDirection=NeedleDirection(renderPercent);
    const auto marginDirection=NeedleDirection(margin);
    constants.display={renderPercent,100.f*float(renderWidth)/float(width),margin,0.f};
    constants.needleDirections={renderDirection.x,renderDirection.y,marginDirection.x,marginDirection.y};
    const auto offset=Reserve(frame,sizeof(constants),256);
    std::memcpy(frame.uploadGeneration->mapped+offset,&constants,sizeof(constants));
    return frame.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
}

void Graphics::Impl::DrawPhosphorOrbit(ID3D12GraphicsCommandList7* list,D3D12_GPU_VIRTUAL_ADDRESS constants){
    const uint32_t draws[]{Physical(phosphorOrbit.vertexSrv),Physical(phosphorOrbit.indexSrv),
                           Physical(phosphorOrbit.plateSrv)};
    list->SetGraphicsRootConstantBufferView(0,constants);
    list->SetGraphicsRoot32BitConstants(1,3,draws,0);
    list->SetPipelineState(phosphorOrbit.opaquePSO.Get());
    list->DrawInstanced(phosphorOrbit.opaqueIndexCount,1,0,0);
    list->SetPipelineState(phosphorOrbit.glassPSO.Get());
    list->DrawInstanced(phosphorOrbit.glassIndexCount,1,phosphorOrbit.opaqueIndexCount,0);
}

}
