#include "GraphicsInternal.h"

#if OASIS_MG2_PREVIEW
#include "Mg2Pages.h"
#include <map>
#endif

namespace OASIS {
void Graphics::Impl::EnableClusterPreview(){
#if OASIS_MG2_PREVIEW
    static const IID features[]{D3D12ExperimentalShaderModels,D3D12RaytracingPreview};
    static const HRESULT enabled=D3D12EnableExperimentalFeatures(UINT(std::size(features)),features,nullptr,nullptr);
    clusterPreview=SUCCEEDED(enabled);
#endif
}

std::string Graphics::ProbeMg2(const std::wstring& denseSource){
#if !OASIS_MG2_PREVIEW
    (void)denseSource;
    return "mg2_clas etat=sdk_retail gameplay=dxr_standard\r\n";
#else
    auto& render=*impl_;
    if(!render.clusterPreview)return "mg2_clas etat=fonctionnalites_experimentales_indisponibles gameplay=dxr_standard\r\n";
    ComPtr<ID3D12DeviceRaytracing2> deviceRaytracing;
    D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 options{};
    if(FAILED(render.device.As(&deviceRaytracing))||
       FAILED(render.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2,&options,sizeof(options)))||
       !options.ClustersAndPTLASSupported)
        return "mg2_clas etat=adaptateur_non_compatible gameplay=dxr_standard\r\n";
    if(!std::filesystem::is_regular_file(denseSource))
        return "mg2_clas etat=temoin_absent gameplay=dxr_standard\r\n";

