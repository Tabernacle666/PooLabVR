#include "GraphicsInternal.h"
#include <numeric>

namespace OASIS {
    void Graphics::Impl::ApplySurfaceEdits(std::span<const uint32_t> indices){
        if(baseSurfaceData.empty())return;
        std::vector<uint32_t> all;
        if(indices.empty()){
            all.resize(baseSurfaceData.size());
            std::iota(all.begin(),all.end(),0u);
            indices=all;
        }
        for(uint32_t i:indices){
            Surface surface=baseSurfaceData[i];
            for(const auto& edit:surfaceEdits)if(edit.material==i){
                surface.colorRoughness={edit.color.x,edit.color.y,edit.color.z,edit.roughness};
                surface.procedural={edit.normalAmplitude,edit.normalFrequency,edit.junctionWidth,edit.junctionContrast};
            }
            const auto slot=materialSlots[i];
            if(slot<materialTuning.size()&&materialTuning[slot]!=DefaultMaterialTuning){
                const auto& values=materialTuning[slot];
                auto percent=[&](MaterialParameter parameter){return .01f*float(values[size_t(parameter)]);};
                surface.tintGamma={percent(MaterialParameter::Red),percent(MaterialParameter::Green),
                                   percent(MaterialParameter::Blue),percent(MaterialParameter::Gamma)};
                surface.overrideUv=percent(MaterialParameter::Uv);
                for(auto& mapping:surface.mapping){
                    mapping.offsetScale.z*=surface.overrideUv;
                    mapping.offsetScale.w*=surface.overrideUv;
                }
                surface.roughnessBias=percent(MaterialParameter::Roughness);
                surface.metallicBias=percent(MaterialParameter::Metallic);
                surface.parameters.x*=percent(MaterialParameter::Normal);
                const float emission=percent(MaterialParameter::Emission);
                surface.emissionMetallic.x*=emission;
                surface.emissionMetallic.y*=emission;
                surface.emissionMetallic.z*=emission;
                const float opacity=percent(MaterialParameter::Opacity);
                surface.opacity.x=std::min(1.f,surface.opacity.x*opacity);
                surface.opacity.w=opacity;
            }
            const auto& previous=surfaceData[i];
            if(std::memcmp(&surface,&previous,sizeof(Surface))==0)continue;
            if(surface.extra.y!=0u&&
               (surface.opacity.x!=previous.opacity.x||surface.overrideUv!=previous.overrideUv))
                ++shadowSurfaceRevision[i];
            surfaceData[i]=surface;
            ++roomProbeMaterialRevision;
            auto it=dirtySurfaces.begin();
            uint32_t first=i,end=i+1u;
            while(it!=dirtySurfaces.end()&&it->first+it->count<first)++it;
            while(it!=dirtySurfaces.end()&&it->first<=end){
                first=std::min(first,it->first);
                end=std::max(end,it->first+it->count);
                it=dirtySurfaces.erase(it);
            }
            dirtySurfaces.insert(it,{first,end-first});
        }
        staticReflectionCandidates={};
        for(uint32_t i=0;i<surfaceData.size();++i){
            if(i/64u>=staticMaterialBits.size()||
               (staticMaterialBits[i/64u]&(uint64_t(1)<<(i%64u)))==0)continue;
            for(uint32_t q=0;q<staticReflectionCandidates.size();++q)
                staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[i],q);
        }
    }

    void Graphics::Impl::SwapSampleTargets(SampleTargets& t){
        std::swap(hdr,t.hdr);std::swap(reflections,t.reflections);std::swap(depth,t.depth);
        std::swap(hdrResolved,t.hdrResolved);std::swap(reflectionsResolved,t.reflectionsResolved);
        std::swap(depthSRV,t.depthSRV);std::swap(hdrResolvedSRV,t.hdrResolvedSRV);
        std::swap(hdrResolvedUAV,t.hdrResolvedUAV);std::swap(reflectionsResolvedSRV,t.reflectionsResolvedSRV);
    }

    Graphics::Impl::Impl():frameGraph(passGraph){meshDomain=std::make_unique<MeshSolidDomain>(*this);domainRegistry.push_back(meshDomain.get());publications.reserve(32);externalDomains.reserve(8);domainRegistry.reserve(9);}

    ComPtr<ID3D12PipelineState> Graphics::Impl::Pipeline(const wchar_t* name,const D3D12_PIPELINE_STATE_STREAM_DESC& desc,UINT64 hash){
        // NVAPI opts in at device level, outside the D3D12 PSO descriptor.
        const wchar_t* clusterSuffix=forestStatus.configured==Mg2RayBackend::Nvapi?L"_nvapi":L"";
        wchar_t key[160];swprintf_s(key,L"%s_%016llx_%u_v%u_s%u%s",name,hash,uint32_t(colorFormat),viewCount,MsaaSamples,clusterSuffix);
        ComPtr<ID3D12PipelineState> p;
        if(!library||FAILED(library->LoadPipeline(key,&desc,IID_PPV_ARGS(&p)))){
            Hr(device->CreatePipelineState(&desc,IID_PPV_ARGS(&p)),"pipeline state");
            if(library)library->StorePipeline(key,p.Get());
        }
        p->SetName(name);
        return p;
    }

    UINT64 Graphics::Impl::Hash(const std::vector<std::byte>& a,UINT64 seed){for(auto b:a){seed^=std::to_integer<uint8_t>(b);seed*=1099511628211ull;}return seed;}

    std::vector<std::byte> Graphics::Impl::Shader(const wchar_t* name){std::wstring path=L"shaders/";path+=name;path+=L".cso";return ReadBytes(AssetPath(path.c_str()));}

    void Graphics::Impl::Pipelines(){
        const auto vs=Shader(L"Fullscreen"),tone=Shader(L"Tonemap");
        std::array<D3D12_VIEW_INSTANCE_LOCATION,MaxViews> locations{};
        for(uint32_t view=0;view<viewCount;++view)locations[view]={0,view};
        const D3D12_VIEW_INSTANCING_DESC views{viewCount,locations.data(),
            D3D12_VIEW_INSTANCING_FLAG_NONE};

        PresentPipeline present{};
        present.root.data=root.Get();
        present.vs.data={vs.data(),vs.size()};present.ps.data={tone.data(),tone.size()};
        present.mask.data=UINT_MAX;
        present.raster.data.FillMode=D3D12_FILL_MODE_SOLID;
        present.raster.data.CullMode=D3D12_CULL_MODE_NONE;
        present.raster.data.DepthClipEnable=TRUE;
        present.depth.data.DepthEnable=TRUE;
        present.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
        present.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        present.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        present.formats.data.NumRenderTargets=1;
        present.formats.data.RTFormats[0]=colorFormat;
        present.depthFormat.data=DXGI_FORMAT_D32_FLOAT;
        present.samples.data={1,0};present.views.data=views;
        for(auto& blend:present.blend.data.RenderTarget){
            blend.SrcBlend=D3D12_BLEND_ONE;blend.DestBlend=D3D12_BLEND_ZERO;
            blend.BlendOp=D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_ZERO;
            blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;blend.LogicOp=D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        const D3D12_PIPELINE_STATE_STREAM_DESC presentDesc{sizeof(present),&present};
        const auto fullscreenHash=Hash(vs);
        presentPSO=Pipeline(L"present",presentDesc,Hash(tone,fullscreenHash));
        if(dlss.available&&MsaaSamples==1u&&!dlssPresentPSO){
            const auto dlssTone=Shader(L"TonemapDlss");
            present.ps.data={dlssTone.data(),dlssTone.size()};
            dlssPresentPSO=Pipeline(L"present_dlss",presentDesc,Hash(dlssTone,fullscreenHash));
        }

        if(sphereBaselineTiming&&viewCount==2u){
            const auto perception=Shader(L"TonemapSpherePerception");
            present.ps.data={perception.data(),perception.size()};
            spherePerceptionPSO=Pipeline(L"present_sphere_perception",presentDesc,Hash(perception,fullscreenHash));
        }

        if(sphereLod.enabled){
            const auto proxyVS=Shader(L"SphereProxyBounds"),proxyPS=Shader(L"SphereProxyPixel");
            present.vs.data={proxyVS.data(),proxyVS.size()};
            present.ps.data={proxyPS.data(),proxyPS.size()};
            sphereProxyPSO=Pipeline(L"sphere_proxy",presentDesc,Hash(proxyPS,Hash(proxyVS)));
            const auto inspect=Shader(L"SphereProxyInspect");
            present.vs.data={vs.data(),vs.size()};
            present.ps.data={inspect.data(),inspect.size()};
            sphereInspectPSO=Pipeline(L"sphere_proxy_inspect",presentDesc,Hash(inspect,fullscreenHash));
        }

        if(meshes.ranges.empty())return;

        const auto debrisVS=Shader(L"DebrisVS"),debrisPS=Shader(L"DebrisPS");
        present.vs.data={debrisVS.data(),debrisVS.size()};
        present.ps.data={debrisPS.data(),debrisPS.size()};
        present.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;
        present.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        auto& blend=present.blend.data.RenderTarget[0];
        blend.BlendEnable=TRUE;
        blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
        blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;
        debrisDrawPSO=Pipeline(L"debris",presentDesc,Hash(debrisPS,Hash(debrisVS)));

        auto cutMs=Shader(L"CutMeshMain"),cutShadowMs=Shader(L"CutShadowMain");
        auto ms=Shader(L"MeshMain"),ps=Shader(L"PixelMain"),shadowMS=Shader(L"ShadowMain"),
            depthPixel=Shader(L"DepthPixel");
        MeshPipeline p{};p.root.data=root.Get();p.ms.data={ms.data(),ms.size()};p.mask.data=UINT_MAX;p.raster.data.FillMode=D3D12_FILL_MODE_SOLID;p.raster.data.CullMode=D3D12_CULL_MODE_NONE;p.raster.data.DepthClipEnable=TRUE;p.raster.data.MultisampleEnable=MsaaSamples>1;p.depth.data.DepthEnable=TRUE;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.depth.data.DepthFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;p.topology.data=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.depthFormat.data=DXGI_FORMAT_D32_FLOAT;p.samples.data={MsaaSamples,0};p.views.data=views;p.blend.data.IndependentBlendEnable=TRUE;for(auto& r:p.blend.data.RenderTarget){r.SrcBlend=D3D12_BLEND_ONE;r.DestBlend=D3D12_BLEND_ZERO;r.BlendOp=D3D12_BLEND_OP_ADD;r.SrcBlendAlpha=D3D12_BLEND_ONE;r.DestBlendAlpha=D3D12_BLEND_ZERO;r.BlendOpAlpha=D3D12_BLEND_OP_ADD;r.LogicOp=D3D12_LOGIC_OP_NOOP;r.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;}D3D12_PIPELINE_STATE_STREAM_DESC desc{sizeof(p),&p};p.ps.data={depthPixel.data(),depthPixel.size()};depthPSO=Pipeline(L"depth",desc,Hash(depthPixel,Hash(ms)));
        p.ms.data={cutMs.data(),cutMs.size()};
        cutDepthPSO=Pipeline(L"cut_depth",desc,Hash(depthPixel,Hash(cutMs)));
        p.ms.data={ms.data(),ms.size()};
        p.flags.data=ForestPipelineFlags();
        p.ps.data={ps.data(),ps.size()};p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[0]=p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO;scenePSO=Pipeline(L"scene",desc,Hash(ps,Hash(ms))^uint64_t(p.flags.data));
        p.ms.data={cutMs.data(),cutMs.size()};
        cutScenePSO=Pipeline(L"cut_scene",desc,Hash(ps,Hash(cutMs))^uint64_t(p.flags.data));
        p.ms.data={ms.data(),ms.size()};
        p.formats.data.NumRenderTargets=1;p.formats.data.RTFormats[1]=DXGI_FORMAT_UNKNOWN;sceneDirectPSO=Pipeline(L"scene_direct",desc,Hash(ps,Hash(ms))^uint64_t(p.flags.data));
        p.ms.data={cutMs.data(),cutMs.size()};
        cutSceneDirectPSO=Pipeline(L"cut_scene_direct",desc,Hash(ps,Hash(cutMs))^uint64_t(p.flags.data));
        p.ms.data={ms.data(),ms.size()};


        const auto crtMesh=contributions.shuttleCrt?Shader(L"MeshMain_M32"):std::vector<std::byte>{};
        for(uint32_t mask=0;mask<sceneVariantPSO.size();++mask){
            const bool crt=mask==ShuttleCrtVariant;
            const bool used=crt?contributions.shuttleCrt:std::any_of(surfaceData.begin(),surfaceData.end(),
                [&](const Surface& s){return MaterialTextureMask(s)==mask;});
            if(!used){sceneVariantPSO[mask].Reset();sceneDirectVariantPSO[mask].Reset();continue;}
            wchar_t entry[64],name[64];
            swprintf_s(entry,L"PixelMain_M%u",mask);
            auto specialized=Shader(entry);
            const auto& meshBytecode=crt?crtMesh:ms;
            const auto variantHash=Hash(specialized,Hash(meshBytecode))^uint64_t(p.flags.data);
            p.ms.data={meshBytecode.data(),meshBytecode.size()};
            p.ps.data={specialized.data(),specialized.size()};
            swprintf_s(name,L"scene_direct_M%u",mask);
            sceneDirectVariantPSO[mask]=Pipeline(name,desc,variantHash);
            p.formats.data.NumRenderTargets=2;
            p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
            swprintf_s(name,L"scene_M%u",mask);
            sceneVariantPSO[mask]=Pipeline(name,desc,variantHash);
            p.formats.data.NumRenderTargets=1;
            p.formats.data.RTFormats[1]=DXGI_FORMAT_UNKNOWN;
        }
        p.ms.data={ms.data(),ms.size()};
        p.ps.data={ps.data(),ps.size()};
        for(auto& blend:p.blend.data.RenderTarget){blend.BlendEnable=TRUE;blend.SrcBlend=D3D12_BLEND_SRC_ALPHA;blend.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;blend.SrcBlendAlpha=D3D12_BLEND_ONE;blend.DestBlendAlpha=D3D12_BLEND_INV_SRC_ALPHA;}transparentDirectPSO=Pipeline(L"transparent_direct",desc,Hash(ps,Hash(ms))^uint64_t(p.flags.data));p.formats.data.NumRenderTargets=2;p.formats.data.RTFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;transparentPSO=Pipeline(L"transparent",desc,Hash(ps,Hash(ms))^uint64_t(p.flags.data));for(auto& blend:p.blend.data.RenderTarget)blend.BlendEnable=FALSE;p.ps.data={depthPixel.data(),depthPixel.size()};p.ms.data={shadowMS.data(),shadowMS.size()};p.flags.data=D3D12_PIPELINE_STATE_FLAG_NONE;p.formats.data={};p.samples.data.Count=1;p.views.data={};p.depth.data.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;shadowPSO=Pipeline(L"shadow",desc,Hash(depthPixel,Hash(shadowMS)));
        p.ms.data={cutShadowMs.data(),cutShadowMs.size()};
        cutShadowPSO=Pipeline(L"cut_shadow",desc,Hash(depthPixel,Hash(cutShadowMs)));
        p.ms.data={shadowMS.data(),shadowMS.size()};
        auto cs=[&](const wchar_t* entry,ComPtr<ID3D12PipelineState>& out,
                    D3D12_PIPELINE_STATE_FLAGS flags=D3D12_PIPELINE_STATE_FLAG_NONE){
            auto bytes=Shader(entry);
            ComputePipeline s;
            s.root.data=root.Get();s.cs.data={bytes.data(),bytes.size()};s.flags.data=flags;
            D3D12_PIPELINE_STATE_STREAM_DESC d{sizeof(s),&s};
            out=Pipeline(entry,d,Hash(bytes)^uint64_t(flags));
        };cs(L"CullMain",cullPSO);cs(L"BatchOpaqueMain",batchOpaquePSO);cs(L"TileBatchMark",tileBatchMarkPSO);cs(L"TileBatchScan",tileBatchScanPSO);cs(L"TileBatchOffsetScan",tileBatchOffsetScanPSO);cs(L"TileBatchOffsetFinalize",tileBatchOffsetFinalizePSO);cs(L"TileBatchEmit",tileBatchEmitPSO);cs(L"DepthMain",depthResolvePSO);cs(L"HiZMain",hizPSO);cs(L"ClusterCountMain",clusterCountPSO);cs(L"ClusterOffsetsMain",clusterOffsetsPSO);cs(L"ClusterFillMain",clusterFillPSO);
         cs(L"LightBvhPartialBounds",lightBvhPartialPSO);
         cs(L"LightBvhGlobalBounds",lightBvhBoundsPSO);
         cs(L"LightBvhMakeMorton",lightBvhMortonPSO);
         cs(L"LightBvhBitonic",lightBvhSortPSO);
         cs(L"LightBvhRefitLeaves",lightBvhLeavesPSO);
         cs(L"UpdateRoomProbes",roomProbesPSO,ForestPipelineFlags());
         cs(L"LightBvhRefitParents",lightBvhParentsPSO);cs(L"DenoiseMain",denoisePSO);cs(L"MergeReflection",mergeReflectionPSO);cs(L"FxaaMain",fxaaPSO);cs(L"DeformMain",deformPSO);cs(L"DeformBoundsMain",deformBoundsPSO);cs(L"HotDataMain",hotDataPSO);cs(L"RayBuildMain",rayBuildPSO);cs(L"RayMapFillMain",rayMapFillPSO);
        // Prepare the cut PSOs at scene startup, never on the first VR trigger.
        EnsureCutPipelines();
        if(sphereLod.enabled){
            cs(L"CaptureSphereView",sphereCapturePSO);
            cs(L"CaptureSphereMean",sphereMeanPSO);
        }
        if(!membranePSO)cs(L"BuildMembraneField",membranePSO);
        if(!debrisResetPSO)cs(L"ResetDebris",debrisResetPSO);
        if(!debrisBuildPSO)cs(L"BuildDebris",debrisBuildPSO);
        if(vrsImage.resource&&!vrsPSO)cs(L"BuildShadingRateImage",vrsPSO);
        if(library&&persistentCache){SIZE_T size=library->GetSerializedSize();std::vector<std::byte> data(size);if(SUCCEEDED(library->Serialize(data.data(),size))){wchar_t suffix[40];swprintf_s(suffix,L".%lu.tmp",GetCurrentProcessId());auto temporary=cachePath+suffix;FILE* file=nullptr;_wfopen_s(&file,temporary.c_str(),L"wb");if(file){bool ok=fwrite(&cacheIdentity,1,sizeof(cacheIdentity),file)==sizeof(cacheIdentity)&&fwrite(data.data(),1,size,file)==size;ok=fclose(file)==0&&ok;if(ok)MoveFileExW(temporary.c_str(),cachePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);else DeleteFileW(temporary.c_str());}}}

    }

    void Graphics::Impl::Initialize(const XrGraphicsRequirementsD3D12KHR& req){
        Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT i=0;;i++){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 d;a->GetDesc1(&d);if(d.AdapterLuid.HighPart==req.adapterLuid.HighPart&&d.AdapterLuid.LowPart==req.adapterLuid.LowPart){Hr(a.As(&adapter),"DXGI VRAM adapter");break;}}
        if(!adapter)Fail("OpenXR GPU adapter");InitializeDevice();
    }
    void Graphics::Impl::InitializeDesktop(){
        Hr(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory)),"DXGI factory");
        for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;HRESULT result=factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate));if(result==DXGI_ERROR_NOT_FOUND)break;Hr(result,"DXGI GPU adapter");DXGI_ADAPTER_DESC1 desc{};Hr(candidate->GetDesc1(&desc),"DXGI GPU description");if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){Hr(candidate.As(&adapter),"DXGI GPU adapter");break;}}
        if(!adapter)Fail("No hardware DXGI adapter");InitializeDevice();
    }
    void Graphics::Impl::InitializeDevice(){
        EnableClusterPreview();
        Hr(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_2,IID_PPV_ARGS(&device)),"D3D12 feature level 12_2");D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_6};Hr(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm)),"Shader Model 6.6");if(sm.HighestShaderModel<D3D_SHADER_MODEL_6_6)Fail("Shader Model 6.6 required");D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3,&o3,sizeof(o3)),"view instancing");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&o5,sizeof(o5)),"DXR");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7,&o7,sizeof(o7)),"mesh shaders");Hr(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12,&o12,sizeof(o12)),"enhanced barriers");if(o3.ViewInstancingTier<D3D12_VIEW_INSTANCING_TIER_1||o5.RaytracingTier<D3D12_RAYTRACING_TIER_1_1||o7.MeshShaderTier==D3D12_MESH_SHADER_TIER_NOT_SUPPORTED||!o12.EnhancedBarriersSupported)Fail("DXR 1.1, mesh shaders, enhanced barriers required");residency.Initialize(device.Get(),adapter.Get());vramCache.Initialize(device.Get(),residency);QueueInit(direct,D3D12_COMMAND_LIST_TYPE_DIRECT);QueueInit(compute,D3D12_COMMAND_LIST_TYPE_COMPUTE);QueueInit(copy,D3D12_COMMAND_LIST_TYPE_COPY);passGraph.Initialize(copy,compute,direct);CommandsInit(startup,D3D12_COMMAND_LIST_TYPE_DIRECT);
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 shadingRate{};
        D3D12_FEATURE_DATA_FORMAT_SUPPORT vrsFormat{};
        vrsFormat.Format=DXGI_FORMAT_R8_UINT;
        if(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6,
                         &shadingRate,sizeof(shadingRate)))&&
           shadingRate.VariableShadingRateTier>=D3D12_VARIABLE_SHADING_RATE_TIER_2&&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT,
                         &vrsFormat,sizeof(vrsFormat)))&&
           (vrsFormat.Support2&D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            vrsTileSize=shadingRate.ShadingRateImageTileSize;
        auto heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible,ComPtr<ID3D12DescriptorHeap>& out){D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=type;h.NumDescriptors=count;h.Flags=visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE;Hr(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&out)),"descriptor heap");};heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,DescriptorCapacity,true,descriptors);heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,1024,true,samplers);heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,64,false,rtvs);heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,64,false,dsvs);descriptorStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);rtvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);dsvStride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);D3D12_SAMPLER_DESC s{};s.Filter=D3D12_FILTER_ANISOTROPIC;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;s.MaxAnisotropy=16;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;s.MaxLOD=D3D12_FLOAT32_MAX;auto sh=samplers->GetCPUDescriptorHandleForHeapStart();device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_GREATER_EQUAL;s.AddressU=s.AddressV=s.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;device->CreateSampler(&s,sh);sh.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);s.Filter=D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;s.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;device->CreateSampler(&s,sh);
        D3D12_ROOT_PARAMETER1 params[2]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_CBV;params[0].Descriptor.ShaderRegister=0;params[0].Descriptor.Flags=D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={1,0,5};D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};rs.Version=D3D_ROOT_SIGNATURE_VERSION_1_1;rs.Desc_1_1.NumParameters=2;rs.Desc_1_1.pParameters=params;rs.Desc_1_1.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED|D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;ComPtr<ID3DBlob> blob;Hr(D3D12SerializeVersionedRootSignature(&rs,&blob,nullptr),"Root Signature 1.1");Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root signature");rootHash=HashMemory(blob->GetBufferPointer(),blob->GetBufferSize());abiHash=HashMemory(OASIS_PIPELINE_CACHE_KEY,strlen(OASIS_PIPELINE_CACHE_KEY));D3D12_INDIRECT_ARGUMENT_DESC args[2]{};args[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;args[0].Constant.RootParameterIndex=1;args[0].Constant.Num32BitValuesToSet=1;args[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;D3D12_COMMAND_SIGNATURE_DESC cs{};cs.ByteStride=16;cs.NumArgumentDescs=2;cs.pArgumentDescs=args;Hr(device->CreateCommandSignature(&cs,root.Get(),IID_PPV_ARGS(&indirect)),"mesh indirect signature");
         // Stable block ID and triangle count ride together in an indirect
         // record. passIndex remains intact for shadow rendering.
         D3D12_INDIRECT_ARGUMENT_DESC cutArgs[3]{};
         cutArgs[0].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
         cutArgs[0].Constant.RootParameterIndex=1;
         cutArgs[0].Constant.DestOffsetIn32BitValues=0;
         cutArgs[0].Constant.Num32BitValuesToSet=1;
         cutArgs[1].Type=D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
         cutArgs[1].Constant.RootParameterIndex=1;
         cutArgs[1].Constant.DestOffsetIn32BitValues=2;
         cutArgs[1].Constant.Num32BitValuesToSet=1;
         cutArgs[2].Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
         D3D12_COMMAND_SIGNATURE_DESC cutSignature{};
         cutSignature.ByteStride=20;
         cutSignature.NumArgumentDescs=3;
         cutSignature.pArgumentDescs=cutArgs;
         Hr(device->CreateCommandSignature(&cutSignature,root.Get(),
            IID_PPV_ARGS(&cutBlockIndirect)),"cut block indirect signature");D3D12_INDIRECT_ARGUMENT_DESC dispatchArg{};dispatchArg.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;D3D12_COMMAND_SIGNATURE_DESC dispatchDesc{};dispatchDesc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);dispatchDesc.NumArgumentDescs=1;dispatchDesc.pArgumentDescs=&dispatchArg;Hr(device->CreateCommandSignature(&dispatchDesc,nullptr,IID_PPV_ARGS(&dispatchIndirect)),"compute dispatch indirect signature");
        D3D12_INDIRECT_ARGUMENT_DESC debrisArg{};
        debrisArg.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        const D3D12_COMMAND_SIGNATURE_DESC debrisSignature{
            sizeof(D3D12_DRAW_ARGUMENTS),1,&debrisArg,0};
        Hr(device->CreateCommandSignature(&debrisSignature,nullptr,
            IID_PPV_ARGS(&debrisDrawSignature)),"debris draw indirect signature");
        DXGI_ADAPTER_DESC desc{};Hr(adapter->GetDesc(&desc),"cache GPU identity");LARGE_INTEGER driver{};persistentCache=SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice),&driver));cacheIdentity.vendor=desc.VendorId;cacheIdentity.device=desc.DeviceId;cacheIdentity.subsystem=desc.SubSysId;cacheIdentity.revision=desc.Revision;cacheIdentity.driverHigh=uint32_t(driver.HighPart);cacheIdentity.driverLow=driver.LowPart;cacheIdentity.root=rootHash;cacheIdentity.abi=abiHash;
        wchar_t folder[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,0,folder)))Fail("cache directory");std::wstring dir=folder;dir+=L"\\OASIS_D3D12";CreateDirectoryW(dir.c_str(),nullptr);wchar_t name[80];swprintf_s(name,L"\\pipelines_%016llx.bin",HashMemory(&cacheIdentity,sizeof(cacheIdentity)));cachePath=dir+name;
        FILE* cache=nullptr;if(persistentCache)_wfopen_s(&cache,cachePath.c_str(),L"rb");if(cache){CacheHeader header{};bool valid=fread(&header,1,sizeof(header),cache)==sizeof(header)&&memcmp(&header,&cacheIdentity,sizeof(header))==0;if(valid){_fseeki64(cache,0,SEEK_END);auto n=_ftelli64(cache)-int64_t(sizeof(header));_fseeki64(cache,sizeof(header),SEEK_SET);if(n>0&&n<128*1024*1024){cacheBlob.resize(size_t(n));if(fread(cacheBlob.data(),1,cacheBlob.size(),cache)!=cacheBlob.size())cacheBlob.clear();}}fclose(cache);}
        if(FAILED(device->CreatePipelineLibrary(cacheBlob.empty()?nullptr:cacheBlob.data(),cacheBlob.size(),IID_PPV_ARGS(&library)))){cacheBlob.clear();device->CreatePipelineLibrary(nullptr,0,IID_PPV_ARGS(&library));}
        std::atomic_store_explicit(&uploadGeneration,NewUploadGeneration(UploadChunk),std::memory_order_release);managedMemory.Initialize(device.Get(),residency,*this,this,[](void* context,ID3D12Resource* resource){static_cast<Impl*>(context)->passGraph.Forget(resource);});CollectManaged();domainServicesReady=true;for(auto* domain:domainRegistry)domain->Initialize(*this);
    }

    GpuCacheKey Graphics::Impl::StaticBlasKey(uint32_t mesh) {
        if(mesh>=meshes.ranges.size())Fail("BLAS cache mesh index");
        const auto& range=meshes.ranges[mesh];
        if(uint64_t(range.firstIndex)+range.indexCount>meshes.indices.size())Fail("BLAS index range");
        uint64_t hash=HashMemory(&range.firstIndex,sizeof(range.firstIndex));
        hash=HashMemory(&range.indexCount,sizeof(range.indexCount),hash);
        for(uint32_t i=0;i<range.indexCount;++i){
            const uint32_t index=meshes.indices[range.firstIndex+i];
            if(index>=meshes.vertices.size())Fail("BLAS vertex index");
            hash=HashMemory(&index,sizeof(index),hash);
            hash=HashMemory(&meshes.vertices[index].position,sizeof(meshes.vertices[index].position),hash);
        }
        uint64_t generation=HashMemory(&abiHash,sizeof(abiHash),hash);if(!hash)hash=1;if(!generation)generation=1;return {hash,generation,CacheStaticBlas};
    }

    void Graphics::Impl::BuildBLAS(){
        uint32_t meshCount=uint32_t(meshes.ranges.size());blas.resize(meshCount);if(!meshCount)return;std::vector<GpuCacheKey> keys(meshCount);std::vector<uint32_t> missing;missing.reserve(meshCount);
        for(uint32_t i=0;i<meshCount;i++){keys[i]=StaticBlasKey(i);auto hit=vramCache.AcquireExisting(keys[i],true);if(hit)blas[i]=hit.resource;else missing.push_back(i);}
        if(missing.empty())return;
        vramCache.RequireActive();if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Budget().headroom))Fail("Static BLAS build exceeds available VRAM budget");residency.Wait(direct.queue.Get());
        startup.Reset();uint32_t missCount=uint32_t(missing.size());std::vector<ComPtr<ID3D12Resource>> scratch(missCount),uncompacted(missCount);auto sizes=Temporary(Buffer(UINT64(missCount)*8,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);auto readback=Host(UINT64(missCount)*8,D3D12_HEAP_TYPE_READBACK);
        for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];auto& range=meshes.ranges[i];D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.VertexBuffer.StartAddress=vertices->GetGPUVirtualAddress();geometry.Triangles.VertexBuffer.StrideInBytes=sizeof(Vertex);geometry.Triangles.VertexCount=uint32_t(meshes.vertices.size());geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(range.firstIndex)*4;geometry.Triangles.IndexCount=range.indexCount;geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION|D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;input.NumDescs=1;input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;input.pGeometryDescs=&geometry;D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);uncompacted[ordinal]=Temporary(Buffer(info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);scratch[ordinal]=Temporary(Buffer(info.ScratchDataSizeInBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs=input;build.DestAccelerationStructureData=uncompacted[ordinal]->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch[ordinal]->GetGPUVirtualAddress();D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC compact{};compact.DestBuffer=sizes->GetGPUVirtualAddress()+UINT64(ordinal)*8;compact.InfoType=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;if(ordinal)BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);startup.list->BuildRaytracingAccelerationStructure(&build,1,&compact);}
        BBarrier(startup.list.Get(),sizes.Get(),D3D12_BARRIER_SYNC_ALL,D3D12_BARRIER_SYNC_COPY,D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,D3D12_BARRIER_ACCESS_COPY_SOURCE);startup.list->CopyBufferRegion(readback.Get(),0,sizes.Get(),0,UINT64(missCount)*8);startup.Submit(direct);WaitStartup(direct);
        UINT64* compactedSizes=nullptr;D3D12_RANGE range{0,SIZE_T(missCount)*8};Hr(readback->Map(0,&range,reinterpret_cast<void**>(&compactedSizes)),"BLAS compact sizes");startup.Reset();for(uint32_t ordinal=0;ordinal<missCount;ordinal++){uint32_t i=missing[ordinal];if(!compactedSizes[ordinal])Fail("BLAS compact size");auto cached=vramCache.Acquire(keys[i],Buffer(compactedSizes[ordinal],D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,true,CompletedFences(),managedMemory.Budget().headroom,L"Cached static BLAS");if(!cached||cached.reused)Fail("Static BLAS cache race");blas[i]=cached.resource;startup.list->CopyRaytracingAccelerationStructure(blas[i]->GetGPUVirtualAddress(),uncompacted[ordinal]->GetGPUVirtualAddress(),D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);BBarrier(startup.list.Get(),blas[i].Get(),D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_RAYTRACING,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);}
        D3D12_RANGE written{0,0};readback->Unmap(0,&written);startup.Submit(direct);WaitStartup(direct);for(uint32_t i:missing)vramCache.Commit(blas[i].Get());residency.Submitted(direct.serial);
    }

    void Graphics::Impl::ResetSceneResources(){
        ResetForest();
        cutPipelines={};
        active=nullptr;sceneLoaded=false;historyReady=false;sceneTimeOrigin=0;sceneTimeOriginValid=false;shadowCache={};
        ResetCutResources();
        for(uint32_t owner=0;owner<sphereCount;++owner)ResetSphereCache(SphereCache(owner));
        ResetSphereCache(oasisRelief);
        sphereObjectOffsets.clear();
        nativeSphere=pendingNativeSphere=Invalid;renderSphere=0u;
        ResetDlss();
        ResetPhosphorOrbit();
        dlssPresentPSO.Reset();
        auto release=[&](DescriptorHandle& handle){if(handle.Valid())ReleaseDescriptor(handle);};
        auto resetResource=[&](auto& resource){passGraph.Forget(resource.Get());resource.Reset();};
        auto releaseCached=[&](auto& resource){if(!resource)return;passGraph.Forget(resource.Get());vramCache.Release(resource.Get(),SubmittedFences());resource.Reset();};
        for(auto& film:sphereMembranes){
            for(auto* handle:{&film.eventSRV,&film.eventUAV,&film.fieldSRV,&film.fieldUAV})
                release(*handle);
            if(film.events)resetResource(film.events);
            if(film.field.resource)resetResource(film.field.resource);
            film={};
        }
        for(auto& drops:sphereDebris){
            for(auto* handle:{&drops.eventSRV,&drops.eventUAV,&drops.floorSRV,&drops.floorUAV,&drops.argsUAV})
                release(*handle);
            if(drops.events)resetResource(drops.events);
            if(drops.floor)resetResource(drops.floor);
            if(drops.drawArgs)resetResource(drops.drawArgs);
            drops={};
        }
        release(vrsImageUAV);
        resetResource(vrsImage.resource);
        vrsImage={};
        vrsImageReady=false;
        for(auto& texture:textures){release(texture.descriptor);releaseCached(texture.resource);}
        textures.clear();surfaceData.clear();baseSurfaceData.clear();materialSlots.clear();materialSurfacesBySlot.clear();materialCaptureOwners.clear();staticOpaqueOrder.clear();shadowSurfaceRevision.clear();dirtySurfaces.clear();release(aaSourceArraySrv);aaSource={};aaSourceLease.reset();visibleMaterialBits.clear();materialResidencyOffsets.clear();materialResidencyPages.clear();residencyPageSeen.clear();requiredResidencyPages.clear();residencyPageGeneration=1;
        for(auto* handle:{&skinSRV,&morphSRV,&indicesSRV,&verticesSRV,&meshletsSRV,&remapSRV,&trianglesSRV,&rangesSRV,&materialsSRV,&deformationsSRV,&meshletRegionsSRV,&triangleSolidsSRV,&meshletRegionsUAV,&triangleSolidsUAV})release(*handle);
        for(auto& handle:geometryUAVs)release(handle);
        releaseCached(vertices);releaseCached(indices);releaseCached(meshlets);releaseCached(remap);releaseCached(triangles);releaseCached(ranges);
        writableSceneMeshes=false;releaseCached(materials);releaseCached(deformations);releaseCached(skinVertices);releaseCached(morphVertices);releaseCached(meshletRegions);releaseCached(triangleSolids);
        for(auto& [key,tile]:residentTileGpu){
            release(tile.srv);release(tile.runSrv);
            passGraph.Forget(tile.buffer.Get());passGraph.Forget(tile.runBuffer.Get());
        }
        residentTileGpu.clear();
        releaseCached(residentStaticInstances);sceneCache.Reset();rayScene.Reset();raySlotLeases.clear();raySlotCount=0;staticMaterialBits.clear();staticTransparent.clear();staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};baseLights.clear();baseLightCount=0;
        for(auto& resource:blas)releaseCached(resource);blas.clear();dynamicMeshes.clear();meshDynamicIndex.clear();meshes={};animation=SceneAnimation{};visualFrame={};contributions={};
        release(roomProbesSRV);release(roomProbesUAV);
        resetResource(roomProbes);
        roomProbeCapacity=0;
        roomProbeState={};
        for(auto& f:frames){
            for(auto* handle:{&f.motionSRV,&f.boneSRV,&f.weightSRV,&f.deformedBoundsSRV,&f.deformedBoundsUAV,&f.dynamicSRV,&f.dynamicUAV,&f.blasAddressesSRV,&f.rayMapSRV,&f.rayMapUAV,&f.opaqueOrderSRV,&f.worldInstanceSRV,&f.worldInstanceUAV,&f.publishedInstanceSRV,&f.instanceRangesSRV,&f.lightsSRV,&f.clusterSRV,&f.clusterUAV,
                &f.clusterIndicesSRV,&f.clusterIndicesUAV,
                &f.clusterGroupCountsSRV,&f.clusterGroupCountsUAV,
                &f.clusterGroupOffsetsSRV,&f.clusterGroupOffsetsUAV,
                &f.clusterDirectionalIndicesSRV,&f.clusterDirectionalIndicesUAV,
                &f.clusterDirectionalCountSRV,&f.clusterDirectionalCountUAV,
                &f.clusterBoundsSRV,&f.clusterBoundsUAV,
                &f.lightBvhNodesSRV,&f.lightBvhNodesUAV,
                &f.lightBvhPairsSRV,&f.lightBvhPairsUAV,
                &f.lightBvhBoundsSRV,&f.lightBvhBoundsUAV})release(*handle);
            resetResource(f.motionBuffer);resetResource(f.boneBuffer);resetResource(f.weightBuffer);resetResource(f.deformedBounds);resetResource(f.dynamicVertices);resetResource(f.dynamicScratch);resetResource(f.blasAddresses);resetResource(f.lights);resetResource(f.clusters);
            f.transferOverflow.clear();f.transferUpload.reset();f.transferBump=0;
            resetResource(f.clusterIndices);resetResource(f.clusterGroupCounts);
            resetResource(f.clusterGroupOffsets);resetResource(f.clusterDirectionalIndices);
            resetResource(f.clusterDirectionalCount);resetResource(f.clusterBounds);
            resetResource(f.lightBvhNodes);resetResource(f.lightBvhPairs);
            resetResource(f.lightBvhBounds);
            f.lightBvhLeaves=f.lightBvhNodeCapacity=f.lightBvhPairCapacity=
                f.lightBvhBoundsCapacity=f.lightBvhSortedCount=0;
            f.lightBvhLastSortFrame=0;
            f.lightBvhSorted=f.lightBvhPendingSort=false;
            f.lightBvhSortSnapshot.clear();
            f.lightCapacity=f.clusterIndexCapacity=f.directionalCapacity=0;
            f.lightData.clear();f.lightCount=0;
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
        auto hdrFlags=D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|(MsaaSamples==1?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE));texture(hdr,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,hdrFlags);if(!meshes.ranges.empty())texture(reflections,DXGI_FORMAT_R16G16B16A16_FLOAT,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);texture(depth,DXGI_FORMAT_R32_TYPELESS,MsaaSamples,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);hdrRTV=MakeRTV(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:hdrRTV);if(!meshes.ranges.empty())reflectionsRTV=MakeRTV(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh?Invalid:reflectionsRTV);else reflectionsRTV=Invalid;depthDSV=MakeDSV(depth.resource.Get(),fresh?Invalid:depthDSV);depthSRV=TextureSrv(depth.resource.Get(),DXGI_FORMAT_R32_FLOAT,depthSRV,true);
        if(MsaaSamples>1){texture(hdrResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAGS(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET));hdrResolvedRTV=MakeRTV(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,fresh||hdrResolvedRTV==Invalid?Invalid:hdrResolvedRTV);if(!meshes.ranges.empty())texture(reflectionsResolved,DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_NONE);hdrResolvedSRV=TextureSrv(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV,true);hdrResolvedUAV=TextureUav(hdrResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV,true);if(!meshes.ranges.empty())reflectionsResolvedSRV=TextureSrv(reflectionsResolved.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV,true);}else{hdrResolvedSRV=TextureSrv(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,hdrResolvedSRV,true);hdrResolvedUAV=TextureUav(hdr.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,hdrResolvedUAV,true);if(!meshes.ranges.empty())reflectionsResolvedSRV=TextureSrv(reflections.resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,reflectionsResolvedSRV,true);}
    }

    void Graphics::Impl::ApplyMultisampling(){
        if(MsaaSamples==requestedSamples)return;

        WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);
        SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        MsaaSamples=requestedSamples;SwapSampleTargets(parkedTargets[MsaaSamples==4?1:0]);
        ConfigureSampleTargets(false);Pipelines();historyReady=false;
    }

    void Graphics::Impl::Configure(const XR& xr,const SceneData& assets,JobSystem& workers,RenderContributions extra){
        OutputSurface output{};output.width=xr.Width();output.height=xr.Height();output.viewCount=xr.ViewCount();output.format=xr.Format();
        for(const auto& image:xr.Colors())output.colors.push_back(image.texture);
        for(const auto& image:xr.Depths())output.depths.push_back(image.texture);
        ConfigureOutput(output,assets,workers,extra);
    }
    void Graphics::Impl::ConfigureOutput(const OutputSurface& output,const SceneData& assets,JobSystem& workers,RenderContributions extra){
        if(!output.width||!output.height||!output.viewCount||output.viewCount>MaxViews||output.format==DXGI_FORMAT_UNKNOWN||output.colors.empty()||output.depths.empty()||std::any_of(output.colors.begin(),output.colors.end(),[](auto* p){return !p;})||std::any_of(output.depths.begin(),output.depths.end(),[](auto* p){return !p;}))Fail("Invalid graphics output surface");
        if(outputConfigured){if(jobs!=&workers)Fail("Graphics runtime services cannot change during scene replacement");WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();xrRTV.clear();xrDSV.clear();rtvCount=dsvCount=0;}
        else jobs=&workers;
        sphereObjectOffsets=assets.sphereObjectOffsets;
        solarSystem=assets.solarSystem;
        if(solarSystem&&sphereCount!=SolarBodyCount)Fail("Solar catalogue size");
        if(sphereCount>1u&&(sphereObjectOffsets.size()!=sphereCount+1u||
           sphereObjectOffsets.front()!=0u||
           sphereObjectOffsets.back()!=assets.objects.size()||
           std::adjacent_find(sphereObjectOffsets.begin(),sphereObjectOffsets.end(),
                              std::greater_equal<uint32_t>{})!=sphereObjectOffsets.end()))
            Fail("Sphere geometry must be ordered by owner");
        outputSurface=output;outputConfigured=true;
        contributions=extra;if(assets.lights.size()>UINT32_MAX)Fail("Scene light count exceeds 32-bit index");baseLightCount=uint32_t(assets.lights.size());baseLights=assets.lights;for(uint32_t i=0;i<baseLightCount;i++)baseLights[i].id=0x80010000u+i;visualFrame={};visualFrame.objects.reserve(256);width=output.width;height=output.height;viewCount=output.viewCount;renderWidth=width;renderHeight=height;colorFormat=output.format;mips=std::bit_width(std::max(width,height));renderMips=mips;clustersCount=((width+63)/64)*((height+63)/64)*24*viewCount;if(MsaaSamples>1)for(DXGI_FORMAT format:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_D32_FLOAT}){D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format,MsaaSamples,D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE,0};Hr(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&q,sizeof(q)),"MSAA 4x");if(!q.NumQualityLevels)Fail("MSAA 4x required");}


        xrColorTargets.clear();for(auto* image:output.colors){xrRTV.push_back(MakeRTV(image,colorFormat));xrColorTargets.emplace_back(image);}
        for(auto* image:output.depths)xrDSV.push_back(MakeDSV(image));
        auto texture=[&](TextureResource& r,DXGI_FORMAT format,UINT samples,UINT layers,UINT levels,D3D12_RESOURCE_FLAGS flags,UINT w=0,UINT h=0){auto desc=Texture(w?w:width,h?h:height,format,samples,layers,levels,flags);if(r.resource&&SameResourceDesc(r.resource->GetDesc(),desc)){passGraph.Forget(r.resource.Get());return;}D3D12_CLEAR_VALUE clear{};clear.Format=format==DXGI_FORMAT_R32_TYPELESS?DXGI_FORMAT_D32_FLOAT:format;clear.DepthStencil.Depth=0;passGraph.Forget(r.resource.Get());r.resource.Reset();r.layout=D3D12_BARRIER_LAYOUT_COMMON;r.resource=Placed(desc,D3D12_RESOURCE_STATE_COMMON,(flags&(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))?&clear:nullptr);};
        meshes=assets.meshes;writableSceneMeshes=assets.writableStaticGeometry;if(writableSceneMeshes&&++geometryGenerationSerial==0)Fail("writable geometry generation overflow");hdrResolvedRTV=Invalid;ConfigureSampleTargets(true);
        if(!assets.meshes.ranges.empty()) {
        if(vrsTileSize&&!output.desktop&&viewCount==2u){
            const uint32_t tileWidth=(width+vrsTileSize-1u)/vrsTileSize;
            const uint32_t tileHeight=(height+vrsTileSize-1u)/vrsTileSize;
            vrsImage.resource=Temporary(Texture(tileWidth,tileHeight,DXGI_FORMAT_R8_UINT,
                1,1,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_COMMON);
            vrsImageUAV=TextureUav(vrsImage.resource.Get(),DXGI_FORMAT_R8_UINT);
        }
        texture(hierarchy,DXGI_FORMAT_R32_FLOAT,1,viewCount,mips,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);hierarchySRV=TextureSrv(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,hierarchySRV,true);if(hierarchyUAV.size()>mips){for(uint32_t i=mips;i<hierarchyUAV.size();i++)ReleaseDescriptor(hierarchyUAV[i]);hierarchyUAV.resize(mips);}uint32_t oldHiZMips=uint32_t(hierarchyUAV.size());hierarchyUAV.resize(mips);for(uint32_t i=0;i<mips;i++)hierarchyUAV[i]=TextureUav(hierarchy.resource.Get(),DXGI_FORMAT_R32_FLOAT,i,i<oldHiZMips?hierarchyUAV[i]:DescriptorHandle{},true);
        texture(shadow,DXGI_FORMAT_R32_TYPELESS,1,1,1,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,3072,2048);shadowDSV=MakeDSV(shadow.resource.Get());shadowSRV=TextureSrv(shadow.resource.Get(),DXGI_FORMAT_R32_FLOAT,shadowSRV);
        for(uint32_t i=0;i<history.size();i++){texture(history[i],DXGI_FORMAT_R16G16B16A16_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);texture(historyDepth[i],DXGI_FORMAT_R32_FLOAT,1,viewCount,1,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);historySRV[i]=TextureSrv(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,historySRV[i],true);historyUAV[i]=TextureUav(history[i].resource.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,0,historyUAV[i],true);historyDepthSRV[i]=TextureSrv(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,historyDepthSRV[i],true);historyDepthUAV[i]=TextureUav(historyDepth[i].resource.Get(),DXGI_FORMAT_R32_FLOAT,0,historyDepthUAV[i],true);}
        }
        animation.Initialize(assets);meshDynamicIndex.resize(meshes.ranges.size(),Invalid);for(uint32_t i=0;i<meshes.deformations.size();i++)if(meshes.deformations[i].axisAmplitude.w!=0||meshes.motions[i].node!=Invalid){meshDynamicIndex[i]=uint32_t(dynamicMeshes.size());dynamicMeshes.push_back(i);}startup.Reset();vertices=writableSceneMeshes?CachedUploadVector(meshes.vertices,CacheWritableVertices,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.vertices,CacheVertices);indices=writableSceneMeshes?CachedUploadVector(meshes.indices,CacheWritableIndices,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.indices,CacheIndices);meshlets=writableSceneMeshes?CachedUploadVector(meshes.meshlets,CacheWritableMeshlets,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.meshlets,CacheMeshlets);remap=writableSceneMeshes?CachedUploadVector(meshes.remap,CacheWritableRemap,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.remap,CacheRemap);triangles=writableSceneMeshes?CachedUploadVector(meshes.triangles,CacheWritableTriangles,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.triangles,CacheTriangles);ranges=writableSceneMeshes?CachedUploadVector(meshes.ranges,CacheWritableRanges,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(meshes.ranges,CacheRanges);indicesSRV=Srv(indices.Get(),uint32_t(meshes.indices.size()),4,indicesSRV);verticesSRV=Srv(vertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),verticesSRV);meshletsSRV=Srv(meshlets.Get(),uint32_t(meshes.meshlets.size()),sizeof(Meshlet),meshletsSRV);remapSRV=Srv(remap.Get(),uint32_t(meshes.remap.size()),4,remapSRV);trianglesSRV=Srv(triangles.Get(),uint32_t(meshes.triangles.size()),12,trianglesSRV);rangesSRV=Srv(ranges.Get(),uint32_t(meshes.ranges.size()),sizeof(MeshRange),rangesSRV);
        if(writableSceneMeshes){
            geometryUAVs[0]=UavBuffer(vertices.Get(),uint32_t(meshes.vertices.size()),sizeof(Vertex),geometryUAVs[0]);
            geometryUAVs[1]=UavBuffer(indices.Get(),uint32_t(meshes.indices.size()),sizeof(uint32_t),geometryUAVs[1]);
            geometryUAVs[2]=UavBuffer(meshlets.Get(),uint32_t(meshes.meshlets.size()),sizeof(Meshlet),geometryUAVs[2]);
            geometryUAVs[3]=UavBuffer(remap.Get(),uint32_t(meshes.remap.size()),sizeof(uint32_t),geometryUAVs[3]);
            geometryUAVs[4]=UavBuffer(triangles.Get(),uint32_t(meshes.triangles.size()),sizeof(std::array<uint32_t,3>),geometryUAVs[4]);
            geometryUAVs[5]=UavBuffer(ranges.Get(),uint32_t(meshes.ranges.size()),sizeof(MeshRange),geometryUAVs[5]);
        }
        if(!assets.triangleSolids.empty()){
            if(assets.triangleSolids.size()*3!=meshes.indices.size())Fail("triangle solid count");
            triangleSolids=writableSceneMeshes?CachedUploadVector(assets.triangleSolids,CacheWritableTriangleSolids,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(assets.triangleSolids,CacheTriangleSolids);
            triangleSolidsSRV=Srv(triangleSolids.Get(),uint32_t(assets.triangleSolids.size()),sizeof(uint32_t),triangleSolidsSRV);
            if(writableSceneMeshes)triangleSolidsUAV=UavBuffer(triangleSolids.Get(),uint32_t(assets.triangleSolids.size()),sizeof(uint32_t),triangleSolidsUAV);
        }
        if(!assets.meshletRegions.empty()){
            if(assets.meshletRegions.size()!=meshes.meshlets.size())Fail("meshlet region count");
            meshletRegions=writableSceneMeshes?CachedUploadVector(assets.meshletRegions,CacheWritableMeshletRegions,geometryGenerationSerial,nullptr,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS):CachedUploadVector(assets.meshletRegions,CacheMeshletRegions);
            meshletRegionsSRV=Srv(meshletRegions.Get(),uint32_t(assets.meshletRegions.size()),sizeof(MeshletRegion),meshletRegionsSRV);
            if(writableSceneMeshes)meshletRegionsUAV=UavBuffer(meshletRegions.Get(),uint32_t(assets.meshletRegions.size()),sizeof(MeshletRegion),meshletRegionsUAV);
        }
        if(assets.samplers.size()>1021)Fail("material sampler capacity");
        for(uint32_t i=0;i<assets.samplers.size();i++){const auto& source=assets.samplers[i];D3D12_SAMPLER_DESC sampler{};auto address=[](uint32_t mode){return mode==33071?D3D12_TEXTURE_ADDRESS_MODE_CLAMP:mode==33648?D3D12_TEXTURE_ADDRESS_MODE_MIRROR:D3D12_TEXTURE_ADDRESS_MODE_WRAP;};sampler.AddressU=address(source.wrapS);sampler.AddressV=address(source.wrapT);sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;bool minLinear=source.minFilter==9729||source.minFilter==9985||source.minFilter==9987,magLinear=source.magFilter!=9728,mipLinear=source.minFilter==9986||source.minFilter==9987;sampler.Filter=D3D12_ENCODE_BASIC_FILTER(minLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,magLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,mipLinear?D3D12_FILTER_TYPE_LINEAR:D3D12_FILTER_TYPE_POINT,D3D12_FILTER_REDUCTION_TYPE_STANDARD);sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxAnisotropy=1;sampler.MaxLOD=source.minFilter==9728||source.minFilter==9729?0:D3D12_FLOAT32_MAX;auto handle=samplers->GetCPUDescriptorHandleForHeapStart();handle.ptr+=SIZE_T(i+3)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);device->CreateSampler(&sampler,handle);}
        std::vector<uint32_t> textureDescriptors;textureDescriptors.reserve(assets.textures.size());for(const auto& texture:assets.textures)textureDescriptors.push_back(UploadTexture(texture));surfaceData.reserve(assets.materials.size());for(const auto& material:assets.materials){Surface s{};s.colorRoughness=material.colorRoughness;s.emissionMetallic=material.emissionMetallic;s.albedo=material.albedo==Invalid?Invalid:textureDescriptors[material.albedo];s.normal=material.normal==Invalid?Invalid:textureDescriptors[material.normal];s.metalRough=material.metalRough==Invalid?Invalid:textureDescriptors[material.metalRough];s.emissive=material.emissive==Invalid?Invalid:textureDescriptors[material.emissive];s.parameters={material.normalScale,material.heightUvScale.x,material.heightUvScale.y,material.macroVariation};s.extra={material.occlusion==Invalid?Invalid:textureDescriptors[material.occlusion],material.alphaMode,material.doubleSided,material.unlit};s.opacity={material.alpha,material.cutoff,material.occlusionStrength,1};s.roughnessOverride=material.roughnessOverride==Invalid?Invalid:textureDescriptors[material.roughnessOverride];materialSlots.push_back(material.authoringSlot);s.mapping=material.mapping;for(uint32_t slot=0;slot<5;slot++)s.mapping[slot].rotationSet.w=material.samplers[slot]==Invalid?0.f:float(3+material.samplers[slot]);surfaceData.push_back(s);}
        visibleMaterialBits.assign((surfaceData.size()+63)/64,0);materialResidencyOffsets.resize(surfaceData.size()+1);std::vector<uint32_t> textureResidencyByDescriptor(DescriptorCapacity,Invalid);uint32_t maxTextureResidency=0;for(const auto& texture:textures){uint32_t descriptor=Physical(texture.descriptor);textureResidencyByDescriptor[descriptor]=texture.residency;maxTextureResidency=std::max(maxTextureResidency,texture.residency);}
        baseSurfaceData=surfaceData;
        shadowSurfaceRevision.assign(surfaceData.size(),0);
        for(uint32_t i=0;i<materialSlots.size();++i)
            if(materialSlots[i]!=Invalid)materialSurfacesBySlot[materialSlots[i]].push_back(i);
        ApplySurfaceEdits();
        materialResidencyPages.clear();materialResidencyPages.reserve(surfaceData.size()*2);for(uint32_t material=0;material<surfaceData.size();material++){materialResidencyOffsets[material]=uint32_t(materialResidencyPages.size());const auto& surface=surfaceData[material];const uint32_t descriptors[]={surface.albedo,surface.normal,surface.metalRough,surface.emissive,surface.extra.x,surface.roughnessOverride};for(uint32_t descriptor:descriptors){if(descriptor==Invalid)continue;if(descriptor>=textureResidencyByDescriptor.size()||textureResidencyByDescriptor[descriptor]==Invalid)Fail("material texture residency dependency");uint32_t page=textureResidencyByDescriptor[descriptor];bool duplicate=false;for(uint32_t i=materialResidencyOffsets[material];i<materialResidencyPages.size();i++)if(materialResidencyPages[i]==page){duplicate=true;break;}if(!duplicate)materialResidencyPages.push_back(page);}}materialResidencyOffsets[surfaceData.size()]=uint32_t(materialResidencyPages.size());residencyPageSeen.assign(textures.empty()?0:maxTextureResidency+1,0);requiredResidencyPages.clear();requiredResidencyPages.reserve(textures.size());std::vector<GpuInstance> staticInstances;staticInstances.resize(assets.objects.size());staticMaterialBits.assign(visibleMaterialBits.size(),0);staticTransparent.clear();staticTransparent.reserve(assets.objects.size());staticDeformedMeshes.clear();staticDeformedRtMeshes.clear();staticReflectionCandidates={};std::vector<uint8_t> staticDeformedSeen(dynamicMeshes.size(),0),staticDeformedRtSeen(dynamicMeshes.size(),0);for(uint32_t i=0;i<assets.objects.size();i++){auto object=assets.objects[i];object.id=0x80000000u+i;auto gpu=ConvertObject(object,1.f);staticInstances[i]=gpu;if(gpu.material<surfaceData.size()){staticMaterialBits[gpu.material>>6]|=uint64_t(1)<<(gpu.material&63);for(uint32_t q=0;q<staticReflectionCandidates.size();q++)staticReflectionCandidates[q]|=SurfaceMayReflect(surfaceData[gpu.material],q);}if(gpu.flags&AlphaBlendObject)staticTransparent.push_back(i);if(gpu.flags&DeformedObject){uint32_t dynamicIndex=meshDynamicIndex[gpu.mesh];if(dynamicIndex==Invalid)Fail("DeformedObject references mesh without dynamic deformation capability");if(!staticDeformedSeen[dynamicIndex]){staticDeformedSeen[dynamicIndex]=1;staticDeformedMeshes.push_back(dynamicIndex);}if(!(gpu.flags&NoRayTracing)&&!staticDeformedRtSeen[dynamicIndex]){staticDeformedRtSeen[dynamicIndex]=1;staticDeformedRtMeshes.push_back(dynamicIndex);}}}materialCaptureOwners.resize(surfaceData.size());
        if(sphereCount>1u)for(uint32_t owner=0;owner<sphereCount;++owner){
            for(uint32_t i=sphereObjectOffsets[owner];i<sphereObjectOffsets[owner+1u];++i){
                const auto material=staticInstances[i].material;
                if(material>=materialCaptureOwners.size())Fail("Sphere capture material out of range");
                auto& owners=materialCaptureOwners[material];
                if(owners.empty()||owners.back()!=owner)owners.push_back(owner);
            }
        }
        sceneCache.Replace(std::move(staticInstances));rayScene.ReplaceStatic(sceneCache.StaticInstances());if(sceneCache.StaticCount())residentStaticInstances=CachedUploadBuffer(sceneCache.StaticInstances().data(),uint64_t(sceneCache.StaticCount())*sizeof(GpuInstance),CacheStaticInstances,sceneCache.ContentGeneration());materials=UploadVector(surfaceData);dirtySurfaces.clear();materialsSRV=Srv(materials.Get(),uint32_t(surfaceData.size()),sizeof(Surface),materialsSRV);deformations=CachedUploadVector(meshes.deformations,CacheDeformations);deformationsSRV=Srv(deformations.Get(),uint32_t(meshes.deformations.size()),sizeof(Deformation),deformationsSRV);skinVertices=CachedUploadVector(meshes.skin,CacheSkin);skinSRV=Srv(skinVertices.Get(),uint32_t(meshes.skin.size()),sizeof(SkinVertex),skinSRV);if(meshes.morphs.empty()){MorphVertex zero;morphVertices=CachedUploadBuffer(&zero,sizeof(zero),CacheMorphs);}else morphVertices=CachedUploadVector(meshes.morphs,CacheMorphs);morphSRV=Srv(morphVertices.Get(),uint32_t(std::max(size_t(1),meshes.morphs.size())),sizeof(MorphVertex),morphSRV);InitializeCutSource(assets);startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);uploads.clear();BuildBLAS();
        for(auto& f:frames){
            if(!meshes.ranges.empty()) {
                DynamicResources(f);
                EnsureInstances(f,sceneCache.StaticCount()+128,sceneCache.StaticCount()+128,sceneCache.StaticCount()+128);
            }
            if(f.dynamicScratchBytes)f.dynamicScratch=Temporary(Buffer(f.dynamicScratchBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if(!meshes.ranges.empty())EnsureLightBuffers(f);
        }
        startup.Reset();
        for(auto& f:frames){
            std::vector<XMUINT4> addresses(meshes.ranges.size());
            for(uint32_t i=0;i<addresses.size();++i){
                auto staticAddress=blas[i]->GetGPUVirtualAddress();
                const auto dynamic=meshDynamicIndex[i];
                auto frameAddress=dynamic==Invalid?staticAddress:f.dynamicBLAS[dynamic]->GetGPUVirtualAddress();
                addresses[i]={uint32_t(staticAddress),uint32_t(staticAddress>>32),
                              uint32_t(frameAddress),uint32_t(frameAddress>>32)};
            }
            f.blasAddresses=UploadVector(addresses);
            f.blasAddressesSRV=Srv(f.blasAddresses.Get(),uint32_t(addresses.size()),
                                  sizeof(XMUINT4),f.blasAddressesSRV);
        }
        ConfigurePhosphorOrbit();
        if(!meshes.ranges.empty()||phosphorOrbit.vertices){
            startup.Submit(direct);
            WaitStartup(direct);
        }else{
            startup.Close();
        }
        uploads.clear();
        for(auto& f:frames)InitializeResourceAbi(f);
        DXGI_ADAPTER_DESC1 adapterInfo{};
        dlss.available=!outputSurface.desktop&&viewCount==2u&&!meshes.ranges.empty()&&
            SUCCEEDED(adapter->GetDesc1(&adapterInfo))&&adapterInfo.VendorId==0x10deu;
        ConfigureForest(assets);
        Pipelines();
        if(DenseSphereField()){
            if(solarSystem&&buildingRevision){
                oasisRelief.enabled=true;
                SetupSphereCache(oasisRelief,SphereCacheLayers,OasisReliefSize);
            }
            const uint32_t sharedBodies=sphereCount-(HasOasisRelief()?1u:0u);
            SetupSphereCache(sphereLod,solarSystem?sharedBodies*SphereCacheLayers:SphereFieldLayers);
            for(renderSphere=0;renderSphere<sphereCount;++renderSphere){
                auto& cache=SphereCache(renderSphere);
                cache.current=cache.candidate=SphereRepresentation::Hidden;
                EnsureMembraneResources();
                if(cutInitialCount)EnsureDebrisResources();
            }
            renderSphere=0u;
        }else{
            SetupSphereCache(sphereLod);
            SetupSphereCache(companionLod);
        }
        sceneLoaded=true;
    }

    void Graphics::Impl::ReplaceScene(const SceneData& assets,RenderContributions extra){if(!outputConfigured||!jobs)Fail("Graphics runtime is not configured");auto output=outputSurface;ConfigureOutput(output,assets,*jobs,extra);}

    void Graphics::Impl::UnloadScene(){if(!outputConfigured||!sceneLoaded)return;WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetSceneResources();}

    void Graphics::Impl::ReleaseOutput(){if(!outputConfigured)return;WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);if(sceneLoaded)ResetSceneResources();xrRTV.clear();xrDSV.clear();xrColorTargets.clear();rtvCount=dsvCount=0;outputSurface={};outputConfigured=false;}

    Graphics::Impl::~Impl(){if(device){residency.Wait(direct.queue.Get());residency.Wait(compute.queue.Get());residency.Wait(copy.queue.Get());WaitStartup(direct);WaitStartup(compute);WaitStartup(copy);ResetForest();ResetDlss();publications.clear();externalDomains.clear();ReleaseDescriptor(aaSourceArraySrv);aaSource={};aaSourceLease.reset();managedMemory.EndFrame(SubmittedFences());CollectManaged();managedMemory.Shutdown();vramCache.Shutdown(CompletedFences());descriptorAllocator.EndRecording(SubmittedFences());descriptorAllocator.Collect(CompletedFences());}library.Reset();}

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

    GpuInstance Graphics::Impl::ConvertObject(const RenderObject& o,float alpha)const{
        if(o.mesh>=meshes.ranges.size()||o.material>=surfaceData.size())Fail("render asset handle");


        const bool stationary=
            o.previous.p.x==o.current.p.x&&o.previous.p.y==o.current.p.y&&
            o.previous.p.z==o.current.p.z&&
            o.previous.q.x==o.current.q.x&&o.previous.q.y==o.current.q.y&&
            o.previous.q.z==o.current.q.z&&o.previous.q.w==o.current.q.w;
        const Pose p=stationary?o.current:Blend(o.previous,o.current,alpha);
        GpuInstance out{};auto bounds=meshes.ranges[o.mesh].bounds;float expansion=(o.flags&DeformedObject)?1+std::abs(meshes.deformations[o.mesh].axisAmplitude.w):1;
        out.positionRadius={p.p.x,p.p.y,p.p.z,bounds.w*std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)})*expansion};out.rotation={p.q.x,p.q.y,p.q.z,p.q.w};out.scale={o.scale.x,o.scale.y,o.scale.z};
        const uint32_t visual=o.flags&(LunarActorVisual|ShuttleCrt|ShuttleSpeedDust);
        if(visual!=0u&&(visual&(visual-1u)))Fail("Instance has multiple visual payloads");
        if((o.flags&SolarWorldVisual)&&!(o.flags&NoRayTracing))Fail("Cosmos visuals require NoRayTracing");
        if(visual==LunarActorVisual)out.visualData=o.lunarActor;
        else if(visual==ShuttleCrt)out.visualData=o.cockpitDisplay;
        else if(visual==ShuttleSpeedDust)out.visualData=o.speedDust;
        out.mesh=o.mesh;out.material=o.material;out.id=o.id;out.flags=o.flags;if(surfaceData[o.material].extra.y==2)out.flags|=DynamicObject|AlphaBlendObject;return out;
    }
}
