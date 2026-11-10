#include "GraphicsInternal.h"
namespace lab::graphics_internal {
GpuSubresourceShape TextureShape(ID3D12Resource* resource){
    auto desc=resource->GetDesc();if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE1D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D&&desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE3D)Fail("Texture declaration requires a texture resource");
    ComPtr<ID3D12Device> device;Hr(resource->GetDevice(IID_PPV_ARGS(&device)),"Texture format device");D3D12_FEATURE_DATA_FORMAT_INFO format{desc.Format,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,&format,sizeof(format)),"Texture plane count");
    return {desc.MipLevels,desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D?1u:uint32_t(desc.DepthOrArraySize),format.PlaneCount};
}
D3D12_BARRIER_SUBRESOURCE_RANGE SingleSubresource(uint32_t index){D3D12_BARRIER_SUBRESOURCE_RANGE range{};range.IndexOrFirstMipLevel=index;return range;}

    GraphResourceState& PassGraph::State(ID3D12Resource* resource,uint32_t subresource){
        for(auto& state:resources_)if(state.resource==resource&&state.subresource==subresource)return state;
        resources_.push_back({});auto& state=resources_.back();state.resource=resource;state.subresource=subresource;return state;
    }

    bool PassGraph::Self(const GraphStamp& stamp,const GraphPass& pass){return stamp.valid&&stamp.queue==pass.queue&&stamp.fence==pass.signal;}

    void PassGraph::Merge(GraphPass& pass,const GraphStamp& stamp){
        if(!stamp.valid||Self(stamp,pass)||stamp.fence<=completed_[QI(stamp.queue)])return;for(uint32_t i=0;i<GraphQueueCount;i++)pass.required[i]=std::max(pass.required[i],stamp.frontier[i]);uint32_t producer=QI(stamp.queue);if(stamp.fence>pass.coverFence[producer]){pass.coverFence[producer]=stamp.fence;pass.cover[producer]=stamp.frontier;}
    }

    GraphStamp PassGraph::PendingStamp(const GraphPass& pass)const{GraphStamp stamp{};stamp.valid=true;stamp.queue=pass.queue;stamp.fence=pass.signal;return stamp;}

    void PassGraph::AutoBufferBarrier(GraphPass& pass,GraphResourceState& state,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers){
        // A compiled batch is one ExecuteCommandLists scope. Queue/fence dependencies
        // establish completion of prior batches; accesses from earlier logical passes in
        // this batch retain their real sync/access state and use normal GPU barriers.
        if(sync==D3D12_BARRIER_SYNC_NONE&&access==D3D12_BARRIER_ACCESS_NO_ACCESS)return;
        const bool local=Self(state.pending,pass);
        CompiledBarrier compiled{};auto& b=compiled.buffer;b.pResource=state.resource;b.Offset=0;b.Size=UINT64_MAX;
        b.SyncBefore=local?state.pendingBarrier.sync:D3D12_BARRIER_SYNC_NONE;b.SyncAfter=sync;
        b.AccessBefore=local?state.pendingBarrier.access:D3D12_BARRIER_ACCESS_NO_ACCESS;b.AccessAfter=access;barriers.push_back(compiled);
    }

    void PassGraph::CompileTextureBarrier(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,const D3D12_BARRIER_SUBRESOURCE_RANGE* range){
        CompiledBarrier compiled{};compiled.type=D3D12_BARRIER_TYPE_TEXTURE;auto& b=compiled.texture;b.SyncBefore=beforeSync;b.SyncAfter=afterSync;b.AccessBefore=beforeAccess;b.AccessAfter=afterAccess;b.LayoutBefore=beforeLayout;b.LayoutAfter=afterLayout;b.pResource=texture.resource.Get();if(range)b.Subresources=*range;else{b.Subresources.IndexOrFirstMipLevel=0xffffffffu;b.Subresources.NumMipLevels=0;}barriers.push_back(compiled);
        LogEvent(traceFrame,range?"gpu.graph_texture_subresource_barrier":"gpu.graph_texture_barrier","pass=%s resource=%p layout=%u->%u sync=%llu->%llu access=%llu->%llu",pass.name,static_cast<void*>(texture.resource.Get()),unsigned(beforeLayout),unsigned(afterLayout),static_cast<unsigned long long>(beforeSync),static_cast<unsigned long long>(afterSync),static_cast<unsigned long long>(beforeAccess),static_cast<unsigned long long>(afterAccess));
    }

    void PassGraph::RecordTextureState(GraphPass& pass,GraphResourceState& state,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access){
        GraphBarrierState actual{};actual.valid=true;actual.texture=true;actual.queue=pass.queue;actual.sync=sync;actual.access=access;actual.layout=layout;Pending(pass,state,actual,&texture);
    }

    void PassGraph::Hazard(GraphPass& pass,GraphResourceState& state,GraphMode mode){
        if(mode==GraphMode::Read){Merge(pass,state.writer);state.readers[QI(pass.queue)]=PendingStamp(pass);return;}
        Merge(pass,state.writer);for(const auto& reader:state.readers)Merge(pass,reader);state.readers={};state.writer=PendingStamp(pass);
    }

    void PassGraph::Pending(GraphPass& pass,GraphResourceState& state,const GraphBarrierState& finalState,TextureResource* texture){state.pending=PendingStamp(pass);state.pendingBarrier=finalState;state.pendingTexture=texture;}

