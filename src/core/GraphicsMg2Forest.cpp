#include "GraphicsInternal.h"
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include <nvapi.h>

namespace OASIS {
namespace {
constexpr uint32_t PagedMeshBit=0x80000000u;
constexpr auto ShaderRead=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto AsState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr auto UavState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
// Headroom is tested before building. This is a publication upper bound,
// not a permanent 256 MiB allocation; only the actual CLAS/BLAS buffers exist.
constexpr UINT64 ForestBuildBudget=256u*1024u*1024u;

struct ForestPartRange {uint32_t first,count,firstIndex,indexCount,material;};
struct ClusterLimits {
    uint32_t clusters,parts,totalTriangles,totalVertices;
    uint32_t maxTriangleCount{},maxVertexCount{},maxPrimitiveBase{},maxClustersPerBlas{};
};
struct ClusterRequirements {UINT64 result{},scratch{};};
struct ClusterBuffers {
    UINT64 result,scratch;
    D3D12_GPU_VIRTUAL_ADDRESS_AND_STRIDE addresses,args;
};

struct NvapiSession {
    const NvAPI_Status status{NvAPI_Initialize()};
    ~NvapiSession(){if(status==NVAPI_OK)NvAPI_Unload();}
};

NvAPI_Status NvapiClusterPipelines(ID3D12Device5* device,bool enable){
    const NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS options{
        NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER,
        enable?NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_CLUSTER_SUPPORT:
               NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_NONE};
    return NvAPI_D3D12_SetCreatePipelineStateOptions(device,&options);
}

struct NvapiClusterBatch {
    using Triangle=NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_TRIANGLE_CLUSTER_ARGS;
    using Blas=NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_CLUSTER_ARGS;
    std::array<NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS,2> inputs{};

    static Triangle TriangleArgs(const Mg2LodCluster& cluster,uint32_t primitiveBase,UINT64 vertices,UINT64 indices){
        Triangle args{};
        args.clusterId=cluster.id;
        args.triangleCount=cluster.indexCount/3u;
        args.vertexCount=cluster.vertexCount;
        args.indexFormat=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INDEX_FORMAT_32BIT;
        args.baseGeometryIndexAndFlags=primitiveBase;
        if(cluster.opaque)args.baseGeometryIndexAndFlags|=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_GEOMETRY_FLAG_OPAQUE;
        args.vertexBufferStride=sizeof(Vertex);
        args.indexBufferStride=sizeof(uint32_t);
        args.vertexBuffer=vertices;
        args.indexBuffer=indices;
        return args;
    }
    static Blas BlasArgs(uint32_t count,UINT64 addresses){return {count,0,addresses};}

    int Prepare(ID3D12Device10* device,ID3D12GraphicsCommandList7*,const ClusterLimits& limits,
                std::array<ClusterRequirements,2>& requirements){
        if(limits.maxPrimitiveBase>NVAPI_D3D12_RAYTRACING_MAXIMUM_GEOMETRY_INDEX)
            return NVAPI_INVALID_ARGUMENT;
        for(auto& input:inputs){
            input.mode=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_IMPLICIT_DESTINATIONS;
            input.flags=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_TRACE;
        }
        auto& clas=inputs[0];
        clas.maxArgCount=limits.clusters;
        clas.type=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        clas.trianglesDesc={DXGI_FORMAT_R32G32B32_FLOAT,limits.maxPrimitiveBase,1u,
            limits.maxTriangleCount,limits.maxVertexCount,limits.totalTriangles,limits.totalVertices,0u};
        auto& blas=inputs[1];
        blas.maxArgCount=limits.parts;
        blas.type=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        blas.clasDesc={limits.clusters,limits.maxClustersPerBlas};
        for(size_t i=0;i<inputs.size();++i){
            NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO info{};
            const NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS query{
                NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER,&inputs[i],&info};
            const auto status=NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo(device,&query);
            if(status!=NVAPI_OK)return status;
            requirements[i]={info.resultDataMaxSizeInBytes,info.scratchDataSizeInBytes};
        }
        return NVAPI_OK;
    }

    int Record(ID3D12GraphicsCommandList7* list,size_t operation,const ClusterBuffers& buffers){
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC desc{};
        desc.inputs=inputs[operation];
        desc.batchResultData=buffers.result;
        desc.batchScratchData=buffers.scratch;
        desc.destinationAddressArray=buffers.addresses;
        desc.indirectArgArray=buffers.args;
        const NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS params{
            NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS_VER,&desc};
        return NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation(list,&params);
    }
};

#if OASIS_MG2_PREVIEW
struct DxrClusterBatch {
    using Triangle=D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS;
    using Blas=D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS;
    D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC triangles{};
    D3D12_RTAS_CLAS_INPUTS_DESC assembly{};
    std::array<D3D12_RTAS_OPERATION_DESC,2> operations{};
    ComPtr<ID3D12CommandListRaytracing2> rayList;

