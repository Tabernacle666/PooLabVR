#include "GraphicsInternal.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace OASIS {
namespace {

enum class ViewModelSurface : uint32_t { Metal, Housing, Display, Glass, Fabric, BasaltHousing, Detail };

struct OrbitVertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    XMFLOAT4 colorRoughness;
    XMFLOAT4 emissionMetallic;
    uint32_t surface{},part{};
};
static_assert(sizeof(OrbitVertex)==72);

struct alignas(256) OrbitConstants {
    XMFLOAT4X4 viewProjection[MaxViews];
    XMFLOAT4 eye[MaxViews];
    XMFLOAT4 metrics;
    XMFLOAT4 needleDirections;
    XMUINT4 controls;
    XMUINT4 features;
    XMFLOAT4 knobCenters;
    XMFLOAT4 knobDirections;
    XMFLOAT4 travelAndContext;
    XMFLOAT4 marks;
    XMFLOAT4 pointer;
    std::array<XMUINT4,6> buildingText;
    std::array<XMFLOAT4,8> jointWire;
};
static_assert(MaxViews==4&&sizeof(OrbitConstants)==768);

XMFLOAT2 NeedleDirection(float percent){
    constexpr float pi=3.14159265f;
    const auto value=std::clamp(percent,0.f,1000.f);
    const auto angle=pi+pi*std::log2(1.f+.08f*value)/std::log2(81.f);
    return {std::cos(angle),std::sin(angle)};
}

ViewModelSurface SurfaceFor(std::string_view name){
    if(name=="Orbit Housing")return ViewModelSurface::Housing;
    if(name=="Orbit Display")return ViewModelSurface::Display;
    if(name=="Orbit Glass")return ViewModelSurface::Glass;
    if(name=="Glove Fabric")return ViewModelSurface::Fabric;
    if(name=="Basalt Olive"||name=="Scoria Graphite")return ViewModelSurface::BasaltHousing;
    if(name.starts_with("Glove ")||name.starts_with("Basalt ")||name.starts_with("Scoria "))
        return ViewModelSurface::Detail;
    return ViewModelSurface::Metal;
}

uint32_t OrbitPartFor(std::string_view name){
    if(name=="Raised set key"||name=="Set engraving")return uint32_t(OrbitControl::Set);
    if(name=="Accent cycle switch")return uint32_t(OrbitControl::Color);
    if(name.starts_with("Accent lamp ")&&name.back()>='1'&&name.back()<='5')
        return uint32_t(OrbitControl::Set)+uint32_t(name.back()-'0');
    if(name.ends_with(" knurl")||name.ends_with(" inlay")||
       name.ends_with(" cap")||name.ends_with(" index")){
        if(name.starts_with("Select "))return uint32_t(OrbitControl::Select);
        if(name.starts_with("Adjust "))return uint32_t(OrbitControl::Adjust);
    }
    return 0u;
}

void AppendViewModel(const SceneData& asset,std::vector<OrbitVertex>& vertices,
                     std::vector<uint32_t>& opaque,std::vector<uint32_t>* glass=nullptr){
    const auto vertexBase=uint32_t(vertices.size());
    vertices.resize(vertices.size()+asset.meshes.vertices.size());
    for(const auto& object:asset.objects){
        const auto& range=asset.meshes.ranges[object.mesh];
        const auto& material=asset.materials[object.material];
        const auto surface=SurfaceFor(asset.materialNames[object.material]);
        const uint32_t part=asset.meshNodeNames.empty()?0u:OrbitPartFor(asset.meshNodeNames[object.mesh]);
        for(uint32_t i=range.firstVertex;i<range.firstVertex+range.vertexCount;++i){
            const auto& vertex=asset.meshes.vertices[i];
            vertices[vertexBase+i]={vertex.position,vertex.normal,vertex.uv,material.colorRoughness,
                                    material.emissionMetallic,uint32_t(surface),part};
        }
        auto& target=glass&&surface==ViewModelSurface::Glass?*glass:opaque;
        for(uint32_t i=range.firstIndex;i<range.firstIndex+range.indexCount;++i)
            target.push_back(vertexBase+asset.meshes.indices[i]);
    }
}

}

