Warning: truncated output (original token count: 39207)
Total output lines: 2067

FILE:shaders/present.hlsl
#include "common.hlsli"
#include "projectile_overlay.hlsli"
struct Out {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint view:TEXCOORD1;};
Out Fullscreen(uint id:SV_VertexID,uint view:SV_ViewID){Out o;o.uv=float2((id<<1)&2,id&2);o.position=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);o.view=view;return o;}
float3 ACES(float3 x){return saturate((x*(2.51*x+.03))/(x*(2.43*x+.59)+.14));}

uint2 HudGlyph(uint c){
    switch(c){
    case 48u:return uint2(0x239ace2eu,0x0eu);
    case 49u:return uint2(0x08421184u,0x0eu);
    case 50u:return uint2(0x1041062eu,0x1fu);
    case 51u:return uint2(0x0217043eu,0x1eu);
    case 52u:return uint2(0x05f928c2u,0x02u);
    case 53u:return uint2(0x021f421fu,0x1eu);
    case 54u:return uint2(0x231f420eu,0x0eu);
    case 55u:return uint2(0x1082083fu,0x08u);
    case 56u:return uint2(0x2317462eu,0x0eu);
    case 57u:return uint2(0x0217c62eu,0x0eu);
    case 65u:return uint2(0x231fc62eu,0x11u);
    case 66u:return uint2(0x231f463eu,0x1eu);
    case 67u:return uint2(0x2108420fu,0x0fu);
    case 68u:return uint2(0x2318c63eu,0x1eu);
    case 69u:return uint2(0x210f421fu,0x1fu);
    case 70u:return uint2(0x210f421fu,0x10u);
    case 71u:return uint2(0x231bc20fu,0x0fu);
    case 73u:return uint2(0x0842109fu,0x1fu);
    case 77u:return uint2(0x231ad771u,0x11u);
    case 78u:return uint2(0x2319d731u,0x11u);
    case 79u:return uint2(0x2318c62eu,0x0eu);
    case 80u:return uint2(0x210f463eu,0x10u);
    case 82u:return uint2(0x254f463eu,0x11u);
    case 83u:return uint2(0x0217420fu,0x1eu);
    case 84u:return uint2(0x0842109fu,0x04u);
    case 85u:return uint2(0x2318c631u,0x0eu);
    case 86u:return uint2(0x1518c631u,0x04u);
    case 37u:return uint2(0x26b21359u,0x0u);
    case 46u:return uint2(0x0c000000u,0x06u);
    case 47u:return uint2(0x10820841u,0x10u);
    case 45u:return uint2(0x000f8000u,0x00u);
    default:return uint2(0u,0u);
    }
}

uint HudWord(uint2 word,uint index){
    if(index>=8u)return 32u;
    uint packed=index<4u?word.x:word.y;
    uint c=(packed>>((index&3u)*8u))&255u;
    return c==0u?32u:c;
}

uint HudNumber(float value,uint index,bool wide){
    uint width=wide?6u:5u;
    if(index>=width)return 32u;
    if(!(value>=0.0))return index==width-3u||index==width-2u?45u:32u;
    uint n=(uint)min(wide?99999.0:9999.0,floor(value*10.0+0.5));
    uint p=wide?index:index+1u;
    if(p==0u)return n>=10000u?48u+n/10000u:32u;
    if(p==1u)return n>=1000u?48u+(n/1000u)%10u:32u;
    if(p==2u)return n>=100u?48u+(n/100u)%10u:32u;
    if(p==3u)return 48u+(n/10u)%10u;
    if(p==4u)return 46u;
    return 48u+n%10u;
}

float HudGlyphCoverage(uint c,uint2 pixel){
    if(pixel.x>=5u||pixel.y>=7u)return 0.0;
    uint2 glyph=HudGlyph(c);
    uint row=pixel.y==6u?glyph.y:(glyph.x>>(pixel.y*5u))&31u;
    return float((row>>(4u-pixel.x))&1u);
}

float HudBox(float2 p,float2 size,float radius){
    float2 d=abs(p-size*0.5)-(size*0.5-radius);
    return length(max(d,0.0))+min(max(d.x,d.y),0.0)-radius;
}

float3 HudOverlay(float3 scene,float2 pixel){
    // First wrist-console pass.  The display is deliberately self-contained in
    // the existing present pass.  Controller bindings and audio remain unused.
    float scale=max(min(float(OutputWidth)/560.0,float(OutputHeight)/360.0),1.0);
    // Temporary wrist anchor until controller poses are wired.  It deliberately
    // lives in the lower-left peripheral zone instead of the legacy center HUD.
    float2 p=(pixel-float2(OutputWidth*0.08,OutputHeight*0.58))/scale;
    float panelHeight=156.0;
    [branch]if(any(p<float2(-1.0,-1.0))||any(p>float2(301.0,panelHeight+1.0)))return scene;
    float distance=HudBox(p,float2(300.0,panelHeight),10.0);
    float aa=0.7/scale;
    [branch]if(distance>aa)return scene;
    float panel=1.0-smoothstep(-aa,aa,distance);
    float border=panel-(1.0-smoothstep(-aa,aa,distance+0.35));
    float3 result=lerp(scene,float3(0.004,0.016,0.012),panel*0.9);
    result=lerp(result,float3(0.10,0.48,0.23),saturate(border)*0.85);
    // Outer military housing, recessed glass and two physical control wells.
    float housing=1.0-smoothstep(0.0,1.0,abs(HudBox(p+float2(0.0,2.0),float2(294.0,150.0),8.0)));
    result+=float3(0.025,0.09,0.04)*housing*panel;
    float2 knobA=p-float2(22.0,43.0),knobB=p-float2(22.0,112.0);
    float ka=1.0-smoothstep(7.0,9.0,length(knobA)),kb=1.0-smoothstep(7.0,9.0,length(knobB));
    result+=float3(0.20,0.72,0.30)*(ka+kb)*panel;
    float2 gauge=p-float2(211.0,70.0);float gr=length(gauge);
    float ring=1.0-smoothstep(0.9,2.1,abs(gr-48.0));
    result+=float3(0.18,0.86,0.38)*ring*panel;
    // Exponential-style tachometer arc and the two live needles.
    float angle=atan2(gauge.y,gauge.x),arc=smoothstep(-2.45,-2.40,angle)*(1.0-smoothstep(2.35,2.40,angle));
    result+=float3(0.18,0.75,0.32)*arc*(1.0-smoothstep(44.0,49.0,gr))*panel;
    float greenRatio=saturate(hud.x/max(hud.y,1.0)),orangeRatio=saturate(hud.z/100.0);
    float2 ng=float2(cos(2.45-greenRatio*4.9),sin(2.45-greenRatio*4.9));
    float2 no=float2(cos(2.45-orangeRatio*4.9),sin(2.45-orangeRatio*4.9));
    float needleG=(1.0-smoothstep(1.0,2.5,abs(gauge.x*ng.y-gauge.y*ng.x)))*(1.0-smoothstep(4.0,52.0,-dot(gauge,ng)));
    float needleO=(1.0-smoothstep(1.0,2.5,abs(gauge.x*no.y-gauge.y*no.x)))*(1.0-smoothstep(5.0,51.0,-dot(gauge,no)));
    result+=float3(0.34,1.0,0.42)*needleG*panel+float3(1.0,0.42,0.06)*needleO*panel;
    // Recessed radio band and the pale set button.
    float band=1.0-smoothstep(0.7,1.8,abs(p.y-139.0))*smoothstep(18.0,28.0,p.x)*smoothstep(285.0,275.0,p.x);
    result+=float3(0.08,0.45,0.20)*band*panel;
    float2 sb=p-float2(278.0,105.0);float sbutton=1.0-smoothstep(9.0,11.0,length(sb));
    result+=float3(0.72,0.55,0.12)*sbutton*panel;

    int2 gridPos=int2(floor(p));
    int textRow=-1;
    if(gridPos.y>=10&&gridPos.y<17)textRow=0;
    else if(gridPos.y>=34&&gridPos.y<41)textRow=1;
    else if(gridPos.y>=49&&gridPos.y<56)textRow=2;
    else if(gridPos.y>=64&&gridPos.y<71)textRow=3;
    else if(gridPos.y>=79&&gridPos.y<86)textRow=4;
    if(textRow<0)return result;
    int row=textRow==0?gridPos.y-10:gridPos.y-(textRow*15+19);
    int start=textRow==0?38:31;
    if(gridPos.x<start)return result;
    int x=gridPos.x-start;
    uint index=(uint)(x/6);
    uint column=(uint)(x%6);
    uint character=32u;
    bool label=true;
    if(textRow==0)character=HudWord(uint2(0x4953414fu,0x00000053u),index);
    else if(textRow==1&&index<6u)character=HudWord(uint2(0x00535046u,0u),index);
    else if(textRow==2&&index<6u)character=HudWord(uint2(0x00545345u,0u),index);
    else if(textRow==3&&index<6u)character=HudWord(uint2(0x4752414du,0x00000045u),index);
    else if(textRow==4&&index<6u)character=HudWord(uint2(0x53534c44u,0u),index);
    else{
        label=false;
        if(textRow==1)character=HudNumber(hud.x,index-6u,true);
        else if(textRow==2)character=HudNumber(hud.y,index-6u,true);
        else if(textRow==4){
            uint c=index-6u;
            if(param2==0xffffffffu)character=HudWord(uint2(0x00412f4eu,0u),c);
            else if(param2==0u)character=HudWord(uint2(0x0046464fu,0u),c);
            else if(c<5u)character=HudNumber(float(param2)*.1f,c,false);
            else if(c==5u)character=37u;
        }
        else if(textRow==3){
            uint c=index-6u;
            if(hud.y<0.0)character=HudNumber(-1.0,c,true);
            else if(hud.z<0.0){
                if(c==0u)character=45u;
                else if(c<7u)character=HudNumber(-hud.z,c-1u,true);
                else if(c==7u)character=37u;
            }else{
                if(c<6u)character=HudNumber(hud.z,c,true);
                else if(c==6u)character=37u;
            }
        }
    }
    float ink=HudGlyphCoverage(character,uint2(column,(uint)row));
    float3 color=textRow==0?float3(0.48,0.95,0.88):(label?float3(0.24,0.58,0.57):float3(0.83,0.92,0.93));
    return lerp(result,color,ink*panel);
}

float AtmosphereShadow(float3 p){
    [branch]if(atmosphereParams.x<0)return 1.0;
    uint slot=(uint)atmosphereParams.x;
    float3 d=p-atmosphereLight.xyz,ad=abs(d);
    uint face=ad.x>=ad.y&&ad.x>=ad.z?(d.x>=0?0:1):ad.y>=ad.z?(d.y>=0?2:3):(d.z>=0?4:5);
    float4 clip=mul(float4(p,1),shadowVP[slot*6+face]);
    if(clip.w<=0)return 1.0;
    float3 ndc=clip.xyz/clip.w;
    if(ndc.z<0||ndc.z>1||any(abs(ndc.xy)>1))return 1.0;
    float2 uv=ndc.xy*float2(.5,-.5)+.5,tile=float2((slot%2)*3+face%3,(slot/2)*2+face/3);
    Texture2D<float> atlas=ResourceDescriptorHeap[ShadowAtlas];
    SamplerComparisonState comparison=SamplerDescriptorHeap[1];
    return atlas.SampleCmpLevelZero(comparison,(clamp(uv,.5/512,1-.5/512)+tile)/float2(6,4),ndc.z+.0006);
}