    static Triangle TriangleArgs(const Mg2LodCluster& cluster,uint32_t primitiveBase,UINT64 vertices,UINT64 indices){
        Triangle args{};
        args.ClusterID=cluster.id;
        args.TriangleCount=uint16_t(cluster.indexCount/3u);
        args.UniqueVertexIndexCount=uint16_t(cluster.vertexCount);
        args.BaseGeometryIndexAndFlags=primitiveBase;
        if(cluster.opaque)args.BaseGeometryIndexAndFlags|=D3D12_RTAS_CLUSTERED_GEOMETRY_FLAG_OPAQUE;
        args.VertexBufferStride=sizeof(Vertex);
        args.IndexBufferStride=sizeof(uint32_t);
        args.VertexBuffer=vertices;
        args.IndexBuffer=indices;
        return args;
    }
    static Blas BlasArgs(uint32_t count,UINT64 addresses){return {count,sizeof(UINT64),addresses};}

    int Prepare(ID3D12Device10* device,ID3D12GraphicsCommandList7* list,const ClusterLimits& limits,
                std::array<ClusterRequirements,2>& requirements){
        if(limits.maxPrimitiveBase>D3D12_RAYTRACING_MAXIMUM_GEOMETRY_INDEX)return E_INVALIDARG;
        ComPtr<ID3D12DeviceRaytracing2> rayDevice;
        if(const auto status=device->QueryInterface(IID_PPV_ARGS(&rayDevice));FAILED(status))return status;
        if(const auto status=list->QueryInterface(IID_PPV_ARGS(&rayList));FAILED(status))return status;
        triangles.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        triangles.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        triangles.VertexFormat=D3D12_VERTEX_FORMAT_FLOAT32_3;
        triangles.IndexFormat=D3D12_INDEX_FORMAT_UINT32;
        auto& bounds=triangles.ClusterLimits;
        bounds.MaxArgCount=limits.clusters;
        bounds.MaxUniqueGeometryIndexAndFlagsCountPerCluster=1;
        bounds.MaxGeometryIndexValue=limits.maxPrimitiveBase;
        bounds.MaxTriangleCountPerCluster=limits.maxTriangleCount;
        bounds.MaxUniqueVertexIndexCountPerCluster=limits.maxVertexCount;
        bounds.MaxTotalTriangleCount=limits.totalTriangles;
        bounds.MaxSumOfUniqueVertexIndexCounts=limits.totalVertices;
        assembly.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        assembly.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        assembly.MaxArgCount=limits.parts;
        assembly.MaxTotalClasCount=limits.clusters;
        assembly.MaxClasCountPerArg=limits.maxClustersPerBlas;
        operations[0].Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        operations[0].Inputs.pClusterTrianglesDesc=&triangles;
        operations[1].Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        operations[1].Inputs.pClasDesc=&assembly;
        for(size_t i=0;i<operations.size();++i){
            D3D12_RTAS_OPERATION_PREBUILD_INFO info{};
            rayDevice->GetRTASOperationPrebuildInfo(&operations[i].Inputs,&info);
            requirements[i]={info.ResultDataMaxSizeInBytes,info.ScratchDataSizeInBytes};
        }
        return 0;
    }

