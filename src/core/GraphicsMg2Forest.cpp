#include "GraphicsInternal.h"
#include <numeric>

namespace OASIS {
namespace {
constexpr uint32_t PagedMeshBit=0x80000000u;
constexpr auto ShaderRead=D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto AsState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr auto UavState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr UINT64 ForestBuildBudget=64u*1024u*1024u;

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

void Graphics::Impl::EnableClusterPreview(){
#if OASIS_MG2_PREVIEW
    static const IID features[]{D3D12ExperimentalShaderModels,D3D12RaytracingPreview};
    static const HRESULT enabled=D3D12EnableExperimentalFeatures(UINT(std::size(features)),features,nullptr,nullptr);
    clusterPreview=SUCCEEDED(enabled);
#endif
}

D3D12_PIPELINE_STATE_FLAGS Graphics::Impl::ForestPipelineFlags()const{
#if OASIS_MG2_PREVIEW
    if(forestClasSupported)return D3D12_PIPELINE_STATE_FLAG_RAYQUERY_ALLOW_CLUSTERED_GEOMETRY_ON_PRE_SM69_SHADERS;
#endif
    return D3D12_PIPELINE_STATE_FLAG_NONE;
}

void Graphics::Impl::ConfigureForest(const SceneData& assets){
    forestParts=assets.mg2ForestParts;
    if(forestParts.empty())return;
    std::ranges::sort(forestParts,{},&Mg2ForestPart::mesh);
    forestHierarchy.emplace(assets,forestParts);
    for(const auto& part:forestParts){
        const auto& bounds=assets.meshes.ranges[part.mesh].bounds;
        forestRadius=std::max(forestRadius,Length({bounds.x,bounds.y,bounds.z})+bounds.w);
    }
#if OASIS_MG2_PREVIEW
    ComPtr<ID3D12DeviceRaytracing2> rayDevice;
    D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 options{};
    forestClasSupported=forestClas=clusterPreview&&SUCCEEDED(device.As(&rayDevice))&&
        SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2,&options,sizeof(options)))&&
        options.ClustersAndPTLASSupported;
#endif
}

void Graphics::Impl::ResetForest(){
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
    forestClasSupported=forestClas=forestRequestFailed=false;
}