float3 ApplyAtmosphere(float3 scene,float2 uv,float depth,uint view){
    [branch]if(atmosphereSphere.w<=0||atmosphereColor.w<=0)return scene;
    float3 origin=eye[view].xyz;
    float3 ray=normalize(Unproject(uv,0.0001,view)-origin);
    float3 relative=origin-atmosphereSphere.xyz;
    float b=dot(relative,ray),discriminant=b*b-dot(relative,relative)+atmosphereSphere.w*atmosphereSphere.w;
    [branch]if(discriminant<=0)return scene;
    float chord=sqrt(discriminant),first=max(0.0,-b-chord),last=-b+chord;
    if(depth>0)last=min(last,length(Unproject(uv,depth,view)-origin));
    [branch]if(last<=first)return scene;
    float3 samplePosition=origin+ray*((first+last)*.5);
    float ground=exp(-max(0.0,samplePosition.y-atmosphereSphere.y+atmosphereSphere.w*.25)*.65);
    float cloud=exp(-abs(samplePosition.y-atmosphereSphere.y-atmosphereSphere.w*.35)*1.1);
    float drift=.88+.12*sin(dot(samplePosition,float3(.83,.23,.61))+timing.z*.16);
    float opacity=1-exp(-(last-first)*atmosphereColor.w*(ground+.3*cloud)*drift);
    float3 toLight=atmosphereLight.xyz-samplePosition;
    float distanceSquared=dot(toLight,toLight);
    float3 eyeToLight=atmosphereLight.xyz-origin;
    float phase=.22+.78*pow(saturate(dot(ray,eyeToLight*rsqrt(max(dot(eyeToLight,eyeToLight),.000001)))),4);
    float illumination=atmosphereEmission.w*AtmosphereShadow(samplePosition)*phase/(1+distanceSquared);
    float3 haze=atmosphereColor.rgb*.035+atmosphereEmission.rgb*illumination*.08;
    return scene*(1-opacity)+haze*opacity;
}

struct Present {float4 color:SV_Target0;float depth:SV_Depth;};
Present FinishPresent(Out i,float3 color,float2 uv,float z){
    float3 mapped=ACES(ApplyAtmosphere(color,uv,z,i.view));
    [branch]if(hud.w>0.5f)mapped=HudOverlay(mapped,i.position.xy);
    Present o;
    o.color=float4(mapped,1);
    o.depth=z;
    return o;
}

Present Tonemap(Out i){
    const uint2 xy=min(uint2(i.position.xy*float2(Width,Height)/float2(outputDimensions.xy)),
        uint2(Width-1,Height-1));
    float z=0.f;
    if(topology.y>1){
        Texture2DMSArray<float,4> depth=ResourceDescriptorHeap[DepthMS];
        [unroll]for(uint k=0;k<4;k++)z=max(z,depth.Load(int3(xy,i.view),k));
    }else{
        Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
        z=depth.Load(int4(xy,i.view,0));
    }
    Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
    float3 color=resolved.Load(int4(xy,i.view,0)).rgb;
    if(topology.z!=0&&param1==0){
        Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
        color+=reflection.Load(int4(xy,i.view,0)).rgb;
    }
    return FinishPresent(i,color,i.uv,z);
}

#ifndef OASIS_DLSS_FRAME
Present TonemapDlss(Out i){
    const uint2 extent=uint2(Width,Height);
    const float2 activePixel=i.position.xy*float2(extent)/float2(outputDimensions.xy)
        +dlssJitter.xy;
    const uint2 xy=uint2(clamp(activePixel,0.0,float2(extent)-1.0));
    Texture2DArray<float> depth=ResourceDescriptorHeap[DepthMS];
    const float z=depth.Load(int4(xy,i.view,0));

    float3 color;
    [branch]if(param2!=0u&&param2!=0xffffffffu){
        Texture2D<float4> upscaled=ResourceDescriptorHeap[NonUniformResourceIndex(dlssOutput[i.view])];
        color=upscaled.Load(int3(uint2(i.position.xy),0)).rgb;
    }else{
        Texture2DArray<float4> resolved=ResourceDescriptorHeap[ColorResolved];
        SamplerState linearSampler=SamplerDescriptorHeap[2];
        float2 samplePixel=clamp(activePixel,.5f,float2(extent)-.5f);
        color=resolved.SampleLevel(linearSampler,
            float3(samplePixel/float2(outputDimensions.xy),i.view),0.f).rgb;
        if(topology.z!=0&&param1==0){
            Texture2DArray<float4> reflection=ResourceDescriptorHeap[HistoryCurrent];
            color+=reflection.Load(int4(xy,i.view,0)).rgb;
        }
    }

    [branch]if(dlssOutput.z!=0xffffffffu){
        Texture2DArray<uint> marks=ResourceDescriptorHeap[dlssOutput.z];
        const uint mark=marks.Load(int4(uint2(i.position.xy),i.view,0));
        color=BlendProjectile(color,mark);
    }
    return FinishPresent(i,color,i.uv+dlssJitter.xy/float2(extent),z);
}
#endif

FILE:src/core/GraphicsRender.cpp
#include "GraphicsInternal.h"
#include <cmath>
#include <cstring>

