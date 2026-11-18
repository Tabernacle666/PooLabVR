#include "GraphicsInternal.h"
#include <nvsdk_ngx_helpers.h>

namespace OASIS {
namespace {

D3D12_RESOURCE_BARRIER DlssTransition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,
                                      D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    return barrier;
}

}

void Graphics::Impl::ConfigureDlss(){
    dlss.available=false;

    const auto directory=cachePath.substr(0,cachePath.find_last_of(L'\\'));
    constexpr char projectId[]="edd8d4ef-b27f-4af2-a979-3937d63b8264";
    if(NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_Init_with_ProjectID(projectId,
       NVSDK_NGX_ENGINE_TYPE_CUSTOM,"1.0",directory.c_str(),device.Get())))return;
    dlss.initialized=true;

    int supported=0;
    if(NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&dlss.parameters))||
       NVSDK_NGX_FAILED(NVSDK_NGX_Parameter_GetI(dlss.parameters,
           NVSDK_NGX_Parameter_SuperSampling_Available,&supported))||!supported){
        ResetDlss();
        return;
    }

    constexpr auto quality=NVSDK_NGX_PerfQuality_Value_MaxQuality;
    float sharpness=0;
    if(NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS(dlss.parameters,width,height,quality,
       &dlss.optimal.x,&dlss.optimal.y,&dlss.maximum.x,&dlss.maximum.y,
       &dlss.minimum.x,&dlss.minimum.y,&sharpness))||
       !dlss.optimal.x||!dlss.optimal.y){
        ResetDlss();
        return;
    }

    if(!dlss.minimum.x||!dlss.minimum.y||!dlss.maximum.x||!dlss.maximum.y)
        dlss.minimum=dlss.maximum=dlss.optimal;
    dlss.maximum.x=std::min(dlss.maximum.x,width);
    dlss.maximum.y=std::min(dlss.maximum.y,height);
    dlss.minScale=std::max(float(dlss.minimum.x)/float(width),
                          float(dlss.minimum.y)/float(height));
    dlss.maxScale=std::min({float(dlss.maximum.x)/float(width),
                           float(dlss.maximum.y)/float(height),0.99f});
    if(dlss.minimum.x>dlss.maximum.x||dlss.minimum.y>dlss.maximum.y){
        ResetDlss();
        return;
    }
    if(dlss.minScale>dlss.maxScale){
        if(dlss.minimum.x!=dlss.maximum.x||dlss.minimum.y!=dlss.maximum.y||
           dlss.optimal.x>=width||dlss.optimal.y>=height){
            ResetDlss();
            return;
        }
        dlss.minScale=dlss.maxScale=float(dlss.optimal.x)/float(width);
    }

    const auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);
    auto makeTexture=[&](ComPtr<ID3D12Resource>& texture,UINT w,UINT h,DXGI_FORMAT format,
                         D3D12_RESOURCE_STATES state){
        const auto desc=Texture(w,h,format,1,1,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        return SUCCEEDED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            state,nullptr,IID_PPV_ARGS(&texture)));
    };
    constexpr std::array formats{DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16G16_FLOAT};
    for(auto& eye:dlss.eyes){
        for(size_t input=0;input<formats.size();++input){
            if(!makeTexture(eye.inputs[input],dlss.maximum.x,dlss.maximum.y,formats[input],
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)){
                ResetDlss();
                return;
            }
            eye.inputUavs[input]=TextureUav(eye.inputs[input].Get(),formats[input]);
        }
        if(!makeTexture(eye.output,width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS)){
            ResetDlss();
            return;
        }
        eye.outputSrv=TextureSrv(eye.output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT);
    }

    NVSDK_NGX_DLSS_Create_Params parameters{};
    parameters.Feature.InWidth=dlss.optimal.x;
    parameters.Feature.InHeight=dlss.optimal.y;
    parameters.Feature.InTargetWidth=width;
    parameters.Feature.InTargetHeight=height;
    parameters.Feature.InPerfQualityValue=quality;
    parameters.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_DepthInverted|
        NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;

    startup.Reset();
    bool created=true;
    for(auto& eye:dlss.eyes){
        if(NVSDK_NGX_FAILED(NGX_D3D12_CREATE_DLSS_EXT(startup.list.Get(),1u,1u,
           &eye.feature,dlss.parameters,&parameters))){
            created=false;
            break;
        }
    }
    startup.Submit(direct);
    WaitStartup(direct);
    if(!created){
        ResetDlss();
        return;
    }
    if(!dlssPreparePSO){
        const auto code=Shader(L"PrepareDlssInputs");
        ComputePipeline pipeline{};
        pipeline.root.data=root.Get();
        pipeline.cs.data={code.data(),code.size()};
        const D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(pipeline),&pipeline};
        dlssPreparePSO=Pipeline(L"PrepareDlssInputs",desc,Hash(code));
    }
    dlss.available=true;
}