    int Record(ID3D12GraphicsCommandList7*,size_t operation,const ClusterBuffers& buffers){
        D3D12_RTAS_BATCHED_OPERATION_DATA data{};
        data.BatchResultData=buffers.result;
        data.BatchScratchData=buffers.scratch;
        data.ResultAddressArray=buffers.addresses;
        data.IndirectArgumentArray=buffers.args;
        auto desc=operations[operation];
        desc.pBatchedOperationData=&data;
        rayList->ExecuteIndirectRTASOperations(1,&desc,D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
        return 0;
    }
};
#endif

struct PackedView {UINT64 offset{};uint32_t count{},stride{};};
class PackedForest {
public:
    std::vector<std::byte> bytes;
    template<class T> PackedView Add(const std::vector<T>& values){
        const auto alignment=std::lcm(size_t(16),sizeof(T));
        const auto offset=(bytes.size()+alignment-1u)/alignment*alignment;
        bytes.resize(offset+values.size()*sizeof(T));
        std::memcpy(bytes.data()+offset,values.data(),values.size()*sizeof(T));
        return {offset,uint32_t(values.size()),sizeof(T)};
    }
};

void Transition(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource,
                D3D12_RESOURCE_STATES oldState,D3D12_RESOURCE_STATES newState){
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,oldState,newState};
    list->ResourceBarrier(1,&barrier);
}
void FinishUav(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource){
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource=resource;
    list->ResourceBarrier(1,&barrier);
}
}

void Graphics::EnableMg2Diagnostic(){impl_->mg2Diagnostic=true;}
Mg2ClusterStatus Graphics::GetMg2ClusterStatus()const{return impl_->forestStatus;}

void Graphics::Impl::EnableClusterPreview(){
#if OASIS_MG2_PREVIEW
    static const IID features[]{D3D12ExperimentalShaderModels,D3D12RaytracingPreview};
    static const HRESULT enabled=D3D12EnableExperimentalFeatures(UINT(std::size(features)),features,nullptr,nullptr);
    clusterPreview=SUCCEEDED(enabled);
#endif
}

D3D12_PIPELINE_STATE_FLAGS Graphics::Impl::ForestPipelineFlags()const{
#if OASIS_MG2_PREVIEW
    if(forestStatus.configured==Mg2RayBackend::DxrClusters)
        return D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_CLUSTERED_GEOMETRY_ON_PRE_SM69_SHADERS;
#endif
    return D3D12_PIPELINE_STATE_FLAG_NONE;
}

void Graphics::Impl::ConfigureForest(const SceneData& assets){
    forestParts=assets.mg2ForestParts;
    if(forestParts.empty())return;
    std::ranges::sort(forestParts,{},&Mg2ForestPart::mesh);
    forestParts.erase(std::unique(forestParts.begin(),forestParts.end(),
        [](const auto& a,const auto& b){return a.mesh==b.mesh;}),forestParts.end());
    // Cluster LOD simplification is CPU-heavy for fully geometric leaves.
    // Build pages off the startup critical path; the original scene meshes
    // remain drawable until the MG2 publication is ready.
    forestBake=std::async(std::launch::async,[&assets,parts=forestParts]{
        return Mg2Pages(assets,parts);
    });
    for(const auto& part:forestParts){
        const auto& bounds=assets.meshes.ranges[part.mesh].bounds;
        forestRadius=std::max(forestRadius,Length({bounds.x,bounds.y,bounds.z})+bounds.w);
    }
    forestStatus.meshes=uint32_t(forestParts.size());
    forestStatus.sourcePages=0u; // Published when asynchronous baking completes.
    DXGI_ADAPTER_DESC1 info{};
    if(SUCCEEDED(adapter->GetDesc1(&info))){
        forestStatus.adapterVendor=info.VendorId;
        forestStatus.adapterDevice=info.DeviceId;
    }
    if(mg2Diagnostic&&SUCCEEDED(direct.queue->GetTimestampFrequency(&forestTimestampHz))&&forestTimestampHz){
        const D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,3u,0u};
        device->CreateQueryHeap(&desc,IID_PPV_ARGS(&forestBuildQueries));
    }
    char setting[16]{};
    const DWORD length=GetEnvironmentVariableA("OASIS_MG2_CLUSTER_API",setting,sizeof(setting));
    const std::string_view request=length>=sizeof(setting)?"invalide":std::string_view(setting,length);
    if(request=="standard"){
        forestStatus.request="standard";
        forestStatus.capability="demande";
        return;
    }
    if(request=="dxr")forestStatus.request="dxr";
    else if(request=="nvapi")forestStatus.request="nvapi";
    else if(!request.empty()&&request!="auto"){
        forestStatus.request="invalide";
        forestStatus.capability="option_invalide";
        return;
    }
#if OASIS_MG2_PREVIEW
    if(request!="nvapi"&&clusterPreview){
        ComPtr<ID3D12DeviceRaytracing2> rayDevice;
        D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 options{};
        if(SUCCEEDED(device.As(&rayDevice))&&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2,&options,sizeof(options)))&&
           options.ClustersAndPTLASSupported){
            forestStatus.configured=forestBuildBackend=Mg2RayBackend::DxrClusters;
            forestStatus.capability="disponible";
            return;
        }
    }
#endif
    if(request=="dxr"){
        forestStatus.capability="dxr_clusters_indisponible";
        return;
    }
    if(forestStatus.adapterVendor!=0x10deu){
        forestStatus.capability="adaptateur_non_nvidia";
        return;
    }
    static const NvapiSession nvapi;
    if(nvapi.status!=NVAPI_OK){
        forestStatus.code=nvapi.status;
        forestStatus.capability="nvapi_init";
        return;
    }
    NvU32 driver{};
    NvAPI_ShortString branch{};
    if(NvAPI_SYS_GetDriverAndBranchVersion(&driver,branch)==NVAPI_OK)forestStatus.driver=driver;
    NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS caps{};
    auto status=NvAPI_D3D12_GetRaytracingCaps(device.Get(),NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS,
                                             &caps,sizeof(caps));
    forestStatus.code=status;
    if(status!=NVAPI_OK||!(caps&NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD)){
        forestStatus.capability="nvapi_clusters_indisponible";
        return;
    }
    status=NvapiClusterPipelines(device.Get(),true);
    forestStatus.code=status;
    if(status!=NVAPI_OK){
        forestStatus.capability="nvapi_options_pso";
        return;
    }
    forestStatus.configured=forestBuildBackend=Mg2RayBackend::Nvapi;
    forestStatus.capability="disponible";
}

