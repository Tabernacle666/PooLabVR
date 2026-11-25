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
uint32_t ForestRouteHash(const Mg2InstanceRoute& key){
    uint32_t h=key[0]*0x9e3779b1u^key[1]*0x85ebca6bu^key[2]*0xc2b2ae35u;
    h^=h>>16u;h*=0x7feb352du;h^=h>>15u;return h;
}
// Exact identity comparisons in an immutable open-addressed table.
// Header contains the mask and maximum probe count for the shader.
std::vector<Mg2InstanceRoute> PackForestRoutes(const Mg2LodCut& cut){
    if(cut.routes.empty()||cut.routes.size()>(1u<<21))return {};
    size_t capacity=2;
    while(capacity<cut.routes.size()*2u)capacity*=2u;
    std::vector<Mg2InstanceRoute> table(capacity+1u,{0u,0u,Invalid,Invalid});
    uint32_t maxProbes=0;
    const uint32_t mask=uint32_t(capacity-1u);
    for(auto key:cut.routes){
        if(key[2]==Invalid||key[3]>=cut.variants)return {};
        uint32_t index=ForestRouteHash(key)&mask,probes=1;
        while(table[index+1u][2]!=Invalid){
            const auto& existing=table[index+1u];
            if(existing[0]==key[0]&&existing[1]==key[1]&&existing[2]==key[2])return {};
            index=(index+1u)&mask;++probes;
        }
        maxProbes=std::max(maxProbes,probes);
        key[3]|=PagedMeshBit;
        table[index+1u]=key;
    }
    table[0]={mask,maxProbes,0u,0u};
    return table;
}
constexpr auto ShaderRead=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto AsState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr auto UavState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