void Graphics::Impl::ResetDlss(){
    if(!dlss.initialized){
        dlss.available=false;
        return;
    }
    for(auto& frame:frames)frame.dlssEvaluation={};
    auto releaseResult=NVSDK_NGX_Result_Success;
    for(auto& eye:dlss.eyes){
        if(eye.feature){
            const auto result=NVSDK_NGX_D3D12_ReleaseFeature(eye.feature);
            if(NVSDK_NGX_FAILED(result))releaseResult=result;
        }
        for(auto& descriptor:eye.inputUavs)
            if(descriptor.Valid())ReleaseDescriptor(descriptor);
        if(eye.outputSrv.Valid())ReleaseDescriptor(eye.outputSrv);
    }
    if(dlss.parameters){
        const auto result=NVSDK_NGX_D3D12_DestroyParameters(dlss.parameters);
        if(NVSDK_NGX_FAILED(result))releaseResult=result;
    }
    const auto shutdownResult=NVSDK_NGX_D3D12_Shutdown1(device.Get());
    if(dlssDiagnosticEnabled){
        dlssDiagnostic.releaseResult=uint32_t(releaseResult);
        dlssDiagnostic.shutdownResult=uint32_t(shutdownResult);
    }
    dlss={};
}

bool Graphics::Impl::PrepareDlss(){
    if(!dlss.available||MsaaSamples!=1u)return false;
    if(!dlss.initialized)ConfigureDlss();
    return dlss.available&&dlss.initialized;
}

bool Graphics::Impl::SetDlssScale(float scale){
    if(scale==0.f){
        dlss.requested=0.f;
        return true;
    }
    if(!std::isfinite(scale)||scale<0.f||scale>=1.f||!PrepareDlss())return false;
    dlss.requested=std::clamp(scale,dlss.minScale,dlss.maxScale);
    return true;
}

void Graphics::Impl::UpdateDlss(int direction){
    if(direction&&dlss.available&&MsaaSamples==1u){
        if(direction<0&&!dlss.initialized)ConfigureDlss();
        if(dlss.available){
            constexpr float step=0.05f;
            if(dlss.requested==0.f){
                if(direction<0)dlss.requested=std::clamp(float(dlss.optimal.x)/float(width),
                                                        dlss.minScale,dlss.maxScale);
            }else if(direction>0&&dlss.requested>=dlss.maxScale){
                dlss.requested=0.f;
            }else{
                dlss.requested=std::clamp(dlss.requested+(direction>0?step:-step),
                                          dlss.minScale,dlss.maxScale);
            }
        }
    }
    if(dlss.initialized&&dlss.requested==0.f&&
       direct.fence->GetCompletedValue()>=dlss.lastUseFence){
        const bool available=dlss.available;
        ResetDlss();
        dlss.available=available;
    }
}

void Graphics::Impl::RecordDlssInputs(ID3D12GraphicsCommandList7* list,Frame& frame){
    constexpr size_t eyeCount=std::tuple_size_v<decltype(DlssState::eyes)>;
    constexpr size_t inputsPerEye=std::tuple_size_v<decltype(DlssEye::inputs)>;
    std::array<D3D12_RESOURCE_BARRIER,eyeCount*inputsPerEye> barriers{};
    size_t index=0;
    for(const auto& eye:dlss.eyes){
        for(const auto& input:eye.inputs){
            barriers[index++]=DlssTransition(input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }
    list->ResourceBarrier(UINT(barriers.size()),barriers.data());

    Bind(list,frame,false);
    list->SetPipelineState(dlssPreparePSO.Get());
    const auto groupsX=(renderWidth+7u)/8u;
    const auto groupsY=(renderHeight+7u)/8u;
    for(uint32_t eye=0;eye<dlss.eyes.size();++eye){
        const auto& inputUavs=dlss.eyes[eye].inputUavs;
        const uint32_t inputs[]{eye,Physical(inputUavs[0]),Physical(inputUavs[1]),Physical(inputUavs[2])};
        list->SetComputeRoot32BitConstants(1,4,inputs,0);
        list->Dispatch(groupsX,groupsY,1u);
    }

    for(auto& barrier:barriers)
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    list->ResourceBarrier(UINT(barriers.size()),barriers.data());
}

bool Graphics::Impl::EvaluateDlss(ID3D12GraphicsCommandList7* list,const Constants& c,bool reset){
    bool ready=true;
    for(auto& eye:dlss.eyes){
        NVSDK_NGX_D3D12_DLSS_Eval_Params parameters{};
        parameters.Feature.pInColor=eye.inputs[0].Get();
        parameters.Feature.pInOutput=eye.output.Get();
        parameters.pInDepth=eye.inputs[1].Get();
        parameters.pInMotionVectors=eye.inputs[2].Get();
        parameters.InJitterOffsetX=c.dlssJitter.x;
        parameters.InJitterOffsetY=c.dlssJitter.y;
        parameters.InRenderSubrectDimensions={renderWidth,renderHeight};
        parameters.InReset=reset?1:0;
        parameters.InPreExposure=1.f;
        parameters.InExposureScale=1.f;
        ready&=NVSDK_NGX_SUCCEED(NGX_D3D12_EVALUATE_DLSS_EXT(list,eye.feature,
                                                           dlss.parameters,&parameters));
    }
    return ready;
}

void Graphics::Impl::TransitionDlssOutputs(ID3D12GraphicsCommandList7* list,D3D12_RESOURCE_STATES before,
                                           D3D12_RESOURCE_STATES after){
    std::array<D3D12_RESOURCE_BARRIER,std::tuple_size_v<decltype(DlssState::eyes)>> barriers{};
    for(size_t eye=0;eye<dlss.eyes.size();++eye)
        barriers[eye]=DlssTransition(dlss.eyes[eye].output.Get(),before,after);
    list->ResourceBarrier(UINT(barriers.size()),barriers.data());
}

}
