#include "GraphicsInternal.h"

namespace lab {
    void Graphics::Impl::FinishTextureReadbacks(){for(auto& slot:textureReadbacks)if(slot->recorded&&!slot->fenceValue){slot->fenceValue=direct.serial;slot->recorded=false;}}

uint32_t Graphics::RequestCapture(uint64_t owner,const LabCaptureRequestV1& r,LabU64& out){return impl_->capture.Request(owner,r,out)?1u:0u;}

uint32_t Graphics::CaptureStatus(uint64_t owner,LabCaptureStatusV1& s)const{return impl_->capture.Status(owner,s)?1u:0u;}

void Graphics::ClearCapture(uint64_t owner){impl_->capture.Clear(owner);if(impl_->captureTicket.owner==owner)impl_->captureTicket={};}

void Graphics::PumpCaptures(){
    impl_->capture.Pump();
    if(impl_->captureTicket.request&&QpcNowSeconds()-impl_->captureTicket.queuedAt>30){impl_->capture.Failed(impl_->captureTicket,"NO RENDERED EYE AVAILABLE");impl_->captureTicket={};}
    for(auto it=impl_->captureReadbacks.begin();it!=impl_->captureReadbacks.end();){TextureReadbackResult r;auto state=PollTextureReadback(it->readback,r);if(!state){
            if(QpcNowSeconds()-it->submittedAt>30){impl_->capture.Failed(it->ticket,"GPU READBACK TIMEOUT");ReleaseTextureReadback(it->readback);it=impl_->captureReadbacks.erase(it);}
            else ++it;
            continue;
        }
        if(state==1&&r.tightRowBytes==r.width*4){std::vector<unsigned char> pixels(size_t(r.tightRowBytes)*r.height);for(uint32_t y=0;y<r.height;y++)std::memcpy(pixels.data()+size_t(y)*r.tightRowBytes,static_cast<const unsigned char*>(r.bytes)+size_t(y)*r.rowPitch,r.tightRowBytes);
            bool bgra=r.format==DXGI_FORMAT_B8G8R8A8_UNORM||r.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;impl_->capture.Encode(it->ticket,r.frameId,r.width,r.height,bgra,std::move(pixels));
        }else impl_->capture.Failed(it->ticket,"GPU CAPTURE READBACK FAILED");ReleaseTextureReadback(it->readback);it=impl_->captureReadbacks.erase(it);
    }
}

uint64_t Graphics::RequestTextureReadback(ID3D12Resource* source,uint32_t subresource,uint64_t frameId,void** context){
    if(context)*context=nullptr;if(!source||!context)return 0;auto d=source->GetDesc();
    // OpenXR creates typeless storage for typed sRGB swapchains. Preserve its
    // format in the copy footprint, and type only the returned CPU pixel data.
    auto pixelFormat=d.Format;
    if(pixelFormat==DXGI_FORMAT_R8G8B8A8_TYPELESS)pixelFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
    else if(pixelFormat==DXGI_FORMAT_B8G8R8A8_TYPELESS)pixelFormat=DXGI_FORMAT_B8G8R8A8_UNORM;
    if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||d.SampleDesc.Count!=1||subresource>=uint32_t(d.MipLevels)*d.DepthOrArraySize||
       (pixelFormat!=DXGI_FORMAT_R8G8B8A8_UNORM&&pixelFormat!=DXGI_FORMAT_B8G8R8A8_UNORM&&pixelFormat!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&pixelFormat!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB))return 0;
    Impl::TextureReadback* slot=nullptr;auto completed=impl_->direct.fence->GetCompletedValue();
    for(auto& item:impl_->textureReadbacks)if(item->released&&(!item->fenceValue||completed>=item->fenceValue)&&!item->recorded){slot=item.get();break;}
    if(!slot){impl_->textureReadbacks.push_back(std::make_unique<Impl::TextureReadback>());slot=impl_->textureReadbacks.back().get();}
    if(slot->mapped){slot->buffer->Unmap(0,nullptr);slot->mapped=nullptr;}
    UINT rows{};UINT64 rowBytes{},bytes{};impl_->device->GetCopyableFootprints(&d,subresource,1,0,&slot->footprint,&rows,&rowBytes,&bytes);
    if(!slot->buffer||slot->capacity<bytes){
        slot->buffer.Reset();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(impl_->device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&slot->buffer)))){slot->released=true;return 0;}slot->capacity=bytes;
    }
    slot->source=source;slot->pixelFormat=pixelFormat;slot->handle=impl_->nextTextureReadback++;slot->frameId=frameId;slot->subresource=subresource;slot->tightRowBytes=uint32_t(rowBytes);slot->bytes=bytes;slot->fenceValue=0;slot->recorded=false;slot->released=false;slot->failed=false;*context=slot;return slot->handle;
}