void Graphics::Impl::ResetForest(){
    // A pending bake still reads the scene's immutable asset geometry. Never
    // release a scene while the worker might dereference it.
    if(forestBake.valid())forestBake.wait();
    forestBake={};
    if(forestSelection.requested){
        jobs->Wait(forestSelection.group);
        forestSelection.requested=false;
    }
    forestSelection.cut={};
    forestSelection.pages=nullptr;
    for(auto& frame:frames){
        frame.forestRaster.reset();
        frame.forestTlas.reset();
        frame.forestPreviousTlas.reset();
        frame.forestDrawReadback.Reset();
        frame.forestDrawFrame=0;
        frame.forestRayProofReadback.Reset();
        frame.forestRayProofPending=false;
    }
    for(const auto& generation:forestGenerations)
        for(auto view:generation->views)if(view.Valid())ReleaseDescriptor(view);
    forestGenerations.clear();
    forestPending.reset();
    forestResident.reset();
    forestHierarchy.reset();
    forestParts.clear();
    forestNextSelection=0.;
    forestRadius=0.f;
    if(forestStatus.configured==Mg2RayBackend::Nvapi)NvapiClusterPipelines(device.Get(),false);
    forestStatus={};
    forestBuildQueries.Reset();
    forestTimestampHz=0;
    forestBuildBackend=Mg2RayBackend::Standard;
    forestRequestFailed=false;
}

