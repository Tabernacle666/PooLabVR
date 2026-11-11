#include "GraphicsInternal.h"
#include <cmath>
#include <cstring>
#include <limits>

namespace lab {
    D3D12_CPU_DESCRIPTOR_HANDLE Graphics::Impl::Cpu(UINT index){auto h=descriptors->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*descriptorStride;return h;}

    D3D12_GPU_DESCRIPTOR_HANDLE Graphics::Impl::Gpu(UINT index){auto h=descriptors->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(index)*descriptorStride;return h;}

    D3D12_CPU_DESCRIPTOR_HANDLE Graphics::Impl::Rtv(UINT index){auto h=rtvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*rtvStride;return h;}

    D3D12_CPU_DESCRIPTOR_HANDLE Graphics::Impl::Dsv(UINT index){auto h=dsvs->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*dsvStride;return h;}

    uint32_t Graphics::Impl::Physical(DescriptorHandle handle)const{return descriptorAllocator.Resolve(handle);}

    uint32_t Graphics::Impl::Generation(DescriptorHandle handle)const{return descriptorAllocator.Generation(handle);}

    ID3D12Device* Graphics::Impl::Device()const noexcept {return device.Get();}

    ID3D12RootSignature* Graphics::Impl::BindlessRootSignature()const noexcept {return root.Get();}

    ID3D12DescriptorHeap* Graphics::Impl::BindlessResourceHeap()const noexcept {return descriptors.Get();}

    ID3D12DescriptorHeap* Graphics::Impl::BindlessSamplerHeap()const noexcept {return samplers.Get();}

    GpuResourceHandle Graphics::Impl::CreateBufferSrv(ID3D12Resource* resource,uint32_t count,uint32_t stride){return Srv(resource,count,stride);}

    GpuResourceHandle Graphics::Impl::CreateBufferUav(ID3D12Resource* resource,uint32_t count,uint32_t stride){return UavBuffer(resource,count,stride);}

    GpuResourceHandle Graphics::Impl::CreateTextureSrv(ID3D12Resource* resource,DXGI_FORMAT format){return TextureSrv(resource,format);}

    GpuResourceHandle Graphics::Impl::CreateTextureUav(ID3D12Resource* resource,DXGI_FORMAT format,uint32_t mip){return TextureUav(resource,format,mip);}

    uint32_t Graphics::Impl::DescriptorIndex(GpuResourceHandle handle)const {return Physical(handle);}

    uint32_t Graphics::Impl::DescriptorGeneration(GpuResourceHandle handle)const {return Generation(handle);}

    GpuFenceValues Graphics::Impl::SubmittedFences()const{return {copy.serial,compute.serial,direct.serial};}

    GpuFenceValues Graphics::Impl::CompletedFences()const{return {copy.fence?copy.fence->GetCompletedValue():0,compute.fence?compute.fence->GetCompletedValue():0,direct.fence?direct.fence->GetCompletedValue():0};}

    void Graphics::Impl::CollectManaged(){auto completed=CompletedFences();descriptorAllocator.Collect(completed);vramCache.Collect(completed);managedMemory.Collect(SubmittedFences(),completed);}

    GpuResourceLease Graphics::Impl::CreateManagedBuffer(const GpuBufferDesc& d){CollectManaged();return managedMemory.Buffer(d);}

    GpuResourceLease Graphics::Impl::CreateManagedTexture2D(const GpuTextureDesc& d){CollectManaged();return managedMemory.Texture(d,false);}

    GpuResourceLease Graphics::Impl::CreateManagedTexture3D(const GpuTextureDesc& d){CollectManaged();return managedMemory.Texture(d,true);}