void Graphics::RecordTextureReadback(void* context,ID3D12GraphicsCommandList7* commands){
    auto* slot=static_cast<Impl::TextureReadback*>(context);if(!slot||!commands||!slot->source)return;
    D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=slot->buffer.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=slot->footprint;
    D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=slot->source.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.SubresourceIndex=slot->subresource;
    commands->CopyTextureRegion(&dst,0,0,0,&src,nullptr);slot->recorded=true;
}

uint32_t Graphics::PollTextureReadback(uint64_t handle,TextureReadbackResult& out){
    out={};for(auto& item:impl_->textureReadbacks){auto& slot=*item;if(slot.handle!=handle||slot.released)continue;
        if(slot.failed||impl_->direct.fence->GetCompletedValue()==UINT64_MAX)return 2;
        if(!slot.fenceValue||impl_->direct.fence->GetCompletedValue()<slot.fenceValue)return 0;
        if(!slot.mapped){D3D12_RANGE read{0,size_t(slot.bytes)};if(FAILED(slot.buffer->Map(0,&read,&slot.mapped))){slot.failed=true;return 2;}}
        out={slot.footprint.Footprint.Width,slot.footprint.Footprint.Height,uint32_t(slot.pixelFormat),slot.footprint.Footprint.RowPitch,slot.tightRowBytes,slot.bytes,slot.frameId,slot.mapped};return 1;
    }return 2;
}

void Graphics::PreparePerformanceCapture(){impl_->performanceProfiler.Prepare(impl_->device.Get(),impl_->copy,impl_->compute,impl_->direct);}

void Graphics::BeginPerformanceCapture(){impl_->performanceProfiler.Begin(impl_->copy,impl_->compute,impl_->direct);}

void Graphics::EndPerformanceCapture(){impl_->performanceProfiler.End();}

void Graphics::CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out){
    std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
    CollectPerformanceCapture(out,queues,gaps);
}

void Graphics::CollectPerformanceCapture(std::vector<GpuPerformancePassSample>& out,
    std::vector<GpuQueueFrameAnalysis>& queues,std::vector<GpuGapSample>& gaps){
    WaitIdle();impl_->performanceProfiler.Collect(out);queues.clear();gaps.clear();
    std::map<std::pair<uint64_t,PerformanceQueue>,std::vector<const GpuPerformancePassSample*>> groups;
    for(const auto& sample:out)groups[{sample.frameId,sample.queue}].push_back(&sample);
    const double toMs=1000.0/double(impl_->performanceProfiler.qpcFrequency);
    for(auto& [key,passes]:groups){
        std::stable_sort(passes.begin(),passes.end(),[](const auto* a,const auto* b){return a->startQpc<b->startQpc;});
        const double spanMs=(passes.back()->endQpc-passes.front()->startQpc)*toMs;
        double workMs=0;for(const auto* pass:passes)workMs+=(pass->endQpc-pass->startQpc)*toMs;
        queues.push_back({key.first,key.second,spanMs,workMs,std::max(0.0,spanMs-workMs),spanMs>0?workMs/spanMs*100.0:0.0});
        for(size_t i=1;i<passes.size();++i){
            double gapMs=(passes[i]->startQpc-passes[i-1]->endQpc)*toMs;
            if(gapMs>.05){auto boundary=passes[i]->batchIndex==passes[i-1]->batchIndex?GpuGapSample::Boundary::IntraBatch:passes[i]->crossQueueWait?GpuGapSample::Boundary::CrossQueue:GpuGapSample::Boundary::InterBatch;gaps.push_back({key.first,key.second,passes[i-1]->name,passes[i]->name,gapMs,boundary});}
        }
    }
}

GraphicsPerformanceInfo Graphics::PerformanceInfo() const {
    const auto& description=impl_->adapterDescription;
    int size=WideCharToMultiByte(CP_UTF8,0,description.Description,-1,nullptr,0,nullptr,nullptr);
    std::string name(size>0?size_t(size):0,'\0');
    if(size>0){WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name.data(),size,nullptr,nullptr);name.pop_back();}
    return {std::move(name),uint64_t(description.DedicatedVideoMemory),impl_->renderWidth,impl_->renderHeight,impl_->viewCount,impl_->copyQueueTimestamps,impl_->performanceProfiler.droppedPasses};
}

void Graphics::ReleaseTextureReadback(uint64_t handle){for(auto& item:impl_->textureReadbacks)if(item->handle==handle){item->released=true;return;}}
}