void Graphics::Impl::PrepareForestPages(Frame& f,const XRFrame& xr,const GpuSceneCache* tiles,bool nativeCampus){
    // The previous use of this frame slot has completed on every queue.
    f.forestPreviousTlas.reset();
    f.forestRaster.reset();
    if(!forestHierarchy&&forestBake.valid()){
        if(forestBake.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
            return;
        try{
            forestHierarchy.emplace(forestBake.get());
            forestStatus.sourcePages=uint32_t(forestHierarchy->PageCount());
        }catch(const std::exception&){
            // A failed optional MG2 bake must not remove the native geometry.
            forestStatus.lastFailure="construction_pages_clod";
            forestParts.clear();
            return;
        }
    }
    if(!forestHierarchy)return;
    if(forestPending&&direct.fence->GetCompletedValue()>=forestPending->fence){
        auto& pending=*forestPending;
        bool valid=true;
        float clasMs=-1.f,blasMs=-1.f;
        if(pending.buildReadback){
            const D3D12_RANGE read{0,SIZE_T(pending.addressCount+(forestBuildQueries?3u:0u))*sizeof(UINT64)},empty{};
            void* mapped=nullptr;
            const bool readable=SUCCEEDED(pending.buildReadback->Map(0,&read,&mapped))&&mapped;
            valid=readable||!pending.addressCount;
            if(readable){
                const auto* values=static_cast<const UINT64*>(mapped);
                for(auto address:std::span(values,pending.addressCount))
                    valid&=address>=pending.blasStart&&address-pending.blasStart<pending.blasCapacity&&
                        address%D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT==0;
                if(forestBuildQueries){
                    const auto* times=values+pending.addressCount;
                    const double tickMs=1000./double(forestTimestampHz);
                    if(pending.addressCount&&times[1]>=times[0])clasMs=float(double(times[1]-times[0])*tickMs);
                    if(times[2]>=times[1])blasMs=float(double(times[2]-times[1])*tickMs);
                }
                pending.buildReadback->Unmap(0,&empty);
            }
            pending.buildReadback.Reset();
        }
        if(valid){
            forestStatus.lastPublished=pending.backend;
            ++forestStatus.publications;
            forestStatus.clasBytes=pending.clasCapacity;
            forestStatus.blasBytes=pending.blasCapacity;
            forestStatus.clasMs=clasMs;
            forestStatus.blasMs=blasMs;
            forestPending->staging.clear();
            forestPending->commands={};
            forestResident=std::move(forestPending);
            shadowCache={};
        }else{
            forestPending.reset();
            forestBuildBackend=Mg2RayBackend::Standard;
            forestStatus.lastFailure="adresses_blas_invalides";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
        }
    }
    std::erase_if(forestGenerations,[&](const auto& generation){
        if(generation.use_count()!=1)return false;
        for(auto view:generation->views)if(view.Valid())ReleaseDescriptor(view);
        return true;
    });
    const bool canPublish=nativeCampus&&tiles;
    if(canPublish)f.forestRaster=forestResident;
    if(forestSelection.requested){
        if(!forestSelection.group.Complete())return;
        forestSelection.requested=false;
        auto cut=std::move(forestSelection.cut);
        if(!canPublish||cut.clusters.empty())return;
        auto candidate=std::make_shared<ForestGeneration>();
        if(!UploadForestPages(cut,*candidate)){
            // Retain working CLAS pages; never permanently downgrade the API
            // because one requested publication exceeds a transient budget.
            forestRequestFailed=true;
            forestNextSelection=xr.input.time+3.;
            return;
        }
        forestRequestFailed=false;
        candidate->selection=std::move(cut.selection);
        forestGenerations.push_back(candidate);
        forestPending=std::move(candidate);
        return;
    }
    if(!canPublish||forestPending||forestGenerations.size()>=3u||
       xr.input.time<forestNextSelection)return;
    forestNextSelection=xr.input.time+.25;

    std::array<float,MaxViews> focal{};
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        const auto& fov=xr.fov[eye];
        const float vertical=std::tan(fov.angleUp)-std::tan(fov.angleDown);
        const float horizontal=std::tan(fov.angleRight)-std::tan(fov.angleLeft);
        focal[eye]=std::max(horizontal>.01f?float(width)/horizontal:float(width)*.5f,
                            vertical>.01f?float(height)/vertical:float(height)*.5f);
    }
    // Treat every resident building/furniture/forest mesh as a possible CLAS
    // source. Share its cluster geometry across all rigid instances.
    struct Nearest {
        float relativeDistance2=std::numeric_limits<float>::infinity();
        std::array<Mg2LodView,MaxViews> eyes{};
    };
    std::unordered_map<uint32_t,Nearest> nearest;
    const auto consider=[&](uint32_t mesh,uint32_t flags,XMFLOAT3 root,
        XMFLOAT4 rotation,XMFLOAT3 scale){
        if((flags&(DeformedObject|AlphaBlendObject))!=0u||
           !std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh))return;
        const float lo=std::min({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        const float hi=std::max({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        if(lo<1.e-6f)return;
        const auto& bounds=meshes.ranges[mesh].bounds;
        const float radius=(Length({bounds.x,bounds.y,bounds.z})+bounds.w)*hi;
        bool visible=false;
        for(uint32_t eye=0;eye<xr.viewCount&&!visible;++eye)
            visible=SphereIntersectsView(XMLoadFloat4x4(&xr.vp[eye]),root,radius,true);
        if(!visible)return;
        const auto delta=Sub(xr.eyes[0],root);
        const float distance2=Dot(delta,delta);
        const float relative=distance2/(lo*lo);
        auto& candidate=nearest[mesh];
        if(relative>=candidate.relativeDistance2)return;
        candidate.relativeDistance2=relative;
        const auto inverse=XMQuaternionConjugate(
            XMVectorSet(rotation.x,rotation.y,rotation.z,rotation.w));
        for(uint32_t eye=0;eye<xr.viewCount;++eye){
            const auto displacement=Sub(xr.eyes[eye],root);
            XMFLOAT3 local{};
            XMStoreFloat3(&local,XMVector3Rotate(XMLoadFloat3(&displacement),inverse));
            candidate.eyes[eye]={mesh,{{local.x/scale.x,local.y/scale.y,local.z/scale.z},
                focal[eye],xr.input.SceneNearZ()/lo},std::sqrt(distance2)};
        }
    };
    if(tiles)for(const auto& [key,tile]:tiles->ResidentTiles())
        for(const auto& object:tile.objects){
            if(!object.enabled)continue;
            consider(object.mesh,object.flags,
                {object.position.x,object.position.y,object.position.z},
                {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
                {object.scale.x,object.scale.y,object.scale.z});
        }
    for(const auto& object:sceneCache.StaticInstances())
        consider(object.mesh,object.flags,
            {object.positionRadius.x,object.positionRadius.y,object.positionRadius.z},
            {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
            {object.scale.x,object.scale.y,object.scale.z});
    std::vector<Mg2LodView> eyes;
    eyes.reserve(nearest.size()*xr.viewCount);
    for(const auto& [mesh,nearestInstance]:nearest)
        for(uint32_t eye=0;eye<xr.viewCount;++eye)eyes.push_back(nearestInstance.eyes[eye]);
    if(eyes.empty())return;
    forestSelection.pages=&*forestHierarchy;
    forestSelection.eyes=std::move(eyes);
    forestSelection.previous=forestResident?forestResident->selection:std::vector<uint32_t>{};
    forestSelection.requested=true;
    jobs->Submit([](void* context,uint32_t,uint32_t){
        auto& work=*static_cast<ForestSelectionWork*>(context);
        work.cut=work.pages->Select(work.eyes,1.1f,work.previous);
    },&forestSelection,0,1,&forestSelection.group);
}

bool Graphics::Impl::UploadForestPages(const Mg2LodCut& cut,ForestGeneration& output){
    constexpr uint32_t RasterVertices=64, RasterTriangles=124, RasterBatch=16;
    constexpr uint32_t DispatchPage=32768, MaxDispatchGroups=1u<<22;
    std::vector<uint32_t> routes(meshes.ranges.size(),Invalid),globalIndices,rasterRemap;
    std::vector<Meshlet> rasterMeshlets;
    std::vector<std::array<uint32_t,3>> triangles;
    std::vector<MeshRange> level;
    std::vector<ForestPartRange> parts;
    const auto invalidCut=[&]{
        forestStatus.lastFailure="plage_meshlet_raster";
        forestStatus.buildCode=0;
        return false;
    };
    for(uint32_t i=0;i<cut.clusters.size();){
        const auto& first=cut.clusters[i];
        if(first.mesh>=routes.size()||routes[first.mesh]!=Invalid)return invalidCut();
        const uint32_t rasterFirst=uint32_t(rasterMeshlets.size());
        auto j=i;
        for(;j<cut.clusters.size()&&cut.clusters[j].mesh==first.mesh;++j){
            const auto& cluster=cut.clusters[j];
            if(!cluster.vertexCount||cluster.vertexCount>128u||!cluster.indexCount||
               cluster.indexCount%3u||uint64_t(cluster.firstVertex)+cluster.vertexCount>cut.vertices.size()||
               cluster.firstIndex!=globalIndices.size()||
               uint64_t(cluster.firstIndex)+cluster.indexCount>cut.indices.size())return invalidCut();
            std::array<uint32_t,128> localToRaster;
            Meshlet raster{};
            const auto startMeshlet=[&]{
                localToRaster.fill(Invalid);
                raster={uint32_t(rasterRemap.size()),uint32_t(triangles.size()),0u,0u};
            };
            startMeshlet();
            for(uint32_t k=0;k<cluster.indexCount;k+=3u){
                const auto* local=cut.indices.data()+cluster.firstIndex+k;
                if(local[0]>=cluster.vertexCount||local[1]>=cluster.vertexCount||
                   local[2]>=cluster.vertexCount)return invalidCut();
                uint32_t added=uint32_t(localToRaster[local[0]]==Invalid);
                if(local[1]!=local[0]&&localToRaster[local[1]]==Invalid)++added;
                if(local[2]!=local[0]&&local[2]!=local[1]&&localToRaster[local[2]]==Invalid)++added;
                if(raster.vertexCount+added>RasterVertices||raster.triangleCount==RasterTriangles){
                    rasterMeshlets.push_back(raster);
                    startMeshlet();
                }
                std::array<uint32_t,3> triangle{};
                for(uint32_t v=0;v<3;++v){
                    const uint32_t vertex=cluster.firstVertex+local[v];
                    auto& mapped=localToRaster[local[v]];
                    if(mapped==Invalid){
                        mapped=raster.vertexCount++;
                        rasterRemap.push_back(vertex);
                    }
                    triangle[v]=mapped;
                    globalIndices.push_back(vertex);
                }
                triangles.push_back(triangle);
                ++raster.triangleCount;
            }
            rasterMeshlets.push_back(raster);
        }
        const uint32_t rasterCount=uint32_t(rasterMeshlets.size())-rasterFirst;
        const uint32_t dispatchGroups=rasterCount<=65535u?rasterCount:
            ((rasterCount+DispatchPage-1u)/DispatchPage)*DispatchPage;
        if(uint64_t(dispatchGroups)*RasterBatch>=MaxDispatchGroups)return invalidCut();
        routes[first.mesh]=PagedMeshBit|uint32_t(level.size());
        output.routedMeshes.push_back(first.mesh);
        const auto& last=cut.clusters[j-1u];
        const auto vertexCount=last.firstVertex+last.vertexCount-first.firstVertex;
        const auto indexCount=last.firstIndex+last.indexCount-first.firstIndex;
        parts.push_back({i,j-i,first.firstIndex,indexCount,first.material});
        level.push_back({rasterFirst,rasterCount,first.firstVertex,vertexCount,first.firstIndex,indexCount,
                         meshes.ranges[first.mesh].bounds});
        i=j;
    }
    if(level.empty()||level.size()>forestParts.size())return invalidCut();
    std::ranges::sort(output.routedMeshes);
    PackedForest pack;
    std::array<PackedView,ForestGeneration::Addresses> layouts{
        pack.Add(routes),pack.Add(level),pack.Add(rasterMeshlets),pack.Add(cut.vertices),
        pack.Add(triangles),pack.Add(globalIndices)};
    const auto remap=pack.Add(rasterRemap);
    const bool clustered=forestBuildBackend!=Mg2RayBackend::Standard;
    const auto localIndices=clustered?pack.Add(cut.indices):PackedView{};
    if(pack.bytes.size()>ForestBuildBudget||managedMemory.Budget().headroom<ForestBuildBudget){
        forestStatus.lastFailure="budget_geometrie";
        forestStatus.buildCode=0;
        return false;
    }

    const auto make=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,
                        D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
        ComPtr<ID3D12Resource> resource;
        const auto heap=Heap(type);
        const auto desc=Buffer(std::max(UINT64(256),bytes),flags);
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,
            nullptr,IID_PPV_ARGS(&resource))))return ComPtr<ID3D12Resource>{};
        return resource;
    };
    const auto upload=[&](const void* data,size_t bytes){
        auto source=make(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const D3D12_RANGE none{};
        void* memory{};
        if(!source||FAILED(source->Map(0,&none,&memory)))return ComPtr<ID3D12Resource>{};
        std::memcpy(memory,data,bytes);
        source->Unmap(0,nullptr);
        return source;
    };
    const auto geometry=make(pack.bytes.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    const auto source=upload(pack.bytes.data(),pack.bytes.size());
    if(!geometry||!source){
        forestStatus.lastFailure="allocation_geometrie";
        forestStatus.buildCode=0;
        return false;
    }
    const auto vertexAddress=geometry->GetGPUVirtualAddress()+layouts[ForestGeneration::Vertices].offset;
    const auto indexAddress=geometry->GetGPUVirtualAddress()+layouts[ForestGeneration::Indices].offset;

    CommandsInit(output.commands,D3D12_COMMAND_LIST_TYPE_DIRECT);
    output.commands.Reset();
    auto* list=output.commands.list.Get();
    list->CopyBufferRegion(geometry.Get(),0,source.Get(),0,pack.bytes.size());
    Transition(list,geometry.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ShaderRead);
    output.resources.push_back(geometry);
    output.staging.push_back(source);

    ComPtr<ID3D12Resource> addresses;
    const auto fail=[&](const char* reason,int code=0){
        forestStatus.lastFailure=reason;
        forestStatus.buildCode=code;
        return false;
    };
    const UINT64 addressBytes=clustered?parts.size()*sizeof(UINT64):0u;
    if(addressBytes||forestBuildQueries){
        output.buildReadback=make(addressBytes+(forestBuildQueries?3u*sizeof(UINT64):0u),
            D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!output.buildReadback)return fail("lecture_differee");
    }
    if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0u);
    if(clustered){
        const auto localAddress=geometry->GetGPUVirtualAddress()+localIndices.offset;
        const auto build=[&]<class Api>(Api& api){
            std::vector<typename Api::Triangle> clusterArgs;
            clusterArgs.reserve(cut.clusters.size());
            ClusterLimits limits{uint32_t(cut.clusters.size()),uint32_t(parts.size()),
                uint32_t(triangles.size()),uint32_t(cut.vertices.size())};
            for(const auto& part:parts){
                limits.maxClustersPerBlas=std::max(limits.maxClustersPerBlas,part.count);
                for(uint32_t i=part.first;i<part.first+part.count;++i){
                    const auto& cluster=cut.clusters[i];
                    const uint32_t primitiveBase=(cluster.firstIndex-part.firstIndex)/3u;
                    limits.maxPrimitiveBase=std::max(limits.maxPrimitiveBase,primitiveBase);
                    limits.maxTriangleCount=std::max(limits.maxTriangleCount,cluster.indexCount/3u);
                    limits.maxVertexCount=std::max(limits.maxVertexCount,cluster.vertexCount);
                    clusterArgs.push_back(Api::TriangleArgs(cluster,primitiveBase,
                        vertexAddress+UINT64(cluster.firstVertex)*sizeof(Vertex),
                        localAddress+UINT64(cluster.firstIndex)*sizeof(uint32_t)));
                }
            }
            std::array<ClusterRequirements,2> sizes{};
            if(const auto code=api.Prepare(device.Get(),list,limits,sizes);code)
                return fail("taille_clas_blas",code);
            const auto scratchBytes=std::max(sizes[0].scratch,sizes[1].scratch);
            if(!sizes[0].result||!sizes[1].result)return fail("taille_clas_blas_nulle");
            if(sizes[0].result+sizes[1].result+scratchBytes+pack.bytes.size()>ForestBuildBudget)
                return fail("budget_clas_blas");
            const auto alloc=[&](UINT64 bytes,D3D12_RESOURCE_STATES state){
                return make(bytes,D3D12_HEAP_TYPE_DEFAULT,state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            };
            const auto clasAddresses=alloc(limits.clusters*sizeof(UINT64),UavState);
            addresses=alloc(addressBytes,UavState);
            const auto clas=alloc(sizes[0].result,AsState);
            const auto blas=alloc(sizes[1].result,AsState);
            const auto scratch=alloc(scratchBytes,UavState);
            if(!clasAddresses||!addresses||!clas||!blas||!scratch)return fail("allocation_clas_blas");
            const auto clasAddressBase=clasAddresses->GetGPUVirtualAddress();
            std::vector<typename Api::Blas> blasArgs;
            blasArgs.reserve(parts.size());
            for(const auto& part:parts)
                blasArgs.push_back(Api::BlasArgs(part.count,clasAddressBase+UINT64(part.first)*sizeof(UINT64)));
            const auto clusterArgBuffer=upload(clusterArgs.data(),clusterArgs.size()*sizeof(clusterArgs.front()));
            const auto blasArgBuffer=upload(blasArgs.data(),blasArgs.size()*sizeof(blasArgs.front()));
            if(!clusterArgBuffer||!blasArgBuffer)return fail("arguments_clas_blas");
            const auto scratchAddress=scratch->GetGPUVirtualAddress();
            const ClusterBuffers clasBuffers{clas->GetGPUVirtualAddress(),scratchAddress,
                {clasAddressBase,sizeof(UINT64)},{clusterArgBuffer->GetGPUVirtualAddress(),sizeof(clusterArgs.front())}};
            const ClusterBuffers blasBuffers{blas->GetGPUVirtualAddress(),scratchAddress,
                {addresses->GetGPUVirtualAddress(),sizeof(UINT64)},{blasArgBuffer->GetGPUVirtualAddress(),sizeof(blasArgs.front())}};
            if(const auto code=api.Record(list,0u,clasBuffers);code)return fail("commande_clas",code);
            FinishUav(list,clas.Get());
            FinishUav(list,scratch.Get());
            Transition(list,clasAddresses.Get(),UavState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1u);
            if(const auto code=api.Record(list,1u,blasBuffers);code)return fail("commande_blas_cluster",code);
            FinishUav(list,blas.Get());
            output.addressCount=uint32_t(parts.size());
            output.clasCapacity=sizes[0].result;
            output.blasStart=blas->GetGPUVirtualAddress();
            output.blasCapacity=sizes[1].result;
            output.resources.insert(output.resources.end(),{clas,blas});
            output.staging.insert(output.staging.end(),{clasAddresses,scratch,clusterArgBuffer,blasArgBuffer});
            return true;
        };
#if OASIS_MG2_PREVIEW
        if(forestBuildBackend==Mg2RayBackend::DxrClusters){
            DxrClusterBatch api;
            if(!build(api))return false;
        }else
#endif
        {
            NvapiClusterBatch api;
            if(!build(api))return false;
        }
    }else{
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> inputs(parts.size());
        std::vector<D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC> operations(parts.size());
        std::vector<UINT64> offsets;
        UINT64 resultBytes=0,scratchBytes=0;
        for(size_t i=0;i<parts.size();++i){
            const auto& part=parts[i];
            auto& mesh=inputs[i];
            mesh.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            mesh.Flags=surfaceData[part.material].extra.y==0u?
                D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE:D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            mesh.Triangles.VertexBuffer={vertexAddress,sizeof(Vertex)};
            mesh.Triangles.VertexCount=uint32_t(cut.vertices.size());
            mesh.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
            mesh.Triangles.IndexBuffer=indexAddress+UINT64(part.firstIndex)*sizeof(uint32_t);
            mesh.Triangles.IndexCount=part.indexCount;
            mesh.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
            auto& input=operations[i].Inputs;
            input.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            input.NumDescs=1;
            input.pGeometryDescs=&mesh;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            device->GetRaytracingAccelerationStructurePrebuildInfo(&input,&info);
            if(!info.ResultDataMaxSizeInBytes)return fail("taille_blas_standard");
            offsets.push_back(resultBytes);
            resultBytes+=Align(info.ResultDataMaxSizeInBytes,UINT64(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
            scratchBytes=std::max(scratchBytes,info.ScratchDataSizeInBytes);
        }
        if(resultBytes+scratchBytes+pack.bytes.size()>ForestBuildBudget)return fail("budget_blas_standard");
        const auto result=make(resultBytes,D3D12_HEAP_TYPE_DEFAULT,AsState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto scratch=make(scratchBytes,D3D12_HEAP_TYPE_DEFAULT,UavState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!result||!scratch)return fail("allocation_blas_standard");
        for(auto& offset:offsets)offset+=result->GetGPUVirtualAddress();
        addresses=upload(offsets.data(),offsets.size()*sizeof(UINT64));
        if(!addresses)return fail("arguments_blas_standard");
        if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1u);
        for(size_t i=0;i<operations.size();++i){
            auto& op=operations[i];
            op.DestAccelerationStructureData=offsets[i];
            op.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&op,0,nullptr);
            if(i+1u<operations.size())FinishUav(list,scratch.Get());
        }
        FinishUav(list,result.Get());
        output.blasCapacity=resultBytes;
        output.resources.push_back(result);
        output.staging.push_back(scratch);
    }
    if(forestBuildQueries){
        list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2u);
        list->ResolveQueryData(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0u,3u,
            output.buildReadback.Get(),addressBytes);
    }
    if(clustered){
        Transition(list,addresses.Get(),UavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(output.buildReadback.Get(),0,addresses.Get(),0,addressBytes);
        Transition(list,addresses.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    output.backend=forestBuildBackend;
    output.resources.push_back(addresses);
    for(uint32_t i=0;i<layouts.size();++i){
        const auto& view=layouts[i];
        output.views[i]=Srv(geometry.Get(),view.count,view.stride,{},view.offset/view.stride);
    }
    output.views[ForestGeneration::Addresses]=Srv(addresses.Get(),uint32_t(parts.size()),sizeof(UINT64));
    output.views[ForestGeneration::Remap]=Srv(geometry.Get(),remap.count,remap.stride,{},remap.offset/remap.stride);
    output.pages=uint32_t(cut.pages);
    output.clusters=uint32_t(cut.clusters.size());
    output.triangles=uint32_t(triangles.size());
    output.bytes=uint32_t(pack.bytes.size());
    output.commands.Submit(direct);
    output.fence=direct.Signal();
    return true;
}
}
