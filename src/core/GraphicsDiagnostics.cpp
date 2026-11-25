#include "GraphicsInternal.h"
#include <cmath>

namespace OASIS {
    void Graphics::SetCutDiagnostic(bool enabled){impl_->cutDiagnosticEnabled=enabled;}

    size_t Graphics::ReadCutDiagnostics(std::array<CutDiagnosticSample,6>& samples){
        if(!impl_->cutDiagnosticEnabled)return 0;
        const auto completed=impl_->direct.fence->GetCompletedValue();
        size_t count=0;
        for(auto& frame:impl_->frames){
            if(!frame.cutDiagnosticFence||frame.cutDiagnosticFence>completed)continue;
            constexpr size_t stride=Impl::CutDiagnosticStride/sizeof(uint32_t);
            const D3D12_RANGE range{0,2u*Impl::CutDiagnosticStride};
            void* mapped{};
            Hr(frame.cutDiagnosticReadback->Map(0,&range,&mapped),"map cut diagnostic");
            const auto* data=static_cast<const uint32_t*>(mapped);
            for(uint32_t kind=0;kind<2u;++kind){
                if((frame.cutDiagnosticMask&(1u<<kind))==0u)continue;
                const auto* words=data+kind*stride;
                if(frame.cutDiagnosticPoolCapacity==impl_->cutCachePoolCapacity&&
                   words[9]==Graphics::Impl::CutPoolFull)impl_->cutCacheNeedsGrowth=true;
                if(frame.cutDiagnosticScratchCapacity==impl_->cutCapacity&&
                   words[9]==Graphics::Impl::CutScratchFull)impl_->cutScratchNeedsGrowth=true;
                if(words[4]==0u)continue;
                // Snapshot layout: output control, cache work, published state.
                samples[count++]={frame.cutDiagnosticFrame,words[18],words[1],words[9],
                                  words[0],words[2],words[3],words[4],
                                  words[12],words[13],words[14],words[15],kind==1u};
            }
            const D3D12_RANGE noWrite{0,0};
            frame.cutDiagnosticReadback->Unmap(0,&noWrite);
            frame.cutDiagnosticFence=0;
        }
        std::sort(samples.begin(),samples.begin()+count,[](const auto& a,const auto& b){
            return a.frame!=b.frame?a.frame<b.frame:a.laser<b.laser;
        });
        return count;
    }

    size_t Graphics::ReadDepthDiagnostics(std::array<DepthDiagnosticSample,6>& samples){
        const auto completed=impl_->compute.fence->GetCompletedValue();
        if(completed==UINT64_MAX)return 0;
        size_t count=0;
        for(auto& frame:impl_->frames){
            if(count==samples.size())break;
            if(!frame.depthSampleFence||frame.depthSampleFence>completed)continue;
            const D3D12_RANGE range{0,Impl::DepthStatWords*sizeof(uint32_t)},noWrite{0,0};
            void* mapped{};
            if(SUCCEEDED(frame.depthStatsReadback->Map(0,&range,&mapped))&&mapped){
                auto& sample=samples[count++];sample={};sample.frame=frame.depthSampleFrame;
                const auto* words=static_cast<const uint32_t*>(mapped);
                for(uint32_t counter=0;counter<sample.counts.size();++counter)
                    for(uint32_t stripe=0;stripe<64u;++stripe)
                        sample.counts[counter]+=words[counter*64u+stripe];
                frame.depthStatsReadback->Unmap(0,&noWrite);
            }
            frame.depthSampleFence=0;
        }
        return count;
    }

    size_t Graphics::ReadOpticalDiagnostics(std::array<OpticalDiagnosticSample,6>& samples){
        const auto completed=impl_->direct.fence->GetCompletedValue();
        size_t count=0;
        for(auto& frame:impl_->frames){
            if(count==samples.size())break;
            if(!frame.opticalSampleFence||frame.opticalSampleFence>completed)continue;
            const D3D12_RANGE range{0,Impl::OpticalStatWords*sizeof(uint32_t)},noWrite{0,0};
            void* mapped{};
            if(SUCCEEDED(frame.opticalStatsReadback->Map(0,&range,&mapped))){
                auto& sample=samples[count++];sample={};sample.frame=frame.opticalSampleFrame;
                const auto* words=static_cast<const uint32_t*>(mapped);
                for(uint32_t counter=0;counter<sample.counts.size();++counter)
                    for(uint32_t stripe=0;stripe<64u;++stripe)
                        sample.counts[counter]+=words[counter*64u+stripe];
                frame.opticalStatsReadback->Unmap(0,&noWrite);
            }
            frame.opticalSampleFence=0;
        }
        std::sort(samples.begin(),samples.begin()+count,[](const auto& a,const auto& b){return a.frame<b.frame;});
        return count;
    }