void PassGraph::Initialize(QueueState& copy,QueueState& compute,QueueState& direct){queues_={&copy,&compute,&direct};}

    void PassGraph::BeginFrame(){for(uint32_t i=0;i<GraphQueueCount;i++){planned_[i]=queues_[i]->serial;completed_[i]=queues_[i]->fence->GetCompletedValue();queueFrontier_[i][i]=std::max(queueFrontier_[i][i],queues_[i]->serial);}}

    void PassGraph::Forget(ID3D12Resource* resource){if(!resource)return;std::erase_if(resources_,[resource](const auto& state){return state.resource==resource;});}

    GraphPass PassGraph::BeginPass(const char* name,GraphQueue queue){GraphPass pass{};pass.name=name;pass.queue=queue;pass.signal=++planned_[QI(queue)];pass.required=queueFrontier_[QI(queue)];LogEvent(traceFrame,"gpu.graph_pass","name=%s queue=%s signal=%llu",name,queues_[QI(queue)]->name,static_cast<unsigned long long>(pass.signal));return pass;}

    void PassGraph::Buffer(GraphPass& pass,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,std::vector<CompiledBarrier>& barriers){
        if(!resource)return;auto& state=State(resource);Hazard(pass,state,mode);AutoBufferBarrier(pass,state,firstSync,firstAccess,barriers);GraphBarrierState final{};final.valid=true;final.queue=pass.queue;final.sync=finalSync;final.access=finalAccess;Pending(pass,state,final,nullptr);
    }

    void PassGraph::TextureRange(GraphPass& pass,ID3D12Resource* resource,TextureResource* canonical,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers,GpuTextureSubresources requested){
        if(!resource)return;auto shape=TextureShape(resource);GpuTextureSubresources range;if(!GpuNormalizeSubresources(shape,requested,range))Fail("Invalid domain texture subresource range");
        GpuForEachSubresource(shape,range,[&](uint32_t index,uint32_t,uint32_t,uint32_t){
            auto& state=State(resource,index);bool local=Self(state.pending,pass);
            auto prior=local?state.pendingBarrier:state.barrier;auto before=prior.valid?prior.layout:initialLayout;
            auto hazard=mode==GraphMode::Read&&before!=layout?GraphMode::ReadWrite:mode;Hazard(pass,state,hazard);
            D3D12_TEXTURE_BARRIER bar{};bar.pResource=resource;bar.Subresources=SingleSubresource(index);bar.LayoutBefore=before;bar.LayoutAfter=layout;
            bar.SyncBefore=local?prior.sync:D3D12_BARRIER_SYNC_NONE;bar.AccessBefore=local?prior.access:D3D12_BARRIER_ACCESS_NO_ACCESS;bar.SyncAfter=sync;bar.AccessAfter=access;
            CompiledBarrier compiled{};compiled.type=D3D12_BARRIER_TYPE_TEXTURE;compiled.texture=bar;barriers.push_back(compiled);
            GraphBarrierState final{};final.valid=final.texture=true;final.queue=pass.queue;final.sync=sync;final.access=access;final.layout=layout;Pending(pass,state,final,canonical);
            LogEvent(traceFrame,"gpu.graph_texture_subresource_barrier","pass=%s resource=%p subresource=%u layout=%u->%u",pass.name,resource,index,unsigned(before),unsigned(layout));
        });
        if(canonical&&range.firstMip==0&&range.mipCount==shape.mips&&range.firstArraySlice==0&&range.arraySliceCount==shape.arraySlices&&range.firstPlane==0&&range.planeCount==shape.planes)canonical->layout=layout;
    }

    void PassGraph::Dependency(GraphPass& pass,ID3D12Resource* resource,GraphMode mode){
        if(!resource)return;if(resource->GetDesc().Dimension==D3D12_RESOURCE_DIMENSION_BUFFER){Hazard(pass,State(resource),mode);return;}
        auto shape=TextureShape(resource);GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){Hazard(pass,State(resource,index),mode);});
    }

    void PassGraph::Dependency(GraphPass& pass,const GraphStamp& producer){Merge(pass,producer);}

    void PassGraph::ExternalWait(GraphPass& pass,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;for(auto& wait:pass.externalWaits)if(wait.fence==fence){wait.value=std::max(wait.value,value);return;}pass.externalWaits.push_back({fence,value});}

    void PassGraph::ExternalSignal(GraphPass& pass,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;for(auto& signal:pass.externalSignals)if(signal.fence==fence){signal.value=std::max(signal.value,value);return;}pass.externalSignals.push_back({fence,value});}

    void PassGraph::TransitionTexture(GraphPass& pass,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,std::vector<CompiledBarrier>& barriers){
        auto shape=TextureShape(texture.resource.Get());GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);
        GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingTexture!=&texture)Fail("Texture transition outside declared graph pass");auto prior=state.pendingBarrier;auto range=SingleSubresource(index);CompileTextureBarrier(pass,texture,prior.layout,prior.sync,prior.access,layout,sync,access,barriers,&range);RecordTextureState(pass,state,texture,layout,sync,access);});
        texture.layout=layout;
    }

    void PassGraph::TransitionTextureSubresource(GraphPass& pass,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& requested,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT afterLayout,D3D12_BARRIER_SYNC afterSync,D3D12_BARRIER_ACCESS afterAccess,std::vector<CompiledBarrier>& barriers,bool completesWholeTexture){
        auto shape=TextureShape(texture.resource.Get());GpuTextureSubresources range;
        if(!requested.NumMipLevels){if(requested.IndexOrFirstMipLevel==UINT32_MAX){GpuNormalizeSubresources(shape,{},range);}else{uint32_t index=requested.IndexOrFirstMipLevel;uint32_t count=shape.mips*shape.arraySlices;if(index>=count*shape.planes)Fail("Texture subresource index");range={index%shape.mips,1,(index/shape.mips)%shape.arraySlices,1,index/count,1};}}
        else if(!GpuNormalizeSubresources(shape,{requested.IndexOrFirstMipLevel,requested.NumMipLevels,requested.FirstArraySlice,requested.NumArraySlices,requested.FirstPlane,requested.NumPlanes},range))Fail("Texture subresource transition range");
        GpuForEachSubresource(shape,range,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingTexture!=&texture)Fail("Texture subresource transition outside declared graph pass");if(state.pendingBarrier.layout!=beforeLayout)Fail("Texture subresource before-layout mismatch");auto native=SingleSubresource(index);CompileTextureBarrier(pass,texture,beforeLayout,beforeSync,beforeAccess,afterLayout,afterSync,afterAccess,barriers,&native);RecordTextureState(pass,state,texture,afterLayout,afterSync,afterAccess);});
        if(completesWholeTexture){GpuTextureSubresources all;GpuNormalizeSubresources(shape,{},all);GpuForEachSubresource(shape,all,[&](uint32_t index,uint32_t,uint32_t,uint32_t){auto& state=State(texture.resource.Get(),index);if(!Self(state.pending,pass)||state.pendingBarrier.layout!=afterLayout)Fail("Whole-texture completion with mixed mip layouts");});texture.layout=afterLayout;}
    }

    void PassGraph::Seal(GraphPass& pass){
        if(pass.sealed)return;pass.sealed=true;auto frontier=pass.required;frontier[QI(pass.queue)]=pass.signal;for(auto& state:resources_){
            auto finish=[&](GraphStamp& stamp){if(Self(stamp,pass))stamp.frontier=frontier;};finish(state.writer);for(auto& reader:state.readers)finish(reader);
            if(Self(state.pending,pass)){state.barrier=state.pendingBarrier;state.pending={};state.pendingTexture=nullptr;}
        }
    }

    UINT64 PassGraph::FenceValue(GraphQueue queue)const{return queues_[QI(queue)]->serial;}

    bool PassGraph::CrossQueueWait(const GraphPass& pass)const{for(auto value:pass.queueWaits)if(value)return true;return !pass.externalWaits.empty();}

    void PassGraph::CompileSubmission(GraphPass& pass){
        Seal(pass);std::array<bool,GraphQueueCount> wait{};for(uint32_t producer=0;producer<GraphQueueCount;producer++)wait[producer]=producer!=QI(pass.queue)&&pass.required[producer]>queueFrontier_[QI(pass.queue)][producer];for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(wait[producer])for(uint32_t covering=0;covering<GraphQueueCount;covering++)if(covering!=producer&&wait[covering]&&pass.coverFence[covering]>=pass.required[covering]&&pass.cover[covering][producer]>=pass.required[producer]){wait[producer]=false;break;}
        for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(wait[producer])pass.queueWaits[producer]=pass.required[producer];
        auto frontier=pass.required;frontier[QI(pass.queue)]=pass.signal;for(uint32_t i=0;i<GraphQueueCount;i++)queueFrontier_[QI(pass.queue)][i]=std::max(queueFrontier_[QI(pass.queue)][i],frontier[i]);
    }

    UINT64 PassGraph::Submit(const GraphPass& pass,bool signalRequired){
        if(!pass.sealed||!pass.commands)Fail("Uncompiled batch submission");auto& queue=*queues_[QI(pass.queue)];
        for(uint32_t producer=0;producer<GraphQueueCount;producer++)if(pass.queueWaits[producer]){UINT64 needed=pass.queueWaits[producer];queue.Wait(*queues_[producer],needed);LogEvent(traceFrame,"gpu.graph_dependency","pass=%s consumer=%s producer=%s fence=%llu",pass.name,queue.name,queues_[producer]->name,static_cast<unsigned long long>(needed));}
        for(const auto& waitFence:pass.externalWaits){Hr(queue.queue->Wait(waitFence.fence,waitFence.value),"external GPU producer wait");LogEvent(traceFrame,"gpu.graph_external_wait","pass=%s queue=%s fence=%p value=%llu",pass.name,queue.name,static_cast<void*>(waitFence.fence),static_cast<unsigned long long>(waitFence.value));}
        pass.commands->Execute(queue);const UINT64 actual=pass.signal;
        if(signalRequired){if(actual<=queue.serial)Fail("Batch fence prediction");Hr(queue.queue->Signal(queue.fence.Get(),actual),"batch queue signal");queue.serial=actual;LogEvent(traceFrame,"gpu.signal","queue=%s fence=%llu",queue.name,static_cast<unsigned long long>(actual));}
        for(const auto& signalFence:pass.externalSignals){Hr(queue.queue->Signal(signalFence.fence,signalFence.value),"external GPU consumer signal");LogEvent(traceFrame,"gpu.graph_external_signal","pass=%s queue=%s fence=%p value=%llu",pass.name,queue.name,static_cast<void*>(signalFence.fence),static_cast<unsigned long long>(signalFence.value));}return actual;
    }


    void BurstGpuProfiler::Prepare(ID3D12Device* device,QueueState&,QueueState&,QueueState&){
        if(prepared)return;
        records.reserve(MaxFrames*MaxPassesPerFrame);
        LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)Fail("Performance QPC frequency");
        qpcFrequency=uint64_t(frequency.QuadPart);
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
        Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&options3,sizeof(options3)),"Performance timestamp support");
        queues[QI(GraphQueue::Copy)].supported=options3.CopyQueueTimestampQueriesSupported!=FALSE;
        queues[QI(GraphQueue::Compute)].supported=true;
        queues[QI(GraphQueue::Direct)].supported=true;
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];if(!q.supported)continue;
            D3D12_QUERY_HEAP_DESC queryDesc{};
            // Copy command lists require their dedicated timestamp heap type.
            queryDesc.Type=i==QI(GraphQueue::Copy)?D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queryDesc.Count=MaxQueries;
            Hr(device->CreateQueryHeap(&queryDesc,IID_PPV_ARGS(&q.queries)),"Performance query heap");
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=uint64_t(MaxQueries)*sizeof(uint64_t);desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;
            desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&q.readback)),"Performance readback");
        }
        prepared=true;
    }

    void BurstGpuProfiler::Begin(QueueState& copy,QueueState& compute,QueueState& direct){
        if(!prepared||active)Fail("Performance profiler not prepared or already active");
        records.clear();droppedPasses=0;
        std::array<QueueState*,GraphQueueCount> sources{&copy,&compute,&direct};
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];q.cursor=0;if(!q.supported)continue;
            Hr(sources[i]->queue->GetTimestampFrequency(&q.frequency),"Performance timestamp frequency");
            Hr(sources[i]->queue->GetClockCalibration(&q.calibrationGpu,&q.calibrationCpu),"Performance clock calibration");
            if(!q.frequency)Fail("Performance zero GPU frequency");
        }
        active=true;
    }

    void BurstGpuProfiler::End(){active=false;}

    void BurstGpuProfiler::PrepareFrame(uint32_t activePasses){nextRecord=uint32_t(records.size());records.resize(std::min<size_t>(records.size()+activePasses,MaxFrames*MaxPassesPerFrame));}

    BurstGpuProfiler::Token BurstGpuProfiler::ReservePass(GraphQueue queue){
        auto& q=queues[QI(queue)];if(!active||!q.supported)return {};
        // Never grow storage or address queries beyond the preallocated burst.
        if(q.cursor>MaxQueries-QueriesPerPass||nextRecord>=records.size()){++droppedPasses;return {};}
        uint32_t begin=q.cursor++,end=q.cursor++;
        return {queue,begin,end,nextRecord++,true};
    }

    void BurstGpuProfiler::FinishReservations(){records.resize(nextRecord);}

    void BurstGpuProfiler::BeginPass(Token token,uint64_t frameId,const char* name,ID3D12GraphicsCommandList7* commands,uint32_t batchIndex,uint32_t nodeIndex,bool crossQueueWait){
        if(!token.valid)return;auto& q=queues[QI(token.queue)];
        commands->EndQuery(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.begin);
        GraphQueue queue=token.queue;uint32_t begin=token.begin,end=token.end;
        Record r{};r.frameId=frameId;r.queue=queue;r.beginQuery=begin;r.endQuery=end;
        r.batchIndex=batchIndex;r.nodeIndex=nodeIndex;r.batchesPerQueue=batchesPerQueue;r.crossQueueWait=crossQueueWait;
        strncpy_s(r.name.data(),r.name.size(),name,_TRUNCATE);records[token.recordIndex]=r;
    }

    void BurstGpuProfiler::EndPass(Token token,ID3D12GraphicsCommandList7* commands){
        if(!token.valid)return;auto& q=queues[QI(token.queue)];
        commands->EndQuery(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.end);
        commands->ResolveQueryData(q.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,token.begin,QueriesPerPass,q.readback.Get(),uint64_t(token.begin)*sizeof(uint64_t));
    }

    void BurstGpuProfiler::Collect(std::vector<GpuPerformancePassSample>& out){
        out.clear();out.reserve(records.size());
        for(uint32_t i=0;i<GraphQueueCount;++i){
            auto& q=queues[i];if(!q.supported||!q.cursor)continue;
            D3D12_RANGE read{0,size_t(q.cursor)*sizeof(uint64_t)};void* mapped{};
            Hr(q.readback->Map(0,&read,&mapped),"Performance readback map");
            auto* timestamps=static_cast<const uint64_t*>(mapped);
            for(const auto& r:records)if(QI(r.queue)==i){
                const uint64_t begin=timestamps[r.beginQuery],end=timestamps[r.endQuery];
                const double scale=double(qpcFrequency)/double(q.frequency);
                const double startQpc=double(q.calibrationCpu)+(double(begin)-double(q.calibrationGpu))*scale;
                const double endQpc=double(q.calibrationCpu)+(double(end)-double(q.calibrationGpu))*scale;
                out.push_back({r.frameId,static_cast<PerformanceQueue>(i),r.name.data(),startQpc,endQpc,(double(end)-double(begin))*1000.0/double(q.frequency),r.batchIndex,r.nodeIndex,r.crossQueueWait,r.batchesPerQueue});
            }
            D3D12_RANGE written{0,0};q.readback->Unmap(0,&written);
        }
    }

    FrameGraphPlan::Compiled& FrameGraphPlan::Compile(){
        dependencies_.StructureKey(structureScratch_);
        ++compileUse_;for(auto& cached:compiled_)if(cached.structure==structureScratch_){if(!dependencies_.RestoreResolved(cached.order,cached.edges))Fail("Compiled frame graph restore");cached.lastUse=compileUse_;return cached;}
        Resolve();Compiled built;built.structure=structureScratch_;built.order=dependencies_.ResolvedOrder();built.edges=dependencies_.DependencyMatrix();built.lastUse=compileUse_;
        if(compiled_.size()<MaxCompiledPlans){compiled_.push_back(std::move(built));return compiled_.back();}
        auto victim=std::min_element(compiled_.begin(),compiled_.end(),[](const auto& a,const auto& b){return a.lastUse<b.lastUse;});*victim=std::move(built);return *victim;
    }

