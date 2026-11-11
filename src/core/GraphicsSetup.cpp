#include "GraphicsInternal.h"

namespace lab {
    void Graphics::Impl::ApplySurfaceEdits(){
        if(baseSurfaceData.empty())return;surfaceData=baseSurfaceData;
        for(const auto& edit:surfaceEdits){auto s=edit.settings;if(s.material>=surfaceData.size())continue;auto& d=surfaceData[s.material];d.colorRoughness={s.color.x,s.color.y,s.color.z,s.roughness};d.procedural={s.normalAmplitude,s.normalFrequency,s.junctionWidth,s.junctionContrast};}
        staticReflectionCandidates={};for(const auto& instance:sceneCache.StaticInstances())if(instance.material<surfaceData.size())for(uint32_t q=0;q<staticReflectionCandidates.size();q++)staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[instance.material],q);
        surfacesDirty=true;
    }

    void Graphics::Impl::SwapSampleTargets(SampleTargets& t){
        std::swap(hdr,t.hdr);std::swap(reflections,t.reflections);std::swap(depth,t.depth);
        std::swap(hdrResolved,t.hdrResolved);std::swap(reflectionsResolved,t.reflectionsResolved);
        std::swap(depthSRV,t.depthSRV);std::swap(hdrResolvedSRV,t.hdrResolvedSRV);
        std::swap(hdrResolvedUAV,t.hdrResolvedUAV);std::swap(reflectionsResolvedSRV,t.reflectionsResolvedSRV);
    }

    Graphics::Impl::Impl():frameGraph(passGraph){meshDomain=std::make_unique<MeshSolidDomain>(*this);domainRegistry.push_back(meshDomain.get());snowDomain=std::make_shared<SnowDomain>();RegisterDomain(snowDomain);atmosphereDomain=std::make_shared<AtmosphereDomain>();RegisterDomain(atmosphereDomain);opticalDomain=std::make_shared<OpticalDomain>();RegisterDomain(opticalDomain);publications.reserve(32);externalDomains.reserve(8);domainRegistry.reserve(9);}

    ComPtr<ID3D12PipelineState> Graphics::Impl::Pipeline(const wchar_t* name,const D3D12_PIPELINE_STATE_STREAM_DESC& desc,UINT64 hash){wchar_t key[160];swprintf_s(key,L"%s_%016llx_%u_v%u_s%u",name,hash,uint32_t(colorFormat),viewCount,MsaaSamples);ComPtr<ID3D12PipelineState> p;if(!library||FAILED(library->LoadPipeline(key,&desc,IID_PPV_ARGS(&p)))){Hr(device->CreatePipelineState(&desc,IID_PPV_ARGS(&p)),"pipeline state");if(library)library->StorePipeline(key,p.Get());}p->SetName(name);return p;}

    UINT64 Graphics::Impl::Hash(const std::vector<std::byte>& a,UINT64 seed){for(auto b:a){seed^=std::to_integer<uint8_t>(b);seed*=1099511628211ull;}return seed;}

    std::vector<std::byte> Graphics::Impl::Shader(const wchar_t* name){std::wstring path=L"shaders/";path+=name;path+=L".cso";return ReadBytes(AssetPath(path.c_str()));}

    void Graphics::Impl::Pipelines(){
        auto ms=Shader(L"MeshMain"),ps=Shader(L"PixelMain"),shadowMS=Shader(L"ShadowMain"),vs=Shader(L"Fullscreen"),tone=Shader(L"Tonemap"),depthPixel=Shader(L"DepthPixel");std::array<D3D12_VIEW_INSTANCE_LOCATION,MaxViews> locations{};for(uint32_t view=0;view<viewCount;view++)locations[view]={0,view};D3D12_VIEW_INSTANCING_DESC views{viewCount,locations.data(),D3D12_VIEW_INSTANCING_FLAG_NONE};MeshPipeline p{};p.root.data=root.Get();p.ms.data={ms.data(),ms.size()};p.mask.data=UINT_MAX;p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;p.raster.data.MultisampleEnable=MsaaSamples>1;p.depth.data.DepthEnable=TRUE;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.depthFormat.data=DXGI_FORMAT_D32_FLOAT;p.samples.data={MsaaSamples,0};p.views.data=views;p.blend.data.IndependentBlendEnable=TRUE;for(auto& r:p.blend.data.RenderTarget){r.SrcBlend=D3D12_BLEND_ONE;r.DestBlend=D3D12_BLEND_ZERO;r.BlendOp=D3D12_BLEND_OP_ADD;r.SrcBlendAlpha=D3D12_BLEND_ONE;r.DestBlendAlpha=D3D12_BLEND_ZERO;r.BlendOpAlpha=D3D12_BLEND_OP_ADD;r.LogicOp=D3D12_LOGIC_OP_NOOP;r.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(p),&p};p.ps.data={depthPixel.data(),depthPixel.size()};depthPSO=Pipeline(L"depth",desc,Hash(depthPixel,Hash(ms)));p.ps.data={ps.data(),ps.size()};p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[0]=p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;scenePSO=Pipeline(L"scene",desc,Hash(ps,Hash(ms)));p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[1]=DXGI_FORMAT_UNKNOWN;sceneDirectPSO=Pipeline(L"scene_direct",desc,Hash(ps,Hash(ms)));
        // Each texture-presence mask gets the same two render-target layouts.
        // Only masks represented in the loaded material library need a PSO.
        for(uint32_t mask=0;mask<sceneVariantPSO.size();++mask){
            bool used=std::any_of(surfaceData.begin(),surfaceData.end(),
                [&](const Surface& s){return MaterialTextureMask(s)==mask;});
            if(!used){sceneVariantPSO[mask].Reset();sceneDirectVariantPSO[mask].Reset();continue;}
            wchar_t entry[64],name[64];
            swprintf_s(entry,L"PixelMain_M%u",mask);
            auto specialized=Shader(entry);
            p.ps.data={specialized.data(),specialized.size()};
            swprintf_s(name,L"scene_direct_M%u",mask);
            sceneDirectVariantPSO[mask]=Pipeline(name,desc,Hash(specialized,Hash(ms)));
            p.formats.data.NumRenderTargets=2;
            p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            swprintf_s(name,L"scene_M%u",mask);
            sceneVariantPSO[mask]=Pipeline(name,desc,Hash(specialized,Hash(ms)));
            p.formats.data.NumRenderTargets=1;
            p.formats.data.RTFormats[1]=DXGI_FORMAT_UNKNOWN;
        }
        p.ps.data={ps.data(),ps.size()};
        for(auto& blend:p.blend.data.RenderTarget){blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;}transparentDirectPSO=Pipeline(L"transparent_direct",desc,Hash(ps,Hash(ms)));p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;transparentPSO=Pipeline(L"transparent",desc,Hash(ps,Hash(ms)));for(auto& blend:p.blend.data.RenderTarget)blend.BlendEnable=FALSE;p.ps.data={depthPixel.data(),depthPixel.size()};p.ms.data={shadowMS.data(),shadowMS.size()};p.formats.data={};p.samples.data.Count=1;p.views.data={};p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;shadowPSO=Pipeline(L"shadow",desc,Hash(depthPixel,Hash(shadowMS)));
        PresentPipeline f{};f.root.data=root.Get();f.vs.data={vs.data(),vs.size()};f.ps.data={tone.data(),tone.size()};f.blend.data=p.blend.data;f.mask.data=UINT_MAX;f.raster.data=p.raster.data;f.raster.data.MultisampleEnable=FALSE;f.depth.data=p.depth.data;f.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;f.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;f.formats.data.NumRenderTargets=1;f.formats.data.RTFormats[0]=colorFormat;f.depthFormat.data=DXGI_FORMAT_D32_FLOAT;f.samples.data={1,0};f.views.data=views;D3D12_PIPELINE_STATE_STREAM_DESC fd{sizeof(f),&f};presentPSO=Pipeline(L"present",fd,Hash(tone,Hash(vs)));
        auto cs=[&](const wchar_t* entry,ComPtr<ID3D12PipelineState>& out){auto bytes=Shader(entry);ComputePipeline s;s.root.data=root.Get();s.cs.data={bytes.data(),bytes.size()};D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof(s),&s};out=Pipeline(entry,d,Hash(bytes));};cs(L"CullMain",cullPSO);cs(L"BatchOpaqueMain",batchOpaquePSO);cs(L"TileBatchMark",tileBatchMarkPSO);cs(L"TileBatchScan",tileBatchScanPSO);cs(L"TileBatchOffsetScan",tileBatchOffsetScanPSO);cs(L"TileBatchOffsetFinalize",tileBatchOffsetFinalizePSO);cs(L"TileBatchEmit",tileBatchEmitPSO);cs(L"DepthMain",depthResolvePSO);cs(L"HiZMain",hizPSO);cs(L"ClusterCountMain",clusterPSO);cs(L"ClusterPrefixMain",clusterPrefixPSO);cs(L"ClusterOffsetsMain",clusterOffsetsPSO);cs(L"ClusterFillMain",clusterFillPSO);cs(L"DenoiseMain",denoisePSO);cs(L"MergeReflection",mergeReflectionPSO);cs(L"FxaaMain",fxaaPSO);cs(L"DeformMain",deformPSO);cs(L"DeformBoundsMain",deformBoundsPSO);cs(L"HotDataMain",hotDataPSO);cs(L"RayBuildMain",rayBuildPSO);cs(L"RayMapFillMain",rayMapFillPSO);
        if(library&&persistentCache){SIZE_T size=library->GetSerializedSize();std::vector<std::byte> data(size);if(SUCCEEDED(library->Serialize(data.data(),size))){wchar_t suffix[40];swprintf_s(suffix,L".%lu.tmp",GetCurrentProcessId());auto temporary=cachePath+suffix;FILE* file=nullptr;_wfopen_s(&file,temporary.c_str(),L"wb");if(file){bool ok=fwrite(&cacheIdentity,1,sizeof(cacheIdentity),file)==sizeof(cacheIdentity)&&fwrite(data.data(),1,size,file)==size;ok=fclose(file)==0&&ok;if(ok)MoveFileExW(temporary.c_str(),cachePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);else DeleteFileW(temporary.c_str());}}}

    }

    void Graphics::Impl::Initialize(const XrGraphicsRequirementsD3D12KHR& req,bool gpuValidation){if(gpuValidation)validation.Enable();frameGraph.SetDiagnostics(gpuValidation);Hr(CreateDXGIFactory2(gpuValidation?DXGI_CREATE_FACTORY_DEBUG:0,IID_PPV_ARGS(&factory)),"DXGI factory");for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d;a->GetDesc1(&d);if(d.AdapterLuid.HighPart==req.adapterLuid.HighPart&&d.AdapterLuid.LowPart==req.adapterLuid.LowPart){Hr(a.As(&adapter),"DXGI VRAM adapter");break;}}if(!adapter)Fail("OpenXR GPU adapter");DXGI_ADAPTER_DESC1 adapterDesc{};Hr(adapter->GetDesc1(&adapterDesc),"DXGI adapter description");adapterDescription=adapterDesc;if(adapterDesc.DedicatedVideoMemory<ConsoleSpec::MinDedicatedVram)Fail("LaboVR console requires 15 GiB dedicated VRAM");Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_2,IID_PPV_ARGS(&device)),"D3D12 feature level 12_2");validation.Attach(device.Get());D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};Hr(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"Shader Model 6.6");if(sm.HighestShaderModel<D3D_SHADER_MODEL_6_6)Fail("Shader Model 6.6 required");D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&o3,sizeof(o3)),"view instancing");copyQueueTimestamps=o3.CopyQueueTimestampQueriesSupported!=FALSE;Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&o5,sizeof(o5)),"DXR");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,&o7,sizeof(o7)),"mesh shaders");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&o12,sizeof(o12)),"enhanced barriers");if(o3.ViewInstancingTier<D3D12_VIEW_INSTANCING_TIER_1||o5.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||o7.MeshShaderTier==D3D12_MESH_SHADER_TIER_NOT_SUPPORTED||!o12.EnhancedBarriersSupported)Fail("DXR 1.1, mesh shaders, enhanced barriers required");LogEvent(0,"gpu.capabilities","view_instancing=%u dxr=%u mesh=%u enhanced_barriers=%u shader_model=%u",unsigned(o3.ViewInstancingTier),unsigned(o5.RaytracingTier),unsigned(o7.MeshShaderTier),unsigned(o12.EnhancedBarriersSupported),unsigned(sm.HighestShaderModel));residency.Initialize(device.Get(),adapter.Get());vramCache.Initialize(device.Get(),residency);QueueInit(direct,D3D12_COMMAND_LIST_TYPE_DIRECT);QueueInit(compute,D3D12_COMMAND_LIST_TYPE_COMPUTE);QueueInit(copy,D3D12_COMMAND_LIST_TYPE_COPY);passGraph.Initialize(copy,compute,direct);CommandsInit(startup,D3D12_COMMAND_LIST_TYPE_DIRECT);
        auto heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible,ComPtr<ID3D12DescriptorHeap>& out){D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=type;h.NumDescriptors=count;h.Flags=visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE;Hr(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&out)),"descriptor heap");};heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,DescriptorCapacity,true,descriptors);heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,1024,true,samplers);heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,64,false,rtvs);heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,64,false,dsvs);descriptorStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);rtvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);dsvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);D3D12_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_ANISOTROPIC;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;s.MaxAnisotropy=16;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;auto sh=samplers->GetCPUDescriptorHandleForHeapStart();device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&s,sh);
        D3D12_ROOT_PARAMETER1 params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;params[0].Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={1,0,5};D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};rs.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;rs.Desc_1_1.NumParameters=2;rs.Desc_1_1.pParameters=params;rs.Desc_1_1.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED|D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;ComPtr<ID3DBlob> blob;Hr(D3D12SerializeVersionedRootSignature(&rs,&blob,nullptr),"Root Signature 1.1");Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root signature");rootHash=HashMemory(blob->GetBufferPointer(),blob->GetBufferSize());abiHash=HashMemory(GetCoreIdentity().pipelineCacheIdentity,strlen(GetCoreIdentity().pipelineCacheIdentity));D3D12_INDIRECT_ARGUMENT_DESC args[2]{};args[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;args[0].Constant.RootParameterIndex=1;args[0].Constant.Num32BitValuesToSet=1;args[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;D3D12_COMMAND_SIGNATURE_DESC cs{};cs.ByteStride=16;cs.NumArgumentDescs=2;cs.pArgumentDescs=args;Hr(device->CreateCommandSignature(&cs,root.Get(),IID_PPV_ARGS(&indirect)),"mesh indirect signature");D3D12_INDIRECT_ARGUMENT_DESC dispatchArg{};dispatchArg.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC dispatchDesc{};dispatchDesc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);dispatchDesc.NumArgumentDescs=1;dispatchDesc.pArgumentDescs=&dispatchArg;Hr(device->CreateCommandSignature(&dispatchDesc,nullptr,IID_PPV_ARGS(&dispatchIndirect)),"compute dispatch indirect signature");
        DXGI_ADAPTER_DESC desc{};Hr(adapter->GetDesc(&desc),"cache GPU identity");LARGE_INTEGER driver{};persistentCache=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));cacheIdentity.vendor=desc.VendorId;cacheIdentity.device=desc.DeviceId;cacheIdentity.subsystem=desc.SubSysId;cacheIdentity.revision=desc.Revision;cacheIdentity.driverHigh=uint32_t(driver.HighPart);cacheIdentity.driverLow=driver.LowPart;cacheIdentity.root=rootHash;cacheIdentity.abi=abiHash;LogEvent(0,"gpu.identity","vendor=%04x device=%04x revision=%u driver=%u.%u vram_bytes=%llu engine=%s cuda_expected=sm_%u pipeline_abi=%s",desc.VendorId,desc.DeviceId,desc.Revision,cacheIdentity.driverHigh,cacheIdentity.driverLow,static_cast<unsigned long long>(desc.DedicatedVideoMemory),GetCoreIdentity().version,LAB_CUDA_ARCHITECTURE,GetCoreIdentity().pipelineCacheIdentity);
        wchar_t folder[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,folder)))Fail("cache directory");std::wstring dir=folder;dir+=L"\\";dir+=GetCoreIdentity().cacheNamespaceW;dir+=L"_";dir+=GetCoreIdentity().versionW;CreateDirectoryW(dir.c_str(),nullptr);wchar_t name[80];swprintf_s(name,L"\\pipelines_%016llx.bin",HashMemory(&cacheIdentity,sizeof(cacheIdentity)));cachePath=dir+name;
        FILE* cache=nullptr;if(persistentCache)_wfopen_s(&cache,cachePath.c_str(),L"rb");if(cache){CacheHeader header{};bool valid=fread(&header,1,sizeof(header),cache)==sizeof(header)&&memcmp(&header,&cacheIdentity,sizeof(header))==0;if(valid){_fseeki64(cache,0,SEEK_END);auto n=_ftelli64(cache)-int64_t(sizeof(header));_fseeki64(cache,sizeof(header),SEEK_SET);if(n>0&&n<128*1024*1024){cacheBlob.resize(size_t(n));if(fread(cacheBlob.data(),1,cacheBlob.size(),cache)!=cacheBlob.size())cacheBlob.clear();}}fclose(cache);}
        if(FAILED(device->CreatePipelineLibrary(cacheBlob.empty()?nullptr:cacheBlob.data(),cacheBlob.size(),IID_PPV_ARGS(&library)))){cacheBlob.clear();device->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(&library));}
        std::atomic_store_explicit(&uploadGeneration,NewUploadGeneration(UploadChunk),std::memory_order_release);managedMemory.Initialize(device.Get(),residency,*this,desc.DedicatedVideoMemory,this,[](void* context,ID3D12Resource* resource){static_cast<Impl*>(context)->passGraph.Forget(resource);});CollectManaged();domainServicesReady=true;for(auto* domain:domainRegistry)domain->Initialize(*this);
    }

    GpuCacheKey Graphics::Impl::StaticBlasKey(uint32_t mesh) {
        if(mesh>=meshes.ranges.size())Fail("BLAS cache mesh index");const auto& range=meshes.ranges[mesh];uint64_t hash=HashMemory(meshes.vertices.data(),meshes.vertices.size()*sizeof(Vertex));hash=HashMemory(&range.firstIndex,sizeof(range.firstIndex),hash);hash=HashMemory(&range.indexCount,sizeof(range.indexCount),hash);if(range.indexCount)hash=HashMemory(meshes.indices.data()+range.firstIndex,size_t(range.indexCount)*sizeof(uint32_t),hash);uint64_t generation=HashMemory(&abiHash,sizeof(abiHash),hash);if(!hash)hash=1;if(!generation)generation=1;return {hash,generation,CacheStaticBlas};
    }

    void Graphics::Impl::BuildBLAS(){
        uint32_t meshCount=uint32_t(meshes.ranges.size());blas.resize(meshCount);if(!meshCount)return;std::vector<GpuCacheKey> keys(meshCount);std::vector<uint32_t> missing;missing.reserve(meshCount);
        for(uint32_t i=0;i<meshCount;i++){keys[i]=StaticBlasKey(i);auto hit=vramCache.AcquireExisting(keys[i],true);if(hit)blas[i]=hit.resource;else missing.push_back(i);}
        if(missing.empty())return;
        vramCache.RequireActive();if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Stats().headroom))Fail("Static BLAS build exceeds available VRAM budget");residency.Wait(direct.queue.Get());
        startup.Reset();uint32_t missCount=uint32_t(missing.size());std::vector<ComPtr<ID3D12Resource>> scratch(missCount),uncompacted(missCount);auto sizes=Temporary(Buffer(UINT64(missCount)*8,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto readback=Host(UINT64(missCount)*8,D3D12_HEAP_TYPE_READBACK);
        for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];auto& range=meshes.ranges[i];D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.VertexBuffer.StartAddress=vertices->GetGPUVirtualAddress();geometry.Triangles.VertexBuffer.StrideInBytes=sizeof(Vertex);geometry.Triangles.VertexCount=uint32_t(meshes.vertices.size());geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(range.firstIndex)*4;geometry.Triangles.IndexCount=range.indexCount;geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;input.NumDescs=1;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=&geometry;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);uncompacted[ordinal]=Temporary(Buffer(info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);scratch[ordinal]=Temporary(Buffer(info.ScratchDataSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;build.DestAccelerationStructureData=uncompacted[ordinal]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch[ordinal]->GetGPUVirtualAddress();D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compact{};compact.DestBuffer=sizes->GetGPUVirtualAddress()+UINT64(ordinal)*8;compact.InfoType=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;if(ordinal)BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);startup.list->BuildRaytracingAccelerationStructure(&build,1,&compact);}
        BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_COPY_SOURCE);startup.list->CopyBufferRegion(readback.Get(),0,sizes.Get(),0,UINT64(missCount)*8);startup.Submit(direct);WaitStartup(direct);
        UINT64* compactedSizes=nullptr;D3D12_RANGE range{0,SIZE_T(missCount)*8};Hr(readback->Map(0,&range,reinterpret_cast<void**>(&compactedSizes)),"BLAS compact sizes");startup.Reset();for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];if(!compactedSizes[ordinal])Fail("BLAS compact size");auto cached=vramCache.Acquire(keys[i],Buffer(compactedSizes[ordinal],D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true,CompletedFences(),managedMemory.Stats().headroom,L"Cached static BLAS");if(!cached||cached.reused)Fail("Static BLAS cache race");blas[i]=cached.resource;startup.list->CopyRaytracingAccelerationStructure(blas[i]->GetGPUVirtualAddress(),uncompacted[ordinal]->GetGPUVirtualAddress(),D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);BBarrier(startup.list.Get(),blas[i].Get(),D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_RAYTRACING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);}
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);startup.Submit(direct);WaitStartup(direct);for(uint32_t i:missing)vramCache.Commit(blas[i].Get());residency.Submitted(direct.serial);
    }

    void Graphics::Impl::ResetSceneResources(){
        active=nullptr;sceneLoaded=false;historyReady=false;sceneTimeOrigin=0;sceneTimeOriginValid=false;shadowCache={};
        auto release=[&](DescriptorHandle& handle){if(handle.Valid())ReleaseDescriptor(handle);};
        auto resetResource=[&](auto& resource){passGraph.Forget(resource.Get());resource.Reset();};
        auto releaseCached=[&](auto& resource){if(!resource)return;passGraph.Forget(resource.Get());vramCache.Release(resource.Get(),SubmittedFences());resource.Reset();};
        for(auto& texture:textures){release(texture.descriptor);releaseCached(texture.resource);}
        textures.clear();surfaceData.clear();baseSurfaceData.clear();surfacesDirty=false;aaSource={};aaSourceLease.reset();visibleMaterialBits.clear();materialResidencyOffsets.clear();materialResidencyPages.clear();residencyPageSeen.clear();requiredResidencyPages.clear();residencyPageGeneration=1;
        for(auto* handle:{&skinSRV,&morphSRV,&indicesSRV,&verticesSRV,&meshletsSRV,&remapSRV,&trianglesSRV,&rangesSRV,&materialsSRV,&deformationsSRV})release(*handle);
        releaseCached(vertices);releaseCached(indices);releaseCached(meshlets);releaseCached(remap);releaseCached(triangles);releaseCached(ranges);releaseCached(materials);releaseCached(deformations);releaseCached(skinVertices);releaseCached(morphVertices);
        for(auto& [key,tile]:residentTileGpu){
            release(tile.srv);release(tile.runSrv);
            passGraph.Forget(tile.buffer.Get());passGraph.Forget(tile.runBuffer.Get());
        }
        residentTileGpu.clear();
        releaseCached(residentStaticInstances);sceneCache.Reset();rayScene.Reset();staticMaterialBits.clear();staticTransparent.clear();staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};baseLights.clear();baseLightCount=0;
        for(auto& resource:blas)releaseCached(resource);blas.clear();dynamicMeshes.clear();meshDynamicIndex.clear();meshes={};animation=SceneAnimation{};visualFrame={};contributions={};
        for(auto& f:frames){
            for(auto* handle:{&f.motionSRV,&f.boneSRV,&f.weightSRV,&f.deformedBoundsSRV,&f.deformedBoundsUAV,&f.dynamicSRV,&f.dynamicUAV,&f.blasAddressesSRV,&f.rayMapSRV,&f.rayMapUAV,&f.worldInstanceSRV,&f.worldInstanceUAV,&f.publishedInstanceSRV,&f.instanceRangesSRV,&f.lightsSRV,&f.clusterSRV,&f.clusterUAV,
                &f.clusterIndicesSRV,&f.clusterIndicesUAV,
                &f.clusterGroupCountsSRV,&f.clusterGroupCountsUAV,
                &f.clusterGroupOffsetsSRV,&f.clusterGroupOffsetsUAV,
                &f.clusterDirectionalIndicesSRV,&f.clusterDirectionalIndicesUAV,
                &f.clusterDirectionalCountSRV,&f.clusterDirectionalCountUAV,
                &f.clusterBoundsSRV,&f.clusterBoundsUAV,
                &f.reflectedCellsSRV,&f.reflectedIndicesSRV,
                &f.reflectedDirectionalsSRV})release(*handle);
            resetResource(f.motionBuffer);resetResource(f.boneBuffer);resetResource(f.weightBuffer);resetResource(f.deformedBounds);resetResource(f.dynamicVertices);resetResource(f.dynamicScratch);resetResource(f.blasAddresses);resetResource(f.lights);resetResource(f.clusters);
            resetResource(f.clusterIndices);resetResource(f.clusterGroupCounts);
            resetResource(f.clusterGroupOffsets);resetResource(f.clusterDirectionalIndices);
            resetResource(f.clusterDirectionalCount);resetResource(f.clusterBounds);
            f.lightCapacity=f.clusterIndexCapacity=f.directionalCapacity=0;
            f.lightData.clear();f.lightCount=0;
            for(auto* resource:{f.reflectedCellsGpu.Get(),f.reflectedIndicesGpu.Get(),
                                f.reflectedDirectionalsGpu.Get()})passGraph.Forget(resource);
            f.reflectedCellsGpu.Reset();f.reflectedIndicesGpu.Reset();
            f.reflectedDirectionalsGpu.Reset();
            if(f.reflectedUploadMapped){
                f.reflectedUpload->Unmap(0,nullptr);f.reflectedUploadMapped=nullptr;
            }
            f.reflectedUpload.Reset();f.reflectedUploadCapacity=0;
            f.reflectedCellCapacity=f.reflectedIndexCapacity=f.reflectedDirectionalCapacity=0;
            f.reflectedCells.clear();f.reflectedIndices.clear();
            f.reflectedDirectionals.clear();f.reflectedLightSnapshot.clear();
            f.reflectedReady=f.reflectedUpdated=false;
            resetResource(f.rayInstanceMap);
            for(auto* h:{&f.cpuVisibility.tileMarksUAV,&f.cpuVisibility.tileMarksSRV,
                &f.cpuVisibility.tilePrefixesUAV,&f.cpuVisibility.tilePrefixesSRV,
                &f.cpuVisibility.tileTotalsUAV,&f.cpuVisibility.tileTotalsSRV,
                &f.cpuVisibility.tileOffsetsUAV,&f.cpuVisibility.tileOffsetsSRV})
                release(*h);
            resetResource(f.cpuVisibility.tileBatchMarks);
            resetResource(f.cpuVisibility.tileBatchPrefixes);
            resetResource(f.cpuVisibility.tileBatchTotals);
            resetResource(f.cpuVisibility.tileBatchOffsets);
            f.cpuVisibility.tileCapacity=f.cpuVisibility.tileBlockCapacity=0;
            resetResource(f.worldInstances);resetResource(f.publishedInstances);
            if(f.instanceRanges&&f.instanceRangeData){f.instanceRanges->Unmap(0,nullptr);f.instanceRangeData=nullptr;}
            f.instanceRanges.Reset();f.instanceRangeCapacity=2;f.worldInstanceCapacity=f.publishedInstanceCapacity=0;
            f.tileTransfers.clear();f.tileRunTransfers.clear();f.heldTileSources.clear();
            f.tileDrawCache.clear();f.snapshotCount=f.residentCount=0;
            f.rayMapSegments.clear();f.committedRayMapSegments.clear();
            f.rayMapBuild=false;f.rayMapCapacity=0;f.rayInstanceCount=0;
            f.dynamicBLAS.clear();f.dynamicBlasBuilt.clear();f.dynamicGeometryRevision.clear();f.dynamicBlasRevision.clear();f.pendingDynamicRevision.clear();f.deformationWork.Reset();f.blasWork.Reset();f.dynamicScratchBytes=0;f.lastTlasCount=0;f.tlasBuilt=false;f.transparent.clear();f.transparentCount=0;f.cpuCount=0;f.dynamicCount=0;f.additionalCount=0;f.cpuObjects.clear();f.objects=nullptr;
        }
        descriptorAllocator.Collect(CompletedFences());vramCache.Collect(CompletedFences());
    }

    void Graphics::Impl::ConfigureSampleTargets(bool fresh){
        auto texture=[&](TextureResource& r,DXGI_FORMAT format,UINT samples,UINT layers,UINT levels,D3D12_RESOURCE_FLAGS flags){
            auto desc=Texture(width,height,format,samples,layers,levels,flags);
            if(r.resource&&SameResourceDesc(r.resource->GetDesc(),desc))return;
            passGraph.Forget(r.resource.Get());r.resource.Reset();r.layout=D3D12_BARRIER_LAYOUT_COMMON;
            D3D12_CLEAR_VALUE clear{};clear.Format=format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_D32_FLOAT:format;clear.DepthStencil.Depth=0;
            r.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON,(flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?&clear:nullptr);
        };
        auto hdrFlags=D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|(MsaaSamples==1?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE));texture(hdr,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,hdrFlags);texture(reflections,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);texture(depth,DXGI_FORMAT_R32_TYPELESS,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);hdrRTV=MakeRTV(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:hdrRTV);reflectionsRTV=MakeRTV(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:reflectionsRTV);depthDSV=MakeDSV(depth.resource.Get(),fresh?Invalid:depthDSV);depthSRV=TextureSrv(depth.resource.Get(),DXGI_FORMAT_R32_FLOAT,depthSRV);
        if(MsaaSamples>1){texture(hdrResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));hdrResolvedRTV=MakeRTV(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh||hdrResolvedRTV==Invalid?Invalid:hdrResolvedRTV);texture(reflectionsResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_NONE);hdrResolvedSRV=TextureSrv(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV);hdrResolvedUAV=TextureUav(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV);reflectionsResolvedSRV=TextureSrv(reflectionsResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV);}else{hdrResolvedSRV=TextureSrv(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV);hdrResolvedUAV=TextureUav(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV);reflectionsResolvedSRV=TextureSrv(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV);}
    }

    void Graphics::Impl::ApplyMultisampling(){
        if(MsaaSamples==requestedSamples)return;
        // Rare option edit: retire all consumers before rebinding cached targets.
        WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);
        SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        MsaaSamples=requestedSamples;SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        ConfigureSampleTargets(false);Pipelines();historyReady=false;
    }

    void Graphics::Impl::Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra){
        if(configuredXr){if(configuredXr!=&xr||jobs!=&workers)Fail("Graphics runtime services cannot change during scene replacement");WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();xrRTV.clear();xrDSV.clear();rtvCount=dsvCount=0;}
        else {configuredXr=&xr;jobs=&workers;}
        contributions=extra;if(assets.lights.size()>UINT32_MAX)Fail("Scene light count exceeds 32-bit index");baseLightCount=uint32_t(assets.lights.size());baseLights=assets.lights;for(uint32_t i=0;i<baseLightCount;i++)baseLights[i].id=0x80010000u+i;visualFrame={};visualFrame.objects.reserve(256);width=xr.Width();height=xr.Height();viewCount=xr.ViewCount();if(!viewCount||viewCount>MaxViews)Fail("Unsupported XR view count");renderWidth=width;renderHeight=height;colorFormat=xr.Format();mips=std::bit_width(std::max(width,height));renderMips=mips;clustersCount=((width+63)/64)*((height+63)/64)*24*viewCount;if(MsaaSamples>1)for(DXGI_FORMAT format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,MsaaSamples,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)),"MSAA 4x");if(!q.NumQualityLevels)Fail("MSAA 4x required");}
        // OpenXR hands off legacy RENDER_TARGET / DEPTH_WRITE states. Keep these
        // images outside enhanced-layout tracking for their entire acquired lifetime.
        xrColorTargets.clear();for(const auto& image:xr.Colors()){xrRTV.push_back(MakeRTV(image.texture,colorFormat));xrColorTargets.emplace_back(image.texture);}
        for(const auto& image:xr.Depths())xrDSV.push_back(MakeDSV(image.texture));
        auto texture=[&](TextureResource& r,DXGI_FORMAT format,UINT samples,UINT layers,UINT levels,D3D12_RESOURCE_FLAGS flags,UINT w=0,UINT h=0){auto desc=Texture(w?w:width,h?h:height,format,samples,layers,levels,flags);if(r.resource&&SameResourceDesc(r.resource->GetDesc(),desc)){passGraph.Forget(r.resource.Get());return;}D3D12_CLEAR_VALUE clear{};clear.Format=format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_D32_FLOAT:format;clear.DepthStencil.Depth=0;passGraph.Forget(r.resource.Get());r.resource.Reset();r.layout=D3D12_BARRIER_LAYOUT_COMMON;r.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON,(flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?&clear:nullptr);};
        hdrResolvedRTV=Invalid;ConfigureSampleTargets(true);
        texture(hierarchy,DXGI_FORMAT_R32_FLOAT,1,viewCount,mips,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);hierarchySRV=TextureSrv(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,hierarchySRV);if(hierarchyUAV.size()>mips){for(uint32_t i=mips;i<hierarchyUAV.size();i++)ReleaseDescriptor(hierarchyUAV[i]);hierarchyUAV.resize(mips);}uint32_t oldHiZMips=uint32_t(hierarchyUAV.size());hierarchyUAV.resize(mips);for(uint32_t i=0;i<mips;i++)hierarchyUAV[i]=TextureUav(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,i,i<oldHiZMips?hierarchyUAV[i]:DescriptorHandle{});
        texture(shadow,DXGI_FORMAT_R32_TYPELESS,1,1,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,3072,2048);shadowDSV=MakeDSV(shadow.resource.Get());shadowSRV=TextureSrv(shadow.resource.Get(),DXGI_FORMAT_R32_FLOAT,shadowSRV);
        for(uint32_t i=0;i<history.size();i++){texture(history[i],DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);texture(historyDepth[i],DXGI_FORMAT_R32_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);historySRV[i]=TextureSrv(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,historySRV[i]);historyUAV[i]=TextureUav(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,historyUAV[i]);historyDepthSRV[i]=TextureSrv(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,historyDepthSRV[i]);historyDepthUAV[i]=TextureUav(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,0,historyDepthUAV[i]);}
        meshes=assets.meshes;animation.Initialize(assets);meshDynamicIndex.resize(meshes.ranges.size(),Invalid);for(uint32_t i=0;i<meshes.deformations.size();i++)if(meshes.deformations[i].axisAmplitude.w!=0||meshes.motions[i].node!=Invalid){meshDynamicIndex[i]=uint32_t(dynamicMeshes.size());dynamicMeshes.push_back(i);}startup.Reset();vertices=CachedUploadVector(meshes.vertices,CacheVertices);indices=CachedUploadVector(meshes.indices,CacheIndices);meshlets=CachedUploadVector(meshes.meshlets,CacheMeshlets);remap=CachedUploadVector(meshes.remap,CacheRemap);triangles=CachedUploadVector(meshes.triangles,CacheTriangles);ranges=CachedUploadVector(meshes.ranges,CacheRanges);indicesSRV=Srv(indices.Get(),uint32_t(meshes.indices.size()),4,indicesSRV);verticesSRV=Srv(vertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),verticesSRV);meshletsSRV=Srv(meshlets.Get(),uint32_t(meshes.meshlets.size()),sizeof(Meshlet),meshletsSRV);remapSRV=Srv(remap.Get(),uint32_t(meshes.remap.size()),4,remapSRV);trianglesSRV=Srv(triangles.Get(),uint32_t(meshes.triangles.size()),12,trianglesSRV);rangesSRV=Srv(ranges.Get(),uint32_t(meshes.ranges.size()),sizeof(MeshRange),rangesSRV);
        if(assets.samplers.size()>1021)Fail("material sampler capacity");
        for(uint32_t i=0;i<assets.samplers.size();i++){const auto& source=assets.samplers[i];D3D12_SAMPLER_DESC sampler{};auto address=[](uint32_t mode){return mode==33071?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:mode==33648?D3D12_TEXTURE_ADDRESS_MODE_MIRROR:D3D12_TEXTURE_ADDRESS_MODE_WRAP;};sampler.AddressU=address(source.wrapS);sampler.AddressV=address(source.wrapT);sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;bool minLinear=source.minFilter==9729||source.minFilter==9985||source.minFilter==9987,magLinear=source.magFilter!=9728,mipLinear=source.minFilter==9986||source.minFilter==9987;sampler.Filter=D3D12_ENCODE_BASIC_FILTER(minLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,magLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,mipLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,D3D12_FILTER_REDUCTION_TYPE_STANDARD);sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;sampler.MaxLOD=source.minFilter==9728||source.minFilter==9729?0:D3D12_FLOAT32_MAX;auto handle=samplers->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(i+3)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);device->CreateSampler(&sampler,handle);}
        std::vector<uint32_t> textureDescriptors;textureDescriptors.reserve(assets.textures.size());for(const auto& texture:assets.textures)textureDescriptors.push_back(UploadTexture(texture));surfaceData.reserve(assets.materials.size());for(const auto& material:assets.materials){Surface s{};s.colorRoughness=material.colorRoughness;s.emissionMetallic=material.emissionMetallic;s.albedo=material.albedo==Invalid?Invalid:textureDescriptors[material.albedo];s.normal=material.normal==Invalid?Invalid:textureDescriptors[material.normal];s.metalRough=material.metalRough==Invalid?Invalid:textureDescriptors[material.metalRough];s.emissive=material.emissive==Invalid?Invalid:textureDescriptors[material.emissive];s.parameters={material.normalScale,material.heightUvScale.x,material.heightUvScale.y,material.macroVariation};s.extra={material.occlusion==Invalid?Invalid:textureDescriptors[material.occlusion],material.alphaMode,material.doubleSided,material.unlit};s.opacity={material.alpha,material.cutoff,material.occlusionStrength,0};s.mapping=material.mapping;for(uint32_t slot=0;slot<5;slot++)s.mapping[slot].rotationSet.w=material.samplers[slot]==Invalid?0.f:float(3+material.samplers[slot]);surfaceData.push_back(s);}
        visibleMaterialBits.assign((surfaceData.size()+63)/64,0);materialResidencyOffsets.resize(surfaceData.size()+1);std::vector<uint32_t> textureResidencyByDescriptor(DescriptorCapacity,Invalid);uint32_t maxTextureResidency=0;for(const auto& texture:textures){uint32_t descriptor=Physical(texture.descriptor);textureResidencyByDescriptor[descriptor]=texture.residency;maxTextureResidency=std::max(maxTextureResidency,texture.residency);}
        baseSurfaceData=surfaceData;ApplySurfaceEdits();
        materialResidencyPages.clear();materialResidencyPages.reserve(surfaceData.size()*2);for(uint32_t material=0;material<surfaceData.size();material++){materialResidencyOffsets[material]=uint32_t(materialResidencyPages.size());const auto& surface=surfaceData[material];const uint32_t descriptors[]={surface.albedo,surface.normal,surface.metalRough,surface.emissive,surface.extra.x};for(uint32_t descriptor:descriptors){if(descriptor==Invalid)continue;if(descriptor>=textureResidencyByDescriptor.size()||textureResidencyByDescriptor[descriptor]==Invalid)Fail("material texture residency dependency");uint32_t page=textureResidencyByDescriptor[descriptor];bool duplicate=false;for(uint32_t i=materialResidencyOffsets[material];i<materialResidencyPages.size();i++)if(materialResidencyPages[i]==page){duplicate=true;break;}if(!duplicate)materialResidencyPages.push_back(page);}}materialResidencyOffsets[surfaceData.size()]=uint32_t(materialResidencyPages.size());residencyPageSeen.assign(textures.empty()?0:maxTextureResidency+1,0);requiredResidencyPages.clear();requiredResidencyPages.reserve(textures.size());std::vector<GpuInstance> staticInstances;staticInstances.resize(assets.objects.size());staticMaterialBits.assign(visibleMaterialBits.size(),0);staticTransparent.clear();staticTransparent.reserve(assets.objects.size());staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};std::vector<uint8_t> staticDeformedSeen(dynamicMeshes.size(),0),staticDeformedRtSeen(dynamicMeshes.size(),0);for(uint32_t i=0;i<assets.objects.size();i++){auto object=assets.objects[i];object.id=0x80000000u+i;auto gpu=ConvertObject(object,1.f);staticInstances[i]=gpu;if(gpu.material<surfaceData.size()){staticMaterialBits[gpu.material>>6]|=uint64_t(1)<<(gpu.material&63);for(uint32_t q=0;q<staticReflectionCandidates.size();q++)staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[gpu.material],q);}if(gpu.flags&AlphaBlendObject)staticTransparent.push_back(i);if(gpu.flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[gpu.mesh];if(dynamicIndex==Invalid)Fail("DeformedObject references mesh without dynamic deformation capability");if(!staticDeformedSeen[dynamicIndex]){staticDeformedSeen[dynamicIndex]=1;staticDeformedMeshes.push_back(dynamicIndex);}if(!(gpu.flags&NoRayTracing)&&!staticDeformedRtSeen[dynamicIndex]){staticDeformedRtSeen[dynamicIndex]=1;staticDeformedRtMeshes.push_back(dynamicIndex);}}}sceneCache.Replace(std::move(staticInstances));rayScene.ReplaceStatic(sceneCache.StaticInstances());if(sceneCache.StaticCount())residentStaticInstances=CachedUploadBuffer(sceneCache.StaticInstances().data(),uint64_t(sceneCache.StaticCount())*sizeof(GpuInstance),CacheStaticInstances,sceneCache.ContentGeneration());materials=UploadVector(surfaceData);surfacesDirty=false;materialsSRV=Srv(materials.Get(),uint32_t(surfaceData.size()),sizeof(Surface),materialsSRV);deformations=CachedUploadVector(meshes.deformations,CacheDeformations);deformationsSRV=Srv(deformations.Get(),uint32_t(meshes.deformations.size()),sizeof(Deformation),deformationsSRV);skinVertices=CachedUploadVector(meshes.skin,CacheSkin);skinSRV=Srv(skinVertices.Get(),uint32_t(meshes.skin.size()),sizeof(SkinVertex),skinSRV);if(meshes.morphs.empty()){MorphVertex zero;morphVertices=CachedUploadBuffer(&zero,sizeof(zero),CacheMorphs);}else morphVertices=CachedUploadVector(meshes.morphs,CacheMorphs);morphSRV=Srv(morphVertices.Get(),uint32_t(std::max(size_t(1),meshes.morphs.size())),sizeof(MorphVertex),morphSRV);startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);uploads.clear();BuildBLAS();
        for(auto& f:frames){
            DynamicResources(f);EnsureInstances(f,sceneCache.StaticCount()+128,sceneCache.StaticCount()+128,sceneCache.StaticCount()+128);
            if(f.dynamicScratchBytes)f.dynamicScratch=Temporary(Buffer(f.dynamicScratchBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            EnsureLightBuffers(f);
        }
        auto name=[&](ID3D12Resource* resource,const wchar_t* wide,const char* label){if(resource){resource->SetName(wide);LogEvent(0,"gpu.resource","name=%s resource=%p width=%llu height=%u layers=%u",label,static_cast<void*>(resource),static_cast<unsigned long long>(resource->GetDesc().Width),resource->GetDesc().Height,unsigned(resource->GetDesc().DepthOrArraySize));}};
        name(residentStaticInstances.Get(),L"Persistent static instances","Persistent static instances");name(hdr.resource.Get(),L"HDR views","HDR views");name(reflections.resource.Get(),L"Reflections views","Reflections views");name(depth.resource.Get(),L"Depth reverse Z","Depth reverse Z");name(hierarchy.resource.Get(),L"HiZ views","HiZ views");name(shadow.resource.Get(),L"Static point shadow atlas","Static point shadow atlas");name(hdrResolved.resource.Get(),L"HDR resolved","HDR resolved");name(reflectionsResolved.resource.Get(),L"Reflections resolved","Reflections resolved");
        for(uint32_t i=0;i<history.size();i++){name(history[i].resource.Get(),L"Reflection history","Reflection history");name(historyDepth[i].resource.Get(),L"History depth","History depth");}
        for(auto& f:frames){name(f.tlas.Get(),L"Frame TLAS","Frame TLAS");name(f.worldInstances.Get(),L"World instances","World instances");name(f.publishedInstances.Get(),L"Published instances","Published instances");name(f.streamControl.Get(),L"GPU publication control","GPU publication control");name(f.rayDescriptors.Get(),L"TLAS instances","TLAS instances");name(f.rayInstanceMap.Get(),L"Ray to render instance mapping","Ray instance mapping");name(f.dynamicVertices.Get(),L"Deformed vertices","Deformed vertices");name(f.clusters.Get(),L"Light clusters","Light clusters");}
        startup.Reset();for(auto& f:frames){std::vector<XMUINT4> addresses(meshes.ranges.size());for(uint32_t i=0;i<addresses.size();i++){auto a=blas[i]->GetGPUVirtualAddress();uint32_t dynamic=meshDynamicIndex[i];auto b=dynamic==Invalid?a:f.dynamicBLAS[dynamic]->GetGPUVirtualAddress();addresses[i]={uint32_t(a),uint32_t(a>>32),uint32_t(b),uint32_t(b>>32)};}f.blasAddresses=UploadVector(addresses);f.blasAddressesSRV=Srv(f.blasAddresses.Get(),uint32_t(addresses.size()),sizeof(XMUINT4),f.blasAddressesSRV);}startup.Submit(direct);WaitStartup(direct);uploads.clear();
        for(auto& f:frames)InitializeResourceAbi(f);
        Pipelines();sceneLoaded=true;
    }

    void Graphics::Impl::ReplaceScene(const SceneData& assets,RenderContributions extra){if(!configuredXr||!jobs)Fail("Graphics runtime is not configured");Configure(*configuredXr,assets,*jobs,extra);}

    void Graphics::Impl::UnloadScene(){if(!configuredXr||!sceneLoaded)return;WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();}

    Graphics::Impl::~Impl(){if(device){residency.Wait(direct.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(copy.queue.Get());WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);publications.clear();externalDomains.clear();terminalDomain.reset();atmosphereDomain.reset();opticalDomain.reset();snowDomain.reset();aaSource={};aaSourceLease.reset();managedMemory.EndFrame(SubmittedFences());CollectManaged();managedMemory.Shutdown();vramCache.Shutdown(CompletedFences());descriptorAllocator.EndRecording(SubmittedFences());descriptorAllocator.Collect(CompletedFences());}library.Reset();}

    uint64_t Graphics::Impl::HashMemory(const void* data,size_t size,uint64_t hash){auto* bytes=static_cast<const uint8_t*>(data);for(size_t i=0;i<size;i++){hash^=bytes[i];hash*=1099511628211ull;}return hash;}

    GpuInstance Graphics::Impl::ConvertResidentBase(const GpuResidentTileObject& o)const{
        RenderObject scene{};
        scene.current.p={o.position.x,o.position.y,o.position.z};
        scene.previous.p={o.previousPosition.x,o.previousPosition.y,o.previousPosition.z};
        scene.current.q={o.rotation.x,o.rotation.y,o.rotation.z,o.rotation.w};
        scene.previous.q={o.previousRotation.x,o.previousRotation.y,o.previousRotation.z,o.previousRotation.w};
        scene.scale={o.scale.x,o.scale.y,o.scale.z};
        scene.mesh=o.mesh;scene.material=o.material;scene.flags=o.flags;scene.id=o.renderId;
        return ConvertObject(scene,0.f);
    }

    GpuInstance Graphics::Impl::ConvertResidentObject(const GpuResidentTileObject& o,double x,double z)const{
        GpuInstance out=ConvertResidentBase(o);
        const double dx=double(o.cellX)-x,dz=double(o.cellZ)-z;
        const double range=double(FarZ)+48.+double(o.cellRadius);
        const double ox=double(o.position.x)-x,oz=double(o.position.z)-z;
        if(!o.enabled||dx*dx+dz*dz>range*range){
            out.flags|=NoShadow|NoRayTracing|128u;return out;
        }
        if(ox*ox+oz*oz>30.*30.){
            if(o.hideFar){out.flags|=NoShadow|NoRayTracing|128u;return out;}
            if(o.farMesh!=UINT32_MAX){
                out.mesh=o.farMesh;
                const auto& bounds=meshes.ranges[out.mesh].bounds;
                out.positionRadius.w=bounds.w*std::max({
                    std::abs(out.scale.x),std::abs(out.scale.y),std::abs(out.scale.z)});
            }
        }
        return out;
    }

    GpuInstance Graphics::Impl::ConvertObject(const RenderObject& o,float alpha)const{
        if(o.mesh>=meshes.ranges.size()||o.material>=surfaceData.size())Fail("render asset handle");
        // Identical consecutive poses are extremely common in streamed scenery.
        // Avoid quaternion slerp and position lerp when the exact input is
        // stationary; all moving PhysX bodies retain their original Blend path.
        const bool stationary=
            o.previous.p.x==o.current.p.x&&o.previous.p.y==o.current.p.y&&
            o.previous.p.z==o.current.p.z&&
            o.previous.q.x==o.current.q.x&&o.previous.q.y==o.current.q.y&&
            o.previous.q.z==o.current.q.z&&o.previous.q.w==o.current.q.w;
        const Pose p=stationary?o.current:Blend(o.previous,o.current,alpha);
        GpuInstance out{};auto bounds=meshes.ranges[o.mesh].bounds;float expansion=(o.flags&DeformedObject)?1+std::abs(meshes.deformations[o.mesh].axisAmplitude.w):1;
        out.positionRadius={p.p.x,p.p.y,p.p.z,bounds.w*std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)})*expansion};out.rotation={p.q.x,p.q.y,p.q.z,p.q.w};out.scale={o.scale.x,o.scale.y,o.scale.z,0};out.mesh=o.mesh;out.material=o.material;out.id=o.id;out.flags=o.flags;if(surfaceData[o.material].extra.y==2)out.flags|=DynamicObject|AlphaBlendObject;return out;
    }
}