    void Graphics::Impl::CollectGpuTimings(){
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
            SphereGpuTimings timings;
            for(uint32_t q=0;q<GraphQueueCount;++q){
                if(!sample.counts[q])continue;
                auto& t=timerQueues[q];
                const SIZE_T begin=SIZE_T(sample.slot)*GpuBatchTimer::QueriesPerSlot*sizeof(UINT64);
                const uint32_t extra=q==QI(GraphQueue::Direct)?GpuBatchTimer::DirectPassQueries(sample.directPassMask):0u;
                const D3D12_RANGE range{begin,begin+SIZE_T(sample.counts[q]*2u+extra)*sizeof(UINT64)};
                void* mapped=nullptr;
                if(FAILED(t.readback->Map(0,&range,&mapped))||!mapped){valid=false;continue;}
                const auto* ticks=static_cast<const UINT64*>(mapped)+sample.slot*GpuBatchTimer::QueriesPerSlot;
                UINT64 elapsed=0,peakTicks=0,firstTick=~UINT64(0),lastTick=0;
                for(uint32_t n=0;n<sample.counts[q];++n){
                    if(ticks[n*2u+1]<ticks[n*2u]){valid=false;break;}
                    const UINT64 duration=ticks[n*2u+1]-ticks[n*2u];
                    elapsed+=duration;
                    firstTick=std::min(firstTick,ticks[n*2u]);
                    lastTick=std::max(lastTick,ticks[n*2u+1]);
                    if(diagnosticSample){
                        if(duration>peakTicks){
                            peakTicks=duration;
                            peakBatch[q]=n;
                        }
                        if(q==QI(GraphQueue::Direct)&&n==sample.dlssBatch)
                            dlssGpuMs=float(1000.0*double(duration)/double(t.frequency));
                    }
                }
                if(extra){
                    const auto* pass=ticks+sample.counts[q]*2u;
                    for(uint32_t i=0,j=0;i<timings.directPassMs.size();++i){
                        if(!(sample.directPassMask&(1u<<i)))continue;
                        if(pass[j+1]>=pass[j])timings.directPassMs[i]=
                            float(1000.0*double(pass[j+1]-pass[j])/double(t.frequency));
                        j+=2u;
                    }
                }
                D3D12_RANGE empty{0,0};t.readback->Unmap(0,&empty);
                const double elapsedMs=1000.0*double(elapsed)/double(t.frequency);
                if(valid&&lastTick>=firstTick&&t.frequency){
                    const float span=float(1000.0*double(lastTick-firstTick)/double(t.frequency));
                    if(q==QI(GraphQueue::Direct))timings.directSpanMs=span;
                    if(q==QI(GraphQueue::Compute))timings.computeSpanMs=span;
                }
                gpuMs=std::max(gpuMs,elapsedMs);
                if(diagnosticSample||sample.directPassMask){
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
                if(sample.directPassMask&&sphereGpuTimingCount<sphereGpuTimings.size()){
                    timings.frame=sample.diagnosticFrame;
                    timings.directMs=queueMs[QI(GraphQueue::Direct)];
                    timings.computeMs=queueMs[QI(GraphQueue::Compute)];
                    sphereGpuTimings[sphereGpuTimingCount++]=timings;
                }
                const float ms=std::max(sample.cpuMs,float(gpuMs));
                auto& cost=frameCosts[size_t(sample.path)];
                if(sample.renderFrame>cost.frame&&ms>0&&std::isfinite(ms)){
                    cost.ms=cost.ms>0.f?cost.ms*.8f+ms*.2f:ms;
                    cost.frame=sample.renderFrame;
                }
            }
            sample.pending=false;
        }
    }

}