    const Mg2Pages pages(LoadSceneGLTF(denseSource));
    Commands commands;
    render.CommandsInit(commands,D3D12_COMMAND_LIST_TYPE_DIRECT);
    ComPtr<ID3D12CommandListRaytracing2> rayCommands;
    if(FAILED(commands.list.As(&rayCommands)))return "mg2_clas etat=liste_non_compatible gameplay=dxr_standard\r\n";
    UINT64 frequency=0;
    if(FAILED(render.direct.queue->GetTimestampFrequency(&frequency))||!frequency)
        return "mg2_clas etat=horloge_indisponible gameplay=dxr_standard\r\n";
    const auto buffer=[&](UINT64 bytes,D3D12_HEAP_TYPE heapType,D3D12_RESOURCE_STATES state){
        ComPtr<ID3D12Resource> resource;
        const auto heap=Heap(heapType);
        const auto desc=Buffer(bytes,heapType==D3D12_HEAP_TYPE_DEFAULT?
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE);
        if(!bytes||FAILED(render.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
              state,nullptr,IID_PPV_ARGS(&resource))))resource.Reset();
        return resource;
    };
    const auto upload=[&](const void* data,size_t bytes){
        auto source=buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* memory=nullptr;
        const D3D12_RANGE read{0,0};
        if(!source||FAILED(source->Map(0,&read,&memory)))return ComPtr<ID3D12Resource>{};
        std::memcpy(memory,data,bytes);
        source->Unmap(0,nullptr);
        return source;
    };

    std::string report;
    constexpr auto asState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    constexpr auto uavState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    constexpr std::array ranges{2.f,30.f};
    for(const float distance:ranges){
        const std::array eyes{Mg2LodEye{{-.032f,5.f,-distance},1200.f,.04f},
                              Mg2LodEye{{.032f,5.f,-distance},1200.f,.04f}};
        const auto cut=pages.Select(eyes,1.f);
        if(cut.clusters.empty())return report+"mg2_clas etat=coupe_vide gameplay=dxr_standard\r\n";
        const auto count=UINT(cut.clusters.size());
        const auto inputVertices=upload(cut.vertices.data(),cut.vertices.size()*sizeof(Vertex));
        const auto inputIndices=upload(cut.indices.data(),cut.indices.size()*sizeof(uint32_t));
        if(!inputVertices||!inputIndices)return report+"mg2_clas etat=entree_refusee gameplay=dxr_standard\r\n";

        D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC clustersInput{};
        clustersInput.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        clustersInput.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        clustersInput.VertexFormat=D3D12_VERTEX_FORMAT_FLOAT32_3;
        clustersInput.IndexFormat=D3D12_INDEX_FORMAT_UINT32;
        auto& limits=clustersInput.ClusterLimits;
        limits.MaxArgCount=count;
        limits.MaxUniqueGeometryIndexAndFlagsCountPerCluster=1;
        limits.MaxTotalTriangleCount=UINT(cut.indices.size()/3u);
        limits.MaxSumOfUniqueVertexIndexCounts=UINT(cut.vertices.size());
        struct ReferenceGeometry {std::vector<uint32_t> indices;bool opaque{};};
        std::map<uint32_t,ReferenceGeometry> referenceGroups;
        std::vector<D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS> buildClusters;
        buildClusters.reserve(count);
        for(const auto& cluster:cut.clusters){
            auto& args=buildClusters.emplace_back();
            args.ClusterID=cluster.id;
            args.TriangleCount=UINT16(cluster.indexCount/3u);
            args.UniqueVertexIndexCount=UINT16(cluster.vertexCount);
            args.BaseGeometryIndexAndFlags=cluster.material;
            if(cluster.opaque)args.BaseGeometryIndexAndFlags|=D3D12_RTAS_CLUSTERED_GEOMETRY_FLAG_OPAQUE;
            args.VertexBufferStride=sizeof(Vertex);
            args.IndexBufferStride=sizeof(uint32_t);
            args.VertexBuffer=inputVertices->GetGPUVirtualAddress()+UINT64(cluster.firstVertex)*sizeof(Vertex);
            args.IndexBuffer=inputIndices->GetGPUVirtualAddress()+UINT64(cluster.firstIndex)*sizeof(uint32_t);
            limits.MaxGeometryIndexValue=std::max(limits.MaxGeometryIndexValue,cluster.material);
            limits.MaxTriangleCountPerCluster=std::max(limits.MaxTriangleCountPerCluster,UINT(args.TriangleCount));
            limits.MaxUniqueVertexIndexCountPerCluster=std::max(limits.MaxUniqueVertexIndexCountPerCluster,cluster.vertexCount);
            auto& reference=referenceGroups[cluster.material];
            reference.opaque=cluster.opaque;
            for(uint32_t i=0;i<cluster.indexCount;++i)
                reference.indices.push_back(cluster.firstVertex+cut.indices[cluster.firstIndex+i]);
        }
        std::vector<uint32_t> standardIndices;
        standardIndices.reserve(cut.indices.size());
        for(const auto& [_,group]:referenceGroups)
            standardIndices.insert(standardIndices.end(),group.indices.begin(),group.indices.end());
        const auto standardIndexBuffer=upload(standardIndices.data(),standardIndices.size()*sizeof(uint32_t));
        if(!standardIndexBuffer)return report+"mg2_clas etat=reference_refusee gameplay=dxr_standard\r\n";
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> standardGeometry;
        UINT64 standardIndex=standardIndexBuffer->GetGPUVirtualAddress();
        for(const auto& [_,group]:referenceGroups){
            auto& geometry=standardGeometry.emplace_back();
            geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometry.Flags=group.opaque?D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE:D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
            geometry.Triangles.VertexBuffer={inputVertices->GetGPUVirtualAddress(),sizeof(Vertex)};
            geometry.Triangles.VertexCount=UINT(cut.vertices.size());
            geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
            geometry.Triangles.IndexBuffer=standardIndex;
            geometry.Triangles.IndexCount=UINT(group.indices.size());
            geometry.Triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
            standardIndex+=group.indices.size()*sizeof(uint32_t);
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC standard{};
        standard.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        standard.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        standard.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        standard.Inputs.NumDescs=UINT(standardGeometry.size());
        standard.Inputs.pGeometryDescs=standardGeometry.data();
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO standardInfo{};
        render.device->GetRaytracingAccelerationStructurePrebuildInfo(&standard.Inputs,&standardInfo);
        D3D12_RTAS_OPERATION_DESC clas{};
        clas.Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        clas.Inputs.pClusterTrianglesDesc=&clustersInput;
        D3D12_RTAS_CLAS_INPUTS_DESC clusteredBlasInput{};
        clusteredBlasInput.Flags=D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
        clusteredBlasInput.Mode=D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        clusteredBlasInput.MaxArgCount=1;
        clusteredBlasInput.MaxTotalClasCount=clusteredBlasInput.MaxClasCountPerArg=count;
        D3D12_RTAS_OPERATION_DESC blas{};
        blas.Inputs.Type=D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        blas.Inputs.pClasDesc=&clusteredBlasInput;
        D3D12_RTAS_OPERATION_PREBUILD_INFO clasInfo{},blasInfo{};
        deviceRaytracing->GetRTASOperationPrebuildInfo(&clas.Inputs,&clasInfo);
        deviceRaytracing->GetRTASOperationPrebuildInfo(&blas.Inputs,&blasInfo);
        if(!clasInfo.ResultDataMaxSizeInBytes||!blasInfo.ResultDataMaxSizeInBytes||!standardInfo.ResultDataMaxSizeInBytes)
            return report+"mg2_clas etat=prebuild_refuse gameplay=dxr_standard\r\n";
        const UINT64 addressBytes=UINT64(count)*sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        const auto clasAddresses=buffer(addressBytes,D3D12_HEAP_TYPE_DEFAULT,uavState);
        const auto blasAddress=buffer(sizeof(D3D12_GPU_VIRTUAL_ADDRESS),D3D12_HEAP_TYPE_DEFAULT,uavState);
        const auto clasSizes=buffer(UINT64(count)*sizeof(UINT64),D3D12_HEAP_TYPE_DEFAULT,uavState);
        const auto blasSize=buffer(sizeof(UINT64),D3D12_HEAP_TYPE_DEFAULT,uavState);
        const auto clasDestination=buffer(clasInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,asState);
        const auto blasDestination=buffer(blasInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,asState);
        const auto standardDestination=buffer(standardInfo.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,asState);
        const auto scratch=buffer(std::max({clasInfo.ScratchDataSizeInBytes,blasInfo.ScratchDataSizeInBytes,standardInfo.ScratchDataSizeInBytes}),
                                  D3D12_HEAP_TYPE_DEFAULT,uavState);
        if(!clasAddresses||!blasAddress||!clasSizes||!blasSize||!clasDestination||!blasDestination||!standardDestination||!scratch)
            return report+"mg2_clas etat=allocation_refusee gameplay=dxr_standard\r\n";

        const D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS buildBlas{count,
            sizeof(D3D12_GPU_VIRTUAL_ADDRESS),clasAddresses->GetGPUVirtualAddress()};
        const auto clasArguments=upload(buildClusters.data(),buildClusters.size()*sizeof(buildClusters.front()));
        const auto blasArguments=upload(&buildBlas,sizeof(buildBlas));
        constexpr UINT QueryCount=4;
        constexpr UINT64 sizeOffset=QueryCount*sizeof(UINT64);
        const UINT64 addressOffset=sizeOffset+UINT64(count+1u)*sizeof(UINT64);
        const UINT64 readbackBytes=addressOffset+UINT64(count+1u)*sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
        const auto readback=buffer(readbackBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,QueryCount,0};
        ComPtr<ID3D12QueryHeap> timestamps;
        if(!clasArguments||!blasArguments||!readback||
           FAILED(render.device->CreateQueryHeap(&queries,IID_PPV_ARGS(&timestamps))))
            return report+"mg2_clas etat=mesure_indisponible gameplay=dxr_standard\r\n";

        D3D12_RTAS_BATCHED_OPERATION_DATA clasData{},blasData{};
        clasData.BatchResultData=clasDestination->GetGPUVirtualAddress();
        clasData.BatchScratchData=scratch->GetGPUVirtualAddress();
        clasData.ResultAddressArray={clasAddresses->GetGPUVirtualAddress(),sizeof(D3D12_GPU_VIRTUAL_ADDRESS)};
        clasData.ResultSizeArray={clasSizes->GetGPUVirtualAddress(),sizeof(UINT64)};
        clasData.IndirectArgumentArray={clasArguments->GetGPUVirtualAddress(),sizeof(buildClusters.front())};
        blasData.BatchResultData=blasDestination->GetGPUVirtualAddress();
        blasData.BatchScratchData=clasData.BatchScratchData;
        blasData.ResultAddressArray={blasAddress->GetGPUVirtualAddress(),sizeof(D3D12_GPU_VIRTUAL_ADDRESS)};
        blasData.ResultSizeArray={blasSize->GetGPUVirtualAddress(),sizeof(UINT64)};
        blasData.IndirectArgumentArray={blasArguments->GetGPUVirtualAddress(),sizeof(buildBlas)};
        clas.pBatchedOperationData=&clasData;
        blas.pBatchedOperationData=&blasData;

        commands.Reset();
        auto* list=commands.list.Get();
        const auto transition=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES next){
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,next};
            list->ResourceBarrier(1,&barrier);
        };
        list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        rayCommands->ExecuteIndirectRTASOperations(1,&clas,D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
        list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        D3D12_RESOURCE_BARRIER barriers[2]{};
        barriers[0].Type=barriers[1].Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barriers[0].UAV.pResource=clasDestination.Get();
        barriers[1].UAV.pResource=scratch.Get();
        list->ResourceBarrier(UINT(std::size(barriers)),barriers);
        constexpr auto addressRead=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition(clasAddresses.Get(),uavState,addressRead);
        rayCommands->ExecuteIndirectRTASOperations(1,&blas,D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
        list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,2);
        list->ResourceBarrier(1,&barriers[1]);
        standard.ScratchAccelerationStructureData=scratch->GetGPUVirtualAddress();
        standard.DestAccelerationStructureData=standardDestination->GetGPUVirtualAddress();
        list->BuildRaytracingAccelerationStructure(&standard,0,nullptr);
        list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,3);
        transition(clasSizes.Get(),uavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(blasSize.Get(),uavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(clasAddresses.Get(),addressRead,D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(blasAddress.Get(),uavState,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,QueryCount,readback.Get(),0);
        list->CopyBufferRegion(readback.Get(),sizeOffset,clasSizes.Get(),0,UINT64(count)*sizeof(UINT64));
        list->CopyBufferRegion(readback.Get(),sizeOffset+UINT64(count)*sizeof(UINT64),blasSize.Get(),0,sizeof(UINT64));
        list->CopyBufferRegion(readback.Get(),addressOffset,clasAddresses.Get(),0,addressBytes);
        list->CopyBufferRegion(readback.Get(),addressOffset+addressBytes,blasAddress.Get(),0,sizeof(D3D12_GPU_VIRTUAL_ADDRESS));
        commands.Submit(render.direct);
        render.WaitStartup(render.direct);
        if(FAILED(render.device->GetDeviceRemovedReason()))return report+"mg2_clas etat=gpu_reinitialise\r\n";
        void* memory=nullptr;
        const D3D12_RANGE read{0,SIZE_T(readbackBytes)},written{0,0};
        if(FAILED(readback->Map(0,&read,&memory)))return report+"mg2_clas etat=lecture_refusee gameplay=dxr_standard\r\n";
        const auto* values=static_cast<const UINT64*>(memory);
        const auto* addresses=values+addressOffset/sizeof(UINT64);
        const auto inside=[](UINT64 address,UINT64 bytes,UINT64 base,UINT64 capacity,UINT alignment){
            return bytes>0&&address>=base&&address%alignment==0u&&
                   address-base<capacity&&bytes<=capacity-(address-base);
        };
        UINT64 clasBytes=0;
        bool sizesValid=true;
        for(UINT i=0;i<count;++i){
            const auto bytes=values[QueryCount+i];
            sizesValid&=inside(addresses[i],bytes,clasData.BatchResultData,clasInfo.ResultDataMaxSizeInBytes,
                               D3D12_RAYTRACING_CLAS_BYTE_ALIGNMENT)&&bytes<=clasInfo.ResultDataMaxSizeInBytes-clasBytes;
            if(sizesValid)clasBytes+=bytes;
        }
        const UINT64 blasBytes=values[QueryCount+count];
        sizesValid&=inside(addresses[count],blasBytes,blasData.BatchResultData,blasInfo.ResultDataMaxSizeInBytes,
                           D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
        const bool valid=sizesValid&&values[0]<=values[1]&&values[1]<=values[2]&&values[2]<=values[3];
        char line[420];
        std::snprintf(line,sizeof(line),"mg2_clas etat=%s gameplay=dxr_standard distance_m=%.0f "
            "pages=%zu pages_total=%zu clusters=%u triangles=%zu vertex_index_octets=%zu "
            "clas_octets=%llu blas_octets=%llu blas_standard_octets_max=%llu "
            "clas_ms=%.3f blas_ms=%.3f blas_standard_ms=%.3f\r\n",
            valid?"construit":"mesure_invalide",distance,cut.pages,pages.PageCount(),count,cut.indices.size()/3u,
            cut.vertices.size()*sizeof(Vertex)+cut.indices.size()*sizeof(uint32_t),
            static_cast<unsigned long long>(clasBytes),static_cast<unsigned long long>(blasBytes),
            static_cast<unsigned long long>(standardInfo.ResultDataMaxSizeInBytes),
            valid?1000.*double(values[1]-values[0])/double(frequency):-1.,
            valid?1000.*double(values[2]-values[1])/double(frequency):-1.,
            valid?1000.*double(values[3]-values[2])/double(frequency):-1.);
        readback->Unmap(0,&written);
        report+=line;
    }
    return report;
#endif
}
}