    GpuResourceLease Graphics::Impl::UploadStaticBuffer(std::span<const std::byte> bytes,uint32_t stride,const wchar_t* name){
        if(bytes.empty()||!stride||bytes.size()%stride)return {};
        GpuBufferDesc desc{};desc.bytes=bytes.size();desc.stride=stride;desc.heap=D3D12_HEAP_TYPE_DEFAULT;desc.srv=true;desc.uav=false;desc.name=name;
        auto lease=CreateManagedBuffer(desc);if(!lease)return {};
        residency.Begin();managedMemory.Pin(lease->resource);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))return {};residency.Wait(direct.queue.Get());
        auto upload=Host(bytes.size(),D3D12_HEAP_TYPE_UPLOAD);std::byte* mapped{};Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"managed static buffer map");memcpy(mapped,bytes.data(),bytes.size());upload->Unmap(0,nullptr);
        startup.Reset();BBarrier(startup.list.Get(),lease->resource,D3D12_BARRIER_SYNC_NONE,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_NO_ACCESS,D3D12_BARRIER_ACCESS_COPY_DEST);startup.list->CopyBufferRegion(lease->resource,0,upload.Get(),0,bytes.size());BBarrier(startup.list.Get(),lease->resource,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);return lease;
    }

    GpuResourceLease Graphics::Impl::UploadStaticTexture2D(const GpuTextureDesc& d,std::span<const GpuSubresourceUpload> subresources){
        if(!StaticUploadFormat(d.format)||!d.width||!d.height||!d.depthOrArraySize||!d.mips)return {};
        const uint64_t expected=uint64_t(d.depthOrArraySize)*d.mips;if(subresources.size()!=expected||expected>UINT32_MAX)return {};
        auto lease=CreateManagedTexture2D(d);if(!lease)return {};auto native=lease->resource;auto nativeDesc=native->GetDesc();
        residency.Begin();managedMemory.Pin(native);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))return {};residency.Wait(direct.queue.Get());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(static_cast<size_t>(expected));std::vector<UINT> rows(static_cast<size_t>(expected));std::vector<UINT64> rowSizes(static_cast<size_t>(expected));UINT64 uploadBytes{};device->GetCopyableFootprints(&nativeDesc,0,UINT(expected),0,footprints.data(),rows.data(),rowSizes.data(),&uploadBytes);if(!uploadBytes)return {};
        auto upload=Host(uploadBytes,D3D12_HEAP_TYPE_UPLOAD);std::byte* mapped{};Hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"managed static texture map");
        for(uint32_t i=0;i<uint32_t(expected);++i){const auto& src=subresources[i];if(!src.data||src.rowPitch<rowSizes[i]||src.slicePitch<src.rowPitch*rows[i]){upload->Unmap(0,nullptr);return {};}auto* source=static_cast<const std::byte*>(src.data);auto* target=mapped+footprints[i].Offset;for(uint32_t row=0;row<rows[i];++row)memcpy(target+uint64_t(row)*footprints[i].Footprint.RowPitch,source+uint64_t(row)*src.rowPitch,size_t(rowSizes[i]));}
        upload->Unmap(0,nullptr);startup.Reset();TextureResource tracked{native};StartupTransition(startup.list.Get(),tracked,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY);
        for(uint32_t i=0;i<uint32_t(expected);++i){D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=native;dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=i;D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[i];startup.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}StartupTransition(startup.list.Get(),tracked,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING);
        startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);return lease;
    }

    GpuMemoryStats Graphics::Impl::MemoryStats()const {auto s=managedMemory.Stats();s.descriptorsLive=descriptorAllocator.LiveCount();s.descriptorsRetired=descriptorAllocator.RetiredCount();s.descriptorsRecycled=descriptorAllocator.RecycledCount();return s;}

    void Graphics::Impl::ReleaseDescriptor(DescriptorHandle& handle){descriptorAllocator.Release(handle,SubmittedFences());}

    ComPtr<ID3D12Resource> Graphics::Impl::Placed(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES initial,const D3D12_CLEAR_VALUE* clear,bool dedicated) {
        auto info=device->GetResourceAllocationInfo(0,1,&desc);if(info.SizeInBytes==UINT64_MAX)Fail("GPU allocation description");Page* page=nullptr;
        if(!dedicated)for(uint32_t i=0;i<pageCount;i++)if(!pages[i].dedicated&&Align(pages[i].used,info.Alignment)+info.SizeInBytes<=pages[i].size){page=&pages[i];break;}
        if(!page){DXGI_QUERY_VIDEO_MEMORY_INFO budget{};Hr(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&budget),"VRAM budget");UINT64 heapAlignment=dedicated?info.Alignment:4*MiB;UINT64 size=Align(dedicated?info.SizeInBytes:std::max(32*MiB,info.SizeInBytes),heapAlignment);auto completed=CompletedFences();auto headroom=managedMemory.Stats().headroom;pages.emplace_back();page=&pages[pageCount++];page->size=size;page->dedicated=dedicated;D3D12_HEAP_DESC h{};h.SizeInBytes=size;h.Alignment=heapAlignment;h.Properties=Heap(D3D12_HEAP_TYPE_DEFAULT);if(dedicated)h.Flags=D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT;else if(!residency.Reserve(size,completed[2],headroom)){vramCache.MakeRoom(size,completed,headroom);if(!residency.Reserve(size,completed[2],headroom))Fail("Resident GPU geometry exceeds available VRAM budget");}Hr(device->CreateHeap(&h,IID_PPV_ARGS(&page->heap)),"placed resource heap");page->residency=residency.Add(page->heap.Get(),size,dedicated?GpuResidencyClass::Required:GpuResidencyClass::Pinned,!dedicated);committed+=size;}
        auto offset=Align(page->used,info.Alignment);ComPtr<ID3D12Resource> r;Hr(device->CreatePlacedResource(page->heap.Get(),offset,&desc,initial,clear,IID_PPV_ARGS(&r)),"placed GPU resource");page->used=offset+info.SizeInBytes;return r;
    }

    ComPtr<ID3D12Resource> Graphics::Impl::Temporary(const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state) {
        auto info=device->GetResourceAllocationInfo(0,1,&desc);if(info.SizeInBytes==UINT64_MAX||!info.SizeInBytes)Fail("temporary GPU allocation description");auto completed=CompletedFences();auto headroom=managedMemory.Stats().headroom;if(!residency.Reserve(info.SizeInBytes,completed[2],headroom)){vramCache.MakeRoom(info.SizeInBytes,completed,headroom);if(!residency.Reserve(info.SizeInBytes,completed[2],headroom))Fail("Temporary GPU resource exceeds available VRAM budget");}auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);ComPtr<ID3D12Resource> resource;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"temporary GPU resource");return resource;
    }

    ComPtr<ID3D12Resource> Graphics::Impl::Host(UINT64 size,D3D12_HEAP_TYPE type){auto heap=Heap(type);auto desc=Buffer(size);ComPtr<ID3D12Resource> r;Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"host buffer");return r;}

    void Graphics::Impl::CommandsInit(Commands& c,D3D12_COMMAND_LIST_TYPE type){Hr(device->CreateCommandAllocator(type,IID_PPV_ARGS(&c.allocator)),"command allocator create");ComPtr<ID3D12GraphicsCommandList> base;Hr(device->CreateCommandList(0,type,c.allocator.Get(),nullptr,IID_PPV_ARGS(&base)),"command list create");Hr(base.As(&c.list),"enhanced command list");Hr(c.list->Close(),"command list close");}

    void Graphics::Impl::QueueInit(QueueState& q,D3D12_COMMAND_LIST_TYPE type){q.name=type==D3D12_COMMAND_LIST_TYPE_DIRECT?"Direct":type==D3D12_COMMAND_LIST_TYPE_COMPUTE?"Compute":"Copy";D3D12_COMMAND_QUEUE_DESC d{};d.Type=type;Hr(device->CreateCommandQueue(&d,IID_PPV_ARGS(&q.queue)),"GPU queue");Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q.fence)),"queue fence");}

    void Graphics::Impl::WaitStartup(QueueState& q){UINT64 v=q.Signal();HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)Fail("GPU event");Hr(q.fence->SetEventOnCompletion(v,event),"GPU completion");WaitForSingleObject(event,INFINITE);CloseHandle(event);}

    DescriptorHandle Graphics::Impl::Srv(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=DXGI_FORMAT_UNKNOWN;d.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;d.Buffer.NumElements=count;d.Buffer.StructureByteStride=stride;device->CreateShaderResourceView(r,&d,Cpu(index));return handle;}

    DescriptorHandle Graphics::Impl::UavBuffer(ID3D12Resource* r,uint32_t count,uint32_t stride,DescriptorHandle handle){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=stride?DXGI_FORMAT_UNKNOWN:DXGI_FORMAT_R32_TYPELESS;d.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;d.Buffer.NumElements=count;d.Buffer.StructureByteStride=stride;d.Buffer.Flags=stride?D3D12_BUFFER_UAV_FLAG_NONE:D3D12_BUFFER_UAV_FLAG_RAW;device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(index));return handle;}

    DescriptorHandle Graphics::Impl::TextureSrv(ID3D12Resource* r,DXGI_FORMAT format,DescriptorHandle handle){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);auto desc=r->GetDesc();D3D12_SHADER_RESOURCE_VIEW_DESC d{};d.Format=format;d.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;d.Texture3D.MipLevels=desc.MipLevels;}else if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else if(desc.DepthOrArraySize>1){d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipLevels=desc.MipLevels;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;d.Texture2D.MipLevels=desc.MipLevels;}device->CreateShaderResourceView(r,&d,Cpu(index));return handle;}

    DescriptorHandle Graphics::Impl::TextureUav(ID3D12Resource* r,DXGI_FORMAT format,uint32_t mip,DescriptorHandle handle){if(handle.Valid())ReleaseDescriptor(handle);descriptorAllocator.Collect(CompletedFences());handle=descriptorAllocator.Allocate(ComPtr<ID3D12Resource>(r));uint32_t index=Physical(handle);auto desc=r->GetDesc();D3D12_UNORDERED_ACCESS_VIEW_DESC d{};d.Format=format;if(desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE3D;d.Texture3D.MipSlice=mip;d.Texture3D.WSize=UINT(-1);}else if(desc.DepthOrArraySize>1){d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;d.Texture2DArray.MipSlice=mip;}else{d.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d.Texture2D.MipSlice=mip;}device->CreateUnorderedAccessView(r,nullptr,&d,Cpu(index));return handle;}

    uint32_t Graphics::Impl::MakeRTV(ID3D12Resource* r,DXGI_FORMAT format,uint32_t reuse){uint32_t index=reuse==Invalid?rtvCount++:reuse;D3D12_RENDER_TARGET_VIEW_DESC d{};d.Format=format;auto desc=r->GetDesc();if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}device->CreateRenderTargetView(r,&d,Rtv(index));return index;}

    uint32_t Graphics::Impl::MakeDSV(ID3D12Resource* r,uint32_t reuse){uint32_t index=reuse==Invalid?dsvCount++:reuse;D3D12_DEPTH_STENCIL_VIEW_DESC d{};d.Format=DXGI_FORMAT_D32_FLOAT;auto desc=r->GetDesc();if(desc.SampleDesc.Count>1){d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;d.Texture2DMSArray.ArraySize=desc.DepthOrArraySize;}else{d.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.ArraySize=desc.DepthOrArraySize;}device->CreateDepthStencilView(r,&d,Dsv(index));return index;}

    GpuCacheKey Graphics::Impl::CacheKey(uint32_t kind,const void* data,size_t bytes,uint64_t generation){
        uint64_t content=HashMemory(data,bytes);if(!content)content=1;uint64_t revision=generation?generation:content;if(!revision)revision=1;return {content,revision,kind};
    }

    ComPtr<ID3D12Resource> Graphics::Impl::UploadBuffer(const void* data,UINT64 bytes){auto r=Placed(Buffer(bytes));auto h=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);void* p;D3D12_RANGE empty{0,0};Hr(h->Map(0,&empty,&p),"asset upload map");memcpy(p,data,size_t(bytes));h->Unmap(0,nullptr);startup.list->CopyBufferRegion(r.Get(),0,h.Get(),0,bytes);BBarrier(startup.list.Get(),r.Get(),D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);uploads.push_back(h);return r;}

    ComPtr<ID3D12Resource> Graphics::Impl::CachedUploadBuffer(const void* data,UINT64 bytes,uint32_t kind,uint64_t generation,const wchar_t* name){
        if(!bytes)Fail("cacheable zero-byte upload");auto key=CacheKey(kind,data,size_t(bytes),generation);auto cached=vramCache.Acquire(key,Buffer(bytes),D3D12_RESOURCE_STATE_COMMON,true,CompletedFences(),managedMemory.Stats().headroom,name);if(!cached)Fail("Cacheable GPU buffer allocation");if(cached.reused)return cached.resource;
        auto host=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);void* p;D3D12_RANGE empty{0,0};Hr(host->Map(0,&empty,&p),"cacheable asset upload map");memcpy(p,data,size_t(bytes));host->Unmap(0,nullptr);startup.list->CopyBufferRegion(cached.resource.Get(),0,host.Get(),0,bytes);BBarrier(startup.list.Get(),cached.resource.Get(),D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_SYNC_ALL_SHADING,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_ACCESS_SHADER_RESOURCE);uploads.push_back(host);vramCache.Commit(cached.resource.Get());return cached.resource;
    }

    uint32_t Graphics::Impl::UploadTexture(const TextureData& t){
        auto format=t.format;auto desc=Texture(t.width,t.height,format,1,1,t.mips);uint64_t hash=1469598103934665603ull;hash=HashMemory(&t.width,sizeof(t.width),hash);hash=HashMemory(&t.height,sizeof(t.height),hash);hash=HashMemory(&t.mips,sizeof(t.mips),hash);hash=HashMemory(&format,sizeof(format),hash);if(!t.bytes.empty())hash=HashMemory(t.bytes.data(),t.bytes.size(),hash);if(!hash)hash=1;GpuCacheKey key{hash,hash,CacheTexture};auto cached=vramCache.Acquire(key,desc,D3D12_RESOURCE_STATE_COMMON,false,CompletedFences(),managedMemory.Stats().headroom,L"Cached immutable texture");if(!cached)Fail("Texture cache allocation");auto resource=cached.resource;uint32_t resident=cached.residency;
        if(!cached.reused){TextureResource view{resource};StartupTransition(startup.list.Get(),view,D3D12_BARRIER_LAYOUT_COPY_DEST,D3D12_BARRIER_ACCESS_COPY_DEST,D3D12_BARRIER_SYNC_COPY);residency.Begin();vramCache.RequireActive();residency.Require(resident);if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))Fail("Texture upload exceeds available VRAM budget");residency.Wait(direct.queue.Get());std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(t.mips);std::vector<UINT> rows(t.mips);std::vector<UINT64> rowSizes(t.mips);UINT64 size;device->GetCopyableFootprints(&desc,0,t.mips,0,footprints.data(),rows.data(),rowSizes.data(),&size);auto host=Host(size,D3D12_HEAP_TYPE_UPLOAD);std::byte* ptr;Hr(host->Map(0,nullptr,reinterpret_cast<void**>(&ptr)),"texture upload");for(uint32_t mip=0;mip<t.mips;mip++){for(uint32_t row=0;row<rows[mip];row++)memcpy(ptr+footprints[mip].Offset+UINT64(row)*footprints[mip].Footprint.RowPitch,t.bytes.data()+t.offsets[mip]+UINT64(row)*rowSizes[mip],size_t(rowSizes[mip]));D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;dst.SubresourceIndex=mip;D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=host.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=footprints[mip];startup.list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}host->Unmap(0,nullptr);StartupTransition(startup.list.Get(),view,D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,D3D12_BARRIER_ACCESS_SHADER_RESOURCE,D3D12_BARRIER_SYNC_ALL_SHADING);uploads.push_back(host);startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);uploads.clear();startup.Reset();vramCache.Commit(resource.Get());}
        auto descriptor=TextureSrv(resource.Get(),format);textures.push_back({resource,descriptor,resident});return Physical(descriptor);
    }

    std::shared_ptr<Graphics::Impl::UploadGeneration> Graphics::Impl::NewUploadGeneration(UINT64 chunk){
        auto generation=std::make_shared<UploadGeneration>();generation->chunk=chunk;generation->generation=uploadGenerationSerial.fetch_add(1,std::memory_order_acq_rel)+1;generation->resource=Host(3*chunk,D3D12_HEAP_TYPE_UPLOAD);D3D12_RANGE empty{0,0};Hr(generation->resource->Map(0,&empty,reinterpret_cast<void**>(&generation->mapped)),"upload generation map");return generation;
    }

    void Graphics::Impl::DynamicResources(Frame& f) {
        f.motionBuffer=Placed(Buffer(std::max(size_t(1),animation.Motions().size())*sizeof(GpuMotion)));f.boneBuffer=Placed(Buffer(std::max(size_t(1),animation.Bones().size())*sizeof(GpuBone)));f.weightBuffer=Placed(Buffer(std::max(size_t(1),animation.Weights().size())*sizeof(float)));
        f.motionSRV=Srv(f.motionBuffer.Get(),uint32_t(std::max(size_t(1),animation.Motions().size())),sizeof(GpuMotion),f.motionSRV);f.boneSRV=Srv(f.boneBuffer.Get(),uint32_t(std::max(size_t(1),animation.Bones().size())),sizeof(GpuBone),f.boneSRV);f.weightSRV=Srv(f.weightBuffer.Get(),uint32_t(std::max(size_t(1),animation.Weights().size())),sizeof(float),f.weightSRV);
        f.deformedBounds=Placed(Buffer(meshes.ranges.size()*sizeof(XMFLOAT4),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));f.deformedBoundsSRV=Srv(f.deformedBounds.Get(),uint32_t(meshes.ranges.size()),sizeof(XMFLOAT4),f.deformedBoundsSRV);f.deformedBoundsUAV=UavBuffer(f.deformedBounds.Get(),uint32_t(meshes.ranges.size()),sizeof(XMFLOAT4),f.deformedBoundsUAV);

        f.dynamicVertices=Placed(Buffer(meshes.vertices.size()*sizeof(Vertex),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));f.dynamicSRV=Srv(f.dynamicVertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),f.dynamicSRV);f.dynamicUAV=UavBuffer(f.dynamicVertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),f.dynamicUAV);f.dynamicBLAS.resize(dynamicMeshes.size());f.dynamicBlasBuilt.assign(dynamicMeshes.size(),0);f.dynamicGeometryRevision.assign(dynamicMeshes.size(),0);f.dynamicBlasRevision.assign(dynamicMeshes.size(),0);f.pendingDynamicRevision.assign(dynamicMeshes.size(),0);f.deformationWork.Configure(uint32_t(dynamicMeshes.size()));f.blasWork.Configure(uint32_t(dynamicMeshes.size()));
        for(uint32_t i=0;i<dynamicMeshes.size();i++){auto geometry=DynamicGeometry(f,dynamicMeshes[i]);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;input.NumDescs=1;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=&geometry;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);f.dynamicBLAS[i]=Placed(Buffer(info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);f.dynamicScratchBytes=std::max(f.dynamicScratchBytes,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes));}
    }

    void Graphics::Impl::InitializeResourceAbi(Frame& f){
        if(HistoryHiZMipBase+mips>ResourceDomainWords)Fail("resource domain table capacity");
        f.resourceDirectory=Host(UINT64(ResourceDomainCount)*sizeof(DomainDirectoryEntry),D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE empty{0,0};void* directory{};Hr(f.resourceDirectory->Map(0,&empty,&directory),"resource directory map");f.resourceDirectoryData=reinterpret_cast<DomainDirectoryEntry*>(directory);
        f.resourceDirectorySRV=Srv(f.resourceDirectory.Get(),ResourceDomainCount,sizeof(DomainDirectoryEntry),f.resourceDirectorySRV);
        // This two-entry source table is per-frame and read-only to GPU passes.
        // Its global ranges are refreshed after publication bindings are known.
        f.instanceRanges=Host(2*sizeof(InstanceSourceRange),D3D12_HEAP_TYPE_UPLOAD);
        void* ranges{};Hr(f.instanceRanges->Map(0,&empty,&ranges),"instance source ranges map");
        f.instanceRangeData=static_cast<InstanceSourceRange*>(ranges);
        f.instanceRangesSRV=Srv(f.instanceRanges.Get(),2,sizeof(InstanceSourceRange),f.instanceRangesSRV);
        f.instanceRangeCapacity=2;
        for(uint32_t domain=0;domain<ResourceDomainCount;domain++){
            f.resourceTables[domain]=Host(UINT64(ResourceDomainWords)*sizeof(DomainBindingEntry),D3D12_HEAP_TYPE_UPLOAD);void* table{};Hr(f.resourceTables[domain]->Map(0,&empty,&table),"resource domain table map");f.resourceTableData[domain]=reinterpret_cast<DomainBindingEntry*>(table);std::fill_n(f.resourceTableData[domain],ResourceDomainWords,DomainBindingEntry{});
            f.resourceTableSRV[domain]=Srv(f.resourceTables[domain].Get(),ResourceDomainWords,sizeof(DomainBindingEntry),f.resourceTableSRV[domain]);f.resourceDirectoryData[domain]={Physical(f.resourceTableSRV[domain]),Generation(f.resourceTableSRV[domain]),ResourceDomainWords,0};
        }
    }

    void Graphics::Impl::EnsureLightBuffers(Frame& f){
        if(!clustersCount)Fail("Light cluster grid is empty");
        const auto grow=[](uint32_t current,uint64_t need){
            uint64_t cap=std::max<uint64_t>(1,current);
            while(cap<need)cap=std::min<uint64_t>(UINT32_MAX,cap*2);
            if(cap<need||cap>UINT32_MAX)Fail("Light buffer exceeds 32-bit address space");
            return uint32_t(cap);
        };
        const uint32_t neededLights=std::max(1u,f.lightCount);
        if(!f.lights||neededLights>f.lightCapacity){
            const uint32_t capacity=grow(std::max<uint32_t>(MaxLights,f.lightCapacity),neededLights);
            passGraph.Forget(f.lights.Get());f.lights.Reset();
            f.lights=Temporary(Buffer(UINT64(capacity)*sizeof(Light)),D3D12_RESOURCE_STATE_COMMON);
            f.lightsSRV=Srv(f.lights.Get(),capacity,sizeof(Light),f.lightsSRV);
            f.lightCapacity=capacity;
        }
        if(!f.clusters){
            // One uint2 (global offset, exact count) per cell; no light mask.
            f.clusters=Temporary(Buffer(UINT64(clustersCount)*sizeof(XMUINT2),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            f.clusterSRV=Srv(f.clusters.Get(),clustersCount,sizeof(XMUINT2),f.clusterSRV);
            f.clusterUAV=UavBuffer(f.clusters.Get(),clustersCount,sizeof(XMUINT2),f.clusterUAV);
            f.clusterBounds=Temporary(Buffer(UINT64(clustersCount)*sizeof(XMFLOAT4),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            f.clusterBoundsSRV=Srv(f.clusterBounds.Get(),clustersCount,
                sizeof(XMFLOAT4),f.clusterBoundsSRV);
            f.clusterBoundsUAV=UavBuffer(f.clusterBounds.Get(),clustersCount,
                sizeof(XMFLOAT4),f.clusterBoundsUAV);
        }
        if(!f.clusterDirectionalCount){
            f.clusterDirectionalCount=Temporary(
                Buffer(sizeof(uint32_t),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COMMON);
            f.clusterDirectionalCountSRV=Srv(f.clusterDirectionalCount.Get(),1,
                sizeof(uint32_t),f.clusterDirectionalCountSRV);
            f.clusterDirectionalCountUAV=UavBuffer(f.clusterDirectionalCount.Get(),1,
                sizeof(uint32_t),f.clusterDirectionalCountUAV);
        }
        if(!f.clusterDirectionalIndices||neededLights>f.directionalCapacity){
            const uint32_t capacity=grow(f.directionalCapacity,neededLights);
            passGraph.Forget(f.clusterDirectionalIndices.Get());f.clusterDirectionalIndices.Reset();
            f.clusterDirectionalIndices=Temporary(Buffer(UINT64(capacity)*sizeof(uint32_t),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            f.clusterDirectionalIndicesSRV=Srv(f.clusterDirectionalIndices.Get(),capacity,
                sizeof(uint32_t),f.clusterDirectionalIndicesSRV);
            f.clusterDirectionalIndicesUAV=UavBuffer(f.clusterDirectionalIndices.Get(),capacity,
                sizeof(uint32_t),f.clusterDirectionalIndicesUAV);
            f.directionalCapacity=capacity;
        }
        // Exactly 32 slots per existing view/depth cluster. A 33rd
        // candidate marks an overflow and is handled by a BVH point query,
        // never by a truncated list or a clustersCount*lightCount buffer.
        const uint64_t requiredIndices=uint64_t(clustersCount)*32u;
        if(requiredIndices>UINT32_MAX)
            Fail("Fixed light cluster index space exceeds UINT32_MAX");
        if(!f.clusterIndices||requiredIndices>f.clusterIndexCapacity){
            const uint32_t capacity=grow(f.clusterIndexCapacity,requiredIndices);
            passGraph.Forget(f.clusterIndices.Get());f.clusterIndices.Reset();
            f.clusterIndices=Temporary(Buffer(UINT64(capacity)*sizeof(uint32_t),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            f.clusterIndicesSRV=Srv(f.clusterIndices.Get(),capacity,
                sizeof(uint32_t),f.clusterIndicesSRV);
            f.clusterIndicesUAV=UavBuffer(f.clusterIndices.Get(),capacity,
                sizeof(uint32_t),f.clusterIndicesUAV);
            f.clusterIndexCapacity=capacity;
        }
    }


    void Graphics::Impl::EnsureLightBvhBuffers(Frame& f){
        // The implicit binary tree is indexed [1,2P). Every resident frame
        // slot has its own tree and sort scratch; no GPU->CPU read is involved.
        uint32_t leaves=1u;
        while(leaves<std::max(1u,f.lightCount)){
            if(leaves>UINT32_MAX/2u)
                Fail("Light BVH leaf count exceeds 32-bit capacity");
            leaves*=2u;
        }
        const uint64_t nodes=uint64_t(leaves)*2u;
        const uint64_t groups=(uint64_t(std::max(1u,f.lightCount))+255u)/256u;
        if(nodes>UINT32_MAX/sizeof(LightBvhNode)||
           uint64_t(leaves)>UINT32_MAX/sizeof(XMUINT2)||
           groups+1u>UINT32_MAX/sizeof(LightBvhNode))
            Fail("Light BVH is not addressable as a DX12 structured buffer");
        if(nodes>UINT32_MAX||groups+1u>UINT32_MAX)
            Fail("Light BVH shader index capacity exceeded");

        auto allocate=[&](ComPtr<ID3D12Resource>& buffer,
                          DescriptorHandle& srv,DescriptorHandle& uav,
                          uint32_t& capacity,uint32_t count,uint32_t stride){
            if(buffer&&capacity>=count)return;
            passGraph.Forget(buffer.Get());
            buffer=Temporary(Buffer(uint64_t(count)*stride,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                D3D12_RESOURCE_STATE_COMMON);
            srv=Srv(buffer.Get(),count,stride,srv);
            uav=UavBuffer(buffer.Get(),count,stride,uav);
            capacity=count;
            f.lightBvhSorted=false;
        };
        allocate(f.lightBvhNodes,f.lightBvhNodesSRV,f.lightBvhNodesUAV,
            f.lightBvhNodeCapacity,uint32_t(nodes),sizeof(LightBvhNode));
        allocate(f.lightBvhPairs,f.lightBvhPairsSRV,f.lightBvhPairsUAV,
            f.lightBvhPairCapacity,leaves,sizeof(XMUINT2));
        allocate(f.lightBvhBounds,f.lightBvhBoundsSRV,f.lightBvhBoundsUAV,
            f.lightBvhBoundsCapacity,uint32_t(groups+1u),sizeof(LightBvhNode));
        if(f.lightBvhLeaves!=leaves){
            f.lightBvhLeaves=leaves;
            f.lightBvhSorted=false;
        }
    }

    void Graphics::Impl::EnsureInstanceRoutes(Frame& f,uint32_t required){
        if(required<=f.instanceRangeCapacity)return;
        if(f.instanceRanges&&f.instanceRangeData){
            f.instanceRanges->Unmap(0,nullptr);f.instanceRangeData=nullptr;
        }
        f.instanceRanges.Reset();
        const uint32_t capacity=GrowCapacity(f.instanceRangeCapacity,required);
        f.instanceRanges=Host(UINT64(capacity)*sizeof(InstanceSourceRange),
                              D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE empty{0,0};void* mapped{};
        Hr(f.instanceRanges->Map(0,&empty,&mapped),"instance route table map");
        f.instanceRangeData=static_cast<InstanceSourceRange*>(mapped);
        f.instanceRangesSRV=Srv(f.instanceRanges.Get(),capacity,
                                sizeof(InstanceSourceRange),f.instanceRangesSRV);
        f.instanceRangeCapacity=capacity;
    }

    void Graphics::Impl::EnsureInstances(Frame& f,uint32_t worldRequired,uint32_t cpuRequired,uint32_t totalRequired){
        if(cpuRequired<worldRequired||totalRequired<cpuRequired)Fail("Instance source ranges overlap");
        const uint32_t publishedRequired=totalRequired-cpuRequired;
        const uint32_t slot=uint32_t(&f-frames.data());
        auto buffer=[&](ComPtr<ID3D12Resource>& destination,UINT64 bytes,D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COMMON){
            passGraph.Forget(destination.Get());destination.Reset();
            destination=Temporary(Buffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),state);
        };
        // Separate allocations: an incoming GPU publication never writes, nor
        // resizes, the buffer that CpuScene.Depth reads.
        if(std::max(1u,worldRequired)>f.worldInstanceCapacity){
            const uint32_t capacity=GrowCapacity(f.worldInstanceCapacity,std::max(1u,worldRequired));
            buffer(f.worldInstances,UINT64(capacity)*sizeof(GpuInstance));
            f.worldInstanceSRV=Srv(f.worldInstances.Get(),capacity,sizeof(GpuInstance),f.worldInstanceSRV);
            f.worldInstanceUAV=UavBuffer(f.worldInstances.Get(),capacity,sizeof(GpuInstance),f.worldInstanceUAV);
            f.worldInstanceCapacity=capacity;
            sceneCache.InvalidateFrame(slot);
            rayScene.InvalidateFrame(slot);
        }
        if(std::max(1u,publishedRequired)>f.publishedInstanceCapacity){
            const uint32_t capacity=GrowCapacity(f.publishedInstanceCapacity,std::max(1u,publishedRequired));
            buffer(f.publishedInstances,UINT64(capacity)*sizeof(GpuInstance));
            f.publishedInstanceSRV=Srv(f.publishedInstances.Get(),capacity,sizeof(GpuInstance),f.publishedInstanceSRV);
            f.publishedInstanceCapacity=capacity;
            rayScene.InvalidateFrame(slot);
        }
        if(totalRequired<=f.instanceCapacity)return;
        const uint32_t capacity=GrowCapacity(f.instanceCapacity,std::max(1u,totalRequired));
        const bool existing=f.instanceCapacity!=0;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.NumDescs=capacity;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE|
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
        if(!info.ResultDataMaxSizeInBytes)Fail("DXR instance resource capacity exhausted");
        buffer(f.rayDescriptors,UINT64(capacity)*sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        buffer(f.hotBounds,UINT64(capacity)*16);
        buffer(f.hotMeshes,UINT64(capacity)*4);
        buffer(f.tlas,info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        buffer(f.scratch,std::max(info.ScratchDataSizeInBytes,info.UpdateScratchDataSizeInBytes),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto srv=[&](ID3D12Resource* resource,uint32_t count,uint32_t stride,DescriptorHandle& descriptor){descriptor=Srv(resource,count,stride,existing?descriptor:DescriptorHandle{});};
        auto uav=[&](ID3D12Resource* resource,uint32_t count,uint32_t stride,DescriptorHandle& descriptor){descriptor=UavBuffer(resource,count,stride,existing?descriptor:DescriptorHandle{});};
        uav(f.rayDescriptors.Get(),capacity*16,0,f.rayUAV);
        srv(f.hotBounds.Get(),capacity,16,f.hotBoundsSRV);uav(f.hotBounds.Get(),capacity,16,f.hotBoundsUAV);
        srv(f.hotMeshes.Get(),capacity,4,f.hotMeshesSRV);uav(f.hotMeshes.Get(),capacity,4,f.hotMeshesUAV);
        if(f.tlasSRV.Valid())ReleaseDescriptor(f.tlasSRV);
        descriptorAllocator.Collect(CompletedFences());
        f.tlasSRV=descriptorAllocator.Allocate(f.tlas);
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.ViewDimension=D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.RaytracingAccelerationStructure.Location=f.tlas->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr,&view,Cpu(Physical(f.tlasSRV)));
        f.lastTlasCount=0;f.tlasBuilt=false;
        f.instanceCapacity=capacity;f.transparent.resize(capacity);
        rayScene.InvalidateFrame(slot);
    }

    void Graphics::Impl::EnsureRayInstanceMap(Frame& f,uint32_t required){
        required=std::max(1u,required);
        if(required<=f.rayMapCapacity)return;
        const uint32_t capacity=GrowCapacity(f.rayMapCapacity,required);
        if(capacity<required)Fail("Ray instance map capacity exhausted");
        passGraph.Forget(f.rayInstanceMap.Get());
        f.rayInstanceMap.Reset();
        f.rayInstanceMap=Temporary(
            Buffer(UINT64(capacity)*sizeof(uint32_t),
                   D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
            D3D12_RESOURCE_STATE_COMMON);
        f.rayMapSRV=Srv(f.rayInstanceMap.Get(),capacity,sizeof(uint32_t),f.rayMapSRV);
        f.rayMapUAV=UavBuffer(f.rayInstanceMap.Get(),capacity,sizeof(uint32_t),
                              f.rayMapUAV);
        f.committedRayMapSegments.clear();
        f.rayMapCapacity=capacity;
        rayScene.InvalidateFrame(uint32_t(&f-frames.data()));
    }

    void Graphics::Impl::EnsureVisibilityBuffers(Frame& f,uint32_t cpuCount,uint32_t publishedCount){
        auto ensure=[&](Frame::VisibilitySet& set,uint32_t required,bool shadow){
            // D3D12 buffers cannot be zero-sized; a vacant category retains one unused slot.
            required=std::max(1u,required);
            if(required<=set.capacity)return;
            const uint32_t capacity=GrowCapacity(set.capacity,required);
            if(capacity<required||capacity>UINT32_MAX/4)Fail("visibility indirect argument capacity exhausted");
            auto argument=[&](ComPtr<ID3D12Resource>& resource,DescriptorHandle& descriptor){
                passGraph.Forget(resource.Get());resource.Reset();
                resource=Temporary(Buffer(UINT64(capacity)*16,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                descriptor=UavBuffer(resource.Get(),capacity*4,0,descriptor);
            };
            argument(set.depthArguments,set.depthArgumentsUAV);
            argument(set.cullArguments,set.cullArgumentsUAV);
            if(shadow){
                argument(set.shadowArguments,set.shadowArgumentsUAV);
                argument(set.batchArguments,set.batchArgumentsUAV);
                passGraph.Forget(set.batchVisibility.Get());set.batchVisibility.Reset();
                set.batchVisibility=Temporary(
                    Buffer(UINT64(capacity)*sizeof(uint32_t),
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                    D3D12_RESOURCE_STATE_COMMON);
                set.batchVisibilityUAV=UavBuffer(set.batchVisibility.Get(),capacity,
                                                sizeof(uint32_t),set.batchVisibilityUAV);
                set.batchVisibilitySRV=Srv(set.batchVisibility.Get(),capacity,
                                           sizeof(uint32_t),set.batchVisibilitySRV);
            }
            if(!set.capacity){
                auto counter=[&](ComPtr<ID3D12Resource>& resource,DescriptorHandle& descriptor){
                    resource=Temporary(Buffer(4,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
                    descriptor=UavBuffer(resource.Get(),1,0);
                };
                counter(set.depthCount,set.depthCountUAV);
                counter(set.cullCount,set.cullCountUAV);
                if(shadow){counter(set.shadowCount,set.shadowCountUAV);
                           counter(set.batchCount,set.batchCountUAV);
                           set.batchRunCapacity=1;}
            }
            set.capacity=capacity;
        };
        ensure(f.cpuVisibility,cpuCount,true);
        ensure(f.publishedVisibility,publishedCount,false);
    }

    void Graphics::Impl::EnsureBatchRunCounters(Frame& f,uint32_t runCount){
        auto& set=f.cpuVisibility;
        runCount=std::max(1u,runCount);
        if(runCount<=set.batchRunCapacity)return;
        const uint32_t capacity=GrowCapacity(set.batchRunCapacity,runCount);
        if(capacity<runCount||capacity>UINT32_MAX/4)
            Fail("Opaque material run counters exceed GPU buffer capacity");
        passGraph.Forget(set.batchCount.Get());set.batchCount.Reset();
        set.batchCount=Temporary(Buffer(uint64_t(capacity)*sizeof(uint32_t),
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                                 D3D12_RESOURCE_STATE_COMMON);
        set.batchCountUAV=UavBuffer(set.batchCount.Get(),capacity,0,set.batchCountUAV);
        set.batchRunCapacity=capacity;
    }

    void Graphics::Impl::EnsureTileBatchBuffers(Frame& f,uint32_t objects,uint32_t blocks){
        auto& v=f.cpuVisibility;
        auto allocate=[&](uint32_t required,uint32_t& capacity,
                          ComPtr<ID3D12Resource>& one,ComPtr<ID3D12Resource>& two,
                          DescriptorHandle& oneUAV,DescriptorHandle& oneSRV,
                          DescriptorHandle& twoUAV,DescriptorHandle& twoSRV){
            required=std::max(1u,required);
            if(required<=capacity)return;
            const uint32_t grown=GrowCapacity(capacity,required);
            if(grown<required)Fail("Tile material batch workspace capacity exhausted");
            auto set=[&](ComPtr<ID3D12Resource>& buffer,
                        DescriptorHandle& uav,DescriptorHandle& srv){
                passGraph.Forget(buffer.Get());
                buffer=Temporary(Buffer(uint64_t(grown)*sizeof(uint32_t),
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
                            D3D12_RESOURCE_STATE_COMMON);
                uav=UavBuffer(buffer.Get(),grown,sizeof(uint32_t),uav);
                srv=Srv(buffer.Get(),grown,sizeof(uint32_t),srv);
            };
            set(one,oneUAV,oneSRV);
            set(two,twoUAV,twoSRV);
            capacity=grown;
        };
        allocate(objects,v.tileCapacity,v.tileBatchMarks,v.tileBatchPrefixes,
                 v.tileMarksUAV,v.tileMarksSRV,v.tilePrefixesUAV,v.tilePrefixesSRV);
        allocate(blocks,v.tileBlockCapacity,v.tileBatchTotals,v.tileBatchOffsets,
                 v.tileTotalsUAV,v.tileTotalsSRV,v.tileOffsetsUAV,v.tileOffsetsSRV);
    }

    void Graphics::Impl::EnsureStreamControl(Frame& f,uint32_t slots){
        if(slots<=f.streamControlSlots)return;uint32_t capacity=GrowCapacity(f.streamControlSlots,slots);passGraph.Forget(f.streamControl.Get());f.streamControl.Reset();f.streamControl=Temporary(Buffer(uint64_t(capacity)*GpuMeshPublicationAbi::ControlBytes),D3D12_RESOURCE_STATE_COMMON);f.streamControlSlots=capacity;
    }

    UINT64 Graphics::Impl::Reserve(Frame& f,UINT64 size,UINT64 alignment){if(!f.uploadGeneration)Fail("frame upload generation missing");auto offset=Align(f.bump,alignment);if(offset+size>f.uploadGeneration->chunk)Fail("upload generation capacity");f.bump=offset+size;return f.base+offset;}

    D3D12_RAYTRACING_GEOMETRY_DESC Graphics::Impl::DynamicGeometry(Frame& f,uint32_t mesh) {
        D3D12_RAYTRACING_GEOMETRY_DESC g{};g.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;g.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;g.Triangles.VertexBuffer={f.dynamicVertices->GetGPUVirtualAddress(),sizeof(Vertex)};g.Triangles.VertexCount=uint32_t(meshes.vertices.size());g.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;g.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(meshes.ranges[mesh].firstIndex)*4;g.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;g.Triangles.IndexCount=meshes.ranges[mesh].indexCount;return g;
    }
}