struct ForestPartRange {uint32_t first,count,firstIndex,indexCount,material;};
struct ClusterLimits {
    uint32_t clusters,parts,totalTriangles,totalVertices;
    uint32_t maxTriangleCount{},maxVertexCount{},maxPrimitiveBase{},maxClustersPerBlas{},totalClasCount{};
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
        blas.clasDesc={limits.totalClasCount,limits.maxClustersPerBlas};
        for(size_t i=limits.clusters?0u:1u;i<inputs.size();++i){
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
        assembly.MaxTotalClasCount=limits.totalClasCount;
        assembly.MaxClasCountPerArg=limits.maxClustersPerBlas;
        operations[0].Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        operations[0].Inputs.pClusterTrianglesDesc=&triangles;
        operations[1].Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        operations[1].Inputs.pClasDesc=&assembly;
        for(size_t i=limits.clusters?0u:1u;i<operations.size();++i){
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

uint32_t Graphics::Impl::ForestGeneration::FindRoute(const Mg2InstanceRoute& key)const{
    if(cpuRoutes.empty())return Invalid;
    const auto& header=cpuRoutes.front();
    uint32_t index=ForestRouteHash(key)&header[0];
    for(uint32_t probe=0;probe<header[1];++probe){
        const auto& route=cpuRoutes[index+1u];
        if(route[2]==Invalid)return Invalid;
        if(route[0]==key[0]&&route[1]==key[1]&&route[2]==key[2])
            return route[3]&~PagedMeshBit;
        index=(index+1u)&header[0];
    }
    return Invalid;
}
UINT64 Graphics::Impl::ForestGeneration::RayAddress(const Mg2InstanceRoute& key)const{
    const uint32_t variant=FindRoute(key);
    if(variant==Invalid)return 0;
    const auto& root=geometryOwner?*geometryOwner:*this;
    if(variant>=root.blasAddresses.size())Fail("MG2 published route without BLAS address");
    return root.blasAddresses[variant];
}

struct Graphics::Impl::ForestPreparedGeneration {
    const char* failure{};
    std::shared_ptr<ForestGeneration> compatible;
    std::vector<Mg2InstanceRoute> routes;
    PackedForest pack;
    std::array<PackedView,ForestGeneration::Addresses> layouts{};
    PackedView remap{},bounds{},localIndices{};
    std::vector<ForestPartRange> parts;
    uint32_t triangleCount{};
    std::vector<Mg2LodCluster> newClusters;
    std::vector<UINT64> initial;
    std::vector<uint32_t> missing;
    std::map<std::pair<uint32_t,uint32_t>,ForestClasEntry> clasCache;
    std::vector<std::pair<uint32_t,uint32_t>> newClasKeys;
    ClusterLimits limits{};
    UINT64 retainedBytes{};
};

std::shared_ptr<Graphics::Impl::ForestPreparedGeneration>
Graphics::Impl::PackForestCut(const ForestSelectionWork& work){
    const auto& cut=work.cut;
    auto prepared=std::make_shared<ForestPreparedGeneration>();
    if(cut.routes.empty())return prepared;
    constexpr uint32_t RasterVertices=64, RasterTriangles=124, RasterBatch=16;
    constexpr uint32_t DispatchPage=32768, MaxDispatchGroups=1u<<22;
    prepared->routes=PackForestRoutes(cut);
    const auto& routes=prepared->routes;
    if(routes.empty()){prepared->failure="routes_instances_invalides";return prepared;}
    // All references in this request were acquired on the render thread.
    // Published geometry and its CLAS cache are immutable.
    if(work.resident&&cut.selection==work.resident->selection&&cut.variants==work.resident->variants)
        prepared->compatible=work.resident;
    else for(auto it=work.generations.rbegin();it!=work.generations.rend();++it)
        if((*it)->published&&(*it)->backend==work.backend&&
           (*it)->variants==cut.variants&&(*it)->selection==cut.selection){
            prepared->compatible=*it;break;
        }
    if(cut.clusters.empty()||prepared->compatible)return prepared;
    std::vector<uint32_t> globalIndices,rasterRemap;
    std::vector<Meshlet> rasterMeshlets;
    std::vector<XMFLOAT4> rasterBounds;
    std::vector<std::array<uint32_t,3>> triangles;
    std::vector<MeshRange> level;
    auto& parts=prepared->parts;
    const auto invalidCut=[&]{
        prepared->failure="plage_meshlet_raster";
        return prepared;
    };
    for(uint32_t i=0;i<cut.clusters.size();){
        const auto& first=cut.clusters[i];
        if(first.mesh>=work.meshBounds.size()||first.part!=level.size())return invalidCut();
        const uint32_t rasterFirst=uint32_t(rasterMeshlets.size());
        auto j=i;
        for(;j<cut.clusters.size()&&cut.clusters[j].part==first.part;++j){
            const auto& cluster=cut.clusters[j];
            if(cluster.mesh!=first.mesh||cluster.material!=first.material)return invalidCut();
            if(!cluster.vertexCount||cluster.vertexCount>128u||!cluster.indexCount||
               cluster.indexCount%3u||uint64_t(cluster.firstVertex)+cluster.vertexCount>cut.vertices.size()||
               cluster.firstIndex!=globalIndices.size()||
               uint64_t(cluster.firstIndex)+cluster.indexCount>cut.indices.size())return invalidCut();
            std::array<uint32_t,128> localToRaster;
            Meshlet raster{};
            XMVECTOR lower,upper;
            const auto finishMeshlet=[&]{
                rasterMeshlets.push_back(raster);
                const auto extent=XMVectorScale(XMVectorSubtract(upper,lower),.5f);
                const auto center=XMVectorAdd(lower,extent);
                XMFLOAT4 sphere;
                XMStoreFloat4(&sphere,XMVectorSetW(center,XMVectorGetX(XMVector3Length(extent))));
                rasterBounds.push_back(sphere);
            };
            const auto startMeshlet=[&]{
                localToRaster.fill(Invalid);
                lower=XMVectorReplicate(std::numeric_limits<float>::max());
                upper=XMVectorNegate(lower);
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
                    finishMeshlet();
                    startMeshlet();
                }
                std::array<uint32_t,3> triangle{};
                for(uint32_t v=0;v<3;++v){
                    const uint32_t vertex=cluster.firstVertex+local[v];
                    auto& mapped=localToRaster[local[v]];
                    if(mapped==Invalid){
                        mapped=raster.vertexCount++;
                        rasterRemap.push_back(vertex);
                        const auto position=XMLoadFloat3(&cut.vertices[vertex].position);
                        lower=XMVectorMin(lower,position);
                        upper=XMVectorMax(upper,position);
                    }
                    triangle[v]=mapped;
                    globalIndices.push_back(vertex);
                }
                triangles.push_back(triangle);
                ++raster.triangleCount;
            }
            finishMeshlet();
        }
        const uint32_t rasterCount=uint32_t(rasterMeshlets.size())-rasterFirst;
        const uint32_t dispatchGroups=rasterCount<=65535u?rasterCount:
            ((rasterCount+DispatchPage-1u)/DispatchPage)*DispatchPage;
        if(uint64_t(dispatchGroups)*RasterBatch>=MaxDispatchGroups)return invalidCut();
        const auto& last=cut.clusters[j-1u];
        const auto vertexCount=last.firstVertex+last.vertexCount-first.firstVertex;
        const auto indexCount=last.firstIndex+last.indexCount-first.firstIndex;
        parts.push_back({i,j-i,first.firstIndex,indexCount,first.material});
        level.push_back({rasterFirst,rasterCount,first.firstVertex,vertexCount,first.firstIndex,indexCount,
                         work.meshBounds[first.mesh]});
        i=j;
    }
    if(level.empty()||level.size()!=cut.variants)return invalidCut();
    auto& pack=prepared->pack;
    prepared->layouts={
        pack.Add(routes),pack.Add(level),pack.Add(rasterMeshlets),pack.Add(cut.vertices),
        pack.Add(triangles),pack.Add(globalIndices)};
    prepared->remap=pack.Add(rasterRemap);
    prepared->bounds=pack.Add(rasterBounds);
    prepared->triangleCount=uint32_t(triangles.size());
    const bool clustered=work.backend!=Mg2RayBackend::Standard;
    prepared->localIndices=clustered?pack.Add(cut.indices):PackedView{};

    if(clustered){
        using Key=std::pair<uint32_t,uint32_t>;
        // Source IDs survive changing variant ordinals. GeometryIndex
        // is a primitive offset in this particular variant, so it MUST
        // participate in the key, not just the source cluster identity.
        std::vector<Key> keys;
        keys.reserve(cut.clusters.size());
        size_t cursor=0;
        for(const auto& part:parts){
            if(cursor>=cut.selection.size()||cut.selection[cursor++]!=part.count||
               cursor+part.count>cut.selection.size()){prepared->failure="identite_clas";return prepared;}
            for(uint32_t i=part.first;i<part.first+part.count;++i)
                keys.emplace_back(cut.selection[cursor++],(cut.clusters[i].firstIndex-part.firstIndex)/3u);
        }
        if(cursor!=cut.selection.size()||keys.size()!=cut.clusters.size()){prepared->failure="identite_clas";return prepared;}
        // The cache is scoped to this immutable hierarchy and backend.
        // Retained allocations already occupy memory in the live DXGI
        // CurrentUsage. Do not discard valid CLAS solely due to a byte cap.
        std::map<Key,ForestClasEntry> reusable;
        std::unordered_map<ID3D12Resource*,UINT64> retained;
        auto& retainedBytes=prepared->retainedBytes;
        for(auto it=work.generations.rbegin();it!=work.generations.rend();++it){
            const auto& generation=**it;
            if(!generation.published||generation.backend!=work.backend||generation.geometryOwner)continue;
            for(const auto& key:keys){
                if(reusable.contains(key))continue;
                const auto found=generation.clasCache.find(key);
                if(found==generation.clasCache.end()||!found->second.address||!found->second.storage)continue;
                auto* storage=found->second.storage.Get();
                if(!retained.contains(storage)){
                    const auto bytes=storage->GetDesc().Width;
                    retained.emplace(storage,bytes);
                    retainedBytes+=bytes;
                }
                reusable.emplace(key,found->second);
            }
        }
        auto& newClusters=prepared->newClusters;
        auto& initial=prepared->initial;initial.resize(keys.size(),0);
        auto& missing=prepared->missing;missing.resize(keys.size(),Invalid);
        std::map<Key,uint32_t> newSlots;
        auto& limits=prepared->limits;
        limits.parts=uint32_t(parts.size());
        limits.totalClasCount=uint32_t(keys.size());
        for(const auto& part:parts){
            limits.maxClustersPerBlas=std::max(limits.maxClustersPerBlas,part.count);
            for(uint32_t i=part.first;i<part.first+part.count;++i){
                const auto& key=keys[i];
                const auto found=reusable.find(key);
                if(found!=reusable.end()){
                    initial[i]=found->second.address;
                    prepared->clasCache.emplace(key,found->second);
                    continue;
                }
                auto [slot,inserted]=newSlots.try_emplace(key,uint32_t(newClusters.size()));
                missing[i]=slot->second;
                if(!inserted)continue;
                auto cluster=cut.clusters[i];
                cluster.id=key.first;
                const uint32_t primitiveBase=key.second;
                limits.maxPrimitiveBase=std::max(limits.maxPrimitiveBase,primitiveBase);
                limits.maxTriangleCount=std::max(limits.maxTriangleCount,cluster.indexCount/3u);
                limits.maxVertexCount=std::max(limits.maxVertexCount,cluster.vertexCount);
                limits.totalTriangles+=cluster.indexCount/3u;
                limits.totalVertices+=cluster.vertexCount;
                newClusters.push_back(cluster);
                prepared->newClasKeys.push_back(key);
            }
        }
        limits.clusters=uint32_t(newClusters.size());

    }
    return prepared;
}

void Graphics::EnableMg2Diagnostic(){impl_->mg2Diagnostic=true;}
void Graphics::SetDiagnosticMg2Budget(uint32_t triangles){
    if(!triangles||impl_->forestSelection.requested||impl_->forestBuild.requested||impl_->forestPending)
        Fail("MG2 diagnostic budget request invalid or busy");
    impl_->forestBudgetTest=true;
    impl_->diagnosticFreezeMg2=true;
    impl_->forestStatus.requestedTriangleBudget=triangles;
    impl_->forestStatus.budgetResolution=0u;
    impl_->forestStatus.lastFailure="aucune";
    impl_->forestStatus.buildCode=0;
    impl_->forestRequestFailed=false;
    impl_->forestNextSelection=0.;
}
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
    QueueInit(forestBuildQueue,D3D12_COMMAND_LIST_TYPE_DIRECT);
    if(SUCCEEDED(forestBuildQueue.queue->GetTimestampFrequency(&forestTimestampHz))&&forestTimestampHz){
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
    if(forestSelection.completion.valid())forestSelection.completion.wait();
    forestSelection.completion={};
    // Stop the producer before releasing the hierarchy, NVAPI configuration,
    // query heap or any allocations. Waiting is allowed only during teardown.
    if(forestBuild.completion.valid())forestBuild.completion.wait();
    if(forestBuildQueue.queue)WaitStartup(forestBuildQueue);
    forestBuild={};
    forestBuildQueue={};
    forestSelection.requested=false;
    forestSelection.prepared.reset();
    forestSelection.generations.clear();
    forestSelection.resident.reset();
    forestSelection.meshBounds.clear();
    forestSelection.cut={};
    forestSelection.inputs.clear();
    forestSelection.pages=nullptr;
    // Scene reset is already a drained render-queue boundary. Join the
    // retirement worker before releasing scene/device ownership.
    if(forestRetire.completion.valid())forestRetire.completion.wait();
    forestRetire={};
    forestRetired.clear();
    for(auto& frame:frames){
        frame.forestRaster.reset();
        frame.forestTlas.reset();
        frame.forestPreviousTlas.reset();
        frame.forestRayReset=false;
        frame.forestDrawReadback.Reset();
        frame.forestDrawFrame=0;
        frame.forestRayProofReadback.Reset();
        frame.forestRayProofPending=false;
    }
    for(const auto& generation:forestGenerations)
        for(uint32_t i=0;i<generation->views.size();++i)
            if((!generation->geometryOwner||i==ForestGeneration::Routes)&&generation->views[i].Valid())
                ReleaseDescriptor(generation->views[i]);
    forestGenerations.clear();
    forestPending.reset();
    forestResident.reset();
    forestHierarchy.reset();
    forestParts.clear();
    forestNextSelection=0.;
    forestShadowUsesMg2=false;
    forestRadius=0.f;
    if(forestStatus.configured==Mg2RayBackend::Nvapi)NvapiClusterPipelines(device.Get(),false);
    forestStatus={};
    forestBudgetTest=false;
    forestBuildQueries.Reset();
    forestTimestampHz=0;
    forestBuildBackend=Mg2RayBackend::Standard;
    forestRequestFailed=false;
}

bool Graphics::Impl::ValidateForestGeneration(ForestGeneration& pending,ForestBuildWork& work){
    // Only the dedicated producer thread may wait here. The render thread
    // consumes a completed future, never this event or an unfinished fence.
    const auto waitStart=QpcCounter();
    auto completed=forestBuildQueue.fence->GetCompletedValue();
    if(completed!=UINT64_MAX&&completed<pending.fence){
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event)Fail("MG2 producer fence event");
        const auto hr=forestBuildQueue.fence->SetEventOnCompletion(pending.fence,event);
        if(FAILED(hr)){CloseHandle(event);Hr(hr,"MG2 producer fence completion");}
        const auto waited=WaitForSingleObject(event,INFINITE);
        CloseHandle(event);
        if(waited!=WAIT_OBJECT_0)Fail("MG2 producer fence wait");
        completed=forestBuildQueue.fence->GetCompletedValue();
    }
    if(completed==UINT64_MAX||completed<pending.fence)Fail("MG2 producer GPU completion invalid");
    work.waitMs=float(1000.*QpcSeconds(QpcCounter()-waitStart));
    const auto validateStart=QpcCounter();
    bool valid=true;
    work.builtClas=uint32_t(pending.newClasKeys.size());
    float clasMs=-1.f,blasMs=-1.f;
    if(pending.buildReadback){
        const D3D12_RANGE read{0,SIZE_T(pending.addressCount+(forestBuildQueries?3u:0u))*sizeof(UINT64)},empty{};
        void* mapped=nullptr;
        const bool readable=SUCCEEDED(pending.buildReadback->Map(0,&read,&mapped))&&mapped;
        valid=readable||!pending.addressCount;
        if(readable){
            const auto* values=static_cast<const UINT64*>(mapped);
            if(pending.addressCount)pending.blasAddresses.assign(values,values+pending.addressCount);
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
    // Addresses are only made reusable after the producing fence completes.
    if(pending.clasReadback){
        const D3D12_RANGE read{0,pending.newClasKeys.size()*sizeof(UINT64)},empty{};
        void* mapped{};
        if(SUCCEEDED(pending.clasReadback->Map(0,&read,&mapped))&&mapped){
            const auto* values=static_cast<const UINT64*>(mapped);
            for(size_t i=0;i<pending.newClasKeys.size();++i){
                const auto found=pending.clasCache.find(pending.newClasKeys[i]);
                if(found==pending.clasCache.end()||!found->second.storage){valid=false;continue;}
                auto& entry=found->second;
                const auto start=entry.storage->GetGPUVirtualAddress();
                const auto address=values[i];
                valid&=address>=start&&address-start<entry.storage->GetDesc().Width&&address%128u==0;
                entry.address=address;
            }
            pending.clasReadback->Unmap(0,&empty);
        }else valid=false;
        pending.clasReadback.Reset();
        pending.newClasKeys.clear();
    }

    const auto& root=pending.geometryOwner?*pending.geometryOwner:pending;
    valid&=root.blasAddresses.size()==pending.variants;
    work.result.clasMs=clasMs;
    work.result.blasMs=blasMs;
    work.validateMs=float(1000.*QpcSeconds(QpcCounter()-validateStart));
    const auto cleanupStart=QpcCounter();
    // The fence covers all CLAS/BLAS commands and their upload buffers.
    pending.staging.clear();
    pending.commands={};
    work.cleanupMs=float(1000.*QpcSeconds(QpcCounter()-cleanupStart));
    return valid;
}

void Graphics::Impl::CollectForestRetirements(){
    // Descriptor Release() conservatively includes the recording frame in
    // its retirement fences. Capture those submitted serials on the NEXT
    // render invocation, after EndRecording has closed that epoch.
    if(forestRetire.completion.valid()&&
       forestRetire.completion.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        forestStatus.retireWorkerMs=forestRetire.completion.get();
        forestStatus.retiredGenerations+=forestRetire.inFlight;
        forestRetire.inFlight=0;
    }
    const auto submitted=SubmittedFences();
    for(auto& retired:forestRetired)if(!retired.captured){
        retired.fences=submitted;
        retired.captured=true;
    }
    if(!forestRetire.completion.valid()){
        const auto completed=CompletedFences();
        bool ready=false;
        for(const auto& retired:forestRetired)
            ready|=GpuFencesComplete(retired.fences,completed);
        if(ready){
            // The generation still owns all descriptor payload resources
            // while the render-owned allocator reclaims its slots.
            descriptorAllocator.Collect(completed);
            for(auto it=forestRetired.begin();it!=forestRetired.end();){
                if(!GpuFencesComplete(it->fences,completed)){++it;continue;}
                forestRetire.batch.push_back(std::move(it->generation));
                it=forestRetired.erase(it);
            }
        }
        if(!forestRetire.batch.empty()){
            forestRetire.inFlight=uint32_t(forestRetire.batch.size());
            try{
                forestRetire.completion=std::async(std::launch::async,[work=&forestRetire]{
                    const auto begin=QpcCounter();
                    work->batch.clear();
                    return float(1000.*QpcSeconds(QpcCounter()-begin));
                });
            }catch(...){
                // Keep ownership and retry on a later frame, never destroy
                // the batch synchronously when the worker cannot be started.
            }
        }
    }
    forestStatus.retireBacklog=uint32_t(forestRetired.size())+forestRetire.inFlight;
}

std::vector<Mg2LodInstance> Graphics::Impl::MakeForestInstances(const ForestSelectionWork& work){
    std::vector<Mg2LodInstance> instances;
    instances.reserve(work.inputs.size());
    for(const auto& input:work.inputs){
        const auto& root=input.root;
        const auto& scale=input.scale;
        const auto& rotation=input.rotation;
        const float lo=std::min({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        const float hi=std::max({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        if(lo<1.e-6f||input.mesh>=work.meshBounds.size())continue;
        const auto& bounds=work.meshBounds[input.mesh];
        const float radius=(Length({bounds.x,bounds.y,bounds.z})+bounds.w)*hi;
        bool visible=false;
        for(uint32_t eye=0;eye<work.eyeCount&&!visible;++eye)
            visible=SphereIntersectsView(XMLoadFloat4x4(&work.vp[eye]),root,radius,true);
        Mg2LodInstance candidate{};
        candidate.key=input.key;
        candidate.mesh=input.mesh;
        candidate.eyeCount=work.eyeCount;
        candidate.visible=visible;
        const auto delta=Sub(work.eyes[0],root);
        candidate.distanceWorld=std::sqrt(Dot(delta,delta));
        const auto inverse=XMQuaternionConjugate(
            XMVectorSet(rotation.x,rotation.y,rotation.z,rotation.w));
        for(uint32_t eye=0;eye<work.eyeCount;++eye){
            const auto displacement=Sub(work.eyes[eye],root);
            XMFLOAT3 local{};
            XMStoreFloat3(&local,XMVector3Rotate(XMLoadFloat3(&displacement),inverse));
            candidate.eyes[eye]={{local.x/scale.x,local.y/scale.y,local.z/scale.z},
                work.focal[eye]*hi/lo,work.nearPlane/lo};
        }
        instances.push_back(candidate);
    }
    return instances;
}

void Graphics::Impl::MatchForestShadowVariants(ForestGeneration& next,const ForestGeneration* previous){
    next.previousShadowVariants.assign(next.variants,Invalid);
    if(!previous)return;
    // Selection contains lexicographically sorted exact cluster-ID sequences.
    // Match actual cuts, not their generation-dependent BLAS addresses.
    const auto ranges=[](const ForestGeneration& generation){
        std::vector<std::span<const uint32_t>> out;
        const std::span<const uint32_t> selection(generation.selection);
        for(size_t i=0;i<selection.size();){
            const size_t count=selection[i++];
            if(count>selection.size()-i)Fail("MG2 shadow variant selection");
            out.push_back(selection.subspan(i,count));i+=count;
        }
        if(out.size()!=generation.variants)Fail("MG2 shadow variant count");
        return out;
    };
    const auto before=ranges(*previous),after=ranges(next);
    size_t i=0,j=0;
    while(i<before.size()&&j<after.size()){
        if(std::ranges::equal(before[i],after[j])){
            next.previousShadowVariants[j]=uint32_t(i);++i;++j;
        }else if(std::lexicographical_compare(before[i].begin(),before[i].end(),after[j].begin(),after[j].end()))++i;
        else ++j;
    }
}

void Graphics::Impl::InvalidateForestShadows(Frame& f,const GpuSceneCache* tiles,
    const ForestGeneration* before,const ForestGeneration* after){
    if(!before||!after||before==after)return;
    // PrepareLights already ran this frame. Update both the current dirty
    // mask and the persistent cache, preserving all unaffected active slots.
    uint32_t pending=0;
    for(uint32_t slot=0;slot<ShadowSlots;++slot){
        if(!shadowCache[slot].valid)continue;
        if(!f.hasShadowAtlasLights||f.shadowIDs[slot]==Invalid){
            shadowCache[slot].valid=false; // no current spatial light record
            continue;
        }
        if(f.shadowDirty&(1u<<slot)){
            // Even if this frame is abandoned, do not revive its old cache.
            shadowCache[slot].valid=false;
        }else pending|=1u<<slot;
    }
    if(!pending)return;
    const auto affected=[&](double x,double z,double reach){
        uint32_t mask=0;
        for(uint32_t slot=0;slot<ShadowSlots;++slot)if(pending&(1u<<slot)){
            const auto& light=f.shadowLights[slot];
            const double dx=x-light.position.x,dz=z-light.position.z;
            const double radius=light.radius+reach;
            if(dx*dx+dz*dz<=radius*radius)mask|=1u<<slot;
        }
        return mask;
    };
    const auto changed=[&](const Mg2InstanceRoute& key){
        const auto old=before->FindRoute(key),next=after->FindRoute(key);
        if(old==Invalid||next==Invalid)return old!=next;
        return next>=after->previousShadowVariants.size()||after->previousShadowVariants[next]!=old;
    };
    const auto consider=[&](uint32_t mesh,uint32_t flags,const Mg2InstanceRoute& key,
                            double x,double z,double sx,double sy,double sz){
        if(!pending||(flags&(DynamicObject|DeformedObject|NoShadow|AlphaBlendObject))||
           mesh>=meshes.ranges.size())return;
        if(!std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh))return;
        const auto& bounds=meshes.ranges[mesh].bounds;
        const double reach=(std::sqrt(double(bounds.x)*bounds.x+double(bounds.y)*bounds.y+
             double(bounds.z)*bounds.z)+bounds.w)*std::max({std::abs(sx),std::abs(sy),std::abs(sz)})*2.5+2.;
        const uint32_t slots=affected(x,z,reach);
        if(!slots||!changed(key))return;
        for(uint32_t slot=0;slot<ShadowSlots;++slot)if(slots&(1u<<slot)){
            shadowCache[slot].valid=false;++forestStatus.traceForestShadowDirty;
        }
        f.shadowDirty|=slots;
        pending&=~slots;
    };
    const auto statics=sceneCache.StaticInstances();
    for(uint32_t i=0;i<statics.size()&&pending;++i){
        const auto& o=statics[i];
        consider(o.mesh,o.flags,{i,o.id,o.mesh,0u},o.positionRadius.x,o.positionRadius.z,o.scale.x,o.scale.y,o.scale.z);
    }
    for(uint32_t i=0;i<f.snapshotCount&&pending;++i){
        const auto& o=f.objects[i];
        consider(o.mesh,o.flags,{uint32_t(statics.size())+i,o.id,o.mesh,0u},
            o.positionRadius.x,o.positionRadius.z,o.scale.x,o.scale.y,o.scale.z);
    }
    if(tiles)for(const auto& [key,tile]:tiles->ResidentTiles()){
        if(!pending)break;
        // Conservative existing shadow-cell bounds avoid scanning unrelated tiles.
        uint32_t cells=0;
        for(uint32_t c=0;c<GpuResidentTile::Cells;++c){
            const auto& v=tile.shadowVolumes[c];
            if(tile.shadowCasterCounts[c]&&affected(v.x,v.z,v.radius))++cells;
        }
        if(!cells)continue;
        for(const auto& o:tile.objects){
            if(!pending)break;
            if(!o.enabled||!o.identity)continue;
            consider(o.mesh,o.flags,{uint32_t(o.identity),uint32_t(o.identity>>32u),o.mesh|Mg2ResidentKeyBit,0u},
                o.position.x,o.position.z,o.scale.x,o.scale.y,o.scale.z);
        }
    }
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
    bool publishedThisFrame=false;
    // A completed producer has finished its GPU fence, validation and cleanup.
    // The render thread transfers only the ready generation's ownership.
    if(forestBuild.requested&&
       forestBuild.completion.wait_for(std::chrono::seconds(0))==std::future_status::ready){
        forestBuild.requested=false;
        try{forestBuild.completion.get();}
        catch(...){
            forestBuild.succeeded=false;
            forestBuild.result.lastFailure="exception_producteur_mg2";
            forestBuild.result.buildCode=0;
        }
        forestStatus.buildWorkerMs=forestBuild.buildMs;
        forestStatus.allocateWorkerMs=forestBuild.allocationMs;
        forestStatus.uploadWorkerMs=forestBuild.uploadMs;
        forestStatus.submitWorkerMs=forestBuild.submitMs;
        forestStatus.waitWorkerMs=forestBuild.waitMs;
        forestStatus.validateWorkerMs=forestBuild.validateMs;
        forestStatus.cleanupWorkerMs=forestBuild.cleanupMs;
        forestStatus.buildAvailableBytes=forestBuild.result.buildAvailableBytes;
        forestStatus.buildRequiredBytes=forestBuild.result.buildRequiredBytes;
        if(forestBuild.succeeded){
            forestRequestFailed=false;
            forestStatus.lastFailure="aucune";
            forestStatus.buildCode=0;
            forestGenerations.push_back(forestBuild.candidate);
            forestPending=std::move(forestBuild.candidate);
        }else{
            forestStatus.lastFailure=forestBuild.result.lastFailure;
            forestStatus.buildCode=forestBuild.result.buildCode;
            forestRequestFailed=true;
            if(forestBudgetTest)forestStatus.budgetResolution=2u;
            else if(forestBuild.validationFailed)forestBuildBackend=Mg2RayBackend::Standard;
            if(!forestBuild.validationFailed)forestNextSelection=xr.input.time+3.;
            forestBuild.candidate.reset();
        }
    }
    if(forestPending&&forestBuildQueue.fence->GetCompletedValue()>=forestPending->fence){
        auto& pending=*forestPending;
        const auto publishStart=QpcCounter();
        {
            // The producer fence has already completed: explicit GPU queue
            // dependencies transfer visibility without waiting for production.
            direct.Wait(forestBuildQueue,pending.fence);
            compute.Wait(forestBuildQueue,pending.fence);
            for(uint32_t i=0;i<pending.viewRequests.size();++i){
                const auto& view=pending.viewRequests[i];
                if(view.resource)pending.views[i]=Srv(view.resource,view.count,view.stride,{},view.first);
            }
            pending.viewRequests={};
            pending.published=true;
            forestStatus.lastPublished=pending.backend;
            ++forestStatus.publications;
            forestStatus.variants=pending.variants;
            forestStatus.routedInstances=uint32_t(pending.instanceRoutes.size());
            if(pending.geometryOwner){
                ++forestStatus.routeOnlyPublications;
                const auto* previousGeometry=forestResident?
                    (forestResident->geometryOwner?forestResident->geometryOwner.get():forestResident.get()):nullptr;
                if(pending.geometryOwner.get()!=previousGeometry)++forestStatus.generationReusePublications;
            }else if(pending.backend!=Mg2RayBackend::Standard){
                const uint32_t unique=uint32_t(pending.clasCache.size());
                forestStatus.clasBuilt+=forestBuild.builtClas;
                forestStatus.clasReused+=unique-forestBuild.builtClas;
                forestStatus.clasDuplicate+=pending.clusters-unique;
            }
            forestStatus.clasBytes=pending.clasCapacity;
            forestStatus.blasBytes=pending.blasCapacity;
            forestStatus.clasMs=forestBuild.result.clasMs;
            forestStatus.blasMs=forestBuild.result.blasMs;
            if(nativeCampus&&tiles&&forestShadowUsesMg2)
                InvalidateForestShadows(f,tiles,forestResident.get(),forestPending.get());
            forestResident=std::move(forestPending);
            publishedThisFrame=true;
            forestStatus.generationAgeMs=float(1000.*QpcSeconds(QpcCounter()-forestSelection.requestedAt));
            if(forestBudgetTest)forestStatus.budgetResolution=1u;
        }
        forestStatus.traceForestPublishMs=float(1000.*QpcSeconds(QpcCounter()-publishStart));
    }
    // Retain the latest idle, validated geometry generation. In-flight
    // raster/TLAS owners still govern descriptor reclamation.
    const auto retireStart=QpcCounter();
    CollectForestRetirements();
    // Bound the number of waiting/worker-owned generations; when the GPU
    // is behind, keep the current generation rather than accumulate builds.
    constexpr uint32_t MaxRetireBacklog=3;
    ForestGeneration* warm=nullptr;
    for(auto it=forestGenerations.rbegin();it!=forestGenerations.rend();++it)
        if(it->use_count()==1&&(*it)->published&&!(*it)->geometryOwner&&
           !forestRequestFailed&&forestStatus.buildRequiredBytes!=0&&
           forestStatus.buildAvailableBytes>=forestStatus.buildRequiredBytes){
            warm=it->get();break;
        }
    for(auto it=forestGenerations.begin();it!=forestGenerations.end();){
        auto& generation=*it;
        if(generation.use_count()!=1||generation.get()==warm||
           forestStatus.retireBacklog>=MaxRetireBacklog){++it;continue;}
        for(uint32_t i=0;i<generation->views.size();++i)
            if((!generation->geometryOwner||i==ForestGeneration::Routes)&&generation->views[i].Valid())
                ReleaseDescriptor(generation->views[i]);
        // Move the final owner out of the render thread. It stays alive
        // until descriptor retirement fences have completed.
        forestRetired.push_back({std::move(generation),{},false});
        ++forestStatus.retireBacklog;
        it=forestGenerations.erase(it);
    }
    forestStatus.traceForestRetireMs=float(1000.*QpcSeconds(QpcCounter()-retireStart));
    const bool canPublish=nativeCampus&&tiles;
    if(canPublish)f.forestRaster=forestResident;
    const bool shadowUsesMg2=bool(f.forestRaster);
    if(shadowUsesMg2!=forestShadowUsesMg2){
        // A native/MG2 source transition has no compatible atlas history.
        for(uint32_t slot=0;slot<ShadowSlots;++slot){
            if(shadowCache[slot].valid&&!(f.shadowDirty&(1u<<slot)))
                ++forestStatus.traceForestShadowDirty;
            shadowCache[slot].valid=false;
            if(f.hasShadowAtlasLights&&f.shadowIDs[slot]!=Invalid)f.shadowDirty|=1u<<slot;
        }
        forestShadowUsesMg2=shadowUsesMg2;
    }
    // The diagnostic freezes only the geometry selection, not drawing.
    // Existing pages stay active with exactly the same triangles in A and B.
    // Allow the initial publication with DXR on, then stop all CLAS/BLAS
    // production. Never submit an acceleration build in the DXR-off phase.
    if(!diagnosticDxrEnabled||
       (forestBudgetTest?forestStatus.budgetResolution!=0u:
                         diagnosticFreezeMg2&&bool(forestResident)))return;
    if(forestSelection.requested){
        if(forestSelection.completion.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        forestSelection.requested=false;
        try{
            forestSelection.completion.get();
        }catch(...){
            forestSelection.prepared.reset();
            forestSelection.generations.clear();
            forestSelection.resident.reset();
            forestSelection.cut={};
            forestStatus.lastFailure="preparation_cpu_mg2";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
            if(forestBudgetTest)forestStatus.budgetResolution=2u;
            forestNextSelection=xr.input.time+3.;
            return;
        }
        forestStatus.selectWorkerMs=forestSelection.selectMs;
        forestStatus.packWorkerMs=forestSelection.packMs;
        forestStatus.candidateWorkerMs=forestSelection.candidateMs;
        auto prepared=std::move(forestSelection.prepared);
        auto cut=std::move(forestSelection.cut);
        forestSelection.generations.clear();
        forestSelection.resident.reset();
        forestStatus.selectionCandidates=cut.candidates;
        forestStatus.selectionEligible=cut.eligible;
        forestStatus.selectionRouted=cut.selectedRoutes;
        forestStatus.selectionCoarsened=cut.budgetCoarsened;
        forestStatus.selectionVisibleCoarsened=cut.visibleCoarsened;
        forestStatus.selectionCoverageTriangles=cut.coverageTriangles;
        forestStatus.selectionCoverageFailed=cut.coverageBudgetFailed;
        if(!canPublish){
            ++forestStatus.discardedSelections;
            return;
        }
        if(cut.coverageBudgetFailed){
            forestStatus.lastFailure="plafond_couverture_mg2";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
            if(forestBudgetTest)forestStatus.budgetResolution=2u;
            forestNextSelection=xr.input.time+3.;
            return;
        }
        if(cut.routes.empty()){
            if(forestResident&&!cut.selection.empty()&&cut.selection==forestResident->selection)
                ++forestStatus.unchangedSelections;
            if(forestBudgetTest){
                const bool unchanged=forestResident&&!cut.selection.empty()&&
                    cut.selection==forestResident->selection;
                forestStatus.budgetResolution=unchanged?1u:2u;
                if(!unchanged)forestStatus.lastFailure="aucune_variante_admise";
            }
            return;
        }
        if(!prepared){
            forestRequestFailed=true;
            forestStatus.lastFailure="preparation_cpu_mg2";
            if(forestBudgetTest)forestStatus.budgetResolution=2u;
            forestNextSelection=xr.input.time+3.;
            return;
        }
        // Only immutable snapshots cross the worker boundary. The DX12
        // device/adapter/query heap and producer queue outlive this task.
        forestBuild.result={};
        forestBuild.backend=forestBuildBackend;
        forestBuild.headroom=managedMemory.Budget().headroom;
        forestBuild.buildMs=forestBuild.allocationMs=forestBuild.uploadMs=forestBuild.submitMs=0.f;
        forestBuild.waitMs=forestBuild.validateMs=forestBuild.cleanupMs=0.f;
        forestBuild.builtClas=0;
        forestBuild.succeeded=false;
        forestBuild.validationFailed=false;
        forestBuild.opaqueMaterials.clear();
        if(forestBuild.backend==Mg2RayBackend::Standard){
            forestBuild.opaqueMaterials.reserve(surfaceData.size());
            for(const auto& material:surfaceData)
                forestBuild.opaqueMaterials.push_back(uint8_t(material.extra.y==0u));
        }
        forestBuild.candidate=std::make_shared<ForestGeneration>();
        try{
            forestBuild.completion=std::async(std::launch::async,
                [this,work=&forestBuild,previous=forestResident,cut=std::move(cut),prepared=std::move(prepared)]() mutable{
                    const auto begin=QpcCounter();
                    work->succeeded=UploadForestPages(cut,*prepared,*work->candidate,*work);
                    work->buildMs=float(1000.*QpcSeconds(QpcCounter()-begin));
                    if(work->succeeded){
                        work->candidate->selection=std::move(cut.selection);
                        work->candidate->instanceRoutes=std::move(cut.routes);
                        work->candidate->cpuRoutes=std::move(prepared->routes);
                    }
                    // Large packed CPU vectors must also die on this worker.
                    prepared.reset();
                    cut={};
                    if(work->succeeded&&!ValidateForestGeneration(*work->candidate,*work)){
                        work->validationFailed=true;
                        work->result.lastFailure="adresses_blas_invalides";
                        work->result.buildCode=0;
                        work->succeeded=false;
                    }
                    if(work->succeeded)
                        MatchForestShadowVariants(*work->candidate,previous.get());
                    // Failed candidates have never been consumed. If submitted,
                    // validation has already waited for their own GPU fence.
                    if(!work->succeeded)work->candidate.reset();
                });
            forestBuild.requested=true;
        }catch(...){
            forestBuild.candidate.reset();
            forestStatus.lastFailure="lancement_producteur_mg2";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
            if(forestBudgetTest)forestStatus.budgetResolution=2u;
            forestNextSelection=xr.input.time+3.;
        }
        return;
    }
    const auto liveGenerations=std::count_if(forestGenerations.begin(),forestGenerations.end(),
        [](const auto& generation){return generation.use_count()>1;});
    // Requests are latest-only: no backlog of camera snapshots can grow
    // behind an outstanding selection/build. Do not enumerate instances
    // on the same frame as publication.
    if(!canPublish||publishedThisFrame||forestBuild.requested||forestPending||
       liveGenerations>=3u||forestStatus.retireBacklog>=MaxRetireBacklog||
       xr.input.time<forestNextSelection)return;
    const auto requestStart=QpcCounter();
    forestNextSelection=xr.input.time+.25;

    std::array<float,MaxViews> focal{};
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        const auto& fov=xr.fov[eye];
        const float vertical=std::tan(fov.angleUp)-std::tan(fov.angleDown);
        const float horizontal=std::tan(fov.angleRight)-std::tan(fov.angleLeft);
        focal[eye]=std::max(horizontal>.01f?float(width)/horizontal:float(width)*.5f,
                            vertical>.01f?float(height)/vertical:float(height)*.5f);
    }
    // Capture only immutable transform/identity records on the render thread.
    // Candidate math (stereo frustum tests, transforms, distance and projected
    // errors) belongs to the worker. Keep the original selection criteria.
    auto& inputs=forestSelection.inputs;
    inputs.clear();
    const auto consider=[&](uint32_t mesh,uint32_t flags,Mg2InstanceRoute key,XMFLOAT3 root,
        XMFLOAT4 rotation,XMFLOAT3 scale){
        if((flags&(DeformedObject|AlphaBlendObject))!=0u||
           !std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh))return;
        inputs.push_back({key,mesh,root,rotation,scale});
    };
    if(tiles)for(const auto& [key,tile]:tiles->ResidentTiles())
        for(const auto& object:tile.objects){
            if(!object.enabled||!object.identity)continue;
            consider(object.mesh,object.flags,
                {uint32_t(object.identity),uint32_t(object.identity>>32u),object.mesh|Mg2ResidentKeyBit,0u},
                {object.position.x,object.position.y,object.position.z},
                {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
                {object.scale.x,object.scale.y,object.scale.z});
        }
    const auto statics=sceneCache.StaticInstances();
    for(uint32_t i=0;i<statics.size();++i){
        const auto& object=statics[i];
        consider(object.mesh,object.flags,{i,object.id,object.mesh,0u},
            {object.positionRadius.x,object.positionRadius.y,object.positionRadius.z},
            {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
            {object.scale.x,object.scale.y,object.scale.z});
    }
    // Snapshot and GPU-published objects have no immutable CPU slot
    // identity across frames; leave them on their native geometry.
    if(inputs.empty()){
        forestStatus.traceForestRequestMs=float(1000.*QpcSeconds(QpcCounter()-requestStart));
        return;
    }
    forestSelection.pages=&*forestHierarchy;
    forestSelection.eyeCount=xr.viewCount;
    forestSelection.nearPlane=xr.input.SceneNearZ();
    forestSelection.focal=focal;
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        forestSelection.vp[eye]=xr.vp[eye];
        forestSelection.eyes[eye]=xr.eyes[eye];
    }
    forestSelection.requestedAt=requestStart;
    forestSelection.triangleBudget=forestBudgetTest?forestStatus.requestedTriangleBudget:0u;
    forestSelection.backend=forestBuildBackend;
    forestSelection.resident=forestResident;
    forestSelection.generations.clear();
    for(const auto& generation:forestGenerations)
        if(generation->published)forestSelection.generations.push_back(generation);
    forestSelection.meshBounds.clear();
    forestSelection.meshBounds.reserve(meshes.ranges.size());
    for(const auto& range:meshes.ranges)forestSelection.meshBounds.push_back(range.bounds);
    // Dedicated asynchronous work: JobSystem::Submit may execute inline when
    // its queue is full. Never let that fallback run MG2 packing in VR.
    try{
        forestSelection.completion=std::async(std::launch::async,[work=&forestSelection]{
            auto start=QpcCounter();
            const auto instances=MakeForestInstances(*work);
            work->candidateMs=float(1000.*QpcSeconds(QpcCounter()-start));
            start=QpcCounter();
            // The resident snapshot owns these immutable vectors throughout
            // the worker. Avoid duplicating large route tables in the VR loop.
            const std::span<const uint32_t> previous=work->resident?
                std::span<const uint32_t>(work->resident->selection):std::span<const uint32_t>{};
            const std::span<const Mg2InstanceRoute> previousRoutes=work->resident?
                std::span<const Mg2InstanceRoute>(work->resident->instanceRoutes):std::span<const Mg2InstanceRoute>{};
            work->cut=work->pages->Select(instances,1.1f,previous,previousRoutes,work->triangleBudget);
            work->selectMs=float(1000.*QpcSeconds(QpcCounter()-start));
            start=QpcCounter();
            work->prepared=PackForestCut(*work);
            work->packMs=float(1000.*QpcSeconds(QpcCounter()-start));
        });
        forestSelection.requested=true;
    }catch(...){
        forestSelection.generations.clear();
        forestSelection.resident.reset();
        forestStatus.lastFailure="lancement_worker_mg2";
        forestStatus.buildCode=0;
        forestRequestFailed=true;
        if(forestBudgetTest)forestStatus.budgetResolution=2u;
        forestNextSelection=xr.input.time+3.;
    }
    forestStatus.traceForestRequestMs=float(1000.*QpcSeconds(QpcCounter()-requestStart));
}

bool Graphics::Impl::UploadForestPages(const Mg2LodCut& cut,ForestPreparedGeneration& prepared,ForestGeneration& output,ForestBuildWork& work){
    // Only actual local memory availability constrains new GPU allocations.
    // The headroom is a reserve, not a fixed construction allowance.
    work.result.buildRequiredBytes=work.result.buildAvailableBytes=0;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if(!adapter||FAILED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))){
        work.result.lastFailure="budget_vram_indisponible";
        work.result.buildCode=0;
        return false;
    }
    const UINT64 reserve=std::max<UINT64>(work.headroom,memory.Budget/20u);
    const UINT64 available=GpuSubtract(GpuSubtract(memory.Budget,reserve),memory.CurrentUsage);
    work.result.buildAvailableBytes=available;
    const auto allocation=[](UINT64 bytes){
        return bytes?Align(bytes,UINT64(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)):UINT64(0);
    };
    const auto fits=[&](UINT64 bytes){
        work.result.buildRequiredBytes=bytes;
        if(bytes<=available)return true;
        work.result.lastFailure="budget_vram_disponible";
        work.result.buildCode=0;
        return false;
    };
    if(prepared.failure){
        work.result.lastFailure=prepared.failure;
        work.result.buildCode=0;
        return false;
    }
    const auto make=[&](UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,
                        D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE){
        const auto begin=QpcCounter();
        ComPtr<ID3D12Resource> resource;
        const auto heap=Heap(type);
        const auto desc=Buffer(std::max(UINT64(256),bytes),flags);
        const auto result=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,
            nullptr,IID_PPV_ARGS(&resource));
        work.allocationMs+=float(1000.*QpcSeconds(QpcCounter()-begin));
        if(FAILED(result))return ComPtr<ID3D12Resource>{};
        return resource;
    };
    const auto upload=[&](const void* data,size_t bytes){
        auto source=make(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        const auto begin=QpcCounter();
        const D3D12_RANGE none{};
        void* memory{};
        if(!source||FAILED(source->Map(0,&none,&memory)))return ComPtr<ID3D12Resource>{};
        std::memcpy(memory,data,bytes);
        source->Unmap(0,nullptr);
        work.uploadMs+=float(1000.*QpcSeconds(QpcCounter()-begin));
        return source;
    };
    const auto submit=[&]{
        const auto begin=QpcCounter();
        output.commands.Submit(forestBuildQueue);
        output.fence=forestBuildQueue.Signal();
        work.submitMs+=float(1000.*QpcSeconds(QpcCounter()-begin));
    };
    const auto& routes=prepared.routes;
    output.routeBytes=uint32_t(routes.size()*sizeof(Mg2InstanceRoute));
    output.variants=cut.variants;
    const auto& compatible=prepared.compatible;
    if(cut.clusters.empty()||compatible){
        // Keep the raster and BLAS resources when only assignments change.
        // TLAS updates follow the existing generation-change path.
        const auto failRoute=[&](const char* reason){
            work.result.lastFailure=reason;work.result.buildCode=0;return false;
        };
        if(!compatible)return failRoute("routes_geometrie_incompatible");
        const UINT64 bytes=output.routeBytes;
        if(!fits(allocation(bytes)))return false;
        const auto geometry=make(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        const auto source=upload(routes.data(),size_t(bytes));
        if(!geometry||!source)return failRoute("allocation_routes");
        CommandsInit(output.commands,D3D12_COMMAND_LIST_TYPE_DIRECT);
        output.commands.Reset();
        auto* list=output.commands.list.Get();
        list->CopyBufferRegion(geometry.Get(),0,source.Get(),0,bytes);
        Transition(list,geometry.Get(),D3D12_RESOURCE_STATE_COPY_DEST,ShaderRead);
        output.geometryOwner=compatible->geometryOwner?compatible->geometryOwner:compatible;
        output.views=compatible->views;
        output.views[ForestGeneration::Routes]={};
        output.viewRequests[ForestGeneration::Routes]={geometry.Get(),0,uint32_t(routes.size()),sizeof(Mg2InstanceRoute)};
        output.resources.push_back(geometry);
        output.staging.push_back(source);
        output.backend=compatible->backend;
        output.blasStart=compatible->blasStart;
        output.blasCapacity=compatible->blasCapacity;
        output.clasCapacity=compatible->clasCapacity;
        output.pages=compatible->pages;
        output.clusters=compatible->clusters;
        output.triangles=compatible->triangles;
        output.bytes=compatible->bytes-compatible->routeBytes+output.routeBytes;
        submit();
        return true;
    }
    const auto& pack=prepared.pack;
    const auto& layouts=prepared.layouts;
    const auto& parts=prepared.parts;
    const auto remap=prepared.remap,bounds=prepared.bounds,localIndices=prepared.localIndices;
    const bool clustered=work.backend!=Mg2RayBackend::Standard;
    if(pack.bytes.size()>UINT32_MAX){
        work.result.lastFailure="limite_abi_octets_mg2";
        work.result.buildCode=0;
        return false;
    }
    if(!fits(allocation(pack.bytes.size())))return false;


    const auto geometry=make(pack.bytes.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    const auto source=upload(pack.bytes.data(),pack.bytes.size());
    if(!geometry||!source){
        work.result.lastFailure="allocation_geometrie";
        work.result.buildCode=0;
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
        work.result.lastFailure=reason;
        work.result.buildCode=code;
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
            const auto& limits=prepared.limits;
            const auto& initial=prepared.initial;
            const auto& missing=prepared.missing;
            const auto retainedBytes=prepared.retainedBytes;
            output.clasCache=std::move(prepared.clasCache);
            output.newClasKeys=std::move(prepared.newClasKeys);
            std::vector<typename Api::Triangle> clusterArgs;
            clusterArgs.reserve(prepared.newClusters.size());
            for(size_t i=0;i<prepared.newClusters.size();++i){
                const auto& cluster=prepared.newClusters[i];
                clusterArgs.push_back(Api::TriangleArgs(cluster,output.newClasKeys[i].second,
                    vertexAddress+UINT64(cluster.firstVertex)*sizeof(Vertex),
                    localAddress+UINT64(cluster.firstIndex)*sizeof(uint32_t)));
            }
            std::array<ClusterRequirements,2> sizes{};
            if(const auto code=api.Prepare(device.Get(),list,limits,sizes);code)
                return fail("taille_clas_blas",code);
            const auto scratchBytes=std::max(sizes[0].scratch,sizes[1].scratch);
            if((limits.clusters&&!sizes[0].result)||!sizes[1].result)return fail("taille_clas_blas_nulle");
            const UINT64 fullBytes=UINT64(initial.size())*sizeof(UINT64);
            const UINT64 newBytes=UINT64(limits.clusters)*sizeof(UINT64);
            const UINT64 required=allocation(pack.bytes.size())+allocation(sizes[0].result)+
                allocation(sizes[1].result)+allocation(scratchBytes)+allocation(fullBytes)+
                allocation(newBytes)+allocation(addressBytes);
            if(!fits(required))return false;
            const auto alloc=[&](UINT64 bytes,D3D12_RESOURCE_STATES state){
                return make(bytes,D3D12_HEAP_TYPE_DEFAULT,state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            };
            const auto clasAddresses=make(fullBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto addressUpload=upload(initial.data(),size_t(fullBytes));
            addresses=alloc(addressBytes,UavState);
            const auto blas=alloc(sizes[1].result,AsState);
            const auto scratch=alloc(scratchBytes,UavState);
            if(!clasAddresses||!addressUpload||!addresses||!blas||!scratch)return fail("allocation_clas_blas");
            list->CopyBufferRegion(clasAddresses.Get(),0,addressUpload.Get(),0,fullBytes);
            const auto clasAddressBase=clasAddresses->GetGPUVirtualAddress();
            const auto scratchAddress=scratch->GetGPUVirtualAddress();
            if(limits.clusters){
                const auto clas=alloc(sizes[0].result,AsState);
                const auto newAddresses=alloc(newBytes,UavState);
                const auto clusterArgBuffer=upload(clusterArgs.data(),clusterArgs.size()*sizeof(clusterArgs.front()));
                output.clasReadback=make(newBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
                if(!clas||!newAddresses||!clusterArgBuffer||!output.clasReadback)return fail("allocation_clas_incremental");
                const ClusterBuffers clasBuffers{clas->GetGPUVirtualAddress(),scratchAddress,
                    {newAddresses->GetGPUVirtualAddress(),sizeof(UINT64)},
                    {clusterArgBuffer->GetGPUVirtualAddress(),sizeof(clusterArgs.front())}};
                if(const auto code=api.Record(list,0u,clasBuffers);code)return fail("commande_clas",code);
                FinishUav(list,clas.Get());
                FinishUav(list,scratch.Get());
                Transition(list,newAddresses.Get(),UavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyBufferRegion(output.clasReadback.Get(),0,newAddresses.Get(),0,newBytes);
                // Copy consecutive runs of new addresses into the ordered BLAS input.
                for(size_t i=0;i<missing.size();){
                    if(missing[i]==Invalid){++i;continue;}
                    size_t end=i+1;
                    while(end<missing.size()&&missing[end]==missing[i]+uint32_t(end-i))++end;
                    list->CopyBufferRegion(clasAddresses.Get(),UINT64(i)*sizeof(UINT64),
                        newAddresses.Get(),UINT64(missing[i])*sizeof(UINT64),UINT64(end-i)*sizeof(UINT64));
                    i=end;
                }
                for(const auto& key:output.newClasKeys)output.clasCache.emplace(key,ForestClasEntry{0,clas});
                output.staging.insert(output.staging.end(),{newAddresses,clusterArgBuffer});
            }
            Transition(list,clasAddresses.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            std::vector<typename Api::Blas> blasArgs;
            blasArgs.reserve(parts.size());
            for(const auto& part:parts)
                blasArgs.push_back(Api::BlasArgs(part.count,clasAddressBase+UINT64(part.first)*sizeof(UINT64)));
            const auto blasArgBuffer=upload(blasArgs.data(),blasArgs.size()*sizeof(blasArgs.front()));
            if(!blasArgBuffer)return fail("arguments_clas_blas");
            const ClusterBuffers blasBuffers{blas->GetGPUVirtualAddress(),scratchAddress,
                {addresses->GetGPUVirtualAddress(),sizeof(UINT64)},{blasArgBuffer->GetGPUVirtualAddress(),sizeof(blasArgs.front())}};
            if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1u);
            if(const auto code=api.Record(list,1u,blasBuffers);code)return fail("commande_blas_cluster",code);
            FinishUav(list,blas.Get());
            output.addressCount=uint32_t(parts.size());
            output.clasCapacity=retainedBytes+sizes[0].result;
            output.blasStart=blas->GetGPUVirtualAddress();
            output.blasCapacity=sizes[1].result;
            output.resources.push_back(blas);
            output.staging.insert(output.staging.end(),{clasAddresses,addressUpload,scratch,blasArgBuffer});
            return true;
        };
#if OASIS_MG2_PREVIEW
        if(work.backend==Mg2RayBackend::DxrClusters){
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
            if(part.material>=work.opaqueMaterials.size())return fail("materiau_blas_standard");
            mesh.Flags=work.opaqueMaterials[part.material]?
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
        if(!fits(allocation(pack.bytes.size())+allocation(resultBytes)+allocation(scratchBytes)))return false;
        const auto result=make(resultBytes,D3D12_HEAP_TYPE_DEFAULT,AsState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto scratch=make(scratchBytes,D3D12_HEAP_TYPE_DEFAULT,UavState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!result||!scratch)return fail("allocation_blas_standard");
        for(auto& offset:offsets)offset+=result->GetGPUVirtualAddress();
        output.blasAddresses=offsets;
        output.blasStart=result->GetGPUVirtualAddress();
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
    output.backend=work.backend;
    output.resources.push_back(addresses);
    for(uint32_t i=0;i<layouts.size();++i){
        const auto& view=layouts[i];
        output.viewRequests[i]={geometry.Get(),view.offset/view.stride,view.count,view.stride};
    }
    output.viewRequests[ForestGeneration::Addresses]={addresses.Get(),0,uint32_t(parts.size()),sizeof(UINT64)};
    output.viewRequests[ForestGeneration::Remap]={geometry.Get(),remap.offset/remap.stride,remap.count,remap.stride};
    output.viewRequests[ForestGeneration::Bounds]={geometry.Get(),bounds.offset/bounds.stride,bounds.count,bounds.stride};
    output.pages=uint32_t(cut.pages);
    output.clusters=uint32_t(cut.clusters.size());
    output.triangles=prepared.triangleCount;
    output.bytes=uint32_t(pack.bytes.size());
    submit();
    return true;
}
}