void Graphics::Impl::ConfigurePhosphorOrbit(){
    if(outputSurface.desktop)return;
    std::vector<OrbitVertex> vertices;
    std::vector<uint32_t> indices,glassIndices;
    AppendViewModel(LoadSceneGLTF(AssetPath(L"phosphor_orbit.glb"),true),vertices,indices,&glassIndices);
    if(indices.empty()||glassIndices.empty())Fail("Phosphor Orbit geometry");
    phosphorOrbit.opaqueIndexCount=uint32_t(indices.size());
    phosphorOrbit.glassIndexCount=uint32_t(glassIndices.size());
    indices.insert(indices.end(),glassIndices.begin(),glassIndices.end());

    auto append=[&](const wchar_t* name,ViewModelRange& model){
        model.firstIndex=uint32_t(indices.size());
        AppendViewModel(LoadSceneGLTF(AssetPath(name)),vertices,indices);
        model.indexCount=uint32_t(indices.size())-model.firstIndex;
        if(!model.indexCount)Fail("VR view model geometry missing");
    };
    append(L"glove_left.glb",phosphorOrbit.leftGlove);
    append(L"glove_right.glb",phosphorOrbit.rightGlove);
    append(L"basalt_equipped.glb",phosphorOrbit.basalt);
    append(L"scoria_equipped.glb",phosphorOrbit.scoria);

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

Graphics::Impl::VrViewModelDraws Graphics::Impl::PrepareVrViewModels(Frame& frame,const XRFrame& xr){
    VrViewModelDraws result{};
    if(!phosphorOrbit.opaquePSO||!xr.tracked)return result;
    auto write=[&](const XMMATRIX& model,OrbitConstants& constants){
        const auto local=XMMatrixInverse(nullptr,model);
        for(uint32_t view=0;view<xr.viewCount;++view){
            XMStoreFloat4x4(&constants.viewProjection[view],model*XMLoadFloat4x4(&xr.vp[view]));
            XMStoreFloat4(&constants.eye[view],
                XMVector3TransformCoord(XMLoadFloat3(&xr.eyes[view]),local));
        }
        const auto offset=Reserve(frame,sizeof(constants),256);
        std::memcpy(frame.uploadGeneration->mapped+offset,&constants,sizeof(constants));
        return frame.uploadGeneration->resource->GetGPUVirtualAddress()+offset;
    };
    OrbitConstants constants{};
    const auto& left=xr.glovePoses[0];
    const auto& right=xr.glovePoses[1];
    const float units=xr.input.sceneUnitsPerMeter;
    const auto modelScale=XMMatrixScaling(units,units,units);
    if(left)result.left=write(modelScale*Matrix(*left),constants);
    if(right){
        result.rightTool=xr.toolExit?xr.rightTool:RightHandTool::Empty;
        result.right=write(modelScale*Matrix(xr.toolExit?*xr.toolExit:*right),constants);
    }
    if(!left)return result;

    const auto& hud=frame.constants->hud;
    const auto renderPercent=hud.x>=0.f?100.f*hud.x/xr.headsetTargetHz:-1000.f;
    const auto margin=hud.z;
    const auto renderDirection=NeedleDirection(renderPercent);
    const auto marginDirection=NeedleDirection(margin);
    const auto& display=xr.orbit;
    constants.metrics={hud.x,renderPercent,margin,0.f};
    constants.needleDirections={renderDirection.x,renderDirection.y,marginDirection.x,marginDirection.y};
    constants.controls={uint32_t(display.hover),uint32_t(display.locked),display.color,display.pointer?1u:0u};
    constants.features={display.rows,uint32_t(display.selected),uint32_t(display.lastTouched),
                        (display.rangeKnown?1u:0u)|(display.buildingPage?2u:0u)|(display.plasterPage?4u:0u)|(display.jointPreview?8u:0u)};
    const auto selectCenter=OrbitControlZones[0].center,adjustCenter=OrbitControlZones[1].center;
    constants.knobCenters={selectCenter.x,selectCenter.y,adjustCenter.x,adjustCenter.y};
    constants.knobDirections={std::cos(display.selectAngle),-std::sin(display.selectAngle),
                              std::cos(display.adjustAngle),-std::sin(display.adjustAngle)};
    constants.travelAndContext={display.colorTravel,display.setTravel,0.f,0.f};
    if(display.lastTouched==OrbitFeature::FogDensity){
        // Reference transmission at unit density weighting in the fog model.
        const auto density=frame.constants->atmosphereColor.w;
        constants.travelAndContext.z=100.f*std::exp(-density);
        constants.travelAndContext.w=100.f*std::exp(-5.f*density);
    }else if(display.lastTouched==OrbitFeature::DlssScale){
        constants.travelAndContext.z=float(renderWidth);
        constants.travelAndContext.w=float(renderHeight);
    }
    constants.marks={display.liveMark,display.factoryMark,display.userMark,display.value};
    if(display.pointer)constants.pointer=*display.pointer;
    constants.buildingText=display.buildingText;
    constants.jointWire=display.jointWire;
    result.orbit=write(PhosphorOrbitModel(*left,units),constants);
    return result;
}

void Graphics::Impl::DrawVrViewModels(ID3D12GraphicsCommandList7* list,const VrViewModelDraws& models){
    const uint32_t draws[]{Physical(phosphorOrbit.vertexSrv),Physical(phosphorOrbit.indexSrv),
                           Physical(phosphorOrbit.plateSrv)};
    list->SetGraphicsRoot32BitConstants(1,3,draws,0);
    list->SetPipelineState(phosphorOrbit.opaquePSO.Get());
    auto draw=[&](D3D12_GPU_VIRTUAL_ADDRESS constants,ViewModelRange model){
        if(!constants)return;
        list->SetGraphicsRootConstantBufferView(0,constants);
        // SV_VertexID starts at zero for a non-indexed draw. The shader
        // applies the range offset before reading the shared index buffer.
        list->SetGraphicsRoot32BitConstant(1,model.firstIndex,3);
        list->DrawInstanced(model.indexCount,1,0,0);
    };
    draw(models.left,phosphorOrbit.leftGlove);
    const auto& right=models.rightTool==RightHandTool::Basalt?phosphorOrbit.basalt:
        models.rightTool==RightHandTool::Scoria?phosphorOrbit.scoria:phosphorOrbit.rightGlove;
    draw(models.right,right);
    if(!models.orbit)return;
    draw(models.orbit,{0,phosphorOrbit.opaqueIndexCount});
    list->SetPipelineState(phosphorOrbit.glassPSO.Get());
    draw(models.orbit,{phosphorOrbit.opaqueIndexCount,phosphorOrbit.glassIndexCount});
}

}