void Graphics::Impl::PrepareForestPages(Frame& f,const XRFrame& xr,const GpuSceneCache* tiles,bool nativeCampus){
    // The previous use of this frame slot has completed on every queue.
    f.forestPreviousTlas.reset();
    f.forestRaster.reset();
    if(!forestHierarchy)return;
    if(forestPending&&direct.fence->GetCompletedValue()>=forestPending->fence){
        bool valid=true;
        if(forestPending->addressCheck){
            auto& pending=*forestPending;
            const D3D12_RANGE read{0,SIZE_T(pending.addressCount)*sizeof(UINT64)},empty{};
            void* mapped=nullptr;
            valid=SUCCEEDED(pending.addressCheck->Map(0,&read,&mapped))&&mapped;
            if(valid){
                for(auto address:std::span(static_cast<const UINT64*>(mapped),pending.addressCount))
                    valid&=address>=pending.blasStart&&address-pending.blasStart<pending.blasCapacity&&
                        address%D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT==0;
                pending.addressCheck->Unmap(0,&empty);
            }
            pending.addressCheck.Reset();
        }
        if(valid){
            forestPending->staging.clear();
            forestPending->commands={};
            forestResident=std::move(forestPending);
            shadowCache={};
        }else{
            forestPending.reset();
            forestClas=false;
            forestRequestFailed=true;
        }
    }
    std::erase_if(forestGenerations,[&](const auto& generation){
        if(generation.use_count()!=1)return false;
        for(auto view:generation->views)if(view.Valid())ReleaseDescriptor(view);
        return true;
    });
    const bool canPublish=nativeCampus&&tiles&&xr.sphereBakeView==Invalid;
    if(canPublish)f.forestRaster=forestResident;
    if(forestSelection.requested){
        if(!forestSelection.group.Complete())return;
        forestSelection.requested=false;
        auto cut=std::move(forestSelection.cut);
        if(!canPublish||cut.clusters.empty())return;
        auto candidate=std::make_shared<ForestGeneration>();
        if(!UploadForestPages(cut,*candidate)){
            forestClas=false;
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
    std::vector<Mg2LodEye> eyes;
    const auto representative=forestParts.front().mesh;
    for(const auto& [key,tile]:tiles->ResidentTiles()){
        if(key.layer!=64)continue;
        for(const auto& object:tile.objects){
            if(!object.enabled||object.mesh!=representative)continue;
            const XMFLOAT3 root{object.position.x,object.position.y,object.position.z};
            const float scale=object.scale.x;
            if(scale<=0.f)continue;
            // The root is at the base of the tree. This bounds the entire
            // pilot asset, including its canopy, before requesting any page.
            bool visible=false;
            for(uint32_t eye=0;eye<xr.viewCount&&!visible;++eye)
                visible=SphereIntersectsView(XMLoadFloat4x4(&xr.vp[eye]),root,forestRadius*scale,true);
            if(!visible)continue;
            const auto& q=object.rotation;
            const auto inverse=XMQuaternionConjugate(XMVectorSet(q.x,q.y,q.z,q.w));
            for(uint32_t eye=0;eye<xr.viewCount;++eye){
                const auto offset=Scale(Sub(xr.eyes[eye],root),1.f/scale);
                XMFLOAT3 local;
                XMStoreFloat3(&local,XMVector3Rotate(XMLoadFloat3(&offset),inverse));
                eyes.push_back({local,focal[eye],xr.input.SceneNearZ()/scale});
            }
        }
    }
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
    std::vector<uint32_t> routes(meshes.ranges.size(),Invalid),globalIndices;
    std::vector<Meshlet> clusterMeshlets;
    std::vector<std::array<uint32_t,3>> triangles;
    std::vector<MeshRange> level;
    struct Part {uint32_t first,count,firstIndex,indexCount,material;};
    std::vector<Part> parts;
    for(uint32_t i=0;i<cut.clusters.size();){
        const auto& first=cut.clusters[i];
        auto j=i;
        for(;j<cut.clusters.size()&&cut.clusters[j].mesh==first.mesh;++j){
            const auto& cluster=cut.clusters[j];
            clusterMeshlets.push_back({cluster.firstVertex,uint32_t(triangles.size()),
                cluster.vertexCount,cluster.indexCount/3u});
            for(uint32_t k=0;k<cluster.indexCount;k+=3u){
                const auto* local=cut.indices.data()+cluster.firstIndex+k;
                triangles.push_back({local[0],local[1],local[2]});
                for(uint32_t v=0;v<3;++v)globalIndices.push_back(cluster.firstVertex+local[v]);
            }
        }
        if(routes[first.mesh]!=Invalid)return false;
        routes[first.mesh]=PagedMeshBit|uint32_t(level.size());
        const auto& last=cut.clusters[j-1u];
        const auto vertexCount=last.firstVertex+last.vertexCount-first.firstVertex;
        const auto indexCount=last.firstIndex+last.indexCount-first.firstIndex;
        parts.push_back({i,j-i,first.firstIndex,indexCount,first.material});
        level.push_back({i,j-i,first.firstVertex,vertexCount,first.firstIndex,indexCount,
                         meshes.ranges[first.mesh].bounds});
        i=j;
    }
    if(level.size()!=forestParts.size())return false;
    PackedForest pack;
    std::array<PackedView,ForestGeneration::Addresses> layouts{
        pack.Add(routes),pack.Add(level),pack.Add(clusterMeshlets),pack.Add(cut.vertices),
        pack.Add(triangles),pack.Add(globalIndices)};
    const auto localIndices=forestClas?pack.Add(cut.indices):PackedView{};
    if(pack.bytes.size()>ForestBuildBudget||managedMemory.Budget().headroom<ForestBuildBudget)return false;

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
    if(!geometry||!source)return false;
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
#if OASIS_MG2_PREVIEW
    if(forestClas){
        ComPtr<ID3D12DeviceRaytracing2> rayDevice;
        ComPtr<ID3D12CommandListRaytracing2> rayList;
        if(FAILED(device.As(&rayDevice))||FAILED(list->QueryInterface(IID_PPV_ARGS(&rayList))))return false;
        D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC clusterInput{};
        clusterInput.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        clusterInput.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        clusterInput.VertexFormat=D3D12_VERTEX_FORMAT_FLOAT32_3;
        clusterInput.IndexFormat=D3D12_INDEX_FORMAT_UINT32;
        auto& limits=clusterInput.ClusterLimits;
        limits.MaxArgCount=uint32_t(cut.clusters.size());
        limits.MaxUniqueGeometryIndexAndFlagsCountPerCluster=1;
        limits.MaxTotalTriangleCount=uint32_t(cut.indices.size()/3u);
        limits.MaxSumOfUniqueVertexIndexCounts=uint32_t(cut.vertices.size());
        std::vector<D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS> clusterArgs;
        const auto localAddress=geometry->GetGPUVirtualAddress()+localIndices.offset;
        for(const auto& part:parts)for(uint32_t i=part.first;i<part.first+part.count;++i){
            const auto& cluster=cut.clusters[i];
            auto& args=clusterArgs.emplace_back();
            args.ClusterID=cluster.id;
            args.TriangleCount=uint16_t(cluster.indexCount/3u);
            args.UniqueVertexIndexCount=uint16_t(cluster.vertexCount);
            const uint32_t primitiveBase=(cluster.firstIndex-part.firstIndex)/3u;
            args.BaseGeometryIndexAndFlags=primitiveBase;
            if(cluster.opaque)args.BaseGeometryIndexAndFlags|=D3D12_RTAS_CLUSTERED_GEOMETRY_FLAG_OPAQUE;
            args.VertexBufferStride=sizeof(Vertex);
            args.IndexBufferStride=sizeof(uint32_t);
            args.VertexBuffer=vertexAddress+UINT64(cluster.firstVertex)*sizeof(Vertex);
            args.IndexBuffer=localAddress+UINT64(cluster.firstIndex)*sizeof(uint32_t);
            limits.MaxGeometryIndexValue=std::max(limits.MaxGeometryIndexValue,primitiveBase);
            limits.MaxTriangleCountPerCluster=std::max(limits.MaxTriangleCountPerCluster,UINT(args.TriangleCount));
            limits.MaxUniqueVertexIndexCountPerCluster=std::max(limits.MaxUniqueVertexIndexCountPerCluster,cluster.vertexCount);
        }
        D3D12_RTAS_CLAS_INPUTS_DESC blasInput{};
        blasInput.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        blasInput.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        blasInput.MaxArgCount=uint32_t(parts.size());
        blasInput.MaxTotalClasCount=limits.MaxArgCount;
        for(const auto& part:parts)blasInput.MaxClasCountPerArg=std::max(blasInput.MaxClasCountPerArg,part.count);
        D3D12_RTAS_OPERATION_DESC clas{},blas{};
        clas.Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        clas.Inputs.pClusterTrianglesDesc=&clusterInput;
        blas.Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        blas.Inputs.pClasDesc=&blasInput;
        D3D12_RTAS_OPERATION_PREBUILD_INFO clasInfo{},blasInfo{};
        rayDevice->GetRTASOperationPrebuildInfo(&clas.Inputs,&clasInfo);
        rayDevice->GetRTASOperationPrebuildInfo(&blas.Inputs,&blasInfo);
        const UINT64 scratchBytes=std::max(clasInfo.ScratchDataSizeInBytes,blasInfo.ScratchDataSizeInBytes);
        if(!clasInfo.ResultDataMaxSizeInBytes||!blasInfo.ResultDataMaxSizeInBytes||
           clasInfo.ResultDataMaxSizeInBytes+blasInfo.ResultDataMaxSizeInBytes+scratchBytes+pack.bytes.size()>ForestBuildBudget)
            return false;
        const auto alloc=[&](UINT64 bytes,D3D12_RESOURCE_STATES state){
            return make(bytes,D3D12_HEAP_TYPE_DEFAULT,state,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        };
        const auto clasAddresses=alloc(limits.MaxArgCount*sizeof(UINT64),UavState);
        const auto clasSizes=alloc(limits.MaxArgCount*sizeof(UINT64),UavState);
        const auto blasSizes=alloc(parts.size()*sizeof(UINT64),UavState);
        addresses=alloc(parts.size()*sizeof(UINT64),UavState);
        const auto clasBuffer=alloc(clasInfo.ResultDataMaxSizeInBytes,AsState);
        const auto blasBuffer=alloc(blasInfo.ResultDataMaxSizeInBytes,AsState);
        const auto scratch=alloc(scratchBytes,UavState);
        if(!clasAddresses||!clasSizes||!blasSizes||!addresses||!clasBuffer||!blasBuffer||!scratch)return false;
        std::vector<D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS> blasArgs;
        const auto clasAddressBase=clasAddresses->GetGPUVirtualAddress();
        for(const auto& part:parts)blasArgs.push_back({part.count,sizeof(UINT64),clasAddressBase+UINT64(part.first)*sizeof(UINT64)});
        const auto clusterArgBuffer=upload(clusterArgs.data(),clusterArgs.size()*sizeof(clusterArgs.front()));
        const auto blasArgBuffer=upload(blasArgs.data(),blasArgs.size()*sizeof(blasArgs.front()));
        if(!clusterArgBuffer||!blasArgBuffer)return false;
        D3D12_RTAS_BATCHED_OPERATION_DATA clasData{},blasData{};
        clasData.BatchResultData=clasBuffer->GetGPUVirtualAddress();
        blasData.BatchResultData=blasBuffer->GetGPUVirtualAddress();
        clasData.BatchScratchData=blasData.BatchScratchData=scratch->GetGPUVirtualAddress();
        clasData.ResultAddressArray={clasAddressBase,sizeof(UINT64)};
        blasData.ResultAddressArray={addresses->GetGPUVirtualAddress(),sizeof(UINT64)};
        clasData.ResultSizeArray={clasSizes->GetGPUVirtualAddress(),sizeof(UINT64)};
        blasData.ResultSizeArray={blasSizes->GetGPUVirtualAddress(),sizeof(UINT64)};
        clasData.IndirectArgumentArray={clusterArgBuffer->GetGPUVirtualAddress(),sizeof(clusterArgs.front())};
        blasData.IndirectArgumentArray={blasArgBuffer->GetGPUVirtualAddress(),sizeof(blasArgs.front())};
        clas.pBatchedOperationData=&clasData;
        blas.pBatchedOperationData=&blasData;
        rayList->ExecuteIndirectRTASOperations(1,&clas,D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
        FinishUav(list,clasBuffer.Get());
        FinishUav(list,scratch.Get());
        Transition(list,clasAddresses.Get(),UavState,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        rayList->ExecuteIndirectRTASOperations(1,&blas,D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
        FinishUav(list,blasBuffer.Get());
        const UINT64 addressBytes=parts.size()*sizeof(UINT64);
        output.addressCheck=make(addressBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        if(!output.addressCheck)return false;
        Transition(list,addresses.Get(),UavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(output.addressCheck.Get(),0,addresses.Get(),0,addressBytes);
        Transition(list,addresses.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        output.addressCount=uint32_t(parts.size());
        output.blasStart=blasBuffer->GetGPUVirtualAddress();
        output.blasCapacity=blasInfo.ResultDataMaxSizeInBytes;
        output.clustered=true;
        output.resources.insert(output.resources.end(),{clasBuffer,blasBuffer});
        output.staging.insert(output.staging.end(),{clasAddresses,clasSizes,blasSizes,scratch,clusterArgBuffer,blasArgBuffer});
    }else
#endif
    {
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
            if(!info.ResultDataMaxSizeInBytes)return false;
            offsets.push_back(resultBytes);
            resultBytes+=Align(info.ResultDataMaxSizeInBytes,UINT64(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT));
            scratchBytes=std::max(scratchBytes,info.ScratchDataSizeInBytes);
        }
        if(resultBytes+scratchBytes+pack.bytes.size()>ForestBuildBudget)return false;
        const auto result=make(resultBytes,D3D12_HEAP_TYPE_DEFAULT,AsState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        const auto scratch=make(scratchBytes,D3D12_HEAP_TYPE_DEFAULT,UavState,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!result||!scratch)return false;
        for(auto& offset:offsets)offset+=result->GetGPUVirtualAddress();
        addresses=upload(offsets.data(),offsets.size()*sizeof(UINT64));
        if(!addresses)return false;
        for(size_t i=0;i<operations.size();++i){
            auto& op=operations[i];
            op.DestAccelerationStructureData=offsets[i];
            op.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
            list->BuildRaytracingAccelerationStructure(&op,0,nullptr);
            if(i+1u<operations.size())FinishUav(list,scratch.Get());
        }
        FinishUav(list,result.Get());
        output.resources.push_back(result);
        output.staging.push_back(scratch);
    }
    output.resources.push_back(addresses);
    for(uint32_t i=0;i<layouts.size();++i){
        const auto& view=layouts[i];
        output.views[i]=Srv(geometry.Get(),view.count,view.stride,{},view.offset/view.stride);
    }
    output.views[ForestGeneration::Addresses]=Srv(addresses.Get(),uint32_t(parts.size()),sizeof(UINT64));
    output.pages=uint32_t(cut.pages);
    output.clusters=uint32_t(cut.clusters.size());
    output.triangles=uint32_t(triangles.size());
    output.bytes=uint32_t(pack.bytes.size());
    output.commands.Submit(direct);
    output.fence=direct.Signal();
    return true;
}
}