FrameGraphPlan::FrameGraphPlan(PassGraph& executor):executor_(&executor){nodes_.reserve(32);compiled_.reserve(MaxCompiledPlans);structureScratch_.reserve(512);}

    void FrameGraphPlan::SetDiagnostics(bool enabled){diagnostics_=enabled;}

    void FrameGraphPlan::BeginFrame(){nodes_.clear();dependencies_.BeginFrame();executionBatches.clear();lastFences_={};executionCompiled_=false;}

    uint32_t FrameGraphPlan::AddPass(const char* name,GraphQueue queue,uint64_t stableKey,std::function<void(const CompiledNode&,ID3D12GraphicsCommandList7*)> record){
        if(!name||!*name)Fail("Frame graph pass name");for(const auto& node:nodes_)if(node.stableKey==stableKey&&node.name==name)Fail("Duplicate frame graph pass");nodes_.push_back({});auto& node=nodes_.back();node.name=name;node.queue=queue;node.stableKey=stableKey;node.record=std::move(record);node.uses.reserve(16);auto index=dependencies_.AddPass(name,queue,stableKey);if(index!=nodes_.size()-1)Fail(dependencies_.ErrorMessage());return index;
    }

    FrameGraphNode& FrameGraphPlan::Node(uint32_t index){if(index>=nodes_.size())Fail("Frame graph node index");return nodes_[index];}

    void FrameGraphPlan::SetStableKey(uint32_t index,uint64_t key){Node(index).stableKey=key;if(!dependencies_.SetStableKey(index,key))Fail(dependencies_.ErrorMessage());}

    void FrameGraphPlan::Buffer(uint32_t node,ID3D12Resource* resource,GraphMode mode,D3D12_BARRIER_SYNC firstSync,D3D12_BARRIER_ACCESS firstAccess,D3D12_BARRIER_SYNC finalSync,D3D12_BARRIER_ACCESS finalAccess,uint32_t contentVersion){
        if(!resource)return;Node(node).uses.push_back({resource,nullptr,mode,false,D3D12_BARRIER_LAYOUT_COMMON,D3D12_BARRIER_LAYOUT_COMMON,firstSync,finalSync,firstAccess,finalAccess});
        if(!dependencies_.Buffer(node,resource,mode,contentVersion))Fail(dependencies_.ErrorMessage());
    }

    void FrameGraphPlan::Texture(uint32_t node,TextureResource* canonical,ID3D12Resource* resource,D3D12_BARRIER_LAYOUT initialLayout,GraphMode mode,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,GpuTextureSubresources requested,uint32_t contentVersion){
        if(!resource)return;auto shape=TextureShape(resource);GpuTextureSubresources normalized;if(!GpuNormalizeSubresources(shape,requested,normalized))Fail("Frame graph texture range");
        Node(node).uses.push_back({resource,canonical,mode,true,initialLayout,layout,sync,sync,access,access,normalized});
        if(!dependencies_.Texture(node,resource,mode,shape,normalized,contentVersion))Fail(dependencies_.ErrorMessage());
    }

    void FrameGraphPlan::ExternalWait(uint32_t node,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;auto& waits=Node(node).waits;for(auto& wait:waits)if(wait.fence==fence){wait.value=std::max(wait.value,value);return;}waits.push_back({fence,value});}

    void FrameGraphPlan::TransitionTexture(uint32_t node,TextureResource& texture,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool during){
        FrameGraphTransition transition{};transition.texture=&texture;transition.layout=layout;transition.sync=sync;transition.access=access;transition.during=during;Node(node).transitions.push_back(transition);
    }

    void FrameGraphPlan::TransitionTextureSubresource(uint32_t node,TextureResource& texture,const D3D12_BARRIER_SUBRESOURCE_RANGE& range,D3D12_BARRIER_LAYOUT beforeLayout,D3D12_BARRIER_SYNC beforeSync,D3D12_BARRIER_ACCESS beforeAccess,D3D12_BARRIER_LAYOUT layout,D3D12_BARRIER_SYNC sync,D3D12_BARRIER_ACCESS access,bool completesWholeTexture){
        FrameGraphTransition transition{};transition.texture=&texture;transition.layout=layout;transition.sync=sync;transition.access=access;transition.during=transition.subresource=true;transition.completesWholeTexture=completesWholeTexture;transition.range=range;transition.beforeLayout=beforeLayout;transition.beforeSync=beforeSync;transition.beforeAccess=beforeAccess;Node(node).transitions.push_back(transition);
    }

    void FrameGraphPlan::ExternalSignal(uint32_t node,ID3D12Fence* fence,UINT64 value){if(!fence||!value)return;auto& signals=Node(node).signals;for(auto& signal:signals)if(signal.fence==fence){signal.value=std::max(signal.value,value);return;}signals.push_back({fence,value});}

    void FrameGraphPlan::AfterAllUsers(uint32_t node,ID3D12Resource* resource){if(resource){Node(node).afterAllUsers.push_back(resource);if(!dependencies_.AfterAllUsers(node,resource))Fail(dependencies_.ErrorMessage());}}

    void FrameGraphPlan::Dependency(uint32_t from,uint32_t to){if(!dependencies_.Dependency(from,to))Fail(dependencies_.ErrorMessage());}

    void FrameGraphPlan::Resolve(){if(!dependencies_.Resolve())Fail(dependencies_.ErrorMessage());}


    void FrameGraphPlan::PublicationComplete(const GpuPublication& publication,uint32_t fallback){
        // Signal each group's consumer when its own last GPU user retires.
        // The legacy combined publication is still returned independently.
        auto retire=[&](ID3D12Fence* producer,uint64_t produced,ID3D12Fence* consumer,uint64_t consumed,
                        const std::vector<uint32_t>* subset){
            if(!producer||!produced||!consumer||!consumed)return;
            Compile();std::vector<uint32_t> users;users.reserve(nodes_.size());
            for(uint32_t node:dependencies_.ResolvedOrder()){
                bool uses=false;
                for(const auto& use:dependencies_.Uses())if(use.pass==node){
                    if(subset){
                        for(uint32_t index:*subset){
                            if(index>=publication.resources.size())Fail("GPU publication group resource index out of range");
                            if(use.resource==publication.resources[index].resource.Get()){uses=true;break;}
                        }
                    }else for(const auto& resource:publication.resources)
                        if(use.resource==resource.resource.Get()){uses=true;break;}
                    if(uses)break;
                }
                if(uses)users.push_back(node);
            }
            // A group without render readers can be returned immediately after
            // its own CUDA signal, rather than waiting for Present or snowfall.
            const uint32_t completion=users.empty()&&subset?
                AddPass("GPU.GroupReturn",GraphQueue::Copy,1ull+nodes_.size()):users.empty()?fallback:users.back();
            for(uint32_t user:users)if(user!=completion)Dependency(user,completion);
            ExternalWait(completion,producer,produced);
            ExternalSignal(completion,consumer,consumed);
        };
        for(const auto& group:publication.groups)
            retire(group.producerFence.Get(),group.producerValue,group.consumerFence.Get(),group.consumerValue,&group.resourceIndices);
        retire(publication.producerFence.Get(),publication.producerValue,
               publication.consumerFence.Get(),publication.consumerValue,nullptr);
    }

    void FrameGraphPlan::CompileExecutionBatches(){
        Compile();
        // Content reads may be independent while their physical layouts are not.
        // Order layout changes along the resolved DAG, including future-version users.
        const auto layoutOrder=dependencies_.ResolvedOrder();bool layoutEdges=false;
        auto overlap=[](uint32_t a,uint32_t countA,uint32_t b,uint32_t countB){return a<b+countB&&b<a+countA;};
        for(size_t i=0;i<layoutOrder.size();++i)for(size_t j=i+1;j<layoutOrder.size();++j){
            uint32_t previous=layoutOrder[i],next=layoutOrder[j];bool ordering=false;
            for(const auto& before:nodes_[previous].uses)if(before.texture&&before.mode==GraphMode::Read)
                for(const auto& after:nodes_[next].uses)if(after.texture&&after.mode==GraphMode::Read&&before.resource==after.resource&&before.layout!=after.layout&&
                    overlap(before.subresources.firstMip,before.subresources.mipCount,after.subresources.firstMip,after.subresources.mipCount)&&
                    overlap(before.subresources.firstArraySlice,before.subresources.arraySliceCount,after.subresources.firstArraySlice,after.subresources.arraySliceCount)&&
                    overlap(before.subresources.firstPlane,before.subresources.planeCount,after.subresources.firstPlane,after.subresources.planeCount))ordering=true;
            if(ordering){Dependency(previous,next);layoutEdges=true;}
        }
        if(layoutEdges)Compile();
        executionBatches.clear();executionBatches.reserve(nodes_.size());
        std::vector<uint32_t> nodeBatches(nodes_.size(),UINT32_MAX);
        std::array<uint64_t,GraphQueueCount> planned{};
        std::array<std::array<uint64_t,GraphQueueCount>,GraphQueueCount> covered{};
        std::array<std::vector<GraphExternalFence>,GraphQueueCount> externalCovered;
        std::array<uint32_t,GraphQueueCount> lastBatch;lastBatch.fill(UINT32_MAX);
        for(uint32_t q=0;q<GraphQueueCount;++q)planned[q]=executor_->FenceValue(GraphQueue(q));
        for(uint32_t index:dependencies_.ResolvedOrder()){
            const auto& node=nodes_[index];const uint32_t q=QI(node.queue);
            std::array<uint64_t,GraphQueueCount> needed{};
            for(uint32_t producer=0;producer<nodes_.size();++producer)if(nodes_[producer].queue!=node.queue&&HasDependency(producer,index)){
                uint32_t b=nodeBatches[producer];if(b==UINT32_MAX)Fail("Batch dependency order");
                const auto& source=executionBatches[b];needed[QI(source.queue)]=std::max(needed[QI(source.queue)],source.signalValue);
            }
            bool newWait=false;for(uint32_t source=0;source<GraphQueueCount;++source)newWait|=needed[source]>covered[q][source];
            for(const auto& wait:node.waits){uint64_t value=0;for(const auto& prior:externalCovered[q])if(prior.fence==wait.fence)value=prior.value;newWait|=wait.value>value;}
            bool split=executionBatches.empty()||executionBatches.back().queue!=node.queue||executionBatches.back().signalRequired||newWait;
            if(split){ExecutionBatch batch{};batch.queue=node.queue;batch.signalValue=++planned[q];batch.nodes.reserve(8);batch.waits.reserve(GraphQueueCount);executionBatches.push_back(std::move(batch));}
            auto& batch=executionBatches.back();const uint32_t batchIndex=uint32_t(executionBatches.size()-1);
            for(uint32_t source=0;source<GraphQueueCount;++source)if(needed[source]>covered[q][source]){
                batch.waits.push_back({GraphQueue(source),needed[source]});covered[q][source]=needed[source];
            }
            for(const auto& wait:node.waits){auto& prior=externalCovered[q];auto found=std::find_if(prior.begin(),prior.end(),[&](const auto& item){return item.fence==wait.fence;});if(found==prior.end())prior.push_back(wait);else found->value=std::max(found->value,wait.value);}
            batch.nodes.push_back(index);nodeBatches[index]=batchIndex;lastBatch[q]=batchIndex;
            batch.signalRequired=dependencies_.HasCrossQueueOutput(index)||!node.signals.empty();
        }
        // Each queue's last batch retires its frame-slot allocators and resources.
        for(uint32_t b:lastBatch)if(b!=UINT32_MAX)executionBatches[b].signalRequired=true;
        executor_->BeginFrame();
        for(auto& batch:executionBatches){
            if(batch.nodes.empty())Fail("Empty execution batch");
            auto pass=executor_->BeginPass(nodes_[batch.nodes.front()].name.c_str(),batch.queue);
            if(pass.signal!=batch.signalValue)Fail("Execution batch fence prediction");
            for(const auto& wait:batch.waits){GraphStamp source{true,wait.sourceQueue,wait.fenceValue,{}};source.frontier[QI(wait.sourceQueue)]=wait.fenceValue;executor_->Dependency(pass,source);}
            batch.compiledNodes.reserve(batch.nodes.size());
            for(uint32_t index:batch.nodes){
                const auto& node=nodes_[index];pass.name=node.name.c_str();CompiledNode compiled{};compiled.node=index;
                for(const auto& wait:node.waits)executor_->ExternalWait(pass,wait.fence,wait.value);
                for(uint32_t producer=0;producer<nodes_.size();++producer)if(HasDependency(producer,index)&&nodes_[producer].completion.valid)executor_->Dependency(pass,nodes_[producer].completion);
                for(const auto& use:node.uses){
                    if(use.texture)executor_->TextureRange(pass,use.resource,use.canonicalTexture,use.initialLayout,use.mode,use.layout,use.firstSync,use.firstAccess,compiled.barriersBefore,use.subresources);
                    else executor_->Buffer(pass,use.resource,use.mode,use.firstSync,use.firstAccess,use.finalSync,use.finalAccess,compiled.barriersBefore);
                }
                for(auto* resource:node.afterAllUsers)executor_->Dependency(pass,resource,GraphMode::ReadWrite);
                // Intra-node barriers retain their original command positions. Final
                // transitions are emitted after record(), inside that node's timestamps.
                for(bool during:{true,false})for(const auto& transition:node.transitions)if(transition.during==during){
                    if(during)compiled.barriersDuring.emplace_back();
                    auto& barriers=during?compiled.barriersDuring.back():compiled.barriersAfter;
                    if(transition.subresource)executor_->TransitionTextureSubresource(pass,*transition.texture,transition.range,transition.beforeLayout,transition.beforeSync,transition.beforeAccess,transition.layout,transition.sync,transition.access,barriers,transition.completesWholeTexture);
                    else executor_->TransitionTexture(pass,*transition.texture,transition.layout,transition.sync,transition.access,barriers);
                }
                for(const auto& signal:node.signals)executor_->ExternalSignal(pass,signal.fence,signal.value);
                batch.compiledNodes.push_back(std::move(compiled));
            }
            executor_->CompileSubmission(pass);batch.pass=std::move(pass);
            GraphStamp completion{true,batch.queue,batch.signalValue,batch.pass.required};completion.frontier[QI(batch.queue)]=batch.signalValue;
            for(uint32_t index:batch.nodes)nodes_[index].completion=completion;
        }
        executionCompiled_=true;
    }

    void FrameGraphPlan::RecordExecutionBatch(void* context,uint32_t,uint32_t){
        auto& recording=*static_cast<BatchRecording*>(context);traceFrame=recording.frameId;
        auto& commands=*recording.commands;commands.Reset();auto* list=commands.list.Get();
        for(const auto& compiled:recording.batch->compiledNodes){
            const auto& node=(*recording.nodes)[compiled.node];
            if(recording.profiler)recording.profiler->BeginPass(compiled.timestamp,recording.frameId,node.name.c_str(),list,recording.batchIndex,compiled.node,recording.crossQueueWait);
            EmitBarriers(list,compiled.barriersBefore);
            if(node.record)node.record(compiled,list);
            EmitBarriers(list,compiled.barriersAfter);
            if(recording.profiler)recording.profiler->EndPass(compiled.timestamp,list);
        }
        commands.Close();recording.ready.store(true,std::memory_order_release);
    }

    void FrameGraphPlan::Execute(JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){ExecuteImpl(nullptr,jobs,frameId,commands,beforeSubmit);}

    void FrameGraphPlan::ExecuteProfiled(BurstGpuProfiler& profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){ExecuteImpl(&profiler,jobs,frameId,commands,beforeSubmit);}

    void FrameGraphPlan::ExecuteImpl(BurstGpuProfiler* profiler,JobSystem& jobs,uint64_t frameId,const std::function<Commands&(GraphQueue,uint32_t)>& commands,const std::function<void(const GraphPass&)>& beforeSubmit){
        if(!executor_||!executionCompiled_)Fail("Frame graph batches not compiled");const uint32_t n=uint32_t(nodes_.size());
        if(profiler){
            profiler->batchesPerQueue={};uint32_t activePasses=0;
            for(const auto& batch:executionBatches){++profiler->batchesPerQueue[QI(batch.queue)];if(profiler->queues[QI(batch.queue)].supported)activePasses+=uint32_t(batch.compiledNodes.size());}
            profiler->PrepareFrame(activePasses);
            for(auto& batch:executionBatches)for(auto& compiled:batch.compiledNodes)compiled.timestamp=profiler->ReservePass(batch.queue);
            profiler->FinishReservations();
        }
        if(diagnostics_)for(uint32_t from=0;from<n;++from)for(uint32_t to=0;to<n;++to)if(HasDependency(from,to))LogEvent(traceFrame,"gpu.frame_graph_edge","from=%s to=%s",nodes_[from].name.c_str(),nodes_[to].name.c_str());
        if(diagnostics_)for(const auto& use:dependencies_.Uses())if(dependencies_.LatestVersion(use.resource,use.subresource)>1)
            LogEvent(traceFrame,"gpu.frame_graph_version","pass=%s resource=%p subresource=%u mode=%u input=%u output=%u",nodes_[use.pass].name.c_str(),const_cast<void*>(use.resource),use.subresource,unsigned(use.mode),use.inputVersion,use.outputVersion);
        // Allocate every context and command list before publishing any job. Each
        // frame slot owns a distinct allocator/list for every queue batch ordinal.
        auto recording=std::make_unique<BatchRecording[]>(executionBatches.size());
        std::array<uint32_t,GraphQueueCount> queueBatch{};
        for(uint32_t index=0;index<executionBatches.size();++index){
            const auto& batch=executionBatches[index];auto& item=recording[index];
            item.commands=&commands(batch.queue,queueBatch[QI(batch.queue)]++);item.pass=batch.pass;item.pass.commands=item.commands;
            item.batch=&batch;item.nodes=&nodes_;item.profiler=profiler;item.frameId=frameId;item.batchIndex=index;
            item.crossQueueWait=executor_->CrossQueueWait(item.pass);
        }
        for(uint32_t index=0;index<executionBatches.size();++index)
            jobs.Submit(RecordExecutionBatch,&recording[index],0,1,&recording[index].group);
        for(uint32_t index=0;index<executionBatches.size();++index){
            const auto& batch=executionBatches[index];auto& item=recording[index];
            jobs.Wait(item.group);
            if(!item.group.Complete()||!item.ready.load(std::memory_order_acquire))Fail("Batch recording incomplete");
            if(beforeSubmit)beforeSubmit(item.pass);
            UINT64 signal=executor_->Submit(item.pass,batch.signalRequired);if(batch.signalRequired)lastFences_[QI(batch.queue)]=signal;
            for(uint32_t nodeIndex:batch.nodes){auto& node=nodes_[nodeIndex];node.signal=signal;LogEvent(traceFrame,"gpu.frame_graph_execute","pass=%s queue=%u batch=%u node=%u signal=%llu",node.name.c_str(),unsigned(node.queue),index,nodeIndex,static_cast<unsigned long long>(signal));}
        }
    }

    const std::vector<uint32_t>& FrameGraphPlan::ResolvedOrder()const{return dependencies_.ResolvedOrder();}

    bool FrameGraphPlan::HasDependency(uint32_t from,uint32_t to)const{return dependencies_.HasDependency(from,to);}

    UINT64 FrameGraphPlan::Signal(uint32_t node)const{return node<nodes_.size()?nodes_[node].signal:0;}

    const std::array<UINT64,GraphQueueCount>& FrameGraphPlan::LastFences()const{return lastFences_;}
}
