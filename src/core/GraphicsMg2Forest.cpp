#include "GraphicsInternal.h"
#include "WorldRegistry.h"
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include <stdexcept>
#include <cstdio>
#include <nvapi.h>

namespace OASIS {
void DiagnosticLine(const char*) noexcept;
namespace {
constexpr uint32_t PagedMeshBit=0x80000000u;
uint32_t ForestRouteHash(const Mg2InstanceRoute& key){
    uint32_t h=key[0]*0x9e3779b1u^key[1]*0x85ebca6bu^key[2]*0xc2b2ae35u;
    h^=h>>16u;h*=0x7feb352du;h^=h>>15u;return h;
}
// Exact identity comparisons in an immutable open-addressed table.
// Header contains the mask and maximum probe count for the shader.
std::vector<Mg2InstanceRoute> PackForestRoutes(const Mg2LodCut& cut){
    // Route tables are structured buffers and packed-generation byte
    // counts are uint32. Bound by that ABI, not a fixed population cap.
    constexpr uint64_t maxElements=UINT32_MAX/sizeof(Mg2InstanceRoute);
    if(cut.routes.empty()||cut.routes.size()>(maxElements-1u)/2u)return {};
    size_t capacity=2;
    while(capacity<cut.routes.size()*2u){
        if(uint64_t(capacity)*2u+1u>maxElements)return {};
        capacity*=2u;
    }
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

struct ForestPartRange {uint32_t first,count,firstIndex,indexCount,material,arena;};
struct ClusterLimits {
    uint32_t clusters,parts,totalTriangles,totalVertices;
    uint32_t maxTriangleCount{},maxVertexCount{},maxPrimitiveBase{},maxClustersPerBlas{},totalClasCount{};
    bool omm{};
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

NvAPI_Status NvapiClusterPipelines(ID3D12Device5* device,bool enable,bool omm=false){
    const NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS options{
        NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER,
        enable?(NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_CLUSTER_SUPPORT|
                (omm?NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_OMM_SUPPORT:0u)):
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
    static void AttachOmm(Triangle& args,UINT64,UINT64 vendor,UINT64 indices){
        args.opacityMicromapArray=vendor;
        args.opacityMicromapIndexBuffer=indices;
        args.opacityMicromapIndexFormat=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INDEX_FORMAT_32BIT;
        args.opacityMicromapIndexBufferStride=sizeof(uint32_t);
    }
    static Blas BlasArgs(uint32_t count,UINT64 addresses){return {count,0,addresses};}

    int Prepare(ID3D12Device10* device,ID3D12GraphicsCommandList7*,const ClusterLimits& limits,
                std::array<ClusterRequirements,2>& requirements){
        if(limits.maxPrimitiveBase>NVAPI_D3D12_RAYTRACING_MAXIMUM_GEOMETRY_INDEX)
            return NVAPI_INVALID_ARGUMENT;
        for(auto& input:inputs){
            input.mode=NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_IMPLICIT_DESTINATIONS;
            input.flags=static_cast<decltype(input.flags)>(
                NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_TRACE|
                (limits.omm?NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_ALLOW_OMM:0u));
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
    static void AttachOmm(Triangle& args,UINT64 native,UINT64,UINT64 indices){
        args.OpacityMicromapArray=native;
        args.OpacityMicromapIndexBuffer=indices;
        args.OpacityMicromapIndexBufferStride=sizeof(uint32_t);
    }
    static Blas BlasArgs(uint32_t count,UINT64 addresses){return {count,sizeof(UINT64),addresses};}

    int Prepare(ID3D12Device10* device,ID3D12GraphicsCommandList7* list,const ClusterLimits& limits,
                std::array<ClusterRequirements,2>& requirements){
        if(limits.maxPrimitiveBase>D3D12_RAYTRACING_MAXIMUM_GEOMETRY_INDEX)return E_INVALIDARG;
        ComPtr<ID3D12DeviceRaytracing2> rayDevice;
        if(const auto status=device->QueryInterface(IID_PPV_ARGS(&rayDevice));FAILED(status))return status;
        if(const auto status=list->QueryInterface(IID_PPV_ARGS(&rayList));FAILED(status))return status;
        triangles.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        if(limits.omm){
            triangles.Flags|=D3D12_RTAS_OPERATION_FLAG_ALLOW_OMM;
            triangles.OpacityMicromapIndexFormat=D3D12_INDEX_FORMAT_UINT32;
        }
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
        if(limits.omm)assembly.Flags|=D3D12_RTAS_OPERATION_FLAG_ALLOW_OMM;
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
void Graphics::Impl::ForestAccelerationCache::TrimCold(uint64_t target){
    if(!target)return;
    struct Group {ID3D12Resource* resource{};uint64_t stamp{};bool pinned{};};
    std::map<ID3D12Resource*,Group> groups;
    const auto touchGroup=[&](ID3D12Resource* resource,bool pinned){
        if(!resource)return;
        auto& group=groups[resource];
        group.resource=resource;group.pinned|=pinned;
        const auto it=lastUse.find(resource);
        group.stamp=it==lastUse.end()?0u:it->second;
    };
    const auto ordered=[&]{
        std::vector<Group> result;
        for(const auto& [resource,group]:groups)if(!group.pinned)result.push_back(group);
        std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.stamp<b.stamp;});
        return result;
    };
    uint64_t released=0;
    for(const auto& [key,entry]:blas)if(entry)touchGroup(entry->storage.Get(),entry.use_count()>1);
    for(const auto& group:ordered()){
        if(released>=target)break;
        const auto allocation=allocations.find(group.resource);
        if(allocation!=allocations.end())released+=allocation->second;
        evictions+=std::erase_if(blas,[&](const auto& item){return item.second->storage.Get()==group.resource;});
    }
    groups.clear();
    // CLAS belonging to any retained CBLAS must stay hot. Generation
    // ownership additionally protects GPU references until their fences.
    for(const auto& [key,entry]:blas)if(entry)
        for(const auto& resource:entry->dependencies)touchGroup(resource.Get(),true);
    for(const auto& [key,entry]:clas)touchGroup(entry.storage.Get(),false);
    for(const auto& group:ordered()){
        if(released>=target)break;
        const auto allocation=allocations.find(group.resource);
        if(allocation!=allocations.end())released+=allocation->second;
        evictions+=std::erase_if(clas,[&](const auto& item){return item.second.storage.Get()==group.resource;});
    }
    Recount();
}

void Graphics::Impl::ForestAccelerationCache::RetireCatalog(const Mg2Pages& catalog){
    if(retiredClusters==catalog.RetiredClusters())return;
    // Existing generations retain their own addresses and allocations until
    // their fences complete. Remove only cache ownership of retired IDs.
    evictions+=std::erase_if(blas,[&](const auto& item){
        return std::ranges::any_of(item.first,[&](uint32_t id){return !catalog.ClusterLive(id);});
    });
    evictions+=std::erase_if(clas,[&](const auto& item){return !catalog.ClusterLive(item.first.first);});
    retiredClusters=catalog.RetiredClusters();
    Recount();
}

void Graphics::Impl::ForestAccelerationCache::Recount(){
    // Recount unique allocations without refreshing their LRU timestamps.
    allocations.clear();bytes=0;
    const auto account=[&](ID3D12Resource* resource){
        if(resource&&!allocations.contains(resource)){
            const auto size=resource->GetDesc().Width;
            allocations.emplace(resource,size);bytes+=size;
        }
    };
    for(const auto& [key,entry]:clas){account(entry.storage.Get());account(entry.slot.buffer.Get());}
    for(const auto& [key,entry]:blas)if(entry){
        account(entry->storage.Get());account(entry->slot.buffer.Get());
        for(const auto& resource:entry->dependencies)account(resource.Get());
        for(const auto& [key,cluster]:entry->clusterAddresses)account(cluster.slot.buffer.Get());
    }
    std::erase_if(lastUse,[&](const auto& item){return !allocations.contains(item.first);});
}

void Graphics::Impl::ForestGeneration::DiffRayRoutes(const ForestGeneration* before,
        const ForestGeneration* after,std::vector<RayChange>& changes){
    changes.clear();
    const auto routes=[](const ForestGeneration* g){
        return g?std::span<const Mg2InstanceRoute>(g->instanceRoutes):std::span<const Mg2InstanceRoute>{};
    };
    // Immutable CBLAS object identity represents the exact same GPU slot.
    // A rebuilt object conservatively dirties its PTLAS instance.
    const auto address=[](const ForestGeneration* g,const Mg2InstanceRoute& route)
        ->std::pair<const ForestBlasEntry*,UINT64>{
        const auto& owner=g->geometryOwner?*g->geometryOwner:*g;
        if(owner.backend!=Mg2RayBackend::Standard){
            if(route[3]>=owner.blasEntries.size()||!owner.blasEntries[route[3]])Fail("MG2 ray diff variant");
            return {owner.blasEntries[route[3]].get(),0};
        }
        if(route[3]>=owner.blasAddresses.size())Fail("MG2 standard ray diff variant");
        return {nullptr,owner.blasAddresses[route[3]]};
    };
    const auto present=[](const auto& identity){return identity.first||identity.second;};
    const auto old=routes(before),next=routes(after);
    size_t a=0,b=0;
    while(a<old.size()||b<next.size()){
        const auto oldKey=[&]{auto k=old[a];k[3]=0;return k;};
        const auto newKey=[&]{auto k=next[b];k[3]=0;return k;};
        if(b==next.size()||(a<old.size()&&oldKey()<newKey())){
            if(present(address(before,old[a])))changes.push_back({oldKey(),false});
            ++a;
        }else if(a==old.size()||newKey()<oldKey()){
            if(present(address(after,next[b])))changes.push_back({newKey(),false});
            ++b;
        }else{
            const auto previous=address(before,old[a]),current=address(after,next[b]);
            if(previous!=current)changes.push_back({newKey(),present(previous)&&present(current)});
            ++a;++b;
        }
    }
}

struct Graphics::Impl::ForestPreparedGeneration {
    const char* failure{};
    std::shared_ptr<ForestGeneration> compatible;
    std::shared_ptr<const Mg2Pages> pages;
    std::vector<Mg2InstanceRoute> routes;
    std::vector<TerritorialDrawRoute> collectiveRoutes;
    PackedForest pack;
    std::array<PackedView,ForestGeneration::Addresses> layouts{};
    PackedView bounds{},cameraBoxes{};
    // The cut demands physical pages, not a relocated vertex/index buffer.
    std::set<ForestSourceTile> sourceTiles;
    uint32_t sourceUniqueClusters{};
    std::vector<uint32_t> arenaIds;
    std::vector<ForestPartRange> parts;
    uint32_t triangleCount{};
    std::vector<Mg2LodCluster> newClusters;
    std::vector<ForestAddressSlot> initial;
    std::vector<uint32_t> missing;
    std::map<std::pair<uint32_t,uint32_t>,ForestClasEntry> clasCache;
    std::vector<std::pair<uint32_t,uint32_t>> newClasKeys,clasKeys;
    std::vector<std::shared_ptr<ForestBlasEntry>> blasEntries;
    std::vector<uint32_t> newBlasParts;
    ClusterLimits limits{};
};

std::shared_ptr<Graphics::Impl::ForestPreparedGeneration>
Graphics::Impl::PackForestCut(ForestSelectionWork& work){
    const auto& cut=work.cut;
    // Both caches belong exclusively to this serialized producer path.
    // A retired source can be reclaimed even if the selected cut is unchanged.
    work.acceleration->RetireCatalog(*work.pages);
    if(work.pages->RetiredClusters()&&
       (forestSourcePool.collectedRetiredClusters!=work.pages->RetiredClusters()||
        forestSourcePool.collectedGenerations!=work.retiredGenerations)){
        const auto bytes=TrimForestSourceBlocks(*work.pages,0u);
        forestSourcePool.collectedRetiredClusters=work.pages->RetiredClusters();
        forestSourcePool.collectedGenerations=work.retiredGenerations;
        if(bytes){
            char line[160];std::snprintf(line,sizeof(line),"lod23_source_reclaimed bytes=%llu\r\n",
                static_cast<unsigned long long>(bytes));DiagnosticLine(line);
        }
    }
    auto prepared=std::make_shared<ForestPreparedGeneration>();
    prepared->pages=work.pages;
    if(cut.routes.empty())return prepared;
    for(const auto& route:cut.routes)if(Mg2IsCollectiveKey(route)){
        auto draw=work.collectiveInputs.at(route[2]);
        draw.key=route;draw.instance.mesh=route[3]|PagedMeshBit;
        prepared->collectiveRoutes.push_back(std::move(draw));
    }
    constexpr uint32_t RasterBatch=16;
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
    if(!EnsureForestSourceArenas(*work.pages)){
        prepared->failure="allocation_source_sparse";return prepared;
    }
    auto& pool=forestSourcePool;
    // Catalog offsets remain stable across all cuts. Collect exact tile
    // demand (both ends of a straddling vertex/index range are included).
    constexpr uint64_t TileBytes=Mg2Pages::SourceTileBytes;
    std::set<uint32_t> uniqueSources;
    const auto requireRange=[&](ForestSourceStream stream,uint64_t first,uint64_t bytes){
        const auto extent=work.pages->SourceBytes(stream);
        if(!bytes||first>=extent||bytes>extent-first)return false;
        for(uint64_t t=first/TileBytes;t<=(first+bytes-1u)/TileBytes;++t){
            if(t>UINT32_MAX)return false;
            prepared->sourceTiles.emplace(stream,uint32_t(t));
        }
        return true;
    };
    for(const auto& cluster:cut.clusters){
        if(!uniqueSources.insert(cluster.sourceFirstVertex).second)continue;
        if(!requireRange(ForestSourceStream::Vertices,uint64_t(cluster.sourceFirstVertex)*sizeof(Vertex),
                         uint64_t(cluster.vertexCount)*sizeof(Vertex))||
           !requireRange(ForestSourceStream::Indices,uint64_t(cluster.sourceFirstIndex)*sizeof(uint32_t),
                         uint64_t(cluster.indexCount)*sizeof(uint32_t))){
            prepared->failure="a06_source_range";return prepared;
        }
    }
    prepared->sourceUniqueClusters=uint32_t(uniqueSources.size());
    // A07: raster and CLAS consume the same immutable source clusters.
    // No per-cut weld, raster meshopt rebuild, local triangle copy or remap.
    // The ray primitive-to-source index table retains its established ABI.
    size_t selectionCursor=0;
    std::vector<uint32_t> globalIndices;
    std::vector<Meshlet> rasterMeshlets;
    std::vector<XMFLOAT4> rasterBounds;
    std::vector<MeshRange> level;
    std::vector<XMFLOAT4> cameraBoxes;
    auto& parts=prepared->parts;
    const auto invalidCut=[&]{
        prepared->failure="a07_cluster_raster_range";
        return prepared;
    };
    for(uint32_t i=0;i<cut.clusters.size();){
        const auto& first=cut.clusters[i];
        const auto collective=work.collectiveInputs.find(first.mesh);
        if((first.mesh>=work.meshBounds.size()&&collective==work.collectiveInputs.end())||first.part!=level.size())return invalidCut();
        const uint32_t rasterFirst=uint32_t(rasterMeshlets.size());
        const auto arena=pool.ArenaFor(ForestSourceStream::Vertices,
            uint64_t(first.sourceFirstVertex)*sizeof(Vertex),uint64_t(first.vertexCount)*sizeof(Vertex));
        if(arena==Invalid)return invalidCut();
        auto foundArena=std::find(prepared->arenaIds.begin(),prepared->arenaIds.end(),arena);
        const uint32_t localArena=uint32_t(foundArena-prepared->arenaIds.begin());
        if(foundArena==prepared->arenaIds.end())prepared->arenaIds.push_back(arena);
        const auto vertexBase=pool.Base(arena,ForestSourceStream::Vertices)/sizeof(Vertex);
        const auto indexBase=pool.Base(arena,ForestSourceStream::Indices)/sizeof(uint32_t);
        const auto center=collective==work.collectiveInputs.end()?work.meshBounds[first.mesh]:collective->second.bounds;
        XMFLOAT4 half{0.f,0.f,0.f,1.f};
        uint64_t expectedVertex=first.firstVertex,expectedIndex=first.firstIndex;
        if(expectedIndex!=globalIndices.size())return invalidCut();
        auto j=i;
        for(;j<cut.clusters.size()&&cut.clusters[j].part==first.part;++j){
            const auto& cluster=cut.clusters[j];
            if(cluster.mesh!=first.mesh||cluster.material!=first.material||
               !cluster.vertexCount||cluster.vertexCount>128u||!cluster.indexCount||
               cluster.indexCount%3u||cluster.indexCount/3u>128u||
               cluster.firstVertex!=expectedVertex||cluster.firstIndex!=expectedIndex||
               expectedVertex+cluster.vertexCount>cut.vertices.size()||
               expectedIndex+cluster.indexCount>cut.indices.size()||
               uint64_t(cluster.sourceFirstVertex)+cluster.vertexCount>work.pages->VertexCount()||
               uint64_t(cluster.sourceFirstIndex)+cluster.indexCount>work.pages->IndexCount())return invalidCut();
            // Logical cluster offsets remain immutable; raster uses local offsets.
            if(pool.ArenaFor(ForestSourceStream::Vertices,uint64_t(cluster.sourceFirstVertex)*sizeof(Vertex),
                    uint64_t(cluster.vertexCount)*sizeof(Vertex))!=arena||
               pool.ArenaFor(ForestSourceStream::Indices,uint64_t(cluster.sourceFirstIndex)*sizeof(uint32_t),
                    uint64_t(cluster.indexCount)*sizeof(uint32_t))!=arena)return invalidCut();
            rasterMeshlets.push_back({uint32_t(cluster.sourceFirstVertex-vertexBase),
                uint32_t(cluster.sourceFirstIndex-indexBase),cluster.vertexCount,cluster.indexCount/3u});
            XMVECTOR lower=XMVectorReplicate(std::numeric_limits<float>::max());
            XMVECTOR upper=XMVectorNegate(lower);
            for(uint32_t v=0;v<cluster.vertexCount;++v){
                const auto& pos=cut.vertices[cluster.firstVertex+v].position;
                if(!std::isfinite(pos.x)||!std::isfinite(pos.y)||!std::isfinite(pos.z))return invalidCut();
                const auto p=XMLoadFloat3(&pos);
                lower=XMVectorMin(lower,p);upper=XMVectorMax(upper,p);
                half.x=std::max(half.x,std::abs(pos.x-center.x));
                half.y=std::max(half.y,std::abs(pos.y-center.y));
                half.z=std::max(half.z,std::abs(pos.z-center.z));
            }
            const auto extent=XMVectorScale(XMVectorSubtract(upper,lower),.5f);
            XMFLOAT4 sphere;
            XMStoreFloat4(&sphere,XMVectorSetW(XMVectorAdd(lower,extent),XMVectorGetX(XMVector3Length(extent))));
            rasterBounds.push_back(sphere);
            // Preserve exact primitive order for opacity, hit shading and standard DXR.
            for(uint32_t k=0;k<cluster.indexCount;++k){
                const auto local=cut.indices[cluster.firstIndex+k];
                if(local>=cluster.vertexCount)return invalidCut();
                globalIndices.push_back(uint32_t(cluster.sourceFirstVertex-vertexBase)+local);
            }
            expectedVertex+=cluster.vertexCount;expectedIndex+=cluster.indexCount;
        }
        if(selectionCursor>=cut.selection.size()||cut.selection[selectionCursor++]!=j-i||
           cut.selection.size()-selectionCursor<j-i)return invalidCut();
        selectionCursor+=j-i;
        const uint32_t vertexCount=uint32_t(expectedVertex-first.firstVertex);
        const uint32_t indexCount=uint32_t(expectedIndex-first.firstIndex);
        const uint32_t rasterCount=j-i;
        const uint32_t dispatchGroups=rasterCount<=65535u?rasterCount:
            ((rasterCount+DispatchPage-1u)/DispatchPage)*DispatchPage;
        if(uint64_t(dispatchGroups)*RasterBatch>=MaxDispatchGroups)return invalidCut();
        parts.push_back({i,j-i,first.firstIndex,indexCount,first.material,arena});
        // firstVertex of a paged MeshRange is the dense arena ordinal.
        level.push_back({rasterFirst,rasterCount,localArena,vertexCount,first.firstIndex,indexCount,center});
        cameraBoxes.push_back(half);
        i=j;
    }
    if(level.empty()||level.size()!=cut.variants||selectionCursor!=cut.selection.size()||
       rasterMeshlets.size()!=cut.clusters.size()||globalIndices.size()!=cut.indices.size()||
       globalIndices.size()%3u)return invalidCut();
    auto& pack=prepared->pack;
    prepared->layouts={
        pack.Add(routes),pack.Add(level),pack.Add(rasterMeshlets),PackedView{},
        PackedView{},pack.Add(globalIndices)};
    prepared->bounds=pack.Add(rasterBounds);
    prepared->cameraBoxes=pack.Add(cameraBoxes);
    prepared->triangleCount=uint32_t(globalIndices.size()/3u);
    const bool clustered=work.backend!=Mg2RayBackend::Standard;

    if(clustered){
        using Key=std::pair<uint32_t,uint32_t>;
        auto& cache=*work.acceleration;
        if(cache.backend!=work.backend){cache.Clear();cache.backend=work.backend;}
        // GeometryIndex encodes a primitive offset local to the cut.
        // Preserve this ABI: equal source IDs at different offsets are NOT
        // interchangeable CLAS. A whole ordered cut, however, is stable.
        auto& keys=prepared->clasKeys;
        keys.reserve(cut.clusters.size());
        prepared->blasEntries.resize(parts.size());
        size_t cursor=0;
        for(uint32_t p=0;p<parts.size();++p){
            const auto& part=parts[p];
            if(cursor>=cut.selection.size()||cut.selection[cursor++]!=part.count||
               cursor+part.count>cut.selection.size()){prepared->failure="identite_clas";return prepared;}
            std::vector<uint32_t> key(cut.selection.begin()+cursor,
                                      cut.selection.begin()+cursor+part.count);
            if(const auto found=cache.blas.find(key);found!=cache.blas.end()){
                prepared->blasEntries[p]=found->second;
                cache.Touch(found->second->storage.Get());
                for(const auto& dependency:found->second->dependencies)cache.Touch(dependency.Get());
            }else if(cache.evictions){
                // After a pressure eviction, a live generation may still own
                // this exact BLAS. Recover it without pinning its raster data.
                for(auto it=work.generations.rbegin();it!=work.generations.rend();++it){
                    const auto& candidate=**it;
                    if(!candidate.published||candidate.backend!=work.backend||candidate.geometryOwner)continue;
                    if(candidate.blasEntries.empty())continue;
                    size_t at=0;
                    for(size_t v=0;v<candidate.blasEntries.size()&&at<candidate.selection.size();++v){
                        const auto count=candidate.selection[at++];
                        if(count>candidate.selection.size()-at)break;
                        if(count==key.size()&&std::equal(key.begin(),key.end(),candidate.selection.begin()+at)){
                            prepared->blasEntries[p]=candidate.blasEntries[v];break;
                        }
                        at+=count;
                    }
                    if(prepared->blasEntries[p])break;
                }
            }
            if(!prepared->blasEntries[p])prepared->newBlasParts.push_back(p);
            for(uint32_t i=part.first;i<part.first+part.count;++i)
                keys.emplace_back(cut.selection[cursor++],(cut.clusters[i].firstIndex-part.firstIndex)/3u);
        }
        if(cursor!=cut.selection.size()||keys.size()!=cut.clusters.size()){prepared->failure="identite_clas";return prepared;}
        auto& newClusters=prepared->newClusters;
        auto& initial=prepared->initial;
        auto& missing=prepared->missing;
        std::map<Key,uint32_t> newSlots;
        auto& limits=prepared->limits;
        limits.parts=uint32_t(prepared->newBlasParts.size());
        // Compact input addresses: only newly required BLAS contribute.
        for(uint32_t p:prepared->newBlasParts){
            const auto& part=parts[p];
            limits.maxClustersPerBlas=std::max(limits.maxClustersPerBlas,part.count);
            for(uint32_t i=part.first;i<part.first+part.count;++i){
                const auto& key=keys[i];
                ForestClasEntry entry{};
                if(const auto found=cache.clas.find(key);found!=cache.clas.end())entry=found->second;
                else if(cache.evictions)for(auto it=work.generations.rbegin();it!=work.generations.rend();++it){
                    const auto& generation=**it;
                    if(!generation.published||generation.backend!=work.backend||generation.geometryOwner)continue;
                    if(const auto found=generation.clasCache.find(key);found!=generation.clasCache.end()){
                        entry=found->second;break;
                    }
                    // A reused CBLAS may be the sole surviving owner of
                    // its CLAS. Recover the exact address without rebuilding.
                    for(const auto& blas:generation.blasEntries){
                        if(!blas)continue;
                        entry=blas->FindCluster(key);
                        if(entry.slot)break;
                    }
                    if(entry.slot)break;
                }
                if(entry.slot&&entry.storage){
                    cache.Touch(entry.storage.Get());
                    initial.push_back(entry.slot);missing.push_back(Invalid);
                    prepared->clasCache.emplace(key,entry);
                    continue;
                }
                auto [slot,inserted]=newSlots.try_emplace(key,uint32_t(newClusters.size()));
                initial.push_back({});missing.push_back(slot->second);
                if(!inserted)continue;
                auto cluster=cut.clusters[i];cluster.id=key.first;
                limits.maxPrimitiveBase=std::max(limits.maxPrimitiveBase,key.second);
                limits.maxTriangleCount=std::max(limits.maxTriangleCount,cluster.indexCount/3u);
                limits.maxVertexCount=std::max(limits.maxVertexCount,cluster.vertexCount);
                if(cluster.indexCount/3u>UINT32_MAX-limits.totalTriangles||
                   cluster.vertexCount>UINT32_MAX-limits.totalVertices){
                    prepared->failure="limite_abi_clas";return prepared;
                }
                limits.totalTriangles+=cluster.indexCount/3u;limits.totalVertices+=cluster.vertexCount;
                newClusters.push_back(cluster);prepared->newClasKeys.push_back(key);
            }
        }
        if(initial.size()>UINT32_MAX){prepared->failure="limite_abi_clas";return prepared;}
        limits.totalClasCount=uint32_t(initial.size());
        limits.clusters=uint32_t(newClusters.size());
    }
    return prepared;
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
    auto flags=foliageOmms.empty()?D3D12_PIPELINE_STATE_FLAG_NONE:
        D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_OPACITY_MICROMAPS_ON_PRE_SM69_SHADERS;
#if OASIS_MG2_PREVIEW
    if(forestStatus.configured==Mg2RayBackend::DxrClusters)
        flags|=D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_CLUSTERED_GEOMETRY_ON_PRE_SM69_SHADERS;
#endif
    return flags;
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
    const bool omm=std::any_of(assets.materialNames.begin(),assets.materialNames.end(),
        [](const auto& name){return name=="OrganicForest OMM foliage";});
    status=NvapiClusterPipelines(device.Get(),true,omm);
    forestStatus.code=status;
    if(status!=NVAPI_OK){
        forestStatus.capability="nvapi_options_pso";
        return;
    }
    NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS ptlasCaps{};
    const auto ptlasStatus=NvAPI_D3D12_GetRaytracingCaps(device.Get(),
        NVAPI_D3D12_RAYTRACING_CAPS_TYPE_PARTITIONED_TLAS,&ptlasCaps,sizeof(ptlasCaps));
    if(ptlasStatus!=NVAPI_OK||!(ptlasCaps&NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_STANDARD))
        Fail("NVAPI partitioned TLAS is required for the MG2 NVAPI backend");
    partitionedTlas=true;
    forestStatus.configured=forestBuildBackend=Mg2RayBackend::Nvapi;
    forestStatus.capability="disponible";
}

void Graphics::Impl::ResetForest(){
    partitionedTlas=false;
    // A pending bake still reads the scene's immutable asset geometry. Never
    // release a scene while the worker might dereference it.
    if(forestBake.valid())forestBake.wait();
    forestBake={};
    if(territorialCatalog.completion.valid())territorialCatalog.completion.wait();
    territorialCatalog={};
    territorialCoverage.clear();territorialCoverageChosen.clear();territorialFrameCut.reset();
    territorialRayChosen.clear();territorialRayCut.reset();territorialParkCut.reset();
    territorialCoverageSerial=territorialCoverageRevision=0;territorialCoverageSource=nullptr;
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
    forestSelection.territorialNodes.clear();forestSelection.territorialRevision=0;
    forestSelection.collectiveInputs.clear();territorialGpu={};
    forestSelection.cut={};
    forestSelection.tileInputs.clear();
    forestSelection.staticInputs.clear();
    forestSelection.stagedInputs.clear();forestSelection.stagedRevision=0;
    forestStagedEntries.clear();forestStageRevision=forestStageCheckedSerial=forestInitialCheckedSerial=0;
    forestStageOpen=forestStageReady=forestInitialReady=false;
    forestSelection.registry.clear();forestSelection.registrySlots.clear();forestSelection.registryDirty.clear();
    forestSelection.residentKeys.clear();forestSelection.parkedRegistry.clear();forestSelection.parkedCut.reset();forestSelection.parkedGeneration=0;
    forestSelection.registryScratchWords=forestSelection.registryUploadBytes=forestSelection.registryGpuBytes=0;
    forestSelection.registryUploadValid=false;
    forestSelection.tileSource=nullptr;
    forestSelection.staticCaptured=false;
    forestSelection.pages.reset();
    forestSelectCatalog.reset();
    forestSelectRoot.Reset();forestSelectPSO.Reset();forestSelectClearPSO.Reset();forestSelectCompactPSO.Reset();forestPreparePSO.Reset();
    forestSelectRegistry.Reset();forestSelectCamera.Reset();forestSelectPriorFirst.Reset();forestSelectPriorSerial=0;
    forestSelectPages.Reset();forestSelectClusters.Reset();forestSelectStructural.Reset();
    forestSelectInput.Reset();forestSelectOutput.Reset();forestSelectReadback.Reset();
    forestSelectNodes.Reset();forestSelectScratch.Reset();forestSelectHash.Reset();
    forestSelection.acceleration=nullptr;
    forestAcceleration.Clear();
    forestAcceleration.evictions=0;
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
        frame.territorialCut.reset();frame.territorialRayCut.reset();frame.territorialTlasCut.reset();
        if(frame.territorialInstanceSRV.Valid())ReleaseDescriptor(frame.territorialInstanceSRV);
        if(frame.territorialSuppressionSRV.Valid())ReleaseDescriptor(frame.territorialSuppressionSRV);
        if(frame.territorialCameraSRV.Valid())ReleaseDescriptor(frame.territorialCameraSRV);
        if(frame.territorialRayInstanceSRV.Valid())ReleaseDescriptor(frame.territorialRayInstanceSRV);
        if(frame.territorialRaySuppressionSRV.Valid())ReleaseDescriptor(frame.territorialRaySuppressionSRV);
        if(frame.territorialShadowSRV.Valid())ReleaseDescriptor(frame.territorialShadowSRV);
        frame.forestDrawReadback.Reset();
        frame.forestDrawFrame=0;
        frame.forestRayProofReadback.Reset();
        frame.forestRayProofPending=false;
    }
    for(const auto& generation:forestGenerations){
        ReleaseForestSourceViews(*generation);
        for(uint32_t i=0;i<generation->views.size();++i)
            if((!generation->geometryOwner||i==ForestGeneration::Routes)&&generation->views[i].Valid())
                ReleaseDescriptor(generation->views[i]);
    }
    forestGenerations.clear();
    forestPending.reset();
    forestResident.reset();
    forestSourcePool.Reset();
    forestHierarchy.reset();
    forestParts.clear();
    forestCameraSampleEyes={};
    forestCameraSampleAt=0;
    forestCameraSampleCount=0;
    forestRefreshSelection=forestRetryUsed=false;
    forestRetryAvailable=0;
    forestRetryRetired=0;
    forestSelection.requestedAt=0;
    forestSelection.eyeMotion={};
    forestShadowUsesMg2=false;
    forestRadius=0.f;
    if(forestStatus.configured==Mg2RayBackend::Nvapi)NvapiClusterPipelines(device.Get(),false);
    forestStatus={};
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
    // The GPU already scattered all native result addresses into the
    // consumer table. Validate ownership/slot bounds, not CPU readbacks.
    // Prebuild max sizes and the API's implicit destinations contract
    // guarantee placement; device removal must never publish a candidate.
    Hr(device->GetDeviceRemovedReason(),"MG2 producer completion");
    bool valid=true;
    work.builtClas=uint32_t(pending.newClasKeys.size());
    float clasMs=-1.f,blasMs=-1.f;
    if(pending.buildReadback){
        const D3D12_RANGE read{0,3u*sizeof(UINT64)},empty{};
        void* mapped=nullptr;
        if(SUCCEEDED(pending.buildReadback->Map(0,&read,&mapped))&&mapped){
            const auto* times=static_cast<const UINT64*>(mapped);
            const double tickMs=1000./double(forestTimestampHz);
            if(work.builtClas&&times[1]>=times[0])clasMs=float(double(times[1]-times[0])*tickMs);
            if((pending.addressCount||pending.backend==Mg2RayBackend::Standard)&&times[2]>=times[1])
                blasMs=float(double(times[2]-times[1])*tickMs);
            pending.buildReadback->Unmap(0,&empty);
        }
        pending.buildReadback.Reset();
    }
    const auto slotValid=[](const ForestAddressSlot& slot){
        return slot.buffer&&slot.offset%sizeof(UINT64)==0&&
            slot.offset<slot.buffer->GetDesc().Width&&
            slot.buffer->GetDesc().Width-slot.offset>=sizeof(UINT64);
    };
    const auto& root=pending.geometryOwner?*pending.geometryOwner:pending;
    if(root.backend==Mg2RayBackend::Standard)valid&=root.blasAddresses.size()==pending.variants;
    else valid&=root.blasEntries.size()==pending.variants;
    if(!pending.geometryOwner&&pending.backend!=Mg2RayBackend::Standard){
        valid&=pending.addressCount==pending.newBlasParts.size();
        std::map<ID3D12Resource*,UINT64> clasStorage,blasStorage;
        for(const auto& entry:pending.blasEntries){
            if(!entry||!entry->storage||!slotValid(entry->slot)){valid=false;continue;}
            blasStorage.try_emplace(entry->storage.Get(),entry->storage->GetDesc().Width);
            for(const auto& storage:entry->dependencies)
                clasStorage.try_emplace(storage.Get(),storage->GetDesc().Width);
        }
        for(const auto& key:pending.newClasKeys){
            const auto found=pending.clasCache.find(key);
            valid&=found!=pending.clasCache.end()&&found->second.storage&&slotValid(found->second.slot);
        }
        pending.clasCapacity=pending.blasCapacity=0;
        for(const auto& [resource,size]:clasStorage)pending.clasCapacity+=size;
        for(const auto& [resource,size]:blasStorage)pending.blasCapacity+=size;
    }
    char line[384];
    std::snprintf(line,sizeof(line),
        "mg2_a17 valid=%u backend=%u route_only=%u new_clas=%u new_blas=%u address_readback_bytes=0 timestamp_readback_bytes=%u fence_complete=1 gpu_address_slots=%u\r\n",
        unsigned(valid),unsigned(pending.backend),unsigned(bool(pending.geometryOwner)),work.builtClas,
        pending.addressCount,unsigned(forestBuildQueries&&!pending.geometryOwner?3u*sizeof(UINT64):0u),
        root.backend==Mg2RayBackend::Standard?0u:uint32_t(root.blasEntries.size()));
    DiagnosticLine(line);
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
    const auto consider=[&](uint32_t mesh,uint32_t flags,const Mg2InstanceRoute&,
                            double x,double z,double sx,double sy,double sz){
        if(!pending||(flags&(DynamicObject|DeformedObject|NoShadow|AlphaBlendObject))||
           mesh>=meshes.ranges.size())return;
        if(!std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh))return;
        const auto& bounds=meshes.ranges[mesh].bounds;
        const double reach=(std::sqrt(double(bounds.x)*bounds.x+double(bounds.y)*bounds.y+
             double(bounds.z)*bounds.z)+bounds.w)*std::max({std::abs(sx),std::abs(sy),std::abs(sz)})*2.5+2.;
        const uint32_t slots=affected(x,z,reach);
        if(!slots)return;
        for(uint32_t slot=0;slot<ShadowSlots;++slot)if(slots&(1u<<slot)){
            shadowCache[slot].valid=false;++forestStatus.traceForestShadowDirty;
        }
        f.shadowDirty|=slots;
        pending&=~slots;
    };
    // This publication's predecessor is exactly the current resident
    // generation. Shadow changes were determined by the serial producer.
    // A deleted or replaced world object is already covered by its tile
    // revision; only the current matching identity can need this invalidation.
    const auto statics=sceneCache.StaticInstances();
    for(const auto& route:after->shadowChanges){
        if(!pending)break;
        if(route[2]&Mg2ResidentKeyBit){
            if(!tiles)continue;
            const uint64_t identity=uint64_t(route[0])|(uint64_t(route[1])<<32u);
            uint32_t index=0;
            const auto* tile=tiles->FindResidentOwner(identity,index);
            if(!tile)continue;
            const auto& o=tile->objects[index];
            if(!o.enabled||o.mesh!=(route[2]&~Mg2ResidentKeyBit))continue;
            consider(o.mesh,o.flags,route,o.position.x,o.position.z,o.scale.x,o.scale.y,o.scale.z);
        }else{
            const uint32_t i=route[0];
            if(i>=statics.size()+f.snapshotCount)continue;
            const auto& o=i<statics.size()?statics[i]:f.objects[i-statics.size()];
            if(o.id!=route[1]||o.mesh!=route[2])continue;
            consider(o.mesh,o.flags,route,o.positionRadius.x,o.positionRadius.z,o.scale.x,o.scale.y,o.scale.z);
        }
    }
}

void Graphics::Impl::PrepareTerritorialCatalog(){
    // A catalog snapshot needs the baked source, not a published raster
    // generation. Start the territorial CPU producer alongside the first
    // GPU selection/build, rather than forcing it to wait for a potentially
    // expensive individual generation. Both workers own immutable snapshots.
    if(!forestHierarchy)return;
    auto& work=territorialCatalog;
    const auto retireActive=[&](const TerritorialCatalogKey& key){
        if(auto it=work.active.find(key);it!=work.active.end()){
            for(const auto& rep:it->second.representations)work.retired.insert(rep.mesh);
            work.active.erase(it);++work.activeRevision;
        }
    };
    // Only the session/render owner drains changed node pointers. The CPU
    // worker never accesses the registry, materials, or live catalog.
    for(const bool forest:{false,true}){
        auto* patrimony=contributions.territorialPatrimony?
            contributions.territorialPatrimony(contributions.context,forest):nullptr;
        const auto accept=[&](TerritorialPatrimony::NodeChanges changes){
            for(auto& [key,node]:changes){
                const TerritorialCatalogKey identity{forest,key};
                // A changed parent is no longer a valid candidate, even
                // while its replacement is cooking. Future cuts must use
                // the descendants until this revision is published.
                retireActive(identity);
                work.pending.insert_or_assign(identity,std::move(node));
            }
            if(!changes.empty())work.blocked=false;
        };
        if(work.sources[unsigned(forest)]!=patrimony){
            // A renderer reset or producer switch can happen after its
            // journal was drained. Seed from the live snapshot exactly once.
            for(auto& [key,node]:work.pending)if(key.first==forest)node.reset();
            if(work.inFlight)for(const auto& [key,node]:*work.inFlight)
                if(key.first==forest)work.pending.insert_or_assign(key,nullptr);
            for(auto it=work.active.begin();it!=work.active.end();){
                if(it->first.first!=forest){++it;continue;}
                work.pending.insert_or_assign(it->first,nullptr);
                for(const auto& rep:it->second.representations)work.retired.insert(rep.mesh);
                it=work.active.erase(it);++work.activeRevision;
            }
            work.sources[unsigned(forest)]=patrimony;
            work.blocked=false;
            if(patrimony)accept(patrimony->SnapshotChanges());
        }
        if(patrimony&&patrimony->HasChanges())accept(patrimony->TakeChanges());
    }
    const auto restore=[&]{
        if(work.inFlight)for(const auto& [key,node]:*work.inFlight)
            work.pending.try_emplace(key,node); // a newer change always wins
        if(work.inFlightRetired)work.retired.insert(work.inFlightRetired->begin(),work.inFlightRetired->end());
        work.inFlight.reset();work.inFlightRetired.reset();work.blocked=true;
    };
    if(work.completion.valid()){
        if(work.completion.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
        // Pool capacities are written on the serialized GPU build worker.
        // Wait for its owner to collect it before checking those resources.
        if(forestBuild.requested)return;
        try{
            auto candidate=work.completion.get();
            if(!candidate.catalog||!forestHierarchy->IsPrefixOf(*candidate.catalog))
                Fail("LOD23 catalog prefix changed");
            // GPU arenas are admitted lazily by the serialized producer.
            // Atomic render-owner publication. No physical or raster routes
            // are changed here. Replaced meshes keep their immutable IDs in
            // the catalog for already captured cuts; only the active map
            // changes. The consumer will later select this active map.
            // Allocate retirement bookkeeping before transferring any
            // active-map nodes. Failure here leaves the old publication intact.
            std::set<uint32_t> obsolete;
            for(const auto& [key,node]:candidate.replacements){
                if(const auto it=work.active.find(key);it!=work.active.end())
                    for(const auto& rep:it->second.representations)obsolete.insert(rep.mesh);
                if(work.pending.contains(key))
                    for(const auto& rep:node.representations)obsolete.insert(rep.mesh);
            }
            work.retired.merge(obsolete);
            while(!candidate.replacements.empty()){
                auto entry=candidate.replacements.extract(candidate.replacements.begin());
                work.active.erase(entry.key());
                // A superseded revision may not become active; its IDs
                // are already queued for the next retirement snapshot.
                if(!work.pending.contains(entry.key())&&!entry.mapped().representations.empty())
                    work.active.insert(std::move(entry));
            }
            forestHierarchy=std::move(candidate.catalog);
            ++work.activeRevision;
            work.nextId=candidate.nextId;
            forestStatus.sourcePages=uint32_t(forestHierarchy->PageCount());
            forestStatus.sourceGeometryBytes=forestHierarchy->SourceBytes();
            char line[384];
            std::snprintf(line,sizeof(line),"lod23_catalog revision=%llu active_nodes=%zu changes=%zu triangles=%llu pages=%zu active_routes=0\r\n",
                static_cast<unsigned long long>(forestHierarchy->Revision()),work.active.size(),
                work.inFlight?work.inFlight->size():0u,static_cast<unsigned long long>(candidate.triangles),forestHierarchy->PageCount());
            DiagnosticLine(line);
            work.inFlight.reset();work.inFlightRetired.reset();work.blocked=false;
        }catch(const std::exception& e){
            char line[384];std::snprintf(line,sizeof(line),"lod23_catalog rejected=1 reason=%s\r\n",e.what());
            DiagnosticLine(line);restore();return;
        }catch(...){
            DiagnosticLine("lod23_catalog rejected=1 reason=unknown\r\n");restore();return;
        }
    }
    if(work.blocked||(work.pending.empty()&&work.retired.empty()))return;
    std::vector<uint32_t> alphaModes;alphaModes.reserve(surfaceData.size());
    for(const auto& surface:surfaceData)alphaModes.push_back(surface.extra.y);
    auto batch=std::make_shared<TerritorialCatalogChanges>();
    auto retired=std::make_shared<std::vector<uint32_t>>(work.retired.begin(),work.retired.end());
    batch->swap(work.pending);work.inFlight=batch;
    work.retired.clear();work.inFlightRetired=retired;
    try{
        work.completion=std::async(std::launch::async,[base=forestHierarchy,batch,retired,
            nextId=work.nextId,nativeMeshes=meshes.ranges.size(),alphaModes=std::move(alphaModes)]() mutable {
            TerritorialCatalogResult result;result.catalog=retired->empty()?base:base->RetireMeshes(*retired);
            result.nextId=nextId;
            std::vector<Mg2Pages::AuthoredMesh> meshes;
            for(const auto& [key,node]:*batch){
                auto& replacement=result.replacements[key];
                if(!node||node->patches.empty())continue;
                replacement.node=node;
                auto& reps=replacement.representations;
                const auto origin=node->patches.front().center;
                struct Group {
                    std::vector<Vertex> vertices;
                    XMFLOAT3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
                    float geometric{},optical{};
                };
                std::map<std::pair<uint32_t,uint32_t>,Group> groups;
                for(const auto& patch:node->patches){
                    if(patch.material>=alphaModes.size()||alphaModes[patch.material]>1u)
                        Fail("LOD23 invalid authored material");
                    auto& group=groups[{patch.material,patch.kind}];
                    for(auto v:patch.Triangle()){
                        v.position=Sub(v.position,origin);
                        group.lo={std::min(group.lo.x,v.position.x),std::min(group.lo.y,v.position.y),std::min(group.lo.z,v.position.z)};
                        group.hi={std::max(group.hi.x,v.position.x),std::max(group.hi.y,v.position.y),std::max(group.hi.z,v.position.z)};
                        group.vertices.push_back(v);
                    }
                    group.geometric=std::max(group.geometric,patch.GeometricError());
                    group.optical=std::max(group.optical,patch.OpticalError());
                }
                for(auto& [materialKind,group]:groups){
                    if(result.nextId<nativeMeshes||!result.nextId)
                        Fail("LOD23 representation identity exhausted");
                    const auto mesh=result.nextId--;
                    const auto [material,kind]=materialKind;
                    const XMFLOAT3 center{float((double(group.lo.x)+group.hi.x)*.5),
                        float((double(group.lo.y)+group.hi.y)*.5),float((double(group.lo.z)+group.hi.z)*.5)};
                    double radius=0.;
                    for(const auto& v:group.vertices){
                        const double x=double(v.position.x)-center.x,y=double(v.position.y)-center.y,z=double(v.position.z)-center.z;
                        radius=std::max(radius,std::sqrt(x*x+y*y+z*z));
                    }
                    const auto bound=std::nextafter(float(radius),std::numeric_limits<float>::infinity());
                    if(!std::isfinite(bound)||!std::isfinite(group.geometric)||!std::isfinite(group.optical))
                        Fail("LOD23 invalid territorial bounds/error");
                    reps.push_back({mesh,material,kind,origin,{center.x,center.y,center.z,bound},group.geometric,group.optical});
                    result.triangles+=group.vertices.size()/3u;
                    meshes.push_back({mesh,material,alphaModes[material]==0u,std::move(group.vertices)});
                }
            }
            if(!meshes.empty()){
                auto segment=Mg2Pages::FromAuthoredTriangles(meshes);
                result.catalog=result.catalog->Extend(segment);
            }
            return result;
        });
    }catch(...){
        DiagnosticLine("lod23_catalog rejected=1 reason=worker_launch\r\n");restore();
    }
}

void Graphics::Impl::UpdateForestFrameShadows(Frame& f,const GpuSceneCache* tiles,
    const ForestGeneration* previous,bool collectiveChanged){
    const bool shadowUsesMg2=bool(f.forestRaster);
    if(collectiveChanged||shadowUsesMg2!=forestShadowUsesMg2){
        // A changed territorial cover must not reuse an old shadow atlas.
        // Local light-volume invalidation can replace this conservative
        // transition once independent cuts are published.
        for(uint32_t slot=0;slot<ShadowSlots;++slot){
            if(shadowCache[slot].valid&&!(f.shadowDirty&(1u<<slot)))
                ++forestStatus.traceForestShadowDirty;
            shadowCache[slot].valid=false;
            if(f.hasShadowAtlasLights&&f.shadowIDs[slot]!=Invalid)f.shadowDirty|=1u<<slot;
        }
    }else if(tiles&&f.forestRaster&&previous!=f.forestRaster.get())
        InvalidateForestShadows(f,tiles,previous,f.forestRaster.get());
    forestShadowUsesMg2=shadowUsesMg2;
}

void Graphics::Impl::PrepareTerritorialFrame(Frame& f,const XRFrame& xr,const GpuSceneCache* tiles){
    const auto serial=f.forestRaster?f.forestRaster->serial:0u;
    const auto revision=territorialCatalog.activeRevision;
    const bool changed=serial!=territorialCoverageSerial||revision!=territorialCoverageRevision||
        tiles!=territorialCoverageSource;
    if(changed){
        territorialCoverage.clear();territorialCoverageChosen.clear();territorialFrameCut.reset();
        territorialRayChosen.clear();territorialRayCut.reset();
        // Preserve the intersection owner when the rebuilt cut covers the
        // same identities; geometry ordinals are separately regenerated.
        territorialCoverageSerial=serial;territorialCoverageRevision=revision;territorialCoverageSource=tiles;
        if(tiles&&f.forestRaster&&contributions.territorialPatrimony){
            std::map<TerritorialCatalogKey,std::vector<const TerritorialDrawRoute*>> groups;
            for(const auto& draw:f.forestRaster->collectiveRoutes)groups[draw.territory].push_back(&draw);
            // Exclusion is applied separately inside each domain cut.
            for(const auto& [key,draws]:groups){
                const auto active=territorialCatalog.active.find(key);
                auto* source=contributions.territorialPatrimony(contributions.context,key.first);
                if(!source||active==territorialCatalog.active.end()||
                   active->second.representations.size()!=draws.size())continue;
                const auto& nodes=source->Nodes();
                const auto live=nodes.find(key.second);
                if(live==nodes.end()||!live->second->valid||
                   live->second.get()!=active->second.node.get())continue;
                TerritorialCoverageGroup group;group.key=key;
                bool valid=true;
                std::set<uint32_t> materials;
                for(const auto* draw:draws){
                    if(draw->node.get()!=live->second.get()||
                       !std::ranges::any_of(active->second.representations,[&](const auto& rep){
                           return rep.mesh==draw->key[2]&&rep.material==draw->instance.material;
                       })||draw->instance.material>=surfaceData.size()){
                        valid=false;break;
                    }
                    const auto& surface=surfaceData[draw->instance.material];
                    // Authored patches now retain both UV sets and normals.
                    if(surface.extra.y>1u){
                        valid=false;break;
                    }
                    materials.insert(draw->instance.material);
                    group.instances.push_back(draw->instance);
                }
                if(!valid)continue;
                // Resolve exact leaf ownership, only on a generation or
                // journal change. Missing/invalid children reject the parent.
                const auto collect=[&](auto&& self,const TerritorialPatrimony::Node& node)->bool{
                    if(!node.valid)return false;
                    if(!node.key.level){
                        group.descendants.insert(group.descendants.end(),node.routes.begin(),node.routes.end());
                        return true;
                    }
                    for(const auto& child:node.children){
                        const auto it=nodes.find(child);
                        if(it==nodes.end()||!self(self,*it->second))return false;
                    }
                    return true;
                };
                if(!collect(collect,*live->second)||group.descendants.empty()||
                   group.descendants.size()!=live->second->sourceRoutes)continue;
                std::sort(group.descendants.begin(),group.descendants.end());
                if(std::adjacent_find(group.descendants.begin(),group.descendants.end())!=group.descendants.end())continue;
                uint32_t commonFlags=0,category=0;
                bool first=true;
                for(const auto id:group.descendants){
                    uint32_t index=0;const auto* tile=tiles->FindResidentOwner(id,index);
                    if(!tile||index>=tile->objects.size()){valid=false;break;}
                    const auto& object=tile->objects[index];
                    const auto flags=object.flags;
                    const auto shadingFlags=flags&(BuildingObject|NoShadow|NoRayTracing);
                    const auto objectCategory=OASIS_PRISM_CATEGORY(flags);
                    // Preserve exact physics and special rendering contracts.
                    // A proxy is not a collider; the original raw instances
                    // and their native collision geometry remain untouched.
                    if(!object.enabled||!(flags&NoCollision)||
                       (flags&(DynamicObject|DeformedObject|AlphaBlendObject|ShuttleCrt|ShuttleSpeedDust|SolarWorldVisual|OASIS_INSTANCE_FOREST_INSECTS))||
                       !materials.contains(object.material)){
                        valid=false;break;
                    }
                    if(first){commonFlags=shadingFlags;category=objectCategory;first=false;}
                    else if(commonFlags!=shadingFlags||category!=objectCategory){valid=false;break;}
                }
                if(!valid||(category!=1u&&category!=8u))continue;
                for(auto& instance:group.instances)
                    instance.flags=commonFlags|NoCollision|OASIS_INSTANCE_FOREST|OASIS_INSTANCE_TERRITORIAL|
                        (category==8u?OASIS_INSTANCE_LOD_VEGETATION:0u);
                XMFLOAT3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
                double optical2=0.;
                for(const auto& patch:live->second->patches){
                    lo={std::min(lo.x,patch.lo.x),std::min(lo.y,patch.lo.y),std::min(lo.z,patch.lo.z)};
                    hi={std::max(hi.x,patch.hi.x),std::max(hi.y,patch.hi.y),std::max(hi.z,patch.hi.z)};
                    group.error=std::max(group.error,patch.GeometricError());
                    const double e=patch.OpticalError();optical2+=e*e;
                }
                const XMFLOAT3 center{float((double(lo.x)+hi.x)*.5),float((double(lo.y)+hi.y)*.5),
                    float((double(lo.z)+hi.z)*.5)};
                const double x=std::max(double(center.x)-lo.x,double(hi.x)-center.x),
                    y=std::max(double(center.y)-lo.y,double(hi.y)-center.y),
                    z=std::max(double(center.z)-lo.z,double(hi.z)-center.z);
                group.support={center.x,center.y,center.z,std::nextafter(float(std::sqrt(x*x+y*y+z*z)),INFINITY)};
                group.error=std::max(group.error,float(std::sqrt(optical2)));
                if(!std::isfinite(group.error)||!std::isfinite(group.support.w))continue;
                territorialCoverage.push_back(std::move(group));
            }
        }
    }
    // A fast head/vehicle movement may invalidate yesterday's cut before
    // the next asynchronous selection is ready. In that case restore the
    // still-resident individual routes immediately, not an empty territory.
    const auto choose=[&](float errorPixels){
        std::vector<size_t> chosen;
        std::set<TerritorialCatalogKey> accepted;
        const auto parent=[](TerritorialCatalogKey k){
            auto& cell=k.second;++cell.level;
            const int64_t side=int64_t(1)<<(2u*cell.level);
            const auto align=[&](int64_t v){const auto r=v%side;return v-(r<0?r+side:r);};
            cell.x=align(cell.x);cell.z=align(cell.z);return k;
        };
        // Reverse map order is parent-first within each category.
        for(size_t index=territorialCoverage.size();index>0;){
            --index;const auto& group=territorialCoverage[index];
            auto ancestor=group.key;bool hidden=false;
            while(ancestor.second.level+1u<TerritorialPatrimony::Levels){
                ancestor=parent(ancestor);
                if(accepted.contains(ancestor)){hidden=true;break;}
            }
            if(hidden)continue;
            bool accept=xr.viewCount!=0u;
            for(uint32_t eye=0;eye<xr.viewCount&&accept;++eye){
                const auto& fov=xr.fov[eye];
                const float vertical=std::tan(fov.angleUp)-std::tan(fov.angleDown);
                const float horizontal=std::tan(fov.angleRight)-std::tan(fov.angleLeft);
                const float focal=std::max(horizontal>.01f?float(width)/horizontal:float(width)*.5f,
                                           vertical>.01f?float(height)/vertical:float(height)*.5f);
                const float d=Length(Sub(xr.eyes[eye],{group.support.x,group.support.y,group.support.z}));
                const float distance=d-group.support.w-1.e-5f*(d+group.support.w+1.f);
                accept=distance>xr.input.SceneNearZ()&&group.error*focal<=errorPixels*distance;
            }
            if(accept){chosen.push_back(index);accepted.insert(group.key);}
        }
        return chosen;
    };
    auto rasterChosen=choose(TerritorialRasterPixels),rayChosen=choose(TerritorialRayPixels);
    const bool rasterChanged=changed||rasterChosen!=territorialCoverageChosen;
    const bool rayChanged=changed||rayChosen!=territorialRayChosen;
    const auto makeCut=[&](const std::vector<size_t>& chosen,bool ray){
        auto cut=std::make_shared<TerritorialFrameCut>();
        for(const auto index:chosen){
            const auto& group=territorialCoverage[index];
            for(auto instance:group.instances){
                if(ray)instance.flags|=OASIS_INSTANCE_TERRITORIAL_RAY;
                cut->instances.push_back(instance);
            }
            cut->suppressed.insert(cut->suppressed.end(),group.descendants.begin(),group.descendants.end());
        }
        std::sort(cut->suppressed.begin(),cut->suppressed.end());
        if(std::adjacent_find(cut->suppressed.begin(),cut->suppressed.end())!=cut->suppressed.end())
            return std::shared_ptr<const TerritorialFrameCut>{};
        if(!ray&&tiles)for(const auto id:cut->suppressed){
            uint32_t local=0;const auto* tile=tiles->FindResidentOwner(id,local);
            if(!tile)return std::shared_ptr<const TerritorialFrameCut>{};
            auto& covered=cut->tiles[tile->key];
            covered.topology=tile->topologyGeneration;covered.revision=tile->revision;
            covered.indices.push_back(local);
        }
        for(auto& [key,covered]:cut->tiles)std::sort(covered.indices.begin(),covered.indices.end());
        return cut->instances.empty()?std::shared_ptr<const TerritorialFrameCut>{}:
            std::shared_ptr<const TerritorialFrameCut>(std::move(cut));
    };
    if(rasterChanged){
        territorialFrameCut=makeCut(rasterChosen,false);
        territorialCoverageChosen=std::move(rasterChosen);
    }
    if(rayChanged){
        territorialRayCut=makeCut(rayChosen,true);
        territorialRayChosen=std::move(rayChosen);
    }
    if(rasterChanged||rayChanged){
        auto parked=std::make_shared<TerritorialFrameCut>();
        if(territorialFrameCut&&territorialRayCut)
            std::set_intersection(territorialFrameCut->suppressed.begin(),territorialFrameCut->suppressed.end(),
                territorialRayCut->suppressed.begin(),territorialRayCut->suppressed.end(),
                std::back_inserter(parked->suppressed));
        if(parked->suppressed.empty())territorialParkCut.reset();
        else if(!territorialParkCut||territorialParkCut->suppressed!=parked->suppressed)
            territorialParkCut=std::move(parked);
        char line[320];std::snprintf(line,sizeof(line),
            "lod23_territorial_publish raster_nodes=%zu ray_nodes=%zu raster_routes=%zu ray_routes=%zu raster_suppressed=%zu ray_suppressed=%zu parked_both=%zu\r\n",
            territorialCoverageChosen.size(),territorialRayChosen.size(),
            territorialFrameCut?territorialFrameCut->instances.size():0u,
            territorialRayCut?territorialRayCut->instances.size():0u,
            territorialFrameCut?territorialFrameCut->suppressed.size():0u,
            territorialRayCut?territorialRayCut->suppressed.size():0u,
            territorialParkCut?territorialParkCut->suppressed.size():0u);DiagnosticLine(line);
    }
    const auto publish=[&](auto& previous,const auto& cut,DescriptorHandle& instanceSRV,DescriptorHandle& suppressionSRV){
        if(previous==cut)return;
        if(instanceSRV.Valid())ReleaseDescriptor(instanceSRV);
        if(suppressionSRV.Valid())ReleaseDescriptor(suppressionSRV);
        if(cut){
            const auto upload=[&](const void* data,size_t count,uint32_t stride){
                if(count>UINT32_MAX)Fail("LOD23 frame cut ABI");
                auto resource=Host(UINT64(count)*stride,D3D12_HEAP_TYPE_UPLOAD);
                void* mapped{};const D3D12_RANGE none{};
                Hr(resource->Map(0,&none,&mapped),"LOD23 frame cut map");
                std::memcpy(mapped,data,count*stride);resource->Unmap(0,nullptr);
                return Srv(resource.Get(),uint32_t(count),stride);
            };
            instanceSRV=upload(cut->instances.data(),cut->instances.size(),sizeof(GpuInstance));
            if(cut->suppressed.size()>=UINT32_MAX)Fail("LOD23 suppression ABI");
            std::vector<std::array<uint32_t,2>> identities;
            identities.reserve(cut->suppressed.size()+1u);
            identities.push_back({uint32_t(cut->suppressed.size()),0u});
            for(const auto id:cut->suppressed)identities.push_back({uint32_t(id),uint32_t(id>>32u)});
            suppressionSRV=upload(identities.data(),identities.size(),sizeof(identities.front()));
        }
        previous=cut;
    };
    publish(f.territorialCut,territorialFrameCut,f.territorialInstanceSRV,f.territorialSuppressionSRV);
    publish(f.territorialRayCut,territorialRayCut,f.territorialRayInstanceSRV,f.territorialRaySuppressionSRV);
    const uint64_t rasterCount=f.territorialCut?f.territorialCut->instances.size():0u;
    const uint64_t rayCount=f.territorialRayCut?f.territorialRayCut->instances.size():0u;
    const uint64_t native=uint64_t(sceneCache.StaticCount())+f.snapshotCount+f.residentCount;
    if(native+rasterCount+rayCount>=Invalid)Fail("LOD23 render ordinal capacity");
    f.cpuCount=uint32_t(native+rasterCount+rayCount);
}

bool Graphics::Impl::StageForestGrowth(std::span<const WorldObject> flora,std::span<const uint64_t> floraIds,
    std::span<const WorldObject> trees,std::span<const uint64_t> treeIds){
    if(flora.size()!=floraIds.size()||trees.size()!=treeIds.size())Fail("MG2 growth identity count");
    if(!forestStageOpen){
        if(!forestHierarchy&&(!flora.empty()||!trees.empty())){
            if(forestParts.empty())Fail("MG2 growth catalog unavailable");
            return false;
        }
        std::vector<Mg2GpuRegistryEntry> staged;
        staged.reserve(flora.size()+trees.size());
        const auto append=[&](auto objects,auto ids){
            for(size_t j=0;j<objects.size();++j){
                const auto& o=objects[j];const auto id=ids[j];
                const float lo=std::min({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)});
                const float hi=std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)});
                Mg2GpuRegistryEntry entry{};
                if(!o.enabled||!id||lo<1.e-6f||o.mesh>=meshes.ranges.size()||
                    (o.flags&(DeformedObject|AlphaBlendObject))||
                    !std::ranges::binary_search(forestParts,o.mesh,{},&Mg2ForestPart::mesh)||
                    !forestHierarchy->RegistryMesh(o.mesh,entry))Fail("MG2 staged vegetation lacks source");
                const auto& bound=meshes.ranges[o.mesh].bounds;
                entry.key={uint32_t(id),uint32_t(id>>32u),o.mesh|Mg2ResidentKeyBit,0u};
                entry.rootRadius={o.current.p.x,o.current.p.y,o.current.p.z,
                    (Length({bound.x,bound.y,bound.z})+bound.w)*hi};
                entry.rotation={o.current.q.x,o.current.q.y,o.current.q.z,o.current.q.w};
                entry.scale={o.scale.x,o.scale.y,o.scale.z,0.f};
                staged.push_back(entry);
            }
        };
        append(flora,floraIds);append(trees,treeIds);
        forestStagedEntries=std::move(staged);
        forestStageOpen=true;forestStageReady=forestStagedEntries.empty();
        forestStageCheckedSerial=0;++forestStageRevision;
        char line[160];std::snprintf(line,sizeof(line),"lod23_growth_stage routes=%zu revision=%llu\r\n",
            forestStagedEntries.size(),static_cast<unsigned long long>(forestStageRevision));DiagnosticLine(line);
    }
    if(forestStagedEntries.size()!=flora.size()+trees.size())Fail("MG2 staged growth transaction changed");
    if(forestStagedEntries.empty())return true;
    if(!forestResident)return false;
    if(forestStageCheckedSerial!=forestResident->serial){
        forestStageReady=std::ranges::all_of(forestStagedEntries,[&](const auto& entry){
            return forestResident->FindRoute(entry.key)!=Invalid;
        });
        forestStageCheckedSerial=forestResident->serial;
    }
    return forestStageReady;
}

void Graphics::Impl::CommitForestGrowth(){
    if(!forestStageOpen||!forestStageReady)Fail("MG2 growth committed before route readiness");
    DiagnosticLine("lod23_growth_commit mg2_ready=1 native_fallback=0\r\n");
    forestStagedEntries.clear();forestStageOpen=forestStageReady=false;
    ++forestStageRevision;
}

bool Graphics::Impl::InitialForestReady(const GpuSceneCache* tiles){
    if(forestParts.empty()||forestInitialReady)return true;
    if(!forestResident||forestInitialCheckedSerial==forestResident->serial)return false;
    forestInitialCheckedSerial=forestResident->serial;
    const auto required=[&](uint32_t mesh,uint32_t flags){
        return !(flags&(DeformedObject|AlphaBlendObject))&&
            std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh);
    };
    uint64_t missing=0;
    const auto statics=sceneCache.StaticInstances();
    for(uint32_t j=0;j<statics.size();++j){
        const auto& o=statics[j];
        if(required(o.mesh,o.flags)&&forestResident->FindRoute({j,o.id,o.mesh,0u})==Invalid)++missing;
    }
    if(tiles)for(const auto& [key,tile]:tiles->ResidentTiles())for(const auto& o:tile.objects){
        if(!o.enabled||!o.identity||!required(o.mesh,o.flags))continue;
        if(forestResident->FindRoute({uint32_t(o.identity),uint32_t(o.identity>>32u),o.mesh|Mg2ResidentKeyBit,0u})==Invalid)++missing;
    }
    forestInitialReady=missing==0;
    char line[160];std::snprintf(line,sizeof(line),"lod23_initial_publication ready=%u missing_routes=%llu\r\n",
        unsigned(forestInitialReady),static_cast<unsigned long long>(missing));DiagnosticLine(line);
    return forestInitialReady;
}

void Graphics::Impl::PrepareForestPages(Frame& f,const XRFrame& xr,const GpuSceneCache* tiles,bool nativeCampus){
    // The previous use of this frame slot has completed on every queue.
    f.forestPreviousTlas.reset();
    f.forestRaster.reset();
    if(!forestHierarchy&&forestBake.valid()){
        if(forestBake.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
            return;
        try{
            forestHierarchy=std::make_shared<const Mg2Pages>(forestBake.get());
            forestStatus.sourcePages=uint32_t(forestHierarchy->PageCount());
        }catch(const std::exception&){
            forestStatus.lastFailure="construction_pages_clod";
            Fail("MG2 mandatory source bake failed");
        }
    }
    if(!forestHierarchy)return;
    // Sample every render invocation, even while the producer is busy.
    // The prediction must not use the time since the last selection as a
    // velocity sample (that selection may have been idle for many seconds).
    const auto cameraSampleAt=QpcCounter();
    const double cameraDt=forestCameraSampleAt?
        QpcSeconds(cameraSampleAt-forestCameraSampleAt):0.;
    std::array<XMFLOAT3,MaxViews> cameraVelocity{};
    if(forestCameraSampleCount==xr.viewCount&&cameraDt>0.)
        for(uint32_t eye=0;eye<xr.viewCount;++eye)
            cameraVelocity[eye]=Scale(Sub(xr.eyes[eye],forestCameraSampleEyes[eye]),float(1./cameraDt));
    forestCameraSampleAt=cameraSampleAt;
    forestCameraSampleCount=xr.viewCount;
    for(uint32_t eye=0;eye<xr.viewCount;++eye)forestCameraSampleEyes[eye]=xr.eyes[eye];
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
        forestStatus.accelerationCacheBytes=forestBuild.result.accelerationCacheBytes;
        forestStatus.accelerationCacheClas=forestBuild.result.accelerationCacheClas;
        forestStatus.accelerationCacheBlas=forestBuild.result.accelerationCacheBlas;
        forestStatus.accelerationCacheEvictions=forestBuild.result.accelerationCacheEvictions;
        if(forestBuild.succeeded){
            forestRequestFailed=false;
            forestRetryUsed=false;
            forestStatus.lastFailure="aucune";
            forestStatus.buildCode=0;
            forestGenerations.push_back(forestBuild.candidate);
            forestPending=std::move(forestBuild.candidate);
        }else{
            forestStatus.lastFailure=forestBuild.result.lastFailure;
            forestStatus.buildCode=forestBuild.result.buildCode;
            forestRequestFailed=true;
            if(forestBuild.validationFailed&&foliageOmms.empty())forestBuildBackend=Mg2RayBackend::Standard;
            forestBuild.candidate.reset();
        }
    }
    PrepareTerritorialCatalog();
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
            PublishForestSourceViews(pending);
            const auto& routeView=pending.viewRequests[ForestGeneration::Routes];
            if(routeView.resource)pending.routeGpuAddress=routeView.resource->GetGPUVirtualAddress()+routeView.first*routeView.stride;
            pending.viewRequests={};
            pending.published=true;
            forestStatus.lastPublished=pending.backend;
            ++forestStatus.publications;
            forestStatus.variants=pending.variants;
            forestStatus.rasterMeshlets=pending.rasterMeshlets;
            forestStatus.routedInstances=uint32_t(pending.instanceRoutes.size());
            if(pending.geometryOwner){
                ++forestStatus.routeOnlyPublications;
                const auto* previousGeometry=forestResident?
                    (forestResident->geometryOwner?forestResident->geometryOwner.get():forestResident.get()):nullptr;
                if(pending.geometryOwner.get()!=previousGeometry)++forestStatus.generationReusePublications;
            }else if(pending.backend!=Mg2RayBackend::Standard){
                const uint32_t unique=uint32_t(pending.clasCache.size());
                forestStatus.clasBuilt+=forestBuild.builtClas;
                forestStatus.ommClasClusters+=forestBuild.result.ommClasClusters;
                forestStatus.ommClasTriangles+=forestBuild.result.ommClasTriangles;
                forestStatus.clasReused+=unique-forestBuild.builtClas;
                forestStatus.clasDuplicate+=pending.clasReferencesBuiltParts-unique;
                forestStatus.clasSkippedByBlasReuse+=pending.clusters-pending.clasReferencesBuiltParts;
                forestStatus.blasBuilt+=uint32_t(pending.newBlasParts.size());
                forestStatus.blasReused+=pending.variants-uint32_t(pending.newBlasParts.size());
            }
            if(!pending.sourceArenas.empty())forestStatus.immutableGeometryBytes=pending.sourceResidentBytes;
            if(pending.geometryUploadBytes)++forestStatus.immutableGeometryUploads;
            forestStatus.geometryUploadBytes+=pending.geometryUploadBytes;
            forestStatus.clasBytes=pending.clasCapacity;
            forestStatus.blasBytes=pending.blasCapacity;
            forestStatus.clasMs=forestBuild.result.clasMs;
            forestStatus.blasMs=forestBuild.result.blasMs;
            forestResident=std::move(forestPending);
            publishedThisFrame=true;
            forestStatus.generationAgeMs=float(1000.*QpcSeconds(QpcCounter()-forestSelection.requestedAt));
            forestStatus.scheduleLeadMs=forestStatus.generationAgeMs;
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
        ReleaseForestSourceViews(*generation);
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
    const bool canPublish=nativeCampus||forestStageOpen;
    if(canPublish)f.forestRaster=forestResident;

    // The diagnostic freezes only the geometry selection, not drawing.
    // Existing pages stay active with exactly the same triangles in A and B.
    // Allow the initial publication with DXR on, then stop all CLAS/BLAS
    // production. Never submit an acceleration build in the DXR-off phase.
    // Freeze applies after initial publication only. No native startup
    // frame may bypass the prerequisite; DXR-off still forbids new builds.
    if(!diagnosticDxrEnabled||((diagnosticFreezeMg2||(visibilityDiagnosticMask&32u)!=0u)&&bool(forestResident)))return;
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
            forestStatus.lastFailure="selection_ou_packing_mg2";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
            return;
        }
        forestStatus.selectWorkerMs=forestSelection.selectMs;
        forestStatus.gpuSelectWorkerMs=forestSelection.gpuSelectMs;
        forestStatus.gpuSelectDispatches=forestSelection.gpuDispatches;
        forestStatus.gpuSelectReadbackBytes=forestSelection.gpuReadbackBytes;
        forestStatus.gpuSelectCompactBytes=forestSelection.gpuCompactBytes;
        forestStatus.gpuSelectUniqueCuts=forestSelection.gpuUniqueCuts;
        forestStatus.gpuSelectVisitedNodes=forestSelection.gpuVisitedNodes;
        forestStatus.gpuSelectPrunedNodes=forestSelection.gpuPrunedNodes;
        forestStatus.packWorkerMs=forestSelection.packMs;
        forestStatus.sourceGeometryBytes=forestSelection.pages->SourceBytes();
        // Legacy diagnostic fields are deliberately zero: no raster-cut cache.
        forestStatus.rasterCacheBytes=forestStatus.rasterCacheHits=forestStatus.rasterCacheMisses=
            forestStatus.rasterCacheEvictions=forestStatus.rasterCacheEntries=0;
        forestStatus.candidateWorkerMs=forestSelection.candidateMs;
        forestStatus.registryUploadBytes=forestSelection.registryUploadBytes;
        forestStatus.registryGpuBytes=forestSelection.registryGpuBytes;
        forestStatus.registryInstances=forestSelection.registry.size();
        auto prepared=std::move(forestSelection.prepared);
        auto cut=std::move(forestSelection.cut);
        forestSelection.generations.clear();
        forestSelection.resident.reset();
        forestStatus.selectionCandidates=cut.candidates;
        forestStatus.selectionEligible=cut.eligible;
        forestStatus.selectionRouted=cut.selectedRoutes;
        if(!canPublish){
            ++forestStatus.discardedSelections;
            forestRefreshSelection=true;
            return;
        }
        if(cut.routes.empty()){
            if(forestResident&&!cut.selection.empty()&&cut.selection==forestResident->selection){
                ++forestStatus.unchangedSelections;
                forestRequestFailed=false;
                forestRetryUsed=false;
                forestStatus.lastFailure="aucune";
                forestStatus.buildCode=0;
            }
            // An unchanged cut measures selection latency, not a full build.
            // Retain the last observed end-to-end publication latency.
            forestStatus.scheduleLeadMs=std::max(forestStatus.generationAgeMs,
                float(1000.*QpcSeconds(QpcCounter()-forestSelection.requestedAt)));
            return;
        }
        if(!prepared){
            forestRequestFailed=true;
            forestStatus.lastFailure="preparation_cpu_mg2";
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
                    const auto pages=prepared->pages;
                    work->succeeded=UploadForestPages(cut,*prepared,*work->candidate,*work);
                    work->buildMs=float(1000.*QpcSeconds(QpcCounter()-begin));
                    if(work->succeeded){
                        work->candidate->selection=std::move(cut.selection);
                        work->candidate->instanceRoutes=std::move(cut.routes);
                        work->candidate->cpuRoutes=std::move(prepared->routes);
                        work->candidate->collectiveRoutes=std::move(prepared->collectiveRoutes);
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
                    if(work->succeeded){
                        // Log only validated geometry and do not traverse the
                        // source spans or perform diagnostic I/O on the VR thread.
                        const auto& source=*work->candidate;
                        if(!source.sourceArenas.empty()){
                            const UINT64 catalog=pages->SourceBytes(Mg2Pages::SourceStream::Vertices)+
                                pages->SourceBytes(Mg2Pages::SourceStream::Indices);
                            char line[768];
                            std::snprintf(line,sizeof(line),"mg2_a06_residency validated=1 persistent=1 catalog_bytes=%llu "
                                "resident_bytes=%llu demand_bytes=%llu unique_clusters=%u selected_cluster_refs=%u "
                                "uploaded_bytes=%llu retained_bytes=%llu evicted_bytes=%llu reused_gpu_bytes=0 "
                                "mapped_blocks=%llu gpu_va=%llu index_gpu_va=%llu vertex_virtual_bytes=%llu index_virtual_bytes=%llu arenas=%zu\r\n",
                                static_cast<unsigned long long>(catalog),
                                static_cast<unsigned long long>(source.sourceResidentBytes),
                                static_cast<unsigned long long>(source.sourceDemandBytes),
                                source.sourceUniqueClusters,source.clusters,
                                static_cast<unsigned long long>(source.geometryUploadBytes),
                                static_cast<unsigned long long>(source.sourceDemandBytes-source.geometryUploadBytes),
                                static_cast<unsigned long long>(source.sourceEvictedBytes),
                                static_cast<unsigned long long>(source.sourceBlocks.size()),
                                static_cast<unsigned long long>(source.sourceArenas.front().vertices->GetGPUVirtualAddress()),
                                static_cast<unsigned long long>(source.sourceArenas.front().indices->GetGPUVirtualAddress()),
                                static_cast<unsigned long long>(source.sourceArenas.front().vertices->GetDesc().Width),
                                static_cast<unsigned long long>(source.sourceArenas.front().indices->GetDesc().Width),
                                source.sourceArenas.size());
                            DiagnosticLine(line);
                            std::snprintf(line,sizeof(line),
                                "mg2_a07_shared validated=1 clusters=%u raster_meshlets=%u triangles=%u "
                                "raster_triangle_copy_bytes=0 raster_remap_copy_bytes=0 ray_index_bytes=%llu "
                                "generation_bytes=%u\r\n",
                                source.clusters,source.rasterMeshlets,source.triangles,
                                static_cast<unsigned long long>(source.triangles)*3ull*sizeof(uint32_t),source.bytes);
                            DiagnosticLine(line);
                        }
                        work->candidate->rayChangeBase=previous?previous->serial:0u;
                        work->candidate->serial=work->candidate->rayChangeBase+1u;
                        ForestGeneration::DiffRayRoutes(previous.get(),work->candidate.get(),work->candidate->rayChanges);
                        MatchForestShadowVariants(*work->candidate,previous.get());
                        if(previous){
                            // Exact geometry comparison, independent of the
                            // BLAS address. Prepare this beside ray deltas.
                            auto& next=*work->candidate;
                            size_t a=0,b=0;
                            const auto& oldRoutes=previous->instanceRoutes;
                            const auto& newRoutes=next.instanceRoutes;
                            while(a<oldRoutes.size()||b<newRoutes.size()){
                                auto oldKey=[&]{auto k=oldRoutes[a];k[3]=0;return k;};
                                auto newKey=[&]{auto k=newRoutes[b];k[3]=0;return k;};
                                if(b==newRoutes.size()||(a<oldRoutes.size()&&oldKey()<newKey())){
                                    next.shadowChanges.push_back(oldKey());++a;
                                }else if(a==oldRoutes.size()||newKey()<oldKey()){
                                    next.shadowChanges.push_back(newKey());++b;
                                }else{
                                    const auto v=newRoutes[b][3];
                                    if(v>=next.previousShadowVariants.size()||
                                       next.previousShadowVariants[v]!=oldRoutes[a][3])
                                        next.shadowChanges.push_back(newKey());
                                    ++a;++b;
                                }
                            }
                        }
                        const auto& candidate=*work->candidate;
                        if(!candidate.geometryOwner&&candidate.backend!=Mg2RayBackend::Standard){
                            // The producer fence has completed and all addresses
                            // are validated. Optional cache insertion cannot
                            // invalidate a successfully produced generation.
                            try{
                                auto& cache=forestAcceleration;
                                for(const auto& [key,entry]:candidate.clasCache){
                                    const auto saved=cache.clas.try_emplace(key,entry).first;
                                    cache.Remember(saved->second.storage.Get());
                                     cache.Remember(saved->second.slot.buffer.Get());
                                }
                                size_t cursor=0;
                                for(const auto& entry:candidate.blasEntries){
                                    const auto count=candidate.selection.at(cursor++);
                                    if(count>candidate.selection.size()-cursor)throw std::length_error("MG2 BLAS key");
                                    std::vector<uint32_t> key(candidate.selection.begin()+cursor,
                                        candidate.selection.begin()+cursor+count);
                                    cursor+=count;
                                    const auto saved=cache.blas.try_emplace(std::move(key),entry).first;
                                    cache.Remember(saved->second->storage.Get());
                                     cache.Remember(saved->second->slot.buffer.Get());
                                     for(const auto& [identity,cluster]:saved->second->clusterAddresses)
                                         cache.Remember(cluster.slot.buffer.Get());
                                    for(const auto& storage:saved->second->dependencies)cache.Remember(storage.Get());
                                }
                            }catch(...){forestAcceleration.Clear();}
                        }
                    }
                    // Failed candidates have never been consumed. If submitted,
                    // validation has already waited for their own GPU fence.
                    if(!work->succeeded){
                        work->candidate.reset();
                        // Retry after genuine memory pressure with only live
                        // generation ownership. No quality or triangle limit.
                        const std::string_view failure=work->result.lastFailure;
                        if(failure=="budget_vram_disponible"||failure.starts_with("allocation_")){
                            const auto required=work->result.buildRequiredBytes;
                            const auto available=work->result.buildAvailableBytes;
                            const auto pressure=required>available?required-available:required;
                            TrimForestSourceBlocks(*pages,pressure);
                            forestAcceleration.TrimCold(pressure);
                        }
                    }
                    work->result.accelerationCacheBytes=forestAcceleration.bytes;
                    work->result.accelerationCacheClas=forestAcceleration.clas.size();
                    work->result.accelerationCacheBlas=forestAcceleration.blas.size();
                    work->result.accelerationCacheEvictions=forestAcceleration.evictions;
                });
            forestBuild.requested=true;
        }catch(...){
            forestBuild.candidate.reset();
            forestStatus.lastFailure="lancement_producteur_mg2";
            forestStatus.buildCode=0;
            forestRequestFailed=true;
        }
        return;
    }
    const auto liveGenerations=std::count_if(forestGenerations.begin(),forestGenerations.end(),
        [](const auto& generation){return generation.use_count()>1;});
    // Requests are latest-only: no backlog of camera snapshots can grow
    // behind an outstanding selection/build. Do not enumerate instances
    // on the same frame as publication.
    if(!canPublish||publishedThisFrame||forestBuild.requested||forestPending||
       liveGenerations>=3u||forestStatus.retireBacklog>=MaxRetireBacklog)return;
    // Work-conserving latest-only scheduling: no wall-clock throttle.
    // Exact camera/scene changes are demand, not an extra visual LOD threshold.
    auto& selection=forestSelection;
    const uint32_t lodMask=(prismSceneMask&16u)&&(prismSceneMask&15u)!=15u?prismSceneMask:0u;
    const bool lodMaskChanged=selection.lodSelectionMask!=lodMask;
    // The diagnostic mask belongs to raster visibility and ray descriptors,
    // not to geometry residency. A mask-specific generation removes routes
    // which the next diagnostic block would otherwise render natively.
    // Keep one compatible complete cut across every diagnostic mask.
    const bool rebuild=selection.tileSource!=tiles||
        (tiles&&tiles->ForestTileResetPending());
    const bool sceneChanged=rebuild||(tiles&&!tiles->ForestDirtyTiles().empty())||
        !selection.staticCaptured||selection.staticGeneration!=sceneCache.ContentGeneration();
    const float nearPlane=xr.input.SceneNearZ();
    std::array<float,MaxViews> focal{};
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        const auto& fov=xr.fov[eye];
        const float vertical=std::tan(fov.angleUp)-std::tan(fov.angleDown);
        const float horizontal=std::tan(fov.angleRight)-std::tan(fov.angleLeft);
        focal[eye]=std::max(horizontal>.01f?float(width)/horizontal:float(width)*.5f,
                            vertical>.01f?float(height)/vertical:float(height)*.5f);
    }
    const float lookAhead=float(std::max(cameraDt,double(forestStatus.scheduleLeadMs)*.001));
    std::array<XMFLOAT3,MaxViews> eyeMotion{};
    bool cameraChanged=selection.eyeCount!=xr.viewCount||selection.nearPlane!=nearPlane;
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        eyeMotion[eye]=Scale(cameraVelocity[eye],lookAhead);
        const auto& a=selection.eyes[eye];
        const auto& b=xr.eyes[eye];
        const auto& oldMotion=selection.eyeMotion[eye];
        const auto& motion=eyeMotion[eye];
        cameraChanged|=a.x!=b.x||a.y!=b.y||a.z!=b.z||selection.focal[eye]!=focal[eye]||
            oldMotion.x!=motion.x||oldMotion.y!=motion.y||oldMotion.z!=motion.z;
        for(uint32_t row=0;row<4;++row)
            for(uint32_t col=0;col<4;++col)
                cameraChanged|=selection.vp[eye].m[row][col]!=xr.vp[eye].m[row][col];
    }
    const bool demandChanged=!selection.requestedAt||forestRefreshSelection||sceneChanged||
        cameraChanged||selection.backend!=forestBuildBackend||selection.pages!=forestHierarchy||
        selection.territorialRevision!=territorialCatalog.activeRevision||
        selection.parkedCut!=territorialParkCut||selection.stagedRevision!=forestStageRevision||
        (forestHierarchy->RetiredClusters()&&selection.retiredGenerations!=forestStatus.retiredGenerations);
    // Cached DXGI budget/usage: do not scan managed allocations to decide
    // whether a new selection is worth launching. The build still performs
    // its own authoritative budget check (including its reserve).
    const UINT64 budget=residency.Budget(),usage=residency.Used();
    const UINT64 available=budget>usage?budget-usage:0;
    const bool resourceProgress=forestStatus.retiredGenerations!=forestRetryRetired||
        (available>forestRetryAvailable&&
         (!forestStatus.buildRequiredBytes||available>=forestStatus.buildRequiredBytes));
    // One immediate recovery attempt permits the existing cold-cache trim to
    // take effect. Further identical failures wait for demand/resource progress.
    if(!demandChanged){
        if(!forestRequestFailed){++forestStatus.scheduleIdleSkips;return;}
        if(forestRetryUsed&&!resourceProgress){++forestStatus.scheduleFailureSkips;return;}
    }
    if(forestRequestFailed){
        ++forestStatus.scheduleRetries;
        forestRetryUsed=true;
    }else forestRetryUsed=false;
    forestRetryAvailable=available;
    forestRetryRetired=forestStatus.retiredGenerations;
    const auto requestStart=QpcCounter();
    // Only refresh changed territorial records; the previous worker has
    // already been joined above. No live scene references cross the boundary.
    const auto covered=[&](uint64_t id){
        return territorialParkCut&&std::binary_search(territorialParkCut->suppressed.begin(),territorialParkCut->suppressed.end(),id);
    };
    const auto insertRegistry=[&](const Mg2GpuRegistryEntry& entry){
        const auto slot=selection.registry.size();
        if(!selection.registrySlots.emplace(entry.key,slot).second)Fail("MG2 duplicate registry identity");
        selection.registry.push_back(entry);
        selection.registryScratchWords+=3u+entry.count/32u+uint32_t(entry.count%32u!=0u);
        selection.registryDirty.insert(slot);
    };
    const auto detachRegistry=[&](const Mg2InstanceRoute& key){
        const auto found=selection.registrySlots.find(key);
        if(found==selection.registrySlots.end())Fail("MG2 missing registry identity");
        const size_t slot=found->second,last=selection.registry.size()-1u;
        const auto removed=selection.registry[slot];
        selection.registryScratchWords-=3u+removed.count/32u+uint32_t(removed.count%32u!=0u);
        if(slot!=last){
            selection.registry[slot]=selection.registry[last];
            selection.registrySlots.at(selection.registry[slot].key)=slot;
            selection.registryDirty.insert(slot);
        }
        selection.registryDirty.erase(last);selection.registry.pop_back();selection.registrySlots.erase(found);
        return removed;
    };
    const auto consider=[&](std::vector<Mg2InstanceRoute>& inputs,
        uint32_t mesh,uint32_t flags,Mg2InstanceRoute key,XMFLOAT3 root,
        XMFLOAT4 rotation,XMFLOAT3 scale){
        if((flags&(DeformedObject|AlphaBlendObject))!=0u||
           !std::ranges::binary_search(forestParts,mesh,{},&Mg2ForestPart::mesh))return;
        const float lo=std::min({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        const float hi=std::max({std::abs(scale.x),std::abs(scale.y),std::abs(scale.z)});
        if(lo<1.e-6f||mesh>=meshes.ranges.size())return;
        Mg2GpuRegistryEntry entry{};
        if(!forestHierarchy->RegistryMesh(mesh,entry))return;
        const auto& bounds=meshes.ranges[mesh].bounds;
        entry.key=key;entry.rootRadius={root.x,root.y,root.z,(Length({bounds.x,bounds.y,bounds.z})+bounds.w)*hi};
        entry.rotation=rotation;entry.scale={scale.x,scale.y,scale.z,0.f};
        if(key[2]&Mg2ResidentKeyBit){
            const uint64_t id=uint64_t(key[0])|(uint64_t(key[1])<<32u);
            if(!selection.residentKeys.emplace(id,key).second)Fail("MG2 duplicate resident identity");
            if(covered(id)&&forestResident&&forestResident->FindRoute(key)!=Invalid)
                selection.parkedRegistry.emplace(key,entry);
            else insertRegistry(entry);
        }else insertRegistry(entry);
        inputs.push_back(key);
    };
    const auto removeInputs=[&](const std::vector<Mg2InstanceRoute>& inputs){
        for(const auto& key:inputs){
            if(key[2]&Mg2ResidentKeyBit)
                selection.residentKeys.erase(uint64_t(key[0])|(uint64_t(key[1])<<32u));
            if(!selection.parkedRegistry.erase(key))detachRegistry(key);
        }
    };
    // Remove staging ownership before capturing newly published tiles.
    const bool stageChanged=selection.stagedRevision!=forestStageRevision;
    if(stageChanged){
        for(const auto& key:selection.stagedInputs)detachRegistry(key);
        selection.stagedInputs.clear();
    }
    uint64_t scanned=0,changedTiles=0;
    const auto removeTileInputs=[&](GpuTileKey key){
        const auto old=selection.tileInputs.find(key);
        if(old!=selection.tileInputs.end()){
            removeInputs(old->second);
            selection.tileInputs.erase(old);
        }
    };
    const auto captureTile=[&](GpuTileKey key,const GpuResidentTile* tile){
        ++changedTiles;
        if(!tile)return;
        scanned+=tile->objects.size();
        std::vector<Mg2InstanceRoute> inputs;
        for(const auto& object:tile->objects){
            if(!object.enabled||!object.identity)continue;
            consider(inputs,object.mesh,object.flags,
                {uint32_t(object.identity),uint32_t(object.identity>>32u),object.mesh|Mg2ResidentKeyBit,0u},
                {object.position.x,object.position.y,object.position.z},
                {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
                {object.scale.x,object.scale.y,object.scale.z});
        }
        if(inputs.empty())selection.tileInputs.erase(key);
        else selection.tileInputs.insert_or_assign(key,std::move(inputs));
    };
    if(rebuild){
        for(const auto& [key,inputs]:selection.tileInputs)removeInputs(inputs);
        selection.tileInputs.clear();
    }
    if(tiles){
        const auto& resident=tiles->ResidentTiles();
        if(rebuild){
            for(const auto& [key,tile]:resident)captureTile(key,&tile);
        }else{
            // Remove all old tile memberships before inserting replacements.
            // An identity can cross a territorial boundary in either order.
            for(const auto& key:tiles->ForestDirtyTiles())removeTileInputs(key);
            for(const auto& key:tiles->ForestDirtyTiles()){
                const auto it=resident.find(key);
                captureTile(key,it==resident.end()?nullptr:&it->second);
            }
        }
        tiles->CommitForestTiles();
    }
    if(stageChanged){
        for(const auto& entry:forestStagedEntries){
            insertRegistry(entry);selection.stagedInputs.push_back(entry.key);
        }
        selection.stagedRevision=forestStageRevision;
    }
    selection.tileSource=tiles;
    // Reconcile coverage only at a cut/generation change. The last
    // published generation supplies an exact fallback variant; if a route
    // has never been built it stays active until it has one.
    const auto serial=forestResident?forestResident->serial:0u;
    if(selection.parkedCut!=territorialParkCut||selection.parkedGeneration!=serial){
        for(auto it=selection.parkedRegistry.begin();it!=selection.parkedRegistry.end();){
            const auto& key=it->first;
            const auto id=uint64_t(key[0])|(uint64_t(key[1])<<32u);
            if(covered(id)&&forestResident&&forestResident->FindRoute(key)!=Invalid){++it;continue;}
            insertRegistry(it->second);it=selection.parkedRegistry.erase(it);
        }
        if(territorialParkCut&&forestResident)for(const auto id:territorialParkCut->suppressed){
            const auto found=selection.residentKeys.find(id);
            if(found==selection.residentKeys.end())continue;
            const auto& key=found->second;
            if(selection.registrySlots.contains(key)&&forestResident->FindRoute(key)!=Invalid){
                auto entry=detachRegistry(key);
                selection.parkedRegistry.emplace(key,std::move(entry));
            }
        }
        selection.parkedCut=territorialParkCut;selection.parkedGeneration=serial;
    }
    uint64_t staticScanned=0;
    if(!selection.staticCaptured||selection.staticGeneration!=sceneCache.ContentGeneration()){
        removeInputs(selection.staticInputs);
        selection.staticInputs.clear();
        const auto statics=sceneCache.StaticInstances();
        staticScanned=statics.size();
        for(uint32_t i=0;i<statics.size();++i){
            const auto& object=statics[i];
            consider(selection.staticInputs,object.mesh,object.flags,{i,object.id,object.mesh,0u},
                {object.positionRadius.x,object.positionRadius.y,object.positionRadius.z},
                {object.rotation.x,object.rotation.y,object.rotation.z,object.rotation.w},
                {object.scale.x,object.scale.y,object.scale.z});
        }
        selection.staticGeneration=sceneCache.ContentGeneration();
        selection.staticCaptured=true;
    }
    selection.lodSelectionMask=lodMask;
    if(lodMaskChanged){
        char line[192];std::snprintf(line,sizeof(line),"lod_mg2_mask=%u registry_instances=%zu mask_independent_cut=1\r\n",lodMask,selection.registry.size());DiagnosticLine(line);
    }
    forestStatus.inputTilesChanged=changedTiles;
    forestStatus.inputObjectsScanned=scanned;
    forestStatus.inputStaticsScanned=staticScanned;
    forestStatus.inputFullRebuild=rebuild;
    // Snapshot and GPU-published objects still use native geometry.
    if(selection.registry.empty()&&selection.parkedRegistry.empty()&&territorialCatalog.active.empty()){
        forestInitialReady=true;
        forestStatus.traceForestRequestMs=float(1000.*QpcSeconds(QpcCounter()-requestStart));
        return;
    }
    forestSelection.pages=forestHierarchy;
    if(forestSelection.territorialRevision!=territorialCatalog.activeRevision){
        forestSelection.territorialNodes=territorialCatalog.active;
        forestSelection.territorialRevision=territorialCatalog.activeRevision;
    }
    forestSelection.retiredGenerations=forestStatus.retiredGenerations;
    const float requestInterval=forestSelection.requestedAt?
        float(1000.*QpcSeconds(requestStart-forestSelection.requestedAt)):0.f;
    forestSelection.eyeCount=xr.viewCount;
    forestSelection.nearPlane=nearPlane;
    forestSelection.focal=focal;
    for(uint32_t eye=0;eye<xr.viewCount;++eye){
        forestSelection.eyeMotion[eye]=eyeMotion[eye];
        forestSelection.vp[eye]=xr.vp[eye];
        forestSelection.eyes[eye]=xr.eyes[eye];
    }
    forestSelection.requestedAt=requestStart;
    forestSelection.backend=forestBuildBackend;
    forestSelection.acceleration=&forestAcceleration;
    forestSelection.resident=forestResident;
    forestSelection.generations.clear();
    for(const auto& generation:forestGenerations)
        if(generation->published)forestSelection.generations.push_back(generation);
    // The downstream raster pack still needs immutable source mesh bounds.
    if(forestSelection.meshBounds.size()!=meshes.ranges.size()){
        forestSelection.meshBounds.clear();
        forestSelection.meshBounds.reserve(meshes.ranges.size());
        for(const auto& range:meshes.ranges)forestSelection.meshBounds.push_back(range.bounds);
    }
    // Dedicated asynchronous work: JobSystem::Submit may execute inline when
    // its queue is full. Never let that fallback run MG2 packing in VR.
    forestRefreshSelection=false;
    try{
        forestSelection.completion=std::async(std::launch::async,[this,work=&forestSelection]{
            auto start=QpcCounter();
            PrepareTerritorialSelection(*work);
            work->candidateMs=float(1000.*QpcSeconds(QpcCounter()-start));
            start=QpcCounter();
            // The resident snapshot owns these immutable vectors throughout
            // the worker. Avoid duplicating large route tables in the VR loop.
            const std::span<const uint32_t> previous=work->resident?
                std::span<const uint32_t>(work->resident->selection):std::span<const uint32_t>{};
            const std::span<const Mg2InstanceRoute> previousRoutes=work->resident?
                std::span<const Mg2InstanceRoute>(work->resident->instanceRoutes):std::span<const Mg2InstanceRoute>{};
            work->gpuSelectMs=0.f;work->gpuDispatches=0;work->gpuReadbackBytes=0;
            work->gpuCompactBytes=work->gpuUniqueCuts=work->gpuVisitedNodes=work->gpuPrunedNodes=0;
            std::vector<Mg2InstanceRoute> frozen;
            frozen.reserve(work->parkedRegistry.size());
            for(const auto& [key,entry]:work->parkedRegistry){
                auto route=key;
                route[3]=work->resident?work->resident->FindRoute(key):Invalid;
                if(route[3]==Invalid)Fail("MG2 parked route lost fallback");
                frozen.push_back(route);
            }
            work->cut=work->pages->Select(work->registry,work->registryScratchWords,1.1f,
                [this,work](std::span<uint32_t> output,float error){
                    const auto begin=QpcCounter();
                    DispatchForestSelection(*work,output,error);
                    work->gpuSelectMs+=float(1000.*QpcSeconds(QpcCounter()-begin));
                    ++work->gpuDispatches;work->gpuReadbackBytes+=output.size_bytes();
                    work->gpuCompactBytes+=uint64_t(output[0])*sizeof(uint32_t);
                    work->gpuUniqueCuts+=output[1];work->gpuVisitedNodes+=output[2];work->gpuPrunedNodes+=output[3];
                },previous,previousRoutes,frozen);
            if(!frozen.empty()){
                char line[256];std::snprintf(line,sizeof(line),"lod23_registry active=%zu parked=%zu avoided_route_readback_words=%llu fallback_routes_retained=%zu\r\n",
                    work->registry.size(),frozen.size(),static_cast<unsigned long long>(2ull*frozen.size()),frozen.size());
                DiagnosticLine(line);
            }
            work->selectMs=float(1000.*QpcSeconds(QpcCounter()-start));
            start=QpcCounter();
            work->prepared=PackForestCut(*work);
            work->packMs=float(1000.*QpcSeconds(QpcCounter()-start));
        });
        forestSelection.requested=true;
        ++forestStatus.scheduleRequests;
        forestStatus.scheduleIntervalMs=requestInterval;
        if(requestInterval>0.f&&(!forestStatus.scheduleMinIntervalMs||
           requestInterval<forestStatus.scheduleMinIntervalMs))
            forestStatus.scheduleMinIntervalMs=requestInterval;
        forestStatus.schedulePredictionMs=lookAhead*1000.f;
    }catch(...){
        forestSelection.generations.clear();
        forestSelection.resident.reset();
        forestStatus.lastFailure="lancement_worker_mg2";
        forestStatus.buildCode=0;
        forestRequestFailed=true;
    }
    forestStatus.traceForestRequestMs=float(1000.*QpcSeconds(QpcCounter()-requestStart));
}

void Graphics::Impl::PrepareTerritorialSelection(ForestSelectionWork& work){
    auto& gpu=territorialGpu;
    const auto buffer=[&](UINT64 bytes,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
        const auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);const auto desc=Buffer(bytes,flags);
        ComPtr<ID3D12Resource> result;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
            IID_PPV_ARGS(&result)),"MG2 territorial buffer");return result;
    };
    const auto write=[](ID3D12Resource* resource,const void* data,size_t bytes){
        void* ptr{};const D3D12_RANGE none{};
        Hr(resource->Map(0,&none,&ptr),"MG2 territorial upload");
        std::memcpy(ptr,data,bytes);const D3D12_RANGE written{0,bytes};resource->Unmap(0,&written);
    };
    std::vector<uint32_t> chosen;
    uint32_t visited=0,readbackCount=0;
    if(gpu.revision!=work.territorialRevision){
        // Missing ancestors are traversal-only. A changed parent cannot
        // hide a ready child. Rebuild only on territorial revision changes.
        std::map<TerritorialCatalogKey,std::vector<TerritorialCatalogKey>> tree;
        const auto parent=[](TerritorialCatalogKey k){
            auto& cell=k.second;++cell.level;
            const int64_t side=int64_t(1)<<(2u*cell.level);
            const auto align=[&](int64_t v){const auto r=v%side;return v-(r<0?r+side:r);};
            cell.x=align(cell.x);cell.z=align(cell.z);return k;
        };
        for(const auto& [key,node]:work.territorialNodes){
            auto k=key;tree.try_emplace(k);
            while(k.second.level+1u<TerritorialPatrimony::Levels){
                const auto upper=parent(k);auto& children=tree[upper];
                if(std::find(children.begin(),children.end(),k)==children.end())children.push_back(k);
                k=upper;
            }
        }
        std::vector<TerritorialGpuSelection::Node> nodes;
        std::vector<uint32_t> roots;
        std::vector<TerritorialCatalogKey> keys;
        const auto emit=[&](auto&& self,const TerritorialCatalogKey& key)->void{
            if(nodes.size()>=(UINT32_MAX-2u)/2u)Fail("MG2 territorial dual-cut output ABI");
            const auto ordinal=uint32_t(nodes.size());
            TerritorialGpuSelection::Node record{};
            if(const auto it=work.territorialNodes.find(key);it!=work.territorialNodes.end()){
                const auto& node=*it->second.node;
                if(!node.patches.empty()&&!it->second.representations.empty()){
                    XMFLOAT3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
                    double opticalSquared=0.;
                    for(const auto& patch:node.patches){
                        lo={std::min(lo.x,patch.lo.x),std::min(lo.y,patch.lo.y),std::min(lo.z,patch.lo.z)};
                        hi={std::max(hi.x,patch.hi.x),std::max(hi.y,patch.hi.y),std::max(hi.z,patch.hi.z)};
                        record.geometric=std::max(record.geometric,patch.GeometricError());
                        const double e=patch.OpticalError();opticalSquared+=e*e;
                    }
                    const XMFLOAT3 center{float((double(lo.x)+hi.x)*.5),float((double(lo.y)+hi.y)*.5),
                        float((double(lo.z)+hi.z)*.5)};
                    const double x=std::max(double(center.x)-lo.x,double(hi.x)-center.x);
                    const double y=std::max(double(center.y)-lo.y,double(hi.y)-center.y);
                    const double z=std::max(double(center.z)-lo.z,double(hi.z)-center.z);
                    record.sphere={center.x,center.y,center.z,
                        std::nextafter(float(std::sqrt(x*x+y*y+z*z)),std::numeric_limits<float>::infinity())};
                    record.optical=float(std::sqrt(opticalSquared));
                    record.selectable=std::isfinite(record.sphere.w)&&std::isfinite(record.geometric)&&std::isfinite(record.optical);
                }
            }
            nodes.push_back(record);keys.push_back(key);
            for(const auto& child:tree.at(key))self(self,child);
            nodes[ordinal].escape=uint32_t(nodes.size());
        };
        for(const auto& [key,children]:tree)if(key.second.level+1u==TerritorialPatrimony::Levels){
            roots.push_back(uint32_t(nodes.size()));emit(emit,key);
        }
        if(nodes.empty()){
            gpu.nodes.Reset();gpu.roots.Reset();gpu.keys.clear();gpu.rootCount=0;
        }else{
            Commands commands;CommandsInit(commands,D3D12_COMMAND_LIST_TYPE_DIRECT);commands.Reset();
            std::vector<ComPtr<ID3D12Resource>> staging;
            const auto upload=[&](const void* data,size_t bytes){
                auto src=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);write(src.Get(),data,bytes);
                auto dst=buffer(bytes,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
                commands.list->CopyBufferRegion(dst.Get(),0,src.Get(),0,bytes);
                Transition(commands.list.Get(),dst.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                staging.push_back(std::move(src));return dst;
            };
            auto n=upload(nodes.data(),nodes.size()*sizeof(nodes.front()));
            auto r=upload(roots.data(),roots.size()*sizeof(uint32_t));
            commands.Submit(forestBuildQueue);WaitStartup(forestBuildQueue);
            gpu.nodes=std::move(n);gpu.roots=std::move(r);
            gpu.keys=std::move(keys);gpu.rootCount=uint32_t(roots.size());
        }
        gpu.revision=work.territorialRevision;
    }
    if(gpu.rootCount){
        if(!gpu.pso){
            D3D12_ROOT_PARAMETER parameters[5]{};
            for(uint32_t j=0;j<3;++j){parameters[j].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[j].Descriptor.ShaderRegister=j;}
            parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[4].Constants={0,0,5};
            D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=5;desc.pParameters=parameters;
            ComPtr<ID3DBlob> blob;Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,nullptr),"MG2 territorial root");
            ComPtr<ID3D12RootSignature> root;Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"MG2 territorial signature");
            const auto shader=Shader(L"Mg2TerritorialMain");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.CS={shader.data(),shader.size()};
            Hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&gpu.pso)),"MG2 territorial PSO");
            gpu.root=std::move(root);
        }
        std::array<XMFLOAT4,MaxViews*2> eyes{};
        for(uint32_t eye=0;eye<work.eyeCount;++eye){
            const auto& e=work.eyes[eye];const auto& m=work.eyeMotion[eye];
            eyes[eye*2]={e.x,e.y,e.z,work.focal[eye]};
            eyes[eye*2+1]={m.x,m.y,m.z,work.nearPlane};
        }
        if(!gpu.camera)gpu.camera=Host(sizeof(eyes),D3D12_HEAP_TYPE_UPLOAD);
        write(gpu.camera.Get(),eyes.data(),sizeof(eyes));
        const UINT64 capacity=(uint64_t(gpu.keys.size())*2u+2u)*sizeof(uint32_t);
        if(!gpu.output||gpu.output->GetDesc().Width<capacity){
            gpu.output=buffer(capacity,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST);
            gpu.readback=Host(capacity,D3D12_HEAP_TYPE_READBACK);
        }
        const uint32_t zero[2]{};
        auto clear=Host(sizeof(zero),D3D12_HEAP_TYPE_UPLOAD);write(clear.Get(),zero,sizeof(zero));
        Commands commands;CommandsInit(commands,D3D12_COMMAND_LIST_TYPE_DIRECT);commands.Reset();
        auto* list=commands.list.Get();
        list->CopyBufferRegion(gpu.output.Get(),0,clear.Get(),0,sizeof(zero));
        Transition(list,gpu.output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(gpu.root.Get());list->SetPipelineState(gpu.pso.Get());
        list->SetComputeRootShaderResourceView(0,gpu.nodes->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(1,gpu.roots->GetGPUVirtualAddress());
        list->SetComputeRootShaderResourceView(2,gpu.camera->GetGPUVirtualAddress());
        list->SetComputeRootUnorderedAccessView(3,gpu.output->GetGPUVirtualAddress());
        const UINT64 groups=(uint64_t(gpu.rootCount)+63u)/64u;
        const uint32_t x=uint32_t(std::min<UINT64>(groups,65535u)),y=uint32_t((groups+x-1u)/x);
        if(y>65535u)Fail("MG2 territorial dispatch ABI");
        const uint32_t settings[]{gpu.rootCount,work.eyeCount,x,std::bit_cast<uint32_t>(TerritorialRasterPixels),
            std::bit_cast<uint32_t>(TerritorialRayPixels)};
        list->SetComputeRoot32BitConstants(4,5,settings,0);list->Dispatch(x,y,2);
        Transition(list,gpu.output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(gpu.readback.Get(),0,gpu.output.Get(),0,sizeof(zero));
        commands.Submit(forestBuildQueue);WaitStartup(forestBuildQueue);
        void* ptr{};const D3D12_RANGE header{0,sizeof(zero)},none{};
        Hr(gpu.readback->Map(0,&header,&ptr),"MG2 territorial header");
        const auto count=static_cast<const uint32_t*>(ptr)[0];readbackCount=count;visited=static_cast<const uint32_t*>(ptr)[1];
        gpu.readback->Unmap(0,&none);
        if(uint64_t(count)>uint64_t(gpu.keys.size())*2u)Fail("MG2 territorial compact count");
        if(count){
            commands.Reset();
            commands.list->CopyBufferRegion(gpu.readback.Get(),0,gpu.output.Get(),sizeof(zero),UINT64(count)*sizeof(uint32_t));
            commands.Submit(forestBuildQueue);WaitStartup(forestBuildQueue);
            const D3D12_RANGE read{0,SIZE_T(count)*sizeof(uint32_t)};
            Hr(gpu.readback->Map(0,&read,&ptr),"MG2 territorial cut");
            chosen.assign(static_cast<const uint32_t*>(ptr),static_cast<const uint32_t*>(ptr)+count);
            gpu.readback->Unmap(0,&none);std::sort(chosen.begin(),chosen.end());
            chosen.erase(std::unique(chosen.begin(),chosen.end()),chosen.end());
        }
    }
    std::map<uint32_t,TerritorialDrawRoute> desired;
    for(const auto index:chosen){
        const auto& key=gpu.keys.at(index);
        const auto& node=work.territorialNodes.at(key);
        for(const auto& rep:node.representations){
            GpuInstance instance{};
            instance.positionRadius={rep.origin.x,rep.origin.y,rep.origin.z,
                Length({rep.bounds.x,rep.bounds.y,rep.bounds.z})+rep.bounds.w};
            instance.rotation={0,0,0,1};instance.scale={1,1,1};
            instance.mesh=rep.mesh;instance.material=rep.material;instance.id=rep.mesh;
            instance.flags=NoCollision; // category flags added at consumer admission
            desired.emplace(rep.mesh,TerritorialDrawRoute{key,node.node,Mg2CollectiveKey(rep.mesh),instance,rep.bounds});
        }
    }
    // Actual MG2 registry; native entries remain until substitution is ready.
    for(const auto& [mesh,route]:work.collectiveInputs)if(!desired.contains(mesh)){
        const auto slot=work.registrySlots.at(route.key),last=work.registry.size()-1u;
        const auto count=work.registry[slot].count;
        work.registryScratchWords-=3u+count/32u+uint32_t(count%32u!=0u);
        if(slot!=last){
            work.registry[slot]=work.registry[last];
            work.registrySlots.at(work.registry[slot].key)=slot;work.registryDirty.insert(slot);
        }
        work.registryDirty.erase(last);work.registry.pop_back();work.registrySlots.erase(route.key);
    }
    for(const auto& [mesh,route]:desired)if(!work.collectiveInputs.contains(mesh)){
        Mg2GpuRegistryEntry entry{};
        if(!work.pages->RegistryMesh(mesh,entry))Fail("MG2 collective mesh missing");
        entry.key=route.key;
        const auto& instance=route.instance;
        entry.rootRadius={instance.positionRadius.x,instance.positionRadius.y,instance.positionRadius.z,instance.positionRadius.w};
        entry.rotation={0,0,0,1};entry.scale={1,1,1,0};
        const auto slot=work.registry.size();
        if(!work.registrySlots.emplace(entry.key,slot).second)Fail("MG2 collective duplicate route");
        work.registry.push_back(entry);work.registryDirty.insert(slot);
        work.registryScratchWords+=3u+entry.count/32u+uint32_t(entry.count%32u!=0u);
    }
    work.collectiveInputs=std::move(desired);
    if(gpu.rootCount){
        char line[256];std::snprintf(line,sizeof(line),
            "lod23_territorial_cut roots=%u visited=%u selected_nodes=%zu collective_routes=%zu compact_readback_bytes=%llu parked_descendants=%zu\r\n",
            gpu.rootCount,visited,chosen.size(),work.collectiveInputs.size(),
            static_cast<unsigned long long>((uint64_t(readbackCount)+2u)*sizeof(uint32_t)),work.parkedRegistry.size());
        DiagnosticLine(line);
    }
}

// Selection uses the same serialized producer queue as CLAS/BLAS builds.
// All waits are on the selection worker; the VR thread keeps the published cut.
void Graphics::Impl::DispatchForestSelection(ForestSelectionWork& work,std::span<uint32_t> output,float error){
    const auto& pages=*work.pages;
    const auto& requests=work.registry;
    const auto makeBuffer=[&](UINT64 bytes,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state){
        const auto heap=Heap(D3D12_HEAP_TYPE_DEFAULT);const auto desc=Buffer(bytes,flags);
        ComPtr<ID3D12Resource> result;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
            IID_PPV_ARGS(&result)),"MG2 selection buffer");
        return result;
    };
    const auto transition=[](ID3D12GraphicsCommandList* list,ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
        list->ResourceBarrier(1,&barrier);
    };
    const auto writeHost=[](ID3D12Resource* resource,const void* data,size_t bytes){
        void* mapped{};const D3D12_RANGE noRead{0,0};
        Hr(resource->Map(0,&noRead,&mapped),"MG2 selection upload map");
        std::memcpy(mapped,data,bytes);
        const D3D12_RANGE written{0,bytes};resource->Unmap(0,&written);
    };
    if(!forestSelectPSO){
        D3D12_ROOT_PARAMETER all[14]{};
        for(uint32_t i=0;i<9;++i){
            all[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;
            all[i].Descriptor.ShaderRegister=i;
        }
        for(uint32_t i=0;i<4;++i){
            all[9u+i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
            all[9u+i].Descriptor.ShaderRegister=i;
        }
        all[13].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        all[13].Constants={0,0,8};
        D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=14;desc.pParameters=all;
        ComPtr<ID3DBlob> blob;
        Hr(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,
            &blob,nullptr),"MG2 selection root serialization");
        ComPtr<ID3D12RootSignature> signature;
        Hr(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),
            IID_PPV_ARGS(&signature)),"MG2 selection root");
        const auto pipeline=[&](const wchar_t* name){
            const auto shader=Shader(name);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=signature.Get();
            pso.CS={shader.data(),shader.size()};
            ComPtr<ID3D12PipelineState> state;
            Hr(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&state)),"MG2 selection PSO");
            return state;
        };
        auto select=pipeline(L"Mg2SelectMain"),clear=pipeline(L"Mg2SelectClear"),compact=pipeline(L"Mg2SelectCompact"),prepare=pipeline(L"Mg2PrepareMain");
        forestPreparePSO=std::move(prepare);
        forestSelectRoot=std::move(signature);forestSelectPSO=std::move(select);
        forestSelectClearPSO=std::move(clear);forestSelectCompactPSO=std::move(compact);
    }
    if(forestSelectCatalog!=work.pages){
        Commands commands;CommandsInit(commands,D3D12_COMMAND_LIST_TYPE_DIRECT);commands.Reset();
        std::vector<ComPtr<ID3D12Resource>> uploads;
        const bool append=forestSelectCatalog&&forestSelectCatalog->IsPrefixOf(pages);
        const auto upload=[&](const auto& data,const auto* oldData,const ComPtr<ID3D12Resource>& oldBuffer){
            using Element=typename std::remove_reference_t<decltype(data)>::value_type;
            const size_t bytes=data.size()*sizeof(Element);
            const size_t prefix=append&&oldData&&oldBuffer?oldData->size()*sizeof(Element):0u;
            if(prefix>bytes)Fail("MG2 catalog prefix mismatch");
            if(prefix==bytes&&oldBuffer&&append)return oldBuffer;
            const uint32_t zero=0;
            const auto buffer=makeBuffer(std::max<size_t>(bytes,sizeof(zero)),
                D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
            if(prefix){
                transition(commands.list.Get(),oldBuffer.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_COPY_SOURCE);
                commands.list->CopyBufferRegion(buffer.Get(),0,oldBuffer.Get(),0,prefix);
                transition(commands.list.Get(),oldBuffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            const size_t tail=bytes?bytes-prefix:sizeof(zero);
            if(tail){
                const void* src=bytes?static_cast<const void*>(reinterpret_cast<const std::byte*>(data.data())+prefix):
                    static_cast<const void*>(&zero);
                auto staging=Host(tail,D3D12_HEAP_TYPE_UPLOAD);writeHost(staging.Get(),src,tail);
                commands.list->CopyBufferRegion(buffer.Get(),prefix,staging.Get(),0,tail);
                uploads.push_back(std::move(staging));
            }
            transition(commands.list.Get(),buffer.Get(),D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            return buffer;
        };
        const auto* old=forestSelectCatalog.get();
        auto pageBuffer=upload(pages.GpuPages(),old?&old->GpuPages():nullptr,forestSelectPages);
        auto clusterBuffer=upload(pages.GpuClusters(),old?&old->GpuClusters():nullptr,forestSelectClusters);
        auto structuralBuffer=upload(pages.GpuStructural(),old?&old->GpuStructural():nullptr,forestSelectStructural);
        auto nodeBuffer=upload(pages.GpuNodes(),old?&old->GpuNodes():nullptr,forestSelectNodes);
        commands.Submit(forestBuildQueue);WaitStartup(forestBuildQueue);
        forestSelectPages=std::move(pageBuffer);forestSelectClusters=std::move(clusterBuffer);
        forestSelectStructural=std::move(structuralBuffer);forestSelectNodes=std::move(nodeBuffer);
        forestSelectCatalog=work.pages;
    }
    const UINT64 inputBytes=UINT64(requests.size())*sizeof(Mg2GpuInstance),outputBytes=output.size_bytes();
    if(!forestSelectInput||forestSelectInput->GetDesc().Width<inputBytes)
        forestSelectInput=makeBuffer(inputBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    const UINT64 registryBytes=UINT64(requests.size())*sizeof(Mg2GpuRegistryEntry);
    if(!forestSelectRegistry||forestSelectRegistry->GetDesc().Width<registryBytes){
        UINT64 capacity=forestSelectRegistry?forestSelectRegistry->GetDesc().Width:sizeof(Mg2GpuRegistryEntry);
        while(capacity<registryBytes)capacity*=2u;
        forestSelectRegistry=makeBuffer(capacity,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COMMON);
        work.registryUploadValid=false;
    }
    work.registryGpuBytes=forestSelectRegistry->GetDesc().Width;
    struct CameraEye { XMFLOAT4 planes[6],positionFocal,motionNear; };
    std::array<CameraEye,MaxViews> camera{};
    static_assert(sizeof(CameraEye)==128);
    for(uint32_t eye=0;eye<work.eyeCount;++eye){
        const auto m=XMMatrixTranspose(XMLoadFloat4x4(&work.vp[eye]));
        const XMVECTOR planes[]{m.r[0]+m.r[3],m.r[3]-m.r[0],m.r[1]+m.r[3],m.r[3]-m.r[1],m.r[3]-m.r[2],m.r[2]};
        for(uint32_t j=0;j<6;++j)XMStoreFloat4(&camera[eye].planes[j],planes[j]);
        const auto& e=work.eyes[eye];const auto& motion=work.eyeMotion[eye];
        camera[eye].positionFocal={e.x,e.y,e.z,work.focal[eye]};
        camera[eye].motionNear={motion.x,motion.y,motion.z,work.nearPlane};
    }
    if(!forestSelectCamera)forestSelectCamera=Host(sizeof(camera),D3D12_HEAP_TYPE_UPLOAD);
    writeHost(forestSelectCamera.Get(),camera.data(),sizeof(camera));
    // Reuse the published GPU route table; upload only its variant lookup.
    const uint32_t priorVariants=work.resident&&work.resident->routeGpuAddress?work.resident->variants:0u;
    if(priorVariants&&(!forestSelectPriorFirst||forestSelectPriorSerial!=work.resident->serial)){
        std::vector<uint32_t> first;
        const auto& previous=work.resident->selection;
        for(size_t j=0;j<previous.size();){
            const auto count=previous[j++];
            if(!count||count>previous.size()-j)Fail("MG2 invalid previous selection");
            first.push_back(previous[j]);j+=count;
        }
        if(first.size()!=priorVariants)Fail("MG2 invalid previous variant count");
        if(!forestSelectPriorFirst||forestSelectPriorFirst->GetDesc().Width<first.size()*sizeof(uint32_t))
            forestSelectPriorFirst=Host(first.size()*sizeof(uint32_t),D3D12_HEAP_TYPE_UPLOAD);
        writeHost(forestSelectPriorFirst.Get(),first.data(),first.size()*sizeof(uint32_t));
        forestSelectPriorSerial=work.resident->serial;
    }
    const bool reuseOutput=forestSelectOutput&&forestSelectOutput->GetDesc().Width>=outputBytes;
    if(!reuseOutput){
        auto readback=Host(outputBytes,D3D12_HEAP_TYPE_READBACK);
        auto gpu=makeBuffer(outputBytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON);
        forestSelectOutput=std::move(gpu);forestSelectReadback=std::move(readback);
    }
    const UINT64 scratchBytes=(work.registryScratchWords+1u)*sizeof(uint32_t);
    UINT64 hashCapacity=1u;
    while(hashCapacity<2u*UINT64(requests.size()))hashCapacity*=2u;
    if(hashCapacity>UINT32_MAX)throw std::length_error("MG2 selection hash ABI");
    const auto ensure=[&](ComPtr<ID3D12Resource>& buffer,UINT64 bytes){
        if(!buffer||buffer->GetDesc().Width<bytes)
            buffer=makeBuffer(bytes,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON);
    };
    ensure(forestSelectScratch,scratchBytes);ensure(forestSelectHash,hashCapacity*4u);
    Commands commands;CommandsInit(commands,D3D12_COMMAND_LIST_TYPE_DIRECT);commands.Reset();
    auto* list=commands.list.Get();
    struct Run {size_t first,count;};
    std::vector<Run> runs;
    if(!work.registryUploadValid)runs.push_back({0,requests.size()});
    else for(const size_t slot:work.registryDirty){
        if(slot>=requests.size())continue;
        if(!runs.empty()&&runs.back().first+runs.back().count==slot)++runs.back().count;
        else runs.push_back({slot,1});
    }
    work.registryUploadBytes=0;
    for(const auto& run:runs)work.registryUploadBytes+=run.count*sizeof(Mg2GpuRegistryEntry);
    ComPtr<ID3D12Resource> registryUpload;
    if(work.registryUploadBytes){
        registryUpload=Host(work.registryUploadBytes,D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{};const D3D12_RANGE none{0,0};
        Hr(registryUpload->Map(0,&none,&mapped),"MG2 registry upload");
        UINT64 offset=0;
        transition(list,forestSelectRegistry.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        for(const auto& run:runs){
            const UINT64 bytes=run.count*sizeof(Mg2GpuRegistryEntry);
            std::memcpy(static_cast<uint8_t*>(mapped)+offset,requests.data()+run.first,size_t(bytes));
            list->CopyBufferRegion(forestSelectRegistry.Get(),run.first*sizeof(Mg2GpuRegistryEntry),registryUpload.Get(),offset,bytes);
            offset+=bytes;
        }
        const D3D12_RANGE written{0,SIZE_T(offset)};registryUpload->Unmap(0,&written);
        transition(list,forestSelectRegistry.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // Buffer state decays to COMMON at the completed ExecuteCommandLists
    // boundary. UAV access promotes it on first write; the copy below needs
    // the actual UAV-to-COPY_SOURCE barrier.
    list->SetComputeRootSignature(forestSelectRoot.Get());
    list->SetComputeRootShaderResourceView(0,forestSelectPages->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1,forestSelectClusters->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(3,forestSelectStructural->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4,forestSelectNodes->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(5,forestSelectRegistry->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(6,priorVariants?work.resident->routeGpuAddress:forestSelectRegistry->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(7,priorVariants?forestSelectPriorFirst->GetGPUVirtualAddress():forestSelectRegistry->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(8,forestSelectCamera->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(9,forestSelectScratch->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(10,forestSelectHash->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(11,forestSelectOutput->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(12,forestSelectInput->GetGPUVirtualAddress());
    const auto dispatch=[&](ID3D12PipelineState* pso,UINT64 groups){
        const uint32_t x=uint32_t(std::min<UINT64>(groups,65535u));
        const UINT64 y=(groups+x-1u)/x;
        if(y>65535u)throw std::length_error("MG2 selection dispatch ABI");
        const uint32_t settings[]{std::bit_cast<uint32_t>(error),uint32_t(requests.size()),x,
            uint32_t(hashCapacity-1u),uint32_t(4u+2u*requests.size()),uint32_t(work.registryScratchWords),priorVariants,work.eyeCount};
        list->SetPipelineState(pso);list->SetComputeRoot32BitConstants(13,8,settings,0);
        list->Dispatch(x,uint32_t(y),1);
    };
    const auto uav=[&]{
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1,&b);
    };
    dispatch(forestSelectClearPSO.Get(),(hashCapacity+63u)/64u);uav();
    dispatch(forestPreparePSO.Get(),(requests.size()+63u)/64u);uav();
    transition(list,forestSelectInput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetComputeRootShaderResourceView(2,forestSelectInput->GetGPUVirtualAddress());
    dispatch(forestSelectPSO.Get(),requests.size());uav();
    dispatch(forestSelectCompactPSO.Get(),(requests.size()+63u)/64u);
    transition(list,forestSelectOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(forestSelectReadback.Get(),0,forestSelectOutput.Get(),0,outputBytes);
    work.registryUploadValid=false;
    commands.Submit(forestBuildQueue);WaitStartup(forestBuildQueue);
    work.registryUploadValid=true;work.registryDirty.clear();
    void* mapped{};const D3D12_RANGE read{0,SIZE_T(outputBytes)},empty{0,0};
    Hr(forestSelectReadback->Map(0,&read,&mapped),"MG2 selection readback");
    const auto used=static_cast<const uint32_t*>(mapped)[0];
    if(used<4u||used>output.size()){
        forestSelectReadback->Unmap(0,&empty);Fail("MG2 selection compact size");
    }
    std::memcpy(output.data(),mapped,size_t(used)*sizeof(uint32_t));
    forestSelectReadback->Unmap(0,&empty);
}

bool Graphics::Impl::EnsureForestSourceArenas(const Mg2Pages& catalog){
    auto& pool=forestSourcePool;
    constexpr UINT64 TileBytes=Mg2Pages::SourceTileBytes;
    static_assert(TileBytes==D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    if(pool.admittedSegments>=catalog.SourceSegments().size())return true;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    if(FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)))||
       options.TiledResourcesTier<D3D12_TILED_RESOURCES_TIER_2)return false;
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT va{};
    const bool hasVa=SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,&va,sizeof(va)))&&
        va.MaxGPUVirtualAddressBitsPerResource;
    const auto streams={ForestSourceStream::Vertices,ForestSourceStream::Indices};
    const auto reserve=[&](ForestSourceStream stream,UINT64 first,UINT64 end,ComPtr<ID3D12Resource>& resource){
        if(end<=first||first%TileBytes||end%TileBytes)return false;
        const UINT64 required=end-first;
        const UINT64 remaining=Mg2Pages::SourceAddressLimit(stream)-first;
        UINT64 target=required;
        if(hasVa){
            const auto bits=std::min(va.MaxGPUVirtualAddressBitsPerResource,63u);
            const UINT64 limit=(UINT64(1)<<bits)&~(TileBytes-1u);
            target=std::max(required,std::min(remaining,limit));
        }
        for(;;){
            ComPtr<ID3D12Resource> candidate;
            const auto desc=Buffer(target);
            if(SUCCEEDED(device->CreateReservedResource(&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,
                    IID_PPV_ARGS(&candidate)))){
                UINT tiles=0;device->GetResourceTiling(candidate.Get(),&tiles,nullptr,nullptr,nullptr,0,nullptr);
                if(tiles==target/TileBytes){resource=std::move(candidate);return true;}
            }
            if(target==required)return false;
            target=std::max(required,(target/2u)&~(TileBytes-1u));
        }
    };
    for(;pool.admittedSegments<catalog.SourceSegments().size();++pool.admittedSegments){
        const auto& segment=catalog.SourceSegments()[pool.admittedSegments];
        bool fits=!pool.arenas.empty();
        if(fits)for(const auto stream:streams){
            const auto k=uint32_t(stream);const auto& a=pool.arenas.back();
            fits&=segment.begin[k]>=a.end[k]&&segment.end[k]>=a.begin[k]&&
                segment.end[k]-a.begin[k]<=a.resources[k]->GetDesc().Width;
        }
        if(fits){pool.arenas.back().end=segment.end;continue;}
        ForestSourcePool::Arena arena;arena.begin=segment.begin;arena.end=segment.end;
        for(const auto stream:streams){
            const auto k=uint32_t(stream);
            if(arena.begin[k]%Mg2Pages::SourceStride(stream)||
               !reserve(stream,arena.begin[k],arena.end[k],arena.resources[k]))return false;
        }
        if(pool.arenas.size()>=UINT32_MAX)return false;
        pool.arenas.push_back(std::move(arena));
    }
    return true;
}

void Graphics::Impl::PublishForestSourceViews(ForestGeneration& generation){
    if(generation.geometryOwner||generation.sourceArenas.empty())return;
    std::vector<std::array<uint32_t,2>> table;
    table.reserve(generation.sourceArenas.size());
    for(size_t i=0;i<generation.sourceArenas.size();++i){
        auto& arena=generation.sourceArenas[i];
        // Every domain slot must contain a valid descriptor. The first
        // arena owns the original slots, extra arenas own separate handles.
        auto& vertexView=i?arena.vertexView:generation.views[ForestGeneration::Vertices];
        auto& indexView=i?arena.indexView:generation.views[ForestGeneration::Triangles];
        vertexView=Srv(arena.vertices.Get(),arena.vertexCount,sizeof(Vertex));
        indexView=Srv(arena.indices.Get(),arena.indexCount,sizeof(uint32_t));
        table.push_back({Physical(vertexView),Physical(indexView)});
    }
    generation.sourceArenaTable=Host(table.size()*sizeof(table.front()),D3D12_HEAP_TYPE_UPLOAD);
    void* mapped{};const D3D12_RANGE none{};
    Hr(generation.sourceArenaTable->Map(0,&none,&mapped),"MG2 arena table map");
    std::memcpy(mapped,table.data(),table.size()*sizeof(table.front()));
    generation.sourceArenaTable->Unmap(0,nullptr);
    generation.views[ForestGeneration::Remap]=Srv(generation.sourceArenaTable.Get(),
        uint32_t(table.size()),sizeof(table.front()));
}

void Graphics::Impl::ReleaseForestSourceViews(ForestGeneration& generation){
    if(generation.geometryOwner)return;
    for(auto& arena:generation.sourceArenas){
        if(arena.vertexView.Valid())ReleaseDescriptor(arena.vertexView);
        if(arena.indexView.Valid())ReleaseDescriptor(arena.indexView);
    }
}

uint64_t Graphics::Impl::TrimForestSourceBlocks(const Mg2Pages& catalog,uint64_t pressure){
    auto& pool=forestSourcePool;
    constexpr UINT64 TileBytes=Mg2Pages::SourceTileBytes;
    // Cold does not mean dead. Keep reusable live source tiles while memory
    // permits. A block held by any generation is never a candidate.
    struct Candidate {ForestSourceTile key;uint64_t lastUse;bool dead;};
    std::vector<Candidate> candidates;
    for(const auto& [key,block]:pool.blocks){
        if(block.use_count()!=1)continue;
        const bool dead=!block->ready||!catalog.SourceRangeLive(block->stream,
            UINT64(block->first)*TileBytes,UINT64(block->count)*TileBytes);
        if(dead||pressure)candidates.push_back({key,block->lastUse,dead});
    }
    std::sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b){
        if(a.dead!=b.dead)return a.dead>b.dead;
        return a.lastUse<b.lastUse;
    });
    uint64_t releasedBytes=0;
    std::vector<std::shared_ptr<ForestSourceBlock>> released;
    released.reserve(candidates.size());
    for(const auto& candidate:candidates){
        if(!candidate.dead&&releasedBytes>=pressure)break;
        auto it=pool.blocks.find(candidate.key);
        if(it==pool.blocks.end()||it->second.use_count()!=1)continue;
        releasedBytes+=UINT64(it->second->count)*TileBytes;
        released.push_back(std::move(it->second));pool.blocks.erase(it);
    }
    for(const auto& block:released){
        const auto local=UINT64(block->first)-pool.Base(block->arena,block->stream)/TileBytes;
        const D3D12_TILED_RESOURCE_COORDINATE coord{UINT(local),0,0,0};
        const D3D12_TILE_REGION_SIZE region{block->count,FALSE,0,0,0};
        const auto flag=D3D12_TILE_RANGE_FLAG_NULL;
        const UINT offset=0,count=block->count;
        forestBuildQueue.queue->UpdateTileMappings(pool.Resource(block->arena,block->stream).Get(),1,&coord,&region,
            nullptr,1,&flag,&offset,&count,D3D12_TILE_MAPPING_FLAG_NONE);
    }
    if(!released.empty())WaitStartup(forestBuildQueue);
    return releasedBytes; // heap owners release only after the unmap fence
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
    UINT64 available=GpuSubtract(GpuSubtract(memory.Budget,reserve),memory.CurrentUsage);
    work.result.buildAvailableBytes=available;
    const auto allocation=[](UINT64 bytes){
        return bytes?Align(bytes,UINT64(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)):UINT64(0);
    };
    UINT64 immutableAllocation=0;
    const auto fits=[&](UINT64 bytes){
        bytes+=immutableAllocation;
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
        if(compatible->bytes<compatible->routeBytes||
           UINT64(compatible->bytes-compatible->routeBytes)+output.routeBytes>UINT32_MAX)
            return failRoute("routes_generation_byte_abi");
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
        output.rasterMeshlets=compatible->rasterMeshlets;
        output.triangles=compatible->triangles;
        output.bytes=compatible->bytes-compatible->routeBytes+output.routeBytes;
        submit();
        return true;
    }
    const auto& pack=prepared.pack;
    const auto& layouts=prepared.layouts;
    const auto& parts=prepared.parts;
    const auto bounds=prepared.bounds,cameraBoxes=prepared.cameraBoxes;
    const bool clustered=work.backend!=Mg2RayBackend::Standard;
    if(pack.bytes.size()>UINT32_MAX){
        work.result.lastFailure="limite_abi_octets_mg2";
        work.result.buildCode=0;
        return false;
    }
    const auto& catalog=prepared.pages;
    if(!catalog)return false;
    // LOD-23: the vertex and index streams have independent stable GPU
    // addresses. Reserve virtual capacity, map only demanded physical tiles.
    // Keep the shader ABI (element offsets) and all CLAS/BLAS identities.
    constexpr UINT64 TileBytes=Mg2Pages::SourceTileBytes;
    static_assert(TileBytes==D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
    auto& pool=forestSourcePool;
    if(prepared.sourceTiles.empty()){
        work.result.lastFailure="a06_source_abi";return false;
    }
    if(!EnsureForestSourceArenas(*catalog)){
        work.result.lastFailure="allocation_source_sparse";return false;
    }
    for(const auto id:prepared.arenaIds){
        const auto& arena=pool.arenas.at(id);
        const auto count=[&](ForestSourceStream stream){
            const auto k=uint32_t(stream);
            const auto logicalEnd=std::min(arena.end[k],catalog->SourceBytes(stream));
            return uint32_t((logicalEnd-arena.begin[k])/Mg2Pages::SourceStride(stream));
        };
        output.sourceArenas.push_back({arena.resources[0],arena.resources[1],
            count(ForestSourceStream::Vertices),count(ForestSourceStream::Indices)});
    }
    output.sourceUniqueClusters=prepared.sourceUniqueClusters;
    output.sourceDemandBytes=UINT64(prepared.sourceTiles.size())*TileBytes;
    const auto lookup=[&](ForestSourceTile tile)->std::shared_ptr<ForestSourceBlock>{
        auto it=pool.blocks.upper_bound(tile);
        if(it==pool.blocks.begin())return {};
        --it;
        const auto& block=it->second;
        return block->ready&&block->stream==tile.first&&tile.second>=block->first&&
            uint64_t(tile.second)-block->first<block->count?block:nullptr;
    };
    const auto mapBlock=[&](const ForestSourceBlock& block,bool remove){
        const auto local=UINT64(block.first)-pool.Base(block.arena,block.stream)/TileBytes;
        const D3D12_TILED_RESOURCE_COORDINATE coord{UINT(local),0,0,0};
        const D3D12_TILE_REGION_SIZE region{block.count,FALSE,0,0,0};
        const auto flag=remove?D3D12_TILE_RANGE_FLAG_NULL:D3D12_TILE_RANGE_FLAG_NONE;
        const UINT offset=0,count=block.count;
        forestBuildQueue.queue->UpdateTileMappings(pool.Resource(block.arena,block.stream).Get(),1,&coord,&region,
            remove?nullptr:block.heap.Get(),1,&flag,remove?nullptr:&offset,&count,D3D12_TILE_MAPPING_FLAG_NONE);
    };
    std::vector<ForestSourceTile> missingTiles;
    std::set<ForestSourceTile> heldBlocks;
    for(const auto tile:prepared.sourceTiles){
        auto existing=lookup(tile);
        if(!existing){missingTiles.push_back(tile);continue;}
        existing->lastUse=++pool.clock;
        if(heldBlocks.emplace(existing->stream,existing->first).second)output.sourceBlocks.push_back(std::move(existing));
    }
    immutableAllocation=UINT64(missingTiles.size())*TileBytes;
    const auto initialRequired=immutableAllocation+allocation(pack.bytes.size());
    output.sourceEvictedBytes=TrimForestSourceBlocks(*catalog,GpuSubtract(initialRequired,available));
    if(output.sourceEvictedBytes&&SUCCEEDED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory))){
        available=GpuSubtract(GpuSubtract(memory.Budget,reserve),memory.CurrentUsage);
        work.result.buildAvailableBytes=available;
    }
    if(!fits(allocation(pack.bytes.size())))return false;
    if(!missingTiles.empty()){
        const auto staging=make(immutableAllocation,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped{};const D3D12_RANGE none{};
        if(!staging||FAILED(staging->Map(0,&none,&mapped))||!mapped){
            work.result.lastFailure="allocation_source_tiles_upload";return false;
        }
        const auto uploadBegin=QpcCounter();
        for(size_t i=0;i<missingTiles.size();++i)
            catalog->PackSourceTile(missingTiles[i].first,UINT64(missingTiles[i].second)*TileBytes,
                std::span<std::byte>(static_cast<std::byte*>(mapped)+i*TileBytes,size_t(TileBytes)));
        staging->Unmap(0,nullptr);
        work.uploadMs+=float(1000.*QpcSeconds(QpcCounter()-uploadBegin));
        // Batch contiguous tiles into small independently reclaimable heaps.
        // This is allocation granularity, never a residency or detail quota.
        constexpr size_t TilesPerHeap=16;
        std::vector<std::shared_ptr<ForestSourceBlock>> added;
        for(size_t i=0;i<missingTiles.size();){
            const auto stream=missingTiles[i].first;
            const auto arena=pool.ArenaFor(stream,UINT64(missingTiles[i].second)*TileBytes,TileBytes);
            if(arena==Invalid){work.result.lastFailure="lod23_source_arena_tile";return false;}
            size_t end=i+1;
            while(end<missingTiles.size()&&end-i<TilesPerHeap&&
                  missingTiles[end].first==stream&&
                  uint64_t(missingTiles[end].second)==uint64_t(missingTiles[end-1].second)+1u&&
                  pool.ArenaFor(stream,UINT64(missingTiles[end].second)*TileBytes,TileBytes)==arena)++end;
            auto b=std::make_shared<ForestSourceBlock>();
            b->arena=arena;b->stream=stream;b->first=missingTiles[i].second;b->count=uint32_t(end-i);
            D3D12_HEAP_DESC heap{};
            heap.SizeInBytes=UINT64(b->count)*TileBytes;
            heap.Properties=Heap(D3D12_HEAP_TYPE_DEFAULT);
            heap.Alignment=D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            heap.Flags=D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            const auto begin=QpcCounter();
            const auto hr=device->CreateHeap(&heap,IID_PPV_ARGS(&b->heap));
            work.allocationMs+=float(1000.*QpcSeconds(QpcCounter()-begin));
            if(FAILED(hr)){work.result.lastFailure="allocation_source_tile_heap";work.result.buildCode=int(hr);return false;}
            added.push_back(std::move(b));i=end;
        }
        // Buffers support simultaneous disjoint reads/writes. Use implicit
        // COMMON -> COPY_DEST promotion on this separate submission and
        // buffer decay back to COMMON before the acceleration build reads.
        // Render queues only access tiles owned by a published generation.
        Commands uploadCommands;
        CommandsInit(uploadCommands,D3D12_COMMAND_LIST_TYPE_DIRECT);uploadCommands.Reset();
        UINT64 cursor=0;
        for(const auto& b:added){
            const UINT64 bytes=UINT64(b->count)*TileBytes;
            uploadCommands.list->CopyBufferRegion(pool.Resource(b->arena,b->stream).Get(),
                UINT64(b->first)*TileBytes-pool.Base(b->arena,b->stream),
                staging.Get(),cursor,bytes);
            cursor+=bytes;
        }
        // Prepare ownership before mapping; no allocation of backing heaps
        // or vector storage after submission until the fence has completed.
        output.sourceBlocks.reserve(output.sourceBlocks.size()+added.size());
        std::map<ForestSourceTile,std::shared_ptr<ForestSourceBlock>> newNodes;
        for(const auto& b:added)newNodes.emplace(ForestSourceTile{b->stream,b->first},b);
        uploadCommands.Close();
        // Transfer preallocated map nodes before mapping so the pool owns
        // every heap even if submission/validation subsequently throws.
        pool.blocks.merge(newNodes);
        if(!newNodes.empty()){work.result.lastFailure="a06_source_overlap";return false;}
        for(const auto& b:added)mapBlock(*b,false);
        uploadCommands.Execute(forestBuildQueue);
        WaitStartup(forestBuildQueue);
        Hr(device->GetDeviceRemovedReason(),"MG2 source tiles");
        for(const auto& b:added){
            b->ready=true;b->lastUse=++pool.clock;
            output.sourceBlocks.push_back(b);
        }
        output.geometryUploadBytes=immutableAllocation;
    }
    for(const auto& [first,b]:pool.blocks)output.sourceResidentBytes+=UINT64(b->count)*TileBytes;
    if(!fits(allocation(pack.bytes.size())))return false;
    const auto geometry=make(pack.bytes.size(),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
    const auto source=upload(pack.bytes.data(),pack.bytes.size());
    if(!geometry||!source){
        work.result.lastFailure="allocation_geometrie";
        work.result.buildCode=0;
        return false;
    }
    const auto sourceAddress=[&](ForestSourceStream stream,UINT64 logical,UINT64 bytes){
        const auto arena=pool.ArenaFor(stream,logical,bytes);
        if(arena==Invalid)Fail("LOD23 source address outside arena");
        return pool.Resource(arena,stream)->GetGPUVirtualAddress()+logical-pool.Base(arena,stream);
    };
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
    const UINT64 addressBytes=clustered?prepared.newBlasParts.size()*sizeof(UINT64):0u;
    if(forestBuildQueries){
        output.buildReadback=make(3u*sizeof(UINT64),
            D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!output.buildReadback)return fail("lecture_differee");
    }
    if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0u);
    if(clustered){
        output.blasEntries=std::move(prepared.blasEntries);
        output.newBlasParts=std::move(prepared.newBlasParts);
        output.addressCount=uint32_t(output.newBlasParts.size());
        const UINT64 tableBytes=UINT64(parts.size())*sizeof(UINT64);
        if(!fits(allocation(pack.bytes.size())+allocation(tableBytes)))return false;
        addresses=make(tableBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!addresses)return fail("allocation_adresses_blas");
        // Reused variants copy the original GPU-generated address directly.
        for(size_t p=0;p<parts.size();){
            if(!output.blasEntries[p]){++p;continue;}
            const auto& slot=output.blasEntries[p]->slot;
            if(!slot)return fail("slot_blas_reutilise");
            size_t end=p+1;
            while(end<parts.size()&&output.blasEntries[end]&&
                output.blasEntries[end]->slot.buffer.Get()==slot.buffer.Get()&&
                output.blasEntries[end]->slot.offset==slot.offset+UINT64(end-p)*sizeof(UINT64))++end;
            list->CopyBufferRegion(addresses.Get(),UINT64(p)*sizeof(UINT64),
                slot.buffer.Get(),slot.offset,UINT64(end-p)*sizeof(UINT64));
            p=end;
        }
        const auto build=[&]<class Api>(Api& api){
            auto limits=prepared.limits;
            // Both CLAS creation and BLAS assembly must opt in, including
            // assemblies made entirely from already resident OMM CLAS.
            limits.omm=!foliageOmms.empty();
            const auto& initial=prepared.initial;
            const auto& missing=prepared.missing;
            output.clasReferencesBuiltParts=limits.totalClasCount;
            output.clasCache=std::move(prepared.clasCache);
            output.newClasKeys=std::move(prepared.newClasKeys);
            std::vector<uint32_t> ommIndices;
            std::vector<UINT64> ommOffsets(prepared.newClusters.size(),UINT64_MAX);
            std::vector<const FoliageOmm*> clusterOmms(prepared.newClusters.size(),nullptr);
            uint64_t territorialKnown=0,territorialUnknown=0;
            for(size_t i=0;i<prepared.newClusters.size();++i){
                const auto& cluster=prepared.newClusters[i];
                const auto* omm=FindFoliageOmm(cluster.mesh);
                const bool territorial=cluster.mesh>=meshes.ranges.size();
                if(!omm&&(!territorial||cluster.opaque))continue;
                if(cluster.opaque||(omm&&omm->material!=cluster.material)||
                   uint64_t(cluster.firstIndex)+cluster.indexCount>cut.indices.size()||
                   uint64_t(cluster.firstVertex)+cluster.vertexCount>cut.vertices.size()||
                   cluster.indexCount%3u)return fail("omm_identite_clas");
                for(uint32_t p=0;p<cluster.indexCount;++p)
                    if(cut.indices[cluster.firstIndex+p]>=cluster.vertexCount)return fail("omm_indices_clas");
                const auto vertex=[&](uint32_t p)->const Vertex&{
                    return cut.vertices[cluster.firstVertex+cut.indices[cluster.firstIndex+p]];
                };
                if(territorial&&!omm){
                    // One existing array per cluster. Exact ordered UV key
                    // and immutable material are required; unknown UVs use
                    // the hardware four-state unknown index, not a fake map.
                    uint32_t best=0;
                    for(const auto& candidate:foliageOmms){
                        if(candidate.material!=cluster.material)continue;
                        uint32_t known=0;
                        for(uint32_t p=0;p<cluster.indexCount;p+=3){
                            uint32_t index;
                            known+=candidate.TryIndex(vertex(p),vertex(p+1),vertex(p+2),index);
                        }
                        if(known>best){best=known;omm=&candidate;}
                    }
                    if(!omm)continue; // ordinary non-opaque triangles
                }
                clusterOmms[i]=omm;
                ommOffsets[i]=UINT64(ommIndices.size())*sizeof(uint32_t);
                for(uint32_t p=0;p<cluster.indexCount;p+=3){
                    if(territorial){
                        uint32_t index;
                        const bool known=omm->TryIndex(vertex(p),vertex(p+1),vertex(p+2),index);
                        territorialKnown+=known;territorialUnknown+=!known;
                        ommIndices.push_back(index);
                    }else ommIndices.push_back(omm->Index(vertex(p),vertex(p+1),vertex(p+2)));
                }
                ++work.result.ommClasClusters;
                work.result.ommClasTriangles+=cluster.indexCount/3u;
            }
            if(territorialKnown||territorialUnknown){
                char line[192];std::snprintf(line,sizeof(line),"lod23_omm reused_triangles=%llu unknown_triangles=%llu\r\n",
                    static_cast<unsigned long long>(territorialKnown),static_cast<unsigned long long>(territorialUnknown));DiagnosticLine(line);
            }
            ComPtr<ID3D12Resource> ommIndexBuffer;
            if(!ommIndices.empty()){
                ommIndexBuffer=upload(ommIndices.data(),ommIndices.size()*sizeof(uint32_t));
                if(!ommIndexBuffer)return fail("allocation_indices_omm");
                output.staging.push_back(ommIndexBuffer);
            }
            std::vector<typename Api::Triangle> clusterArgs;
            clusterArgs.reserve(prepared.newClusters.size());
            for(size_t i=0;i<prepared.newClusters.size();++i){
                const auto& cluster=prepared.newClusters[i];
                auto args=Api::TriangleArgs(cluster,output.newClasKeys[i].second,
                    sourceAddress(ForestSourceStream::Vertices,UINT64(cluster.sourceFirstVertex)*sizeof(Vertex),
                        UINT64(cluster.vertexCount)*sizeof(Vertex)),
                    sourceAddress(ForestSourceStream::Indices,UINT64(cluster.sourceFirstIndex)*sizeof(uint32_t),
                        UINT64(cluster.indexCount)*sizeof(uint32_t)));
                if(ommOffsets[i]!=UINT64_MAX){
                    const auto& omm=*clusterOmms[i];
                    Api::AttachOmm(args,omm.nativeArray->GetGPUVirtualAddress(),
                        omm.nvapiArray?omm.nvapiArray->GetGPUVirtualAddress():0u,
                        ommIndexBuffer->GetGPUVirtualAddress()+ommOffsets[i]);
                }
                clusterArgs.push_back(args);
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
                allocation(newBytes)+allocation(addressBytes)+allocation(tableBytes)+
                allocation(UINT64(ommIndices.size())*sizeof(uint32_t));
            if(!fits(required))return false;
            const auto alloc=[&](UINT64 bytes,D3D12_RESOURCE_STATES state){
                return make(bytes,D3D12_HEAP_TYPE_DEFAULT,state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            };
            const auto clasAddresses=make(fullBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COPY_DEST);
            const auto newBlasAddresses=alloc(addressBytes,UavState);
            const auto blas=alloc(sizes[1].result,AsState);
            const auto scratch=alloc(scratchBytes,UavState);
            if(!clasAddresses||!newBlasAddresses||!blas||!scratch)return fail("allocation_clas_blas");
            for(size_t i=0;i<initial.size();){
                const auto& slot=initial[i];
                if(!slot){++i;continue;}
                size_t end=i+1;
                while(end<initial.size()&&initial[end].buffer.Get()==slot.buffer.Get()&&
                    initial[end].offset==slot.offset+UINT64(end-i)*sizeof(UINT64))++end;
                list->CopyBufferRegion(clasAddresses.Get(),UINT64(i)*sizeof(UINT64),
                    slot.buffer.Get(),slot.offset,UINT64(end-i)*sizeof(UINT64));
                i=end;
            }
            const auto clasAddressBase=clasAddresses->GetGPUVirtualAddress();
            const auto scratchAddress=scratch->GetGPUVirtualAddress();
            if(limits.clusters){
                const auto clas=alloc(sizes[0].result,AsState);
                const auto newAddresses=alloc(newBytes,UavState);
                const auto clusterArgBuffer=upload(clusterArgs.data(),clusterArgs.size()*sizeof(clusterArgs.front()));
                if(!clas||!newAddresses||!clusterArgBuffer)return fail("allocation_clas_incremental");
                const ClusterBuffers clasBuffers{clas->GetGPUVirtualAddress(),scratchAddress,
                    {newAddresses->GetGPUVirtualAddress(),sizeof(UINT64)},
                    {clusterArgBuffer->GetGPUVirtualAddress(),sizeof(clusterArgs.front())}};
                if(const auto code=api.Record(list,0u,clasBuffers);code)return fail("commande_clas",code);
                FinishUav(list,clas.Get());FinishUav(list,scratch.Get());
                Transition(list,newAddresses.Get(),UavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
                for(size_t i=0;i<missing.size();){
                    if(missing[i]==Invalid){++i;continue;}
                    size_t end=i+1;
                    while(end<missing.size()&&missing[end]==missing[i]+uint32_t(end-i))++end;
                    list->CopyBufferRegion(clasAddresses.Get(),UINT64(i)*sizeof(UINT64),
                        newAddresses.Get(),UINT64(missing[i])*sizeof(UINT64),UINT64(end-i)*sizeof(UINT64));
                    i=end;
                }
                for(size_t i=0;i<output.newClasKeys.size();++i)
                    output.clasCache.emplace(output.newClasKeys[i],
                        ForestClasEntry{{newAddresses,UINT64(i)*sizeof(UINT64)},clas});
                output.staging.insert(output.staging.end(),{newAddresses,clusterArgBuffer});
            }
            Transition(list,clasAddresses.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            std::vector<typename Api::Blas> blasArgs;
            blasArgs.reserve(output.newBlasParts.size());
            UINT64 cursor=0;
            size_t blasSlot=0;
            for(const auto p:output.newBlasParts){
                const auto& part=parts[p];
                blasArgs.push_back(Api::BlasArgs(part.count,clasAddressBase+cursor*sizeof(UINT64)));
                cursor+=part.count;
                auto entry=std::make_shared<ForestBlasEntry>();
                entry->storage=blas;
                entry->slot={newBlasAddresses,UINT64(blasSlot++)*sizeof(UINT64)};
                std::unordered_map<ID3D12Resource*,bool> seen;
                entry->clusterAddresses.reserve(part.count);
                for(uint32_t i=part.first;i<part.first+part.count;++i){
                    const auto& key=prepared.clasKeys[i];
                    const auto& cluster=output.clasCache.at(key);
                    if(seen.emplace(cluster.storage.Get(),true).second)entry->dependencies.push_back(cluster.storage);
                    entry->clusterAddresses.emplace_back(key,cluster);
                }
                output.blasEntries[p]=std::move(entry);
            }
            if(cursor!=initial.size())return fail("identite_blas_incremental");
            const auto blasArgBuffer=upload(blasArgs.data(),blasArgs.size()*sizeof(blasArgs.front()));
            if(!blasArgBuffer)return fail("arguments_clas_blas");
            const ClusterBuffers blasBuffers{blas->GetGPUVirtualAddress(),scratchAddress,
                {newBlasAddresses->GetGPUVirtualAddress(),sizeof(UINT64)},{blasArgBuffer->GetGPUVirtualAddress(),sizeof(blasArgs.front())}};
            if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1u);
            if(const auto code=api.Record(list,1u,blasBuffers);code)return fail("commande_blas_cluster",code);
            FinishUav(list,blas.Get());
            Transition(list,newBlasAddresses.Get(),UavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
            // Scatter only the new addresses into their current variant slots.
            for(size_t i=0;i<output.newBlasParts.size();){
                size_t end=i+1;
                while(end<output.newBlasParts.size()&&output.newBlasParts[end]==output.newBlasParts[i]+end-i)++end;
                list->CopyBufferRegion(addresses.Get(),UINT64(output.newBlasParts[i])*sizeof(UINT64),
                    newBlasAddresses.Get(),UINT64(i)*sizeof(UINT64),UINT64(end-i)*sizeof(UINT64));
                i=end;
            }
            output.staging.insert(output.staging.end(),{clasAddresses,scratch,blasArgBuffer,newBlasAddresses});
            return true;
        };
        if(!output.newBlasParts.empty()){
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
        }else if(forestBuildQueries)list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1u);
        Transition(list,addresses.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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
            const auto& arena=pool.arenas.at(part.arena);
            mesh.Triangles.VertexBuffer={arena.resources[0]->GetGPUVirtualAddress(),sizeof(Vertex)};
            mesh.Triangles.VertexCount=uint32_t((std::min(arena.end[0],catalog->SourceBytes(ForestSourceStream::Vertices))-
                arena.begin[0])/sizeof(Vertex));
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
            if(i+1u<operations.size()){
                FinishUav(list,scratch.Get());
                // Bound each independent standard-DXR producer packet.
                // Waiting is confined to this worker, never the render
                // thread; the complete generation still publishes once.
                constexpr size_t BuildsPerPacket=32u;
                if((i+1u)%BuildsPerPacket==0u){
                    FinishUav(list,result.Get());
                    submit();
                    WaitStartup(forestBuildQueue);
                    Hr(device->GetDeviceRemovedReason(),"MG2 standard BLAS packet");
                    output.commands.Reset();
                    list=output.commands.list.Get();
                }
            }
        }
        FinishUav(list,result.Get());
        output.blasCapacity=resultBytes;
        output.resources.push_back(result);
        output.staging.push_back(scratch);
    }
    if(forestBuildQueries){
        list->EndQuery(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2u);
        list->ResolveQueryData(forestBuildQueries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0u,3u,
            output.buildReadback.Get(),0u);
    }
    output.backend=work.backend;
    output.resources.push_back(addresses);
    for(uint32_t i=0;i<layouts.size();++i){
        if(i==ForestGeneration::Vertices||i==ForestGeneration::Triangles)continue;
        const auto& view=layouts[i];
        output.viewRequests[i]={geometry.Get(),view.offset/view.stride,view.count,view.stride};
    }
    output.viewRequests[ForestGeneration::Addresses]={addresses.Get(),0,uint32_t(parts.size()),sizeof(UINT64)};
    // Source SRVs are selected through the arena table in the former Remap slot.
    output.viewRequests[ForestGeneration::Bounds]={geometry.Get(),bounds.offset/bounds.stride,bounds.count,bounds.stride};
    output.viewRequests[ForestGeneration::CameraBoxes]={geometry.Get(),cameraBoxes.offset/cameraBoxes.stride,cameraBoxes.count,cameraBoxes.stride};
    output.pages=uint32_t(cut.pages);
    output.clusters=uint32_t(cut.clusters.size());
    output.rasterMeshlets=layouts[ForestGeneration::Meshlets].count;
    output.triangles=prepared.triangleCount;
    output.bytes=uint32_t(pack.bytes.size());
    submit();
    return true;
}
}