namespace OASIS {
namespace {
float Halton(uint32_t index,uint32_t base){
    float sample=0.f,weight=1.f;
    for(;index;index/=base){
        weight/=float(base);
        sample+=weight*float(index%base);
    }
    return sample;
}
}
    void Graphics::Impl::Render(XRFrame& xr,const GpuProducerCoordinator& coordinator){
if(!active)return;
        const bool measureRender=developerMode||dlssDiagnosticEnabled;
        const int64_t cpuRenderStart=measureRender?QpcCounter():0;
        if(measureRender){
            if(!timerInitialized){
                timerInitialized=true;
                D3D12_FEATURE_DATA_D3D12_OPTIONS3 options{};
                const bool copyTiming=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options,sizeof(options)))&&options.CopyQueueTimestampQueriesSupported;
                std::array<QueueState*,GraphQueueCount> queues{&copy,&compute,&direct};
                for(uint32_t q=0;q<GraphQueueCount;++q){
                    if(q==QI(GraphQueue::Copy)&&!copyTiming)continue;
                    auto& t=timerQueues[q];
                    if(FAILED(queues[q]->queue->GetTimestampFrequency(&t.frequency))||!t.frequency)continue;
                    D3D12_QUERY_HEAP_DESC heap{};
                    heap.Type=q==QI(GraphQueue::Copy)?D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                    heap.Count=2u*GpuBatchTimer::Slots*GpuBatchTimer::MaxBatches;
                    if(FAILED(device->CreateQueryHeap(&heap,IID_PPV_ARGS(&t.queries))))continue;
                    D3D12_HEAP_PROPERTIES properties{};properties.Type=D3D12_HEAP_TYPE_READBACK;
                    D3D12_RESOURCE_DESC buffer{};
                    buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
                    buffer.Width=UINT64(heap.Count)*sizeof(UINT64);
                    buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
                    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    if(FAILED(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&buffer,
                        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&t.readback))))t.queries.Reset();
                }
            }
            std::array<QueueState*,GraphQueueCount> queues{&copy,&compute,&direct};
            for(auto& sample:timerSamples){
                if(!sample.pending)continue;
                bool ready=true;
                for(uint32_t q=0;q<GraphQueueCount;++q)
                    if(sample.counts[q]&&queues[q]->fence->GetCompletedValue()<sample.fences[q])ready=false;
                if(!ready)continue;
                bool valid=true;double gpuMs=0.;
                const bool diagnosticSample=dlssDiagnosticEnabled&&
                    sample.diagnosticFrame>dlssDiagnostic.gpuFrame;
                std::array<float,GraphQueueCount> queueMs{},peakMs{};
                std::array<uint32_t,GraphQueueCount> peakBatch{};
                float dlssGpuMs=-1.f;
                for(uint32_t q=0;q<GraphQueueCount;++q){
                    if(!sample.counts[q])continue;
                    auto& t=timerQueues[q];
                    const SIZE_T begin=SIZE_T(sample.slot)*GpuBatchTimer::MaxBatches*2u*sizeof(UINT64);
                    const D3D12_RANGE range{begin,begin+SIZE_T(sample.counts[q])*2u*sizeof(UINT64)};
                    void* mapped=nullptr;
                    if(FAILED(t.readback->Map(0,&range,&mapped))||!mapped){valid=false;continue;}
                    const auto* ticks=static_cast<const UINT64*>(mapped)+sample.slot*GpuBatchTimer::MaxBatches*2u;
                    UINT64 elapsed=0,peakTicks=0;
                    for(uint32_t n=0;n<sample.counts[q];++n){
                        if(ticks[n*2u+1]<ticks[n*2u]){valid=false;break;}
                        const UINT64 duration=ticks[n*2u+1]-ticks[n*2u];
                        elapsed+=duration;
                        if(diagnosticSample){
                            if(duration>peakTicks){
                                peakTicks=duration;
                                peakBatch[q]=n;
                            }
                            if(q==QI(GraphQueue::Direct)&&n==sample.dlssBatch)
                                dlssGpuMs=float(1000.0*double(duration)/double(t.frequency));
                        }
                    }
                    D3D12_RANGE empty{0,0};t.readback->Unmap(0,&empty);
                    const double elapsedMs=1000.0*double(elapsed)/double(t.frequency);
                    gpuMs=std::max(gpuMs,elapsedMs);
                    if(diagnosticSample){
                        queueMs[q]=float(elapsedMs);
                        peakMs[q]=float(1000.0*double(peakTicks)/double(t.frequency));
                    }
                }
                if(valid){
                    if(diagnosticSample){
                        dlssDiagnostic.gpuFrame=sample.diagnosticFrame;
                        dlssDiagnostic.gpuScale=sample.dlssScale;
                        const auto graphicsQueue=QI(GraphQueue::Direct),computeQueue=QI(GraphQueue::Compute);
                        dlssDiagnostic.directMs=queueMs[graphicsQueue];
                        dlssDiagnostic.computeMs=queueMs[computeQueue];
                        dlssDiagnostic.directPeakMs=peakMs[graphicsQueue];
                        dlssDiagnostic.computePeakMs=peakMs[computeQueue];
                        dlssDiagnostic.directPeakBatch=sample.counts[graphicsQueue]?
                            sample.batchNames[graphicsQueue][peakBatch[graphicsQueue]]:std::array<char,64>{};
                        dlssDiagnostic.computePeakBatch=sample.counts[computeQueue]?
                            sample.batchNames[computeQueue][peakBatch[computeQueue]]:std::array<char,64>{};
                        dlssDiagnostic.dlssGpuMs=dlssGpuMs;
                        dlssDiagnostic.dlssCpuMs=sample.dlssCpuMs;
                    }
                    const float ms=std::max(sample.cpuMs,float(gpuMs));
                    if(ms>0&&std::isfinite(ms))potentialFrameMs=potentialFrameMs>0?
                        potentialFrameMs*.8f+ms*.2f:ms;
                }
                sample.pending=false;
            }
        }
        if(xr.viewCount!=viewCount||!xr.viewCount||xr.viewCount>MaxViews)Fail("XR view topology changed after Graphics configuration");
        if(xr.color>=xrRTV.size()||xr.depth>=xrDSV.size())Fail("Output image index out of range");
        publicationCatalog.Clear();publications.clear();descriptorAllocator.BeginRecording();managedMemory.BeginFrame();CollectManaged();residency.Begin();vramCache.RequireActive();for(auto* domain:domainRegistry)domain->BeginFrame(xr.input.frameId);
        auto endDomains=[&]{for(auto* domain:domainRegistry)domain->EndFrame();managedMemory.EndFrame(SubmittedFences());descriptorAllocator.EndRecording(SubmittedFences());};
        for(auto* domain:domainRegistry){auto publication=domain->Publish();if(!publication)continue;if(publication.groups.empty()||publication.domain!=domain->Identity()||!domain->ValidatePublication(publication))Fail("GPU domain rejected its own publication");DescribePublication(publication);publications.push_back({nullptr,domain,std::move(publication)});domain->BindPublication(publications.back().publication);}
        auto releasePublications=[&](bool immediateSignal){
            for(auto& bound:publications){
                if(!bound.stream)continue;auto& publication=bound.publication;
                if(immediateSignal&&publication.consumerFence&&publication.consumerValue){
                    if(bound.deferred){
                        if(!coordinator.finishFallback)Fail("Deferred GPU producer cancellation without worker join");
                        coordinator.finishFallback();
                    }

                    ReturnUnusedPublication(publication);
                }
                bound.stream->Release(publication);bound.stream=nullptr;bound.publication={};
            }
        };
        for(auto* stream:streams){
            auto* domain=FindDomain(stream->Domain());if(!domain)Fail("GPU stream domain is not registered");
            auto reserved=std::find_if(reservedStreams.begin(),reservedStreams.end(),[&](const auto& r){return r.stream==stream;});
            GpuPublication publication;
            const bool deferred=reserved!=reservedStreams.end();
            if(deferred){publication=std::move(reserved->publication);reserved->stream=nullptr;}
            else {


                if(coordinator.finishFallback)coordinator.finishFallback();
                publication=stream->Acquire();
            }


            if(!publication){continue;}
            if(publication.groups.empty())Fail("GPU publication missing resource groups");if(publication.domain!=stream->Domain())Fail("GPU publication changed stream domain");if(!publication.producerFence||!publication.producerValue||!publication.consumerFence||!publication.consumerValue)Fail("GPU stream publication missing producer/consumer fence");if(!domain->ValidatePublication(publication))Fail("GPU publication rejected by domain contract");
            DescribePublication(publication);publications.push_back({stream,domain,std::move(publication),deferred});domain->BindPublication(publications.back().publication);
        }
        auto& f=*active;
        const bool coreSceneWork=f.cpuCount!=0||!meshDomain->Bindings().empty();
        f.instanceCount=meshDomain->TotalInstances();
        if(coreSceneWork) {
            EnsureInstances(f,sceneCache.StaticCount()+f.snapshotCount,f.cpuCount,f.instanceCount);
            EnsureLightBuffers(f);
            EnsureLightBvhBuffers(f);
        }
        const GpuSceneCache* tileCatalog=contributions.residentTiles?
            contributions.residentTiles(contributions.context):nullptr;
        const auto* tiles=tileCatalog?&tileCatalog->ResidentTiles():nullptr;
        auto residentBatches=PrepareResidentBatches(f,tileCatalog,coreSceneWork);
        auto& tileBatchDispatch=residentBatches.tileBatchDispatch;
        auto& opaqueRuns=residentBatches.opaqueRuns;
        const uint32_t tileBatchBlocks=residentBatches.tileBatchBlocks;
        EnsureStreamControl(f,uint32_t(meshDomain->Bindings().size()));uint32_t frameSlot=uint32_t(&f-frames.data());auto pendingStatic=sceneCache.Pending(frameSlot);bool copyStatic=!pendingStatic.empty();
        auto rayFrame=PrepareRayScene(f,tileCatalog,frameSlot);
        auto& rayTileAcks=rayFrame.acks;
        const auto rayCpuDirty=rayFrame.cpuDirty;
        const auto rayPublicationDirty=rayFrame.publicationDirty;
        const bool rayDirty=rayFrame.dirty;
        if(coreSceneWork||!publications.empty()){
            if(!PrepareResidency(xr,f,coreSceneWork)){releasePublications(true);publications.clear();endDomains();return;}
        }else{
            std::fill(visibleMaterialBits.begin(),visibleMaterialBits.end(),0);requiredResidencyPages.clear();
        }
        const bool trackingChanged=xr.input.trackingRevision!=lastTrackingRevision;
        const bool displayGap=lastDisplayTime&&xr.displayTime-lastDisplayTime>xr.displayPeriod*2;
        if(trackingChanged||displayGap)historyReady=false;
        lastTrackingRevision=xr.input.trackingRevision;
        lastDisplayTime=xr.displayTime;

        const bool dlssUsable=dlss.available&&MsaaSamples==1u&&xr.viewCount==2u&&
            coreSceneWork&&f.instanceCount==f.cpuCount&&f.snapshotCount==0u&&
            f.residentCount==0u&&dynamicMeshes.empty()&&staticTransparent.empty();
        const bool useDlss=dlssUsable&&dlss.requested>0.f;
        const uint32_t nextWidth=useDlss?std::clamp(uint32_t(std::lround(float(width)*dlss.requested)),
                                                   dlss.minimum.x,dlss.maximum.x):width;
        const uint32_t nextHeight=useDlss?std::clamp(uint32_t(std::lround(float(height)*dlss.requested)),
                                                    dlss.minimum.y,dlss.maximum.y):height;
        if(renderWidth!=nextWidth||renderHeight!=nextHeight)historyReady=false;
        renderWidth=nextWidth;
        renderHeight=nextHeight;
        renderMips=std::bit_width(std::max(renderWidth,renderHeight));
        const bool resetDlssHistory=trackingChanged||!dlss.historyReady||
            dlss.lastRenderSize.x!=renderWidth||dlss.lastRenderSize.y!=renderHeight;
        constexpr uint32_t quality=3u;
        uint32_t output=uint32_t(frameNumber&1),previous=1-output;
        auto* shadingRateImage=coreSceneWork&&xr.tracked&&xr.viewCount==2u&&MsaaSamples==1?
            vrsImage.resource.Get():nullptr;
        const bool refreshVrs=shadingRateImage&&(!vrsImageReady||
            vrsWidth!=renderWidth||vrsHeight!=renderHeight||
            std::memcmp(vrsFov.data(),xr.fov.data(),sizeof(vrsFov))!=0);
        auto bindVrs=[shadingRateImage](ID3D12GraphicsCommandList7* l){
            if(!shadingRateImage)return;
            constexpr D3D12_SHADING_RATE_COMBINER combiners[]{
                D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                D3D12_SHADING_RATE_COMBINER_OVERRIDE};
            l->RSSetShadingRate(D3D12_SHADING_RATE_1X1,combiners);
            l->RSSetShadingRateImage(shadingRateImage);
        };
        uint32_t staticCount=sceneCache.StaticCount();f.transparentCount=0;bool hasReflections=false;
        if(coreSceneWork){
            for(uint32_t index:staticTransparent)f.transparent[f.transparentCount++]=index;hasReflections=staticReflectionCandidates[quality];
            for(uint32_t i=0;i<f.dynamicCount;i++){const auto& object=f.objects[i];
                if(i<f.snapshotCount&&(object.flags&AlphaBlendObject))
                    f.transparent[f.transparentCount++]=staticCount+i;
                if(!(object.flags&128u)&&!hasReflections&&object.material<surfaceData.size())
                    hasReflections=SurfaceMayReflect(surfaceData[object.material],quality);
            }
            if(!hasReflections&&tileCatalog)
                for(const auto& [key,tile]:tileCatalog->ResidentTiles()){
                    for(const auto& [material,count]:tile.materialCounts){
                        if(!count)continue;
                        if(material>=surfaceData.size())
                            Fail("Resident tile reflection material out of range");
                        if(SurfaceMayReflect(surfaceData[material],quality)){
                            hasReflections=true;break;
                        }
                    }
                    if(hasReflections)break;
                }
            if(!hasReflections)for(uint32_t material:contributions.streamMaterials){if(material>=surfaceData.size()){hasReflections=true;break;}if(SurfaceMayReflect(surfaceData[material],quality)){hasReflections=true;break;}}


            if(!hasReflections&&!meshDomain->Bindings().empty()&&contributions.streamMaterials.empty())hasReflections=true;
        }else{
            hasReflections=false;historyReady=false;
        }
        // Aiming only draws a laser. Collision and mesh reconstruction run
        // while a projectile can be alive, never permanently after the first aim.
        bool aimVisible=false;
        if(coreSceneWork&&f.instanceCount){
            for(uint32_t hand=0;hand<2;++hand){
                const auto& shot=xr.shotHands[hand];
                aimVisible|=shot.aim.tracked;
                if(shot.fired)
                    cutFlightRemaining=std::max(cutFlightRemaining,hand==0?3.f:5.f);
            }
        }
        const float cutDt=std::clamp(float(double(xr.displayPeriod)*1e-9),.00001f,.05f);
        cutActive=cutFlightRemaining>0.f;
        RenderPassPlan passPlan{};
        if(coreSceneWork){
            passPlan.occlusion=f.instanceCount>SmallSceneOcclusionThreshold;
            passPlan.depthHierarchy=passPlan.occlusion||hasReflections;
            passPlan.clusteredLights=f.lightCount>DirectLightLimit;
            passPlan.reflections=hasReflections;
            passPlan.shadowAtlas=f.hasShadowAtlasLights;
            passPlan.rayQueries=passPlan.reflections||f.hasShadowedLights||cutActive;
            // The first user shot must build TLAS even when the ray snapshot
            // did not change while no ray tracing passes were active.
            passPlan.rayWork=(rayDirty||(cutActive&&!f.tlasBuilt))&&
                passPlan.rayQueries&&f.rayInstanceCount>0;
        }
        auto& c=*f.constants;c={};
        if(tileCatalog)c.tileViewer={
            float(tileCatalog->TileViewerX()),float(tileCatalog->TileViewerZ()),0,0};
        bool reflectionHistoryReady=historyReady&&passPlan.reflections;
        if(useDlss){
            const float scale=float(renderWidth)/float(width);
            const auto phases=uint32_t(std::ceil(8.f/(scale*scale)));
            const auto sample=uint32_t(frameNumber%phases)+1u;
            c.dlssJitter={Halton(sample,2)-.5f,Halton(sample,3)-.5f,0.f,0.f};
        }
        for(uint32_t view=0;view<xr.viewCount;view++){
            c.vp[view]=xr.vp[view];
            c.inverseVP[view]=xr.inverseVP[view];
            if(useDlss){
                const auto jitter=XMMatrixTranslation(2.f*c.dlssJitter.x/float(renderWidth),
                    -2.f*c.dlssJitter.y/float(renderHeight),0.f);
                const auto jittered=XMLoadFloat4x4(&xr.vp[view])*jitter;
                XMStoreFloat4x4(&c.vp[view],jittered);
                XMStoreFloat4x4(&c.inverseVP[view],XMMatrixInverse(nullptr,jittered));
                c.prevDlssVP[view]=resetDlssHistory?xr.vp[view]:dlss.previousVP[view];
            }
            c.previousVP[view]=reflectionHistoryReady?lastVP[view]:c.vp[view];
            c.eye[view]={xr.eyes[view].x,xr.eyes[view].y,xr.eyes[view].z,1};
            lastVP[view]=c.vp[view];
        }
        for(uint32_t i=0;i<ShadowSlots*6;i++)c.shadowVP[i]=shadowMatrices[i];
        SetDomain(f,ResourceDomain::Meshes,{verticesSRV,meshletsSRV,remapSRV,trianglesSRV,rangesSRV,materialsSRV,indicesSRV,meshletRegionsSRV,geometryUAVs[0],geometryUAVs[1],geometryUAVs[2],geometryUAVs[3],geometryUAVs[4],geometryUAVs[5],triangleSolidsSRV});
        SetDomain(f,ResourceDomain::Frame,{f.worldInstanceSRV,{}, {},f.worldInstanceUAV,{}, {},{}, {},f.cpuVisibility.depthArgumentsUAV,f.cpuVisibility.depthCountUAV,f.publishedVisibility.depthArgumentsUAV,f.publishedVisibility.depthCountUAV,f.cpuVisibility.cullArgumentsUAV,f.cpuVisibility.cullCountUAV,f.publishedVisibility.cullArgumentsUAV,f.publishedVisibility.cullCountUAV,f.cpuVisibility.shadowArgumentsUAV,f.cpuVisibility.shadowCountUAV,f.publishedInstanceSRV,f.instanceRangesSRV,
            f.cpuVisibility.batchVisibilityUAV,f.cpuVisibility.batchVisibilitySRV,
            f.cpuVisibility.batchArgumentsUAV,f.cpuVisibility.batchCountUAV,
            f.cpuVisibility.tileMarksUAV,f.cpuVisibility.tileMarksSRV,
            f.cpuVisibility.tilePrefixesUAV,f.cpuVisibility.tilePrefixesSRV,
            f.cpuVisibility.tileTotalsUAV,f.cpuVisibility.tileTotalsSRV,
            f.cpuVisibility.tileOffsetsUAV,f.cpuVisibility.tileOffsetsSRV});
        SetDomain(f,ResourceDomain::Lighting,{
            f.lightsSRV,f.clusterSRV,f.clusterUAV,shadowSRV,
            f.clusterIndicesSRV,f.clusterIndicesUAV,
            f.clusterGroupCountsSRV,f.clusterGroupCountsUAV,
            f.clusterGroupOffsetsSRV,f.clusterGroupOffsetsUAV,
            f.clusterDirectionalIndicesSRV,f.clusterDirectionalIndicesUAV,
            f.clusterDirectionalCountSRV,f.clusterDirectionalCountUAV,
            f.clusterBoundsSRV,f.clusterBoundsUAV,
            f.lightBvhNodesSRV,f.lightBvhNodesUAV,
            f.lightBvhPairsSRV,f.lightBvhPairsUAV,
            f.lightBvhBoundsSRV,f.lightBvhBoundsUAV});
        SetDomain(f,ResourceDomain::History,{hierarchySRV,depthSRV,hdrResolvedSRV,reflectionsResolvedSRV,historySRV[previous],historyDepthSRV[previous],historyUAV[output],historyDepthUAV[output],historySRV[output],hierarchySRV});
        for(uint32_t mip=0;mip<hierarchyUAV.size();mip++)f.resourceTableData[uint32_t(ResourceDomain::History)][HistoryHiZMipBase+mip]={Physical(hierarchyUAV[mip]),Generation(hierarchyUAV[mip])};
        SetDomain(f,ResourceDomain::Animation,{f.dynamicSRV,f.dynamicUAV,deformationsSRV,f.motionSRV,f.boneSRV,f.weightSRV,skinSRV,morphSRV,f.deformedBoundsSRV,f.deformedBoundsUAV});
        SetDomain(f,ResourceDomain::Visibility,{f.hotBoundsSRV,f.hotBoundsUAV,f.hotMeshesSRV,f.hotMeshesUAV});
        SetDomain(f,ResourceDomain::RayTracing,{f.tlasSRV,f.rayUAV,f.blasAddressesSRV,f.rayMapSRV});
        c.resourceAbi={DescriptorIndex(f.resourceDirectorySRV),ResourceAbiVersion,0,0};c.dimensions={renderWidth,renderHeight,renderMips,0};c.outputDimensions={width,height,renderWidth,renderHeight};c.counts={coreSceneWork?f.instanceCount:0u,coreSceneWork?f.additionalCount:0u,coreSceneWork?f.lightCount:0u,coreSceneWork&&reflectionHistoryReady?1u:0u};c.topology={xr.viewCount,MsaaSamples,passPlan.reflections?1u:0u,f.rayInstanceCount};c.timing={NearZ,FarZ,float(f.simulationTime),float(quality)};
        c.lightBvhLayout={f.lightBvhLeaves,0,0,0};
        const auto hudMemory=managedMemory.Budget();
        constexpr double gibibyte=1024.0*1024.0*1024.0;
        const float estimatedFps=potentialFrameMs>0?1000.f/potentialFrameMs:-1.f;
        const float margin=estimatedFps>0&&hudFps>0?100.f*(estimatedFps/hudFps-1.f):-1.f;
        c.hud={hudFps,estimatedFps,margin,developerMode?1.f:0.f};
        c.atmosphereSphere={atmosphere.center.x,atmosphere.center.y,atmosphere.center.z,atmosphere.radius};
        c.atmosphereColor={atmosphere.color.x,atmosphere.color.y,atmosphere.color.z,atmosphere.density};
        c.atmosphereParams={-1.f,1.f,0,0};
        if(f.lightCount){
            c.atmosphereLight=f.lightData[0].positionRadius;c.atmosphereEmission=f.lightData[0].colorPower;
            c.atmosphereParams.x=f.lightData[0].parameters.y;
            if(atmosphere.emissionReferencePower>0)c.atmosphereParams.y=f.lightData[0].colorPower.w/atmosphere.emissionReferencePower;
        }
        if(coreSceneWork){
            XMFLOAT3 center{};for(uint32_t view=0;view<xr.viewCount;view++)center=Add(center,xr.eyes[view]);center=Scale(center,1.0f/float(xr.viewCount));
            std::sort(f.transparent.begin(),f.transparent.begin()+f.transparentCount,[&](uint32_t a,uint32_t b){auto distance=[&](uint32_t i){auto p=CpuInstance(f,i).positionRadius;auto delta=Sub({p.x,p.y,p.z},center);return Dot(delta,delta);};return distance(a)>distance(b);});
        }

        const bool runCut=cutActive&&coreSceneWork&&f.instanceCount&&passPlan.rayQueries;
        const bool runOverlay=runCut||(aimVisible&&coreSceneWork&&f.instanceCount);
        CutDispatch cutDispatch{};
        if(runOverlay)cutDispatch=PrepareCutDispatch(f,xr);
        if(useDlss)c.dlssOutput={Physical(dlss.eyes[0].outputSrv),
            Physical(dlss.eyes[1].outputSrv),runOverlay?Physical(cutOverlaySRV):Invalid,0u};
        const bool drawCut=cutInitialCount!=0;
        uint32_t cutShadowSlots=0;
        if(drawCut&&passPlan.shadowAtlas){
            for(uint32_t slot=0;slot<ShadowSlots;++slot)
                if(f.shadowIDs[slot]!=Invalid)cutShadowSlots|=1u<<slot;
            f.shadowDirty|=cutShadowSlots&~lastCutShadowSlots;
        }
        const uint32_t predicatedShadowSlots=cutShadowsPending?
            cutShadowSlots&~f.shadowDirty:0u;
        const uint32_t shadowWork=f.shadowDirty|predicatedShadowSlots;
        c.resourceAbi.w=drawCut?Physical(cutStateSRV):Invalid;
        c.resourceAbi.z=Invalid;
        c.cutRaster={Invalid,Invalid,Invalid,Invalid};
        if(drawCut){
            c.cutGeometry={Physical(cutOriginalSRV),
                Physical(cutBufferSRVs[0]),Physical(cutBufferSRVs[1]),cutStaticCount};
            c.cutRaster={Physical(cutCacheStateSRV),
                Physical(cutCacheBlockSRVs[0]),Physical(cutCacheBlockSRVs[1]),
                Physical(cutCachePoolSRV)};
        }
        frameGraph.BeginFrame();f.queueFences={};uint32_t params[4]{};
        auto dispatchScene64=[](ID3D12GraphicsCommandList7* list,uint32_t count){
            const uint32_t groups=uint32_t((uint64_t(count)+63u)/64u);
            if(groups)list->Dispatch(std::min(groups,65535u),(groups+65534u)/65535u,1);
        };
        auto useBuffer=[&](uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess){frameGraph.Buffer(node,resource,mode,firstSync,firstAccess,finalSync,finalAccess);};
        auto useCutMesh=[&](uint32_t node){
            if(!drawCut)return;
            for(auto* resource:{cutState.Get(),cutOriginal.Get(),cutBuffers[0].Get(),cutBuffers[1].Get(),
                                cutCacheState.Get(),cutCacheBlocks[0].Get(),cutCacheBlocks[1].Get(),
                                cutCachePool.Get()})
                useBuffer(node,resource,GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cutDrawArguments.Get(),cutCacheDrawArguments.Get()})
                useBuffer(node,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        };
        auto useTexture=[&](uint32_t node,TextureResource& resource,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){frameGraph.Texture(node,&resource,resource.resource.Get(),resource.layout,mode,layout,sync,access);};
        auto useVrs=[&](uint32_t node){
            if(shadingRateImage)useTexture(node,vrsImage,GraphMode::Read,
                D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,
                D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE);
        };
        auto useTileSources=[&](uint32_t node,D3D12_BARRIER_SYNC sync){
            for(const auto& tile:f.heldTileSources)
                useBuffer(node,tile.Get(),GraphMode::Read,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE,sync,
                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        };

        if(surfacesDirty&&materials&&!surfaceData.empty()){
            UINT64 bytes=surfaceData.size()*sizeof(Surface),offset=Reserve(f,bytes);memcpy(f.uploadGeneration->mapped+offset,surfaceData.data(),size_t(bytes));
            auto node=frameGraph.AddPass("Materials.Update",GraphQueue::Direct,9,[&,offset,bytes](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyBufferRegion(materials.Get(),0,f.uploadGeneration->resource.Get(),offset,bytes);surfacesDirty=false;});
            useBuffer(node,materials.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }
        uint32_t coreColorNode=UINT32_MAX;
        if(coreSceneWork){
            auto uploadNode=frameGraph.AddPass("Upload",GraphQueue::Copy,10,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                if(!f.uploadGeneration)Fail("frame upload generation missing");auto uploadResource=f.uploadGeneration->resource.Get();auto copyAnimation=[&](ID3D12Resource* destination,UINT64 offset){l->CopyBufferRegion(destination,0,uploadResource,offset,destination->GetDesc().Width);};copyAnimation(f.motionBuffer.Get(),f.motionOffset);copyAnimation(f.boneBuffer.Get(),f.boneOffset);copyAnimation(f.weightBuffer.Get(),f.weightOffset);if(f.lightCount)l->CopyBufferRegion(f.lights.Get(),0,uploadResource,f.lightsOffset,UINT64(f.lightCount)*sizeof(Light));
                if(copyStatic){if(!residentStaticInstances)Fail("persistent static scene source missing");for(const auto& range:pendingStatic){UINT64 offset=UINT64(range.first)*sizeof(GpuInstance),bytes=UINT64(range.count)*sizeof(GpuInstance);l->CopyBufferRegion(f.worldInstances.Get(),offset,residentStaticInstances.Get(),offset,bytes);}}if(f.snapshotCount)l->CopyBufferRegion(f.worldInstances.Get(),UINT64(staticCount)*sizeof(GpuInstance),uploadResource,f.objectsOffset,UINT64(f.snapshotCount)*sizeof(GpuInstance));


                for(const auto& transfer:f.tileTransfers){
                    UINT64 sourceOffset=0;
                    for(const auto& range:transfer.ranges){
                        const UINT64 destinationOffset=UINT64(range.first)*
                            sizeof(GpuResidentTileGpuInstance);
                        const UINT64 bytes=UINT64(range.count)*
                            sizeof(GpuResidentTileGpuInstance);
                        l->CopyBufferRegion(transfer.buffer.Get(),destinationOffset,
                            transfer.upload.Get(),sourceOffset,bytes);
                        sourceOffset+=bytes;
                    }
                }
                for(const auto& transfer:f.tileRunTransfers)
                    l->CopyBufferRegion(transfer.buffer.Get(),0,
                        transfer.upload.Get(),0,transfer.upload->GetDesc().Width);

            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})if(resource)useBuffer(uploadNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic||f.snapshotCount)useBuffer(uploadNode,f.worldInstances.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);if(copyStatic)useBuffer(uploadNode,residentStaticInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);
            for(const auto& transfer:f.tileTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            for(const auto& transfer:f.tileRunTransfers)
                useBuffer(uploadNode,transfer.buffer.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,
                    D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
 if(f.lightCount)useBuffer(uploadNode,f.lights.Get(),GraphMode::Write,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
        }

        frameResources.Clear();auto frameAddress=f.uploadGeneration->resource->GetGPUVirtualAddress()+f.constantsOffset;
        frameResources.Set({GpuFrameResourceId::SceneDepth,depth.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,depth.layout,depthSRV,{}, {},Dsv(depthDSV),0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::DepthResolved,hierarchy.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,hierarchy.layout,hierarchySRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::HdrColor,hdr.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,hdr.layout,MsaaSamples==1?hdrResolvedSRV:GpuResourceHandle{},MsaaSamples==1?hdrResolvedUAV:GpuResourceHandle{},Rtv(hdrRTV),{},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedColor=MsaaSamples>1?hdrResolved:hdr;frameResources.Set({GpuFrameResourceId::ResolvedColor,resolvedColor.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedColor.layout,hdrResolvedSRV,hdrResolvedUAV,MsaaSamples==1?Rtv(hdrRTV):Rtv(hdrResolvedRTV), {},0,0,0,renderWidth,renderHeight,viewCount});
        auto& resolvedReflection=MsaaSamples>1?reflectionsResolved:reflections;frameResources.Set({GpuFrameResourceId::Reflections,resolvedReflection.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R16G16B16A16_FLOAT,resolvedReflection.layout,reflectionsResolvedSRV,{}, {},{},0,0,0,renderWidth,renderHeight,viewCount});
        frameResources.Set({GpuFrameResourceId::ShadowAtlas,shadow.resource.Get(),GpuPublishedResourceType::Texture2D,DXGI_FORMAT_R32_FLOAT,shadow.layout,shadowSRV,{}, {},Dsv(shadowDSV),0,0,0,3072,2048,1});
        frameResources.Set({GpuFrameResourceId::Lighting,f.lights.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightsSRV,{}, {},{},0,f.lights?f.lights->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::Clusters,f.clusters.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterSRV,f.clusterUAV,{}, {},0,f.clusters?f.clusters->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::ClusterIndices,f.clusterIndices.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.clusterIndicesSRV,f.clusterIndicesUAV,{}, {},0,f.clusterIndices?f.clusterIndices->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::LightBvhNodes,f.lightBvhNodes.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.lightBvhNodesSRV,f.lightBvhNodesUAV,{}, {},0,f.lightBvhNodes?f.lightBvhNodes->GetDesc().Width:0,0,0,0,0});
        frameResources.Set({GpuFrameResourceId::TLAS,f.tlas.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,f.tlasSRV,{}, {},{},0,f.tlas?f.tlas->GetDesc().Width:0,f.tlas?f.tlas->GetGPUVirtualAddress():0,0,0,0});
        GpuFrameResource constants{GpuFrameResourceId::FrameConstants,f.uploadGeneration->resource.Get(),GpuPublishedResourceType::Buffer,DXGI_FORMAT_UNKNOWN,D3D12_BARRIER_LAYOUT_COMMON,{}, {}, {},{},f.constantsOffset,sizeof(Constants),frameAddress,0,0,0};frameResources.Set(constants);constants.id=GpuFrameResourceId::ViewConstants;frameResources.Set(constants);
        for(const auto& bound:publications)publicationCatalog.Add(bound.publication);
        GpuFrameContext domainContext{&frameResources,xr.input.frameId,renderWidth,renderHeight,xr.viewCount,frameAddress,&publicationCatalog,frameSlot,uint32_t(frames.size()),f.simulationTime};


        if(coreSceneWork&&!f.deformationWork.Empty()){
            auto deformNode=frameGraph.AddPass("Deform",GraphQueue::Compute,15,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetPipelineState(deformPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch((meshes.ranges[mesh].vertexCount+63)/64,1,1);}
                BBarrier(l,f.dynamicVertices.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                l->SetPipelineState(deformBoundsPSO.Get());
                for(uint32_t dynamicIndex:f.deformationWork.Active()){uint32_t mesh=dynamicMeshes[dynamicIndex];l->SetComputeRoot32BitConstant(1,mesh,2);l->Dispatch(1,1,1);}
                BBarrier(l,f.deformedBounds.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            });
            for(auto* resource:{f.motionBuffer.Get(),f.boneBuffer.Get(),f.weightBuffer.Get()})useBuffer(deformNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.dynamicVertices.Get(),f.deformedBounds.Get()})useBuffer(deformNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }


        if(coreSceneWork&&f.cpuCount){
            auto cpuVisibilityNode=frameGraph.AddPass("CpuScene.Visibility",GraphQueue::Compute,20,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,false);
                l->SetComputeRoot32BitConstants(1,4,params,0);
                l->SetComputeRoot32BitConstant(1,0,0);
                l->SetComputeRoot32BitConstant(1,f.cpuCount,2);
                l->SetPipelineState(hotDataPSO.Get());
                dispatchScene64(l,f.cpuCount);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                ClearCount(l,f.cpuVisibility.depthCount.Get(),f.cpuVisibility.depthCountUAV);
                l->SetPipelineState(cullPSO.Get());
                l->SetComputeRoot32BitConstant(1,3,1);
                dispatchScene64(l,f.cpuCount);
                ArgumentsReady(l,f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get());
            });
            useTileSources(cpuVisibilityNode,D3D12_BARRIER_SYNC_COMPUTE_SHADING);
            useBuffer(cpuVisibilityNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuVisibilityNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuVisibilityNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(cpuVisibilityNode,f.cpuVisibility.batchVisibility.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        }


        if(coreSceneWork&&f.cpuCount){
            auto cpuDepthNode=frameGraph.AddPass("CpuScene.Depth",GraphQueue::Direct,21,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                Bind(l,f,true);
                Viewport(l,renderWidth,renderHeight);
                auto depthHandle=Dsv(depthDSV);
                l->ClearDepthStencilView(depthHandle,D3D12_CLEAR_FLAG_DEPTH,0,0,0,nullptr);
                l->OMSetRenderTargets(0,nullptr,FALSE,&depthHandle);
                l->SetPipelineState(depthPSO.Get());
                l->SetGraphicsRoot32BitConstants(1,4,params,0);
                l->SetGraphicsRoot32BitConstant(1,0,4);
                l->ExecuteIndirect(indirect.Get(),f.cpuCount,f.cpuVisibility.depthArguments.Get(),0,f.cpuVisibility.depthCount.Get(),0);
                if(drawCut){
                    l->SetGraphicsRoot32BitConstant(1,1,4);
                    l->ExecuteIndirect(indirect.Get(),1,cutDrawArguments.Get(),0,nullptr,0);
                    l->SetPipelineState(cutDepthPSO.Get());
                    l->SetGraphicsRoot32BitConstant(1,2,4);
                    l->ExecuteIndirect(cutBlockIndirect.Get(),cutSpatialBlockCount,
                        cutCacheDrawArguments.Get(),0,nullptr,0);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                }
            });
            useCutMesh(cpuDepthNode);
            useTileSources(cpuDepthNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            useBuffer(cpuDepthNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.worldInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(cpuDepthNode,f.dynamicVertices.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{f.cpuVisibility.depthArguments.Get(),f.cpuVisibility.depthCount.Get()})
                useBuffer(cpuDepthNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useTexture(cpuDepthNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
        }


        DomainPassBuilder domainBuilder(*this);domainBuilder.SetDomain(meshDomain->Identity());meshDomain->BuildPasses(domainBuilder,domainContext);domainBuilder.SetDomain(GpuDomainId::Invalid);




        const bool bvhSortNeeded=AddLightBvhPass(f,coreSceneWork);

        if(coreSceneWork){


            if(f.instanceCount>f.cpuCount){
                auto publishedNode=frameGraph.AddPass("MeshSolid.Visibility",GraphQueue::Compute,25,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    l->SetComputeRoot32BitConstants(1,4,params,0);
                    auto dispatchGpu=[&](uint32_t passIndex){
                        l->SetComputeRoot32BitConstant(1,passIndex,1);
                        for(const auto& bound:meshDomain->Bindings()){
                            l->SetComputeRoot32BitConstant(1,bound.base,0);
                            l->SetComputeRoot32BitConstant(1,Invalid,2);
                            uint64_t controlBase=uint64_t(bound.controlSlot)*GpuMeshPublicationAbi::ControlBytes+GpuMeshPublicationAbi::MetadataBytes;
                            l->ExecuteIndirect(dispatchIndirect.Get(),1,f.streamControl.Get(),controlBase+GpuMeshPublicationAbi::ActiveDispatchOffset,nullptr,0);
                        }
                        l->SetComputeRoot32BitConstants(1,4,params,0);
                    };
                    l->SetPipelineState(hotDataPSO.Get());
                    dispatchGpu(0);
                    for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                        BBarrier(l,resource,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    ClearCount(l,f.publishedVisibility.depthCount.Get(),f.publishedVisibility.depthCountUAV);
                    l->SetPipelineState(cullPSO.Get());
                    dispatchGpu(4);
                    ArgumentsReady(l,f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get());
                });
                useBuffer(publishedNode,f.streamControl.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                useBuffer(publishedNode,f.publishedInstances.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useBuffer(publishedNode,f.deformedBounds.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.hotBounds.Get(),f.hotMeshes.Get()})
                    useBuffer(publishedNode,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                for(auto* resource:{f.publishedVisibility.depthArguments.Get(),f.publishedVisibility.depthCount.Get()})
                    useBuffer(publishedNode,resource,GraphMode::Write,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }

            if(passPlan.rayWork){
            auto computeNode=frameGraph.AddPass("DeformCull",GraphQueue::Compute,30,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);if(passPlan.rayWork){
                if(f.rayMapBuild){
                    l->SetPipelineState(rayMapFillPSO.Get());
                    for(const auto& segment:f.rayMapSegments){
                        uint32_t mapArgs[4]{
                            segment.compactFirst,segment.renderFirst,segment.count,
                            Physical(f.rayMapUAV)};
                        l->SetComputeRoot32BitConstants(1,4,mapArgs,0);
                        const uint32_t groups=(segment.count+63u)/64u;
                        l->Dispatch(std::min(groups,65535u),
                                    (groups+65534u)/65535u,1);
                    }
                    BBarrier(l,f.rayInstanceMap.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                }
                l->SetPipelineState(rayBuildPSO.Get());for(const auto& range:rayCpuDirty){l->SetComputeRoot32BitConstant(1,range.first,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch(std::min((range.count+63u)/64u,65535u),
                    (((range.count+63u)/64u)+65534u)/65535u,1);}for(uint32_t bindingIndex:rayPublicationDirty){if(bindingIndex>=rayPublicationRanges.size())Fail("compact ray publication dirty index");const auto& range=rayPublicationRanges[bindingIndex];if(!range.count)continue;l->SetComputeRoot32BitConstant(1,range.compactFirst,0);l->SetComputeRoot32BitConstant(1,range.count,2);l->Dispatch(std::min((range.count+63u)/64u,65535u),
                    (((range.count+63u)/64u)+65534u)/65535u,1);}l->SetComputeRoot32BitConstants(1,4,params,0);}});
            if(passPlan.rayWork)useBuffer(computeNode,f.rayInstanceMap.Get(),
                f.rayMapBuild?GraphMode::ReadWrite:GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                f.rayMapBuild?(D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                               D3D12_BARRIER_ACCESS_SHADER_RESOURCE):
                              …9207 tokens truncated…);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]={Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    float clear[]={.008f,.013f,.021f,1},black[]={0,0,0,0};
                    l->ClearRenderTargetView(targets[0],clear,0,nullptr);
                    if(passPlan.reflections)l->ClearRenderTargetView(targets[1],black,0,nullptr);
                    l->OMSetRenderTargets(passPlan.reflections?2:1,targets,FALSE,&depthHandle);
                    bindVrs(l);
                    for(uint32_t i=0;i<opaqueRuns.size();++i){
                        const auto& run=opaqueRuns[i];
                        auto* pso=(passPlan.reflections?
                            sceneVariantPSO[run.mask]:sceneDirectVariantPSO[run.mask]).Get();
                        if(!pso)Fail("Opaque material specialization unavailable");
                        l->SetPipelineState(pso);
                        l->ExecuteIndirect(indirect.Get(),run.count,cpuOpaqueArguments,
                            uint64_t(run.first)*16,cpuOpaqueCount,uint64_t(i)*4);
                    }
                    if(shadingRateImage)l->RSSetShadingRateImage(nullptr);
                });
            useVrs(opaqueNode);
            auto opaqueTileNode=frameGraph.AddPass(
                "Opaque.Resident",GraphQueue::Direct,61);
            frameGraph.Node(opaqueTileNode).reusableDirectLists=
                residentOpaqueLists;
            if(!residentOpaqueLists.empty())useVrs(opaqueTileNode);
            frameGraph.Dependency(opaqueNode,opaqueTileNode);
            useCutMesh(opaqueNode);
            useCutMesh(opaqueTileNode);
            auto opaqueTailNode=frameGraph.AddPass("Opaque.Tail",GraphQueue::Direct,62,
                [&,publishedOpaqueArguments,publishedOpaqueCount]
                (const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,true);
                    l->SetGraphicsRoot32BitConstants(1,4,params,0);
                    l->SetGraphicsRoot32BitConstant(1,0,4);
                    Viewport(l,renderWidth,renderHeight);
                    auto depthHandle=Dsv(depthDSV);
                    D3D12_CPU_DESCRIPTOR_HANDLE targets[]{
                        Rtv(hdrRTV),Rtv(reflectionsRTV)};
                    l->OMSetRenderTargets(passPlan.reflections?2u:1u,targets,
                        FALSE,&depthHandle);
                    bindVrs(l);
                    if(f.instanceCount>f.cpuCount){
                        l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());
                        l->ExecuteIndirect(indirect.Get(),f.instanceCount-f.cpuCount,
                            publishedOpaqueArguments,0,publishedOpaqueCount,0);
                    }
                    if(drawCut){
                        l->SetPipelineState((passPlan.reflections?scenePSO:sceneDirectPSO).Get());
                        l->SetGraphicsRoot32BitConstant(1,1,4);
                        l->ExecuteIndirect(indirect.Get(),1,cutDrawArguments.Get(),0,nullptr,0);
                        l->SetPipelineState((passPlan.reflections?cutScenePSO:cutSceneDirectPSO).Get());
                        l->SetGraphicsRoot32BitConstant(1,2,4);
                        l->ExecuteIndirect(cutBlockIndirect.Get(),cutSpatialBlockCount,
                            cutCacheDrawArguments.Get(),0,nullptr,0);
                        l->SetGraphicsRoot32BitConstant(1,0,4);
                    }
                    if(shadingRateImage)l->RSSetShadingRateImage(nullptr);
                    if(f.transparentCount){
                        l->SetPipelineState((passPlan.reflections?
                                            transparentPSO:transparentDirectPSO).Get());
                        for(uint32_t index=0;index<f.transparentCount;index++){
                            uint32_t object=f.transparent[index];
                            l->SetGraphicsRoot32BitConstant(1,object,0);
                            l->DispatchMesh(meshes.ranges[CpuInstance(f,object).mesh].meshletCount,1,1);
                        }
                    }
                });
            frameGraph.Dependency(opaqueTileNode,opaqueTailNode);
            useVrs(opaqueTailNode);
            useCutMesh(opaqueTailNode);
            if(f.rayInstanceCount)useBuffer(opaqueNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueNode;
            useTileSources(opaqueNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);


            if(f.rayInstanceCount)useBuffer(opaqueTileNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTileNode;
            useTileSources(opaqueTileNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTileNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTileNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTileNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTileNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTileNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTileNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTileNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTileNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTileNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTileNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTileNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTileNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(f.rayInstanceCount)useBuffer(opaqueTailNode,f.rayInstanceMap.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(MsaaSamples==1)coreColorNode=opaqueTailNode;
            useTileSources(opaqueTailNode,D3D12_BARRIER_SYNC_ALL_SHADING);
            if(f.cpuCount)useBuffer(opaqueTailNode,f.worldInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(f.instanceCount>f.cpuCount)useBuffer(opaqueTailNode,f.publishedInstances.Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(opaqueTailNode,materials.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cpuOpaqueArguments,cpuOpaqueCount,publishedOpaqueArguments,publishedOpaqueCount})useBuffer(opaqueTailNode,resource,GraphMode::Read,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);if(f.lightCount)useBuffer(opaqueTailNode,f.lights.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.clusteredLights)for(auto* lightIndexResource:{
                f.clusters.Get(),f.clusterIndices.Get(),
                f.clusterDirectionalIndices.Get(),f.clusterDirectionalCount.Get()})
                useBuffer(opaqueTailNode,lightIndexResource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(passPlan.clusteredLights||passPlan.reflections)
                useBuffer(opaqueTailNode,f.lightBvhNodes.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            if(passPlan.rayQueries&&f.rayInstanceCount)useBuffer(opaqueTailNode,f.tlas.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);if(passPlan.shadowAtlas)useTexture(opaqueTailNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(opaqueTailNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ);useTexture(opaqueTailNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);if(passPlan.reflections)useTexture(opaqueTailNode,reflections,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);

            if(MsaaSamples>1){frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);}else{frameGraph.TransitionTexture(opaqueTailNode,hdr,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(opaqueTailNode,reflections,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}frameGraph.TransitionTexture(opaqueTailNode,depth,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

            if(MsaaSamples>1){
                auto resolveNode=frameGraph.AddPass("ResolveColor",GraphQueue::Direct,70,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){for(uint32_t view=0;view<viewCount;view++){l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);if(passPlan.reflections)l->ResolveSubresource(reflectionsResolved.resource.Get(),view,reflections.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);}});
                coreColorNode=resolveNode;
                useTexture(resolveNode,hdr,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);if(passPlan.reflections){useTexture(resolveNode,reflections,GraphMode::Read,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE);useTexture(resolveNode,reflectionsResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);}
                frameGraph.TransitionTexture(resolveNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)frameGraph.TransitionTexture(resolveNode,reflectionsResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }

            if(passPlan.reflections){
                auto postNode=frameGraph.AddPass("Post",GraphQueue::Direct,90,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);l->SetComputeRoot32BitConstants(1,4,params,0);l->SetPipelineState(denoisePSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(postNode,resolvedReflection,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,hierarchy,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(reflectionHistoryReady){useTexture(postNode,history[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(postNode,historyDepth[previous],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);}useTexture(postNode,history[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);useTexture(postNode,historyDepth[output],GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                frameGraph.TransitionTexture(postNode,history[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);frameGraph.TransitionTexture(postNode,historyDepth[output],D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                auto mergeNode=frameGraph.AddPass("Reflection.Composite",GraphQueue::Direct,91,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t mergeParams[]{0,0,Physical(hdrResolvedUAV),0};l->SetComputeRoot32BitConstants(1,4,mergeParams,0);l->SetPipelineState(mergeReflectionPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
                useTexture(mergeNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(mergeNode,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);coreColorNode=mergeNode;
            }

        }else{
            auto clearNode=frameGraph.AddPass("Core.ClearVisualTarget",GraphQueue::Direct,60,[&](const CompiledNode& compiled,ID3D12GraphicsCommandList7* l){
                Viewport(l,renderWidth,renderHeight);
                const float color[]={.008f,.013f,.021f,1.0f};auto rtv=Rtv(hdrRTV);auto dsv=Dsv(depthDSV);
                l->ClearRenderTargetView(rtv,color,0,nullptr);
                l->ClearDepthStencilView(dsv,D3D12_CLEAR_FLAG_DEPTH,0.0f,0,0,nullptr);
                EmitBarriers(l,compiled.barriersDuring[0]);
                EmitBarriers(l,compiled.barriersDuring[1]);
                if(MsaaSamples>1){
                    EmitBarriers(l,compiled.barriersDuring[2]);
                    for(uint32_t view=0;view<viewCount;++view)l->ResolveSubresource(hdrResolved.resource.Get(),view,hdr.resource.Get(),view,DXGI_FORMAT_R16G16B16A16_FLOAT);
                }
            });
            useTexture(clearNode,hdr,GraphMode::Write,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET);
            useTexture(clearNode,depth,GraphMode::Write,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE);
            if(MsaaSamples>1)useTexture(clearNode,hdrResolved,GraphMode::Write,D3D12_BARRIER_LAYOUT_RESOLVE_DEST,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_DEST);
            frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RENDER_TARGET,D3D12_BARRIER_SYNC_RENDER_TARGET,D3D12_BARRIER_ACCESS_RENDER_TARGET,true);
            frameGraph.TransitionTexture(clearNode,depth,D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,D3D12_BARRIER_SYNC_DEPTH_STENCIL,D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ|D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,true);
            if(MsaaSamples>1){
                frameGraph.TransitionTexture(clearNode,hdr,D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE,D3D12_BARRIER_SYNC_RESOLVE,D3D12_BARRIER_ACCESS_RESOLVE_SOURCE,true);
                frameGraph.TransitionTexture(clearNode,hdrResolved,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
            coreColorNode=clearNode;
        }


        if(aaMode&&!useDlss){
            if(!aaSourceLease){GpuTextureDesc d{};d.width=renderWidth;d.height=renderHeight;d.depthOrArraySize=viewCount;d.format=DXGI_FORMAT_R16G16B16A16_FLOAT;d.uav=false;d.name=L"FXAA.SpatialSource";aaSourceLease=CreateManagedTexture2D(d);if(!aaSourceLease)Fail("FXAA source allocation");aaSource.resource=aaSourceLease->resource;aaSourceArraySrv=TextureSrv(aaSource.resource.Get(),d.format,{},true);aaSource.layout=D3D12_BARRIER_LAYOUT_COMMON;}
            auto copy=frameGraph.AddPass("FXAA.Snapshot",GraphQueue::Direct,92,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){l->CopyResource(aaSource.resource.Get(),resolvedColor.resource.Get());});
            useTexture(copy,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_COPY_SOURCE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_SOURCE);useTexture(copy,aaSource,GraphMode::Write,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_COPY_DEST);
            auto filter=frameGraph.AddPass("FXAA.Spatial",GraphQueue::Direct,93,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){Bind(l,f,false);uint32_t p[]{DescriptorIndex(aaSourceArraySrv),Physical(hdrResolvedUAV),aaMode,0};l->SetComputeRoot32BitConstants(1,4,p,0);l->SetPipelineState(fxaaPSO.Get());l->Dispatch((renderWidth+7)/8,(renderHeight+7)/8,viewCount);});
            useTexture(filter,aaSource,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(filter,resolvedColor,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
        for(auto& domain:externalDomains){domainBuilder.SetDomain(domain->Identity());domain->BuildPasses(domainBuilder,domainContext);}domainBuilder.SetDomain(GpuDomainId::Invalid);

        uint32_t dlssOverlayNode=UINT32_MAX;
        if(runOverlay){
            // The overlay is shared across XR frame slots; the direct queue and
            // the graph hazards serialize mask/list recycling without a CPU wait.
            const uint32_t current=cutDispatch.overlayCurrent,previous=cutDispatch.overlayPrevious;
            const bool reset=cutDispatch.overlayReset;
            auto clearMarks=frameGraph.AddPass("Projectile.ClearPreviousTiles",GraphQueue::Direct,94,[&,current,previous,reset](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                const UINT zeros[4]{};
                ID3D12DescriptorHeap* heaps[]{descriptors.Get(),samplers.Get()};
                l->SetDescriptorHeaps(2,heaps);
                if(reset){
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayUAV)),Cpu(Physical(cutOverlayUAV)),cutOverlayMask.resource.Get(),zeros,0,nullptr);
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayTileLastSeenUAV)),Cpu(Physical(cutOverlayTileLastSeenUAV)),cutOverlayTileLastSeen.Get(),zeros,0,nullptr);
                    for(uint32_t i=0;i<2;++i)
                        l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayActiveCountUAVs[i])),Cpu(Physical(cutOverlayActiveCountUAVs[i])),cutOverlayActiveCounts[i].Get(),zeros,0,nullptr);
                }else{
                    BindCutPipeline(l,CutKernel::ClearTiles,cutDispatch.overlay);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,cutOverlayTileDispatchArgs.Get(),
                                       UINT64(previous)*sizeof(D3D12_DISPATCH_ARGUMENTS),nullptr,0);
                    // The opposite list is now vacant for this frame.
                    l->ClearUnorderedAccessViewUint(Gpu(Physical(cutOverlayActiveCountUAVs[current])),Cpu(Physical(cutOverlayActiveCountUAVs[current])),cutOverlayActiveCounts[current].Get(),zeros,0,nullptr);
                }
            });
            useTexture(clearMarks,cutOverlayMask,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(clearMarks,cutOverlayActiveCounts[current].Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(reset){
                useBuffer(clearMarks,cutOverlayTileLastSeen.Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(clearMarks,cutOverlayActiveCounts[previous].Get(),GraphMode::Write,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            }else{
                useBuffer(clearMarks,cutOverlayActiveTiles[previous].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useBuffer(clearMarks,cutOverlayTileDispatchArgs.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            }
            auto markBuffers=[&](uint32_t node){
                for(auto* buffer:{cutOverlayTileLastSeen.Get(),cutOverlayActiveTiles[current].Get(),
                                  cutOverlayActiveCounts[current].Get()})
                    useBuffer(node,buffer,GraphMode::ReadWrite,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            };
            uint32_t marksProducer=clearMarks;
            if(runCut){
                auto stamp=frameGraph.AddPass("Projectile.Stamp",GraphQueue::Direct,95,
                    [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                        BindCutPipeline(l,CutKernel::Stamp,cutDispatch.overlay);
                        l->Dispatch(CutProjectileCapacity,xr.viewCount,1);
                    });
                frameGraph.Dependency(clearMarks,stamp);
                useBuffer(stamp,cutProjectiles[cutDispatch.destination].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(stamp,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                markBuffers(stamp);
                marksProducer=stamp;
            }
            auto laser=frameGraph.AddPass("Projectile.Laser",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                BindCutPipeline(l,CutKernel::Laser,cutDispatch.overlay);
                l->Dispatch((std::max(width,height)+63u)/64u,2,xr.viewCount);
            });
            frameGraph.Dependency(marksProducer,laser);
            useTexture(laser,cutOverlayMask,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            markBuffers(laser);
            auto args=frameGraph.AddPass("Projectile.BuildTileArgs",GraphQueue::Direct,95,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                BindCutPipeline(l,CutKernel::BuildTileArgs,cutDispatch.overlay);
                l->Dispatch(1,1,1);
            });
            frameGraph.Dependency(laser,args);
            useBuffer(args,cutOverlayActiveCounts[current].Get(),GraphMode::Read,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(args,cutOverlayTileDispatchArgs.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            if(useDlss)dlssOverlayNode=args;
            else{
                auto composite=frameGraph.AddPass("Projectile.Composite",GraphQueue::Direct,96,[&,current](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    BindCutPipeline(l,CutKernel::Composite,cutDispatch.overlay);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,cutOverlayTileDispatchArgs.Get(),
                                       UINT64(current)*sizeof(D3D12_DISPATCH_ARGUMENTS),nullptr,0);
                });
                frameGraph.Dependency(coreColorNode,composite);
                frameGraph.Dependency(args,composite);
                useBuffer(composite,cutOverlayTileDispatchArgs.Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                useBuffer(composite,cutOverlayActiveTiles[current].Get(),GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                useTexture(composite,cutOverlayMask,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                useTexture(composite,resolvedColor,GraphMode::ReadWrite,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                coreColorNode=composite;
            }
        }
        // 4e: raster/collision read the previously committed block ranges.
        // The current frame clips only BVH-selected blocks after all readers,
        // then publishes their new cache generation atomically for next frame.
        uint32_t cutStageNode=UINT32_MAX;
        if(runCut&&drawCut){
            auto& cf=cutFrames[frameSlot];
            auto stage=frameGraph.AddPass("Cut.LocalBlocks",GraphQueue::Direct,97,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    BindCutPipeline(l,CutKernel::Begin,cutDispatch.begin);
                    l->Dispatch(1,1,1);
                    BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BBarrier(l,cutDispatchArguments.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                    // BeginCut also advances the unique attempt stamp in
                    // cutState.w. Publish it alongside BVH candidate marks.
                    BBarrier(l,cutState.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    BBarrier(l,cutCandidateSelection.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    BindCutPipeline(l,CutKernel::Clip,cutDispatch.clip);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,cutDispatchArguments.Get(),0,nullptr,0);
                    BBarrier(l,cutControl.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    // Only selected blocks populate temporary triangle scratch.
                    // CacheCount and CacheScatter read that local scratch.
                    for(auto& buffer:cutBuffers)
                        BBarrier(l,buffer.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    // The persistent block cache is the authoritative source
                    // for collision and raster after the first successful cut.
                    // Scratch is never published as a complete world mesh.
                    auto cacheBarrier=[&](ID3D12Resource* r){
                        BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    };
                    BindCutPipeline(l,CutKernel::CacheBegin,cutDispatch.begin);
                    l->Dispatch(1,1,1);
                    cacheBarrier(cutCacheResults.Get());
                    cacheBarrier(cutCacheWork.Get());
                    BBarrier(l,cutCacheDispatch.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                    BindCutPipeline(l,CutKernel::CacheCount,cutDispatch.begin);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,
                        cutCacheDispatch.Get(),0,nullptr,0);
                    cacheBarrier(cutCacheResults.Get());
                    cacheBarrier(cutCacheWork.Get());
                    BindCutPipeline(l,CutKernel::CacheReserve,cutDispatch.begin);
                    l->Dispatch(1,1,1);
                    for(auto* r:{cutCacheBlocks[0].Get(),cutCacheBlocks[1].Get(),
                                 cutCacheAllocators[0].Get(),cutCacheAllocators[1].Get(),
                                 cutCacheFreeLists[0].Get(),cutCacheFreeLists[1].Get(),
                                 cutCacheWork.Get()})cacheBarrier(r);
                    // ClipCacheBlocks read the prior pool generation. Scatter
                    // writes only newly allocated ranges after this barrier.
                    BBarrier(l,cutCachePool.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BindCutPipeline(l,CutKernel::CacheScatter,cutDispatch.begin);
                    l->ExecuteIndirect(dispatchIndirect.Get(),1,
                        cutCacheDispatch.Get(),0,nullptr,0);
                    for(auto* r:{cutCachePool.Get(),cutCacheResults.Get(),
                                 cutCacheWork.Get()})cacheBarrier(r);
                    BindCutPipeline(l,CutKernel::CacheCommit,cutDispatch.begin);
                    l->Dispatch(1,1,1);
                    for(auto* r:{cutCacheState.Get(),cutCacheWork.Get(),
                                 cutCacheAllocators[0].Get(),cutCacheAllocators[1].Get(),
                                 cutCacheFreeLists[0].Get(),cutCacheFreeLists[1].Get()})
                        cacheBarrier(r);
                    // Advance the cut revision ONLY after its local ranges
                    // have been validated. A failed impact leaves both
                    // generations and the previous visible mesh untouched.
                    // The local clipping/count passes read cutState as SRV;
                    // CommitCut is the sole late-frame UAV publisher.
                    BBarrier(l,cutState.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE|
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BBarrier(l,cutShadowPredicates.Get(),
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BindCutPipeline(l,CutKernel::Commit,cutDispatch.commit);
                    l->Dispatch(1,1,1);
                    BBarrier(l,cutState.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                    BBarrier(l,cutDrawArguments.Get(),D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                        D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    BindCutPipeline(l,CutKernel::CacheDraw,cutDispatch.begin);
                    l->Dispatch((cutSpatialBlockCount+63u)/64u,1,1);
                    for(auto* r:{cutCacheDrawArguments.Get(),cutDrawArguments.Get()})
                        BBarrier(l,r,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                            D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                            D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                            D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
                });
            cutStageNode=stage;
            frameGraph.Dependency(coreColorNode,stage);
            useBuffer(stage,cf.impacts.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cf.control.Get(),GraphMode::Read,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            for(auto* resource:{cutOriginal.Get(),cutSpatialNodes.Get(),cutSpatialBlocks.Get()})
                useBuffer(stage,resource,GraphMode::Read,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cutCandidateSelection.Get(),GraphMode::ReadWrite,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto* resource:{cutState.Get(),cutBuffers[0].Get(),
                                cutBuffers[1].Get(),cutControl.Get()})
                useBuffer(stage,resource,GraphMode::ReadWrite,D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cutDispatchArguments.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(stage,cutDrawArguments.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            for(auto* r:{cutCachePool.Get(),cutCacheState.Get(),
                         cutCacheBlocks[0].Get(),cutCacheBlocks[1].Get(),
                         cutCacheAllocators[0].Get(),cutCacheAllocators[1].Get(),
                         cutCacheFreeLists[0].Get(),cutCacheFreeLists[1].Get(),
                         cutCacheResults.Get(),cutCacheWork.Get()})
                useBuffer(stage,r,GraphMode::ReadWrite,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                    D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                        D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            useBuffer(stage,cutCacheDispatch.Get(),GraphMode::ReadWrite,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING|
                    D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS|
                    D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
            useBuffer(stage,cutShadowPredicates.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            useBuffer(stage,cutCacheDrawArguments.Get(),GraphMode::Write,
                D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                D3D12_BARRIER_SYNC_EXECUTE_INDIRECT,
                D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        }
        uint32_t dlssEvaluationNode=UINT32_MAX;
        if(useDlss){
            const auto inputNode=frameGraph.AddPass("DLSS.Inputs",GraphQueue::Direct,98,
                [&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
                    Bind(l,f,false);
                    l->SetPipelineState(dlssPreparePSO.Get());
                    for(uint32_t eye=0;eye<dlss.eyes.size();++eye){
                        const auto& inputUavs=dlss.eyes[eye].inputUavs;
                        const uint32_t inputParams[]{eye,Physical(inputUavs[0]),
                            Physical(inputUavs[1]),Physical(inputUavs[2])};
                        l->SetComputeRoot32BitConstants(1,4,inputParams,0);
                        l->Dispatch((renderWidth+7u)/8u,(renderHeight+7u)/8u,1u);
                    }
                });
            frameGraph.Dependency(coreColorNode,inputNode);
            for(auto* source:{&resolvedColor,&depth})
                useTexture(inputNode,*source,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            for(auto& eye:dlss.eyes){
                for(auto& input:eye.inputs){
                    useTexture(inputNode,input,GraphMode::Write,D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS,
                        D3D12_BARRIER_SYNC_COMPUTE_SHADING,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
                    frameGraph.TransitionTexture(inputNode,input,D3D12_BARRIER_LAYOUT_COMMON,
                        D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_ACCESS_NO_ACCESS);
                }
            }
            if(!f.dlssEvaluation.list)CommandsInit(f.dlssEvaluation,D3D12_COMMAND_LIST_TYPE_DIRECT);
            dlssEvaluationNode=frameGraph.AddPass("DLSS.Evaluate",GraphQueue::Direct,99);
            frameGraph.Node(dlssEvaluationNode).standaloneDirectList=f.dlssEvaluation.list.Get();
            frameGraph.Dependency(inputNode,dlssEvaluationNode);
        }

        bool dlssPresented=false;
        auto presentNode=frameGraph.AddPass("Present",GraphQueue::Direct,100,[&](const CompiledNode&,ID3D12GraphicsCommandList7* l){
            uint32_t dlssStatus=dlssUsable?0u:Invalid;
            if(useDlss)dlssStatus=dlssPresented?uint32_t(std::lround(1000.f*float(renderWidth)/float(width))):Invalid;
            auto* output=xrColorTargets[xr.color].Get();
            D3D12_TEXTURE_BARRIER displayBarrier{};
            D3D12_BARRIER_GROUP displayGroup{};displayGroup.Type=D3D12_BARRIER_TYPE_TEXTURE;displayGroup.NumBarriers=1;displayGroup.pTextureBarriers=&displayBarrier;
            if(outputSurface.desktop){
                const bool simultaneous=(output->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)!=0;
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_NONE;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_RENDER_TARGET;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_NO_ACCESS;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_RENDER_TARGET;
                displayBarrier.LayoutBefore=D3D12_BARRIER_LAYOUT_PRESENT;displayBarrier.LayoutAfter=simultaneous?D3D12_BARRIER_LAYOUT_COMMON:D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                displayBarrier.pResource=output;l->Barrier(1,&displayGroup);
            }
            Viewport(l,width,height);
            const auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);
            l->OMSetRenderTargets(1,&target,FALSE,&ds);
            Bind(l,f,true);
            l->SetGraphicsRoot32BitConstants(1,4,params,0);
            l->SetGraphicsRoot32BitConstant(1,passPlan.reflections?1u:0u,3);
            if(useDlss)l->SetGraphicsRoot32BitConstant(1,dlssStatus,4);
            l->SetPipelineState(useDlss?dlssPresentPSO.Get():presentPSO.Get());
            l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            l->DrawInstanced(3,1,0,0);

            RecordCapture(l,output);
            if(outputSurface.desktop){
                displayBarrier.SyncBefore=D3D12_BARRIER_SYNC_ALL;displayBarrier.SyncAfter=D3D12_BARRIER_SYNC_NONE;
                displayBarrier.AccessBefore=D3D12_BARRIER_ACCESS_RENDER_TARGET;displayBarrier.AccessAfter=D3D12_BARRIER_ACCESS_NO_ACCESS;
                displayBarrier.LayoutBefore=displayBarrier.LayoutAfter;displayBarrier.LayoutAfter=D3D12_BARRIER_LAYOUT_PRESENT;
                l->Barrier(1,&displayGroup);
            }
        });useTexture(presentNode,resolvedColor,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);if(passPlan.reflections)useTexture(presentNode,history[output],GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);useTexture(presentNode,depth,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        if(atmosphere.radius>0&&atmosphere.density>0&&f.hasShadowAtlasLights&&f.lightCount&&f.lightData[0].parameters.y>=0)useTexture(presentNode,shadow,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);

        frameGraph.Dependency(coreColorNode,presentNode);
        if(useDlss){
            frameGraph.Dependency(dlssEvaluationNode,presentNode);
            for(auto& eye:dlss.eyes){
                useTexture(presentNode,eye.output,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
                frameGraph.TransitionTexture(presentNode,eye.output,D3D12_BARRIER_LAYOUT_COMMON,
                    D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_ACCESS_NO_ACCESS);
            }
            if(dlssOverlayNode!=UINT32_MAX){
                frameGraph.Dependency(dlssOverlayNode,presentNode);
                useTexture(presentNode,cutOverlayMask,GraphMode::Read,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                    D3D12_BARRIER_SYNC_PIXEL_SHADING,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            }
        }
        if(cutStageNode!=UINT32_MAX)
            frameGraph.Dependency(cutStageNode,presentNode);
        if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Budget().headroom)){releasePublications(true);publications.clear();managedMemory.EndFrame(SubmittedFences());endDomains();active=nullptr;historyReady=false;return;}
        residency.Wait(copy.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(direct.queue.Get());
        for(const auto& bound:publications)frameGraph.PublicationComplete(bound.publication,presentNode);
        frameGraph.CompileExecutionBatches();
        float dlssCpuMs=-1.f;
        if(useDlss){
            f.dlssEvaluation.Reset();
            const int64_t evaluationStart=dlssDiagnosticEnabled?QpcCounter():0;
            dlssPresented=EvaluateDlss(f.dlssEvaluation.list.Get(),c,resetDlssHistory);
            if(evaluationStart)dlssCpuMs=float(QpcSeconds(QpcCounter()-evaluationStart)*1000.0);
            f.dlssEvaluation.Close();
        }
        auto batchCommands=[&](GraphQueue queue,uint32_t index)->Commands&{
            auto& slots=f.batchCommands[QI(queue)];while(slots.size()<=index){auto commands=std::make_unique<Commands>();CommandsInit(*commands,queue==GraphQueue::Copy?D3D12_COMMAND_LIST_TYPE_COPY:queue==GraphQueue::Compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT);slots.push_back(std::move(commands));}return *slots[index];
        };
        const auto progress=coordinator.progress?
            coordinator.progress:std::make_shared<GpuSubmissionProgress>();


        for(const auto& bound:publications)if(bound.deferred){
            if(bound.publication.submission)bound.publication.submission->Observe(progress);
            for(const auto& group:bound.publication.groups)
                if(group.submission)group.submission->Observe(progress);
        }
        auto canSubmit=[&](const GraphPass& pass)->bool{
            for(const auto& wait:pass.externalWaits)
                for(const auto& bound:publications)if(bound.deferred){
                    const auto& pub=bound.publication;
                    if(wait.fence==pub.producerFence.Get()){
                        if(wait.value>pub.producerValue)Fail("GPU publication wait exceeds reserved signal");
                        if(pub.submission){
                            if(!pub.submission->Enqueued(wait.value)){
                                if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                    Fail("GPU producer completed without enqueueing publication signal");
                                return false;
                            }
                        }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                    }
                    for(const auto& group:pub.groups)
                        if(wait.fence==group.producerFence.Get()){
                            if(wait.value>group.producerValue)Fail("GPU group wait exceeds reserved signal");
                            if(group.submission){
                                if(!group.submission->Enqueued(wait.value)){
                                    if(coordinator.fallbackSubmitted&&coordinator.fallbackSubmitted())
                                        Fail("GPU producer completed without enqueueing group signal");
                                    return false;
                                }
                            }else if(!coordinator.fallbackSubmitted||!coordinator.fallbackSubmitted())return false;
                        }
                }
            return true;
        };
        GpuBatchTimer timer{};
        bool recordTiming=false;
        uint32_t dlssBatch=Invalid;
        if(measureRender){
            std::array<uint32_t,GraphQueueCount> counts{};
            for(const auto& batch:frameGraph.executionBatches){
                auto& count=counts[QI(batch.queue)];
                if(dlssDiagnosticEnabled&&useDlss&&batch.queue==GraphQueue::Direct&&
                   std::find(batch.nodes.begin(),batch.nodes.end(),dlssEvaluationNode)!=batch.nodes.end())
                    dlssBatch=count;
                ++count;
            }
            const uint32_t slot=uint32_t(frameNumber%GpuBatchTimer::Slots);
            bool supported=!timerSamples[slot].pending;
            for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                if(counts[q]>GpuBatchTimer::MaxBatches||!timerQueues[q].queries||!timerQueues[q].readback)supported=false;
            }
            if(supported){
                recordTiming=true;timer.slot=slot;timer.counts=counts;
                for(uint32_t q=0;q<GraphQueueCount;++q)if(counts[q]){
                    timer.queries[q]=timerQueues[q].queries.Get();
                    timer.readbacks[q]=timerQueues[q].readback.Get();
                }
            }
        }
        frameGraph.Execute(*jobs,batchCommands,canSubmit,progress,recordTiming?&timer:nullptr);
if(coreSceneWork)for(uint32_t dynamicIndex:f.deformationWork.Active())f.dynamicGeometryRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];if(passPlan.rayQueries)for(uint32_t dynamicIndex:f.blasWork.Active())f.dynamicBlasRevision[dynamicIndex]=f.pendingDynamicRevision[dynamicIndex];f.queueFences=frameGraph.LastFences();f.fenceValue=frameGraph.Signal(presentNode);if(!f.fenceValue)Fail("Frame graph present pass missing fence");


        if(coreSceneWork&&bvhSortNeeded){
            f.lightBvhSorted=true;
            f.lightBvhPendingSort=false;
            f.lightBvhSortedCount=f.lightCount;
            f.lightBvhSortSnapshot=f.lightData;
            f.lightBvhLastSortFrame=frameNumber;
        }

        if(copyStatic)sceneCache.Commit(frameSlot);

        for(const auto& transfer:f.tileRunTransfers){
            auto it=residentTileGpu.find(transfer.key);
            if(it!=residentTileGpu.end()&&
               it->second.runRevision==transfer.revision&&
               it->second.runBuffer.Get()==transfer.buffer.Get())
                it->second.runsDirty=false;
        }
        if(coreSceneWork&&(!rayDirty||passPlan.rayWork))rayScene.Commit(frameSlot);
        if(coreSceneWork&&tileCatalog){
            for(const auto& transfer:f.tileTransfers)
                tileCatalog->CommitTileChanges(transfer.key,
                    transfer.topologyGeneration,transfer.updates);
            if(passPlan.rayWork)
                for(const auto& ack:rayTileAcks)
                    tileCatalog->CommitRayTileChanges(frameSlot,ack.key,
                        ack.topologyGeneration,ack.updates);
        }
        if(passPlan.rayWork&&f.rayMapBuild)
            f.committedRayMapSegments=f.rayMapSegments;
        if(recordTiming){
            auto& sample=timerSamples[timer.slot];
            sample.slot=timer.slot;sample.counts=timer.counts;sample.fences=f.queueFences;
            sample.cpuMs=cpuPrepareMs+float(QpcSeconds(QpcCounter()-cpuRenderStart)*1000.0);
            sample.diagnosticFrame=dlssDiagnosticEnabled?xr.input.frameId:0;
            sample.dlssBatch=dlssBatch;
            sample.dlssScale=useDlss?float(renderWidth)/float(width):0.f;
            sample.dlssCpuMs=dlssCpuMs;
            if(dlssDiagnosticEnabled){
                std::array<uint32_t,GraphQueueCount> ordinal{};
                for(const auto& batch:frameGraph.executionBatches){
                    const auto queue=QI(batch.queue);
                    auto& name=sample.batchNames[queue][ordinal[queue]++];
                    const auto& first=frameGraph.Node(batch.nodes.front()).name;
                    const auto& last=frameGraph.Node(batch.nodes.back()).name;
                    if(batch.nodes.size()==1u)std::snprintf(name.data(),name.size(),"%s",first.c_str());
                    else std::snprintf(name.data(),name.size(),"%s..%s",first.c_str(),last.c_str());
                }
            }
            sample.pending=true;
        }
        if(runCut){
            ++cutStepSerial;
            cutFlightRemaining=std::max(0.f,cutFlightRemaining-cutDt);
        }
        if(refreshVrs){
            vrsFov=xr.fov;
            vrsWidth=renderWidth;
            vrsHeight=renderHeight;
            vrsImageReady=true;
        }
        dlss.historyReady=dlssPresented;
        if(useDlss){
            dlss.lastUseFence=f.fenceValue;
            if(dlssPresented){
                dlss.lastRenderSize={renderWidth,renderHeight};
                for(uint32_t eye=0;eye<dlss.eyes.size();++eye)dlss.previousVP[eye]=xr.vp[eye];
            }else{
                dlss.requested=0.f;
                dlss.available=false;
            }
        }
        cutShadowsPending=cutStageNode!=UINT32_MAX;
        lastCutShadowSlots=cutShadowSlots;
        residency.Submitted(SubmittedFences());releasePublications(false);managedMemory.EndFrame(SubmittedFences());
        if(coreSceneWork)for(uint32_t slot=0;slot<ShadowSlots;slot++)if(f.shadowDirty&(1u<<slot))shadowCache[slot]={f.shadowIDs[slot],f.shadowKeys[slot],true};frameNumber++;historyReady=coreSceneWork&&passPlan.reflections;endDomains();active=nullptr;xr.submitted=true;
        const int64_t hudNow=QpcCounter();
        if(!hudStart)hudStart=hudNow;
        else{
            ++hudFrames;
            if(hudNow-hudStart>=QpcFrequency()){
                hudFps=float(double(hudFrames)*double(QpcFrequency())/double(hudNow-hudStart));
                hudFrames=0;hudStart=hudNow;
            }
        }
    }


}

