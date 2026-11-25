#include "GraphicsInternal.h"
#include <nvapi.h>
#include <omm.h>
#include <cmath>
#include <string_view>

namespace OASIS {
namespace {
constexpr auto AsState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr auto Uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto UavFlag=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
constexpr uint32_t UnknownOmm=uint32_t(D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE);
constexpr std::string_view FoliageName="OrganicForest OMM foliage";

void CheckOmm(ommResult result){
    if(result!=ommResult_SUCCESS)Fail("NVIDIA OMM CPU bake failed");
}
void CheckNv(NvAPI_Status status){
    if(status!=NVAPI_OK)Fail("NVIDIA OMM GPU build failed");
}
struct Bake {
    ommBaker baker{};
    ommCpuTexture texture{};
    ommCpuBakeResult result{};
    ~Bake(){
        if(result)ommCpuDestroyBakeResult(result);
        if(texture)ommCpuDestroyTexture(baker,texture);
        if(baker)ommDestroyBaker(baker);
    }
};
uint32_t Corner(const Vertex& vertex){
    const auto& uv=vertex.uv;
    if(uv.x==0.f&&uv.y==0.f)return 0;
    if(uv.x==1.f&&uv.y==0.f)return 1;
    if(uv.x==1.f&&uv.y==1.f)return 2;
    if(uv.x==0.f&&uv.y==1.f)return 3;
    return Invalid;
}
void AsBarrier(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource){
    BBarrier(list,resource,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_RAYTRACING,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
}
}
uint32_t Graphics::Impl::FoliageOmm::Index(const Vertex& a,const Vertex& b,const Vertex& c)const{
    const auto x=Corner(a),y=Corner(b),z=Corner(c);
    // Noncanonical UVs are not approximated: hardware reports unknown,
    // and the existing exact alpha test resolves them.
    if(x==Invalid||y==Invalid||z==Invalid)return UnknownOmm;
    return indices[x*16u+y*4u+z];
}
const Graphics::Impl::FoliageOmm* Graphics::Impl::FindFoliageOmm(uint32_t mesh)const{
    for(const auto& map:foliageOmms)if(map.mesh==mesh)return &map;
    return nullptr;
}

void Graphics::Impl::PrepareFoliageOmm(const SceneData& assets){
    const auto isFoliage=[&](uint32_t material){
        return material<assets.materialNames.size()&&assets.materialNames[material]==FoliageName;
    };
    std::vector<Mg2ForestPart> parts;
    for(const auto& part:assets.mg2ForestParts)if(isFoliage(part.material)){
        if(std::none_of(parts.begin(),parts.end(),[&](const auto& p){return p.mesh==part.mesh;}))
            parts.push_back(part);
    }
    if(parts.empty())return;
    if(forestBuildBackend==Mg2RayBackend::Standard)
        Fail("OMM foliage requires hardware clustered geometry (NVAPI or DXR2)");
    const bool vendor=forestBuildBackend==Mg2RayBackend::Nvapi;
    if(vendor){
        NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS caps{};
        CheckNv(NvAPI_D3D12_GetRaytracingCaps(device.Get(),
            NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP,&caps,sizeof(caps)));
        if(!(caps&NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD))
            Fail("NVAPI hardware opacity micromaps required");
    }
    blas.resize(meshes.ranges.size());
    vramCache.RequireActive();
    if(!residency.Prepare(direct.fence->GetCompletedValue(),managedMemory.Budget().headroom))
        Fail("OMM source geometry exceeds available VRAM");
    residency.Wait(direct.queue.Get());
    startup.Reset();
    std::vector<ComPtr<ID3D12Resource>> staging;
    auto host=[&](const void* data,UINT64 bytes){
        if(!bytes)Fail("empty OMM input");
        auto resource=Host(bytes,D3D12_HEAP_TYPE_UPLOAD);
        void* mapped{};const D3D12_RANGE empty{};
        Hr(resource->Map(0,&empty,&mapped),"OMM upload");
        std::memcpy(mapped,data,size_t(bytes));resource->Unmap(0,nullptr);
        staging.push_back(resource);
        return resource;
    };
    auto allocate=[&](UINT64 bytes,D3D12_RESOURCE_STATES state){
        if(!bytes)Fail("empty OMM allocation");
        return Temporary(Buffer(bytes,UavFlag),state);
    };
    auto scratch=[&](UINT64 bytes){
        auto resource=allocate(std::max<UINT64>(bytes,256u),Uav);
        staging.push_back(resource);return resource;
    };
    for(const auto& part:parts){
        if(part.mesh>=meshes.ranges.size()||part.material>=assets.materials.size()||
           meshDynamicIndex[part.mesh]!=Invalid)
            Fail("OMM foliage mesh must be immutable");
        const auto& material=assets.materials[part.material];
        const auto& surface=surfaceData[part.material];
        if(material.alphaMode!=1u||material.alpha!=1.f||material.cutoff!=.5f||
           material.authoringSlot!=Invalid||material.albedo>=assets.textures.size()||
           material.samplers[0]>=assets.samplers.size()||surface.opacity.x!=1.f||
           surface.opacity.y!=.5f||surface.overrideUv!=1.f)
            Fail("OMM foliage alpha contract");
        const auto& sampler=assets.samplers[material.samplers[0]];
        const auto& mapping=material.mapping[0];
        if(sampler.wrapS!=33071u||sampler.wrapT!=33071u||sampler.minFilter!=9987u||
           sampler.magFilter!=9729u||mapping.offsetScale.x!=0.f||mapping.offsetScale.y!=0.f||
           mapping.offsetScale.z!=1.f||mapping.offsetScale.w!=1.f||
           mapping.rotationSet.x!=1.f||mapping.rotationSet.y!=0.f||mapping.rotationSet.z!=0.f)
            Fail("OMM foliage sampler and UV contract");
        const auto& texture=assets.textures[material.albedo];
        const size_t texels=size_t(texture.width)*texture.height;
        if(texture.format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||!texels||
           texture.offsets.empty()||texture.offsets[0]+texels*4>texture.bytes.size())
            Fail("OMM foliage atlas format");
        std::vector<uint8_t> alpha(texels);
        for(size_t i=0;i<texels;++i)
            alpha[i]=std::to_integer<uint8_t>(texture.bytes[texture.offsets[0]+i*4+3]);

        Bake bake;
        auto creation=ommBakerCreationDescDefault();creation.type=ommBakerType_CPU;
        CheckOmm(ommCreateBaker(&creation,&bake.baker));
        auto mip=ommCpuTextureMipDescDefault();mip.width=texture.width;mip.height=texture.height;
        mip.rowPitch=texture.width;mip.textureData=alpha.data();
        auto inputTexture=ommCpuTextureDescDefault();inputTexture.format=ommCpuTextureFormat_UNORM8;
        inputTexture.mips=&mip;inputTexture.mipCount=1;
        // RayOpacity uses SampleLevel(...,0) and alpha >= 0.5. The SDK uses >.
        // Move the comparison by one representable float, preserving equality.
        const float threshold=std::nextafter(.5f,0.f);inputTexture.alphaCutoff=threshold;
        CheckOmm(ommCpuCreateTexture(bake.baker,&inputTexture,&bake.texture));
        constexpr std::array<XMFLOAT2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
        std::vector<uint32_t> triplets,keys;
        for(uint32_t a=0;a<4;++a)for(uint32_t b=0;b<4;++b)for(uint32_t c=0;c<4;++c)
            if(a!=b&&b!=c&&a!=c){triplets.insert(triplets.end(),{a,b,c});keys.push_back(a*16+b*4+c);}
        auto input=ommCpuBakeInputDescDefault();
        input.texture=bake.texture;input.bakeFlags=ommCpuBakeFlags_Force32BitIndices;
        input.alphaMode=ommAlphaMode_Test;input.runtimeSamplerDesc.addressingMode=ommTextureAddressMode_Clamp;
        input.runtimeSamplerDesc.filter=ommTextureFilterMode_Linear;
        input.texCoordFormat=ommTexCoordFormat_UV32_FLOAT;input.texCoords=uv.data();
        input.texCoordStrideInBytes=sizeof(XMFLOAT2);
        input.indexFormat=ommIndexFormat_UINT_32;input.indexBuffer=triplets.data();
        input.indexCount=uint32_t(triplets.size());input.alphaCutoff=threshold;
        input.format=ommFormat_OC1_4_State;input.maxSubdivisionLevel=8;
        CheckOmm(ommCpuBake(bake.baker,&input,&bake.result));
        const ommCpuBakeResultDesc* result{};
        CheckOmm(ommCpuGetBakeResultDesc(bake.result,&result));
        if(!result||result->indexCount!=keys.size()||result->indexFormat!=ommIndexFormat_UINT_32||
           !result->arrayDataSize||!result->descArrayCount||!result->descArrayHistogramCount)
            Fail("OMM foliage bake returned no usable maps");

        FoliageOmm map{};map.mesh=part.mesh;map.material=part.material;map.indices.fill(UnknownOmm);
        const auto* bakedIndices=static_cast<const uint32_t*>(result->indexBuffer);
        for(size_t i=0;i<keys.size();++i)map.indices[keys[i]]=bakedIndices[i];
        static_assert(sizeof(ommCpuOpacityMicromapDesc)==sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_DESC));
        static_assert(sizeof(ommCpuOpacityMicromapDesc)==sizeof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_DESC));
        const auto raw=host(result->arrayData,result->arrayDataSize);
        const auto descriptors=host(result->descArray,UINT64(result->descArrayCount)*sizeof(ommCpuOpacityMicromapDesc));
        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> histogram;
        std::vector<NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT> nvHistogram;
        for(uint32_t i=0;i<result->descArrayHistogramCount;++i){
            const auto& entry=result->descArrayHistogram[i];
            if(entry.format!=ommFormat_OC1_4_State)Fail("OMM foliage must be four-state");
            histogram.push_back({entry.count,entry.subdivisionLevel,D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
            nvHistogram.push_back({entry.count,entry.subdivisionLevel,NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE});
        }
        D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC array{};
        array.NumOmmHistogramEntries=uint32_t(histogram.size());array.pOmmHistogram=histogram.data();
        array.InputBuffer=raw->GetGPUVirtualAddress();
        array.PerOmmDescs={descriptors->GetGPUVirtualAddress(),sizeof(ommCpuOpacityMicromapDesc)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
        build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.pOpacityMicromapArrayDesc=&array;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&build.Inputs,&info);
        map.nativeArray=allocate(info.ResultDataMaxSizeInBytes,AsState);
        build.DestAccelerationStructureData=map.nativeArray->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=scratch(info.ScratchDataSizeInBytes)->GetGPUVirtualAddress();
        startup.list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        AsBarrier(startup.list.Get(),map.nativeArray.Get());
        forestStatus.ommArrayBytes+=info.ResultDataMaxSizeInBytes;
        // Native DXR and NVAPI have distinct construction contracts. Cook the
        // same immutable four-state data into each required driver representation.
        if(vendor){
            NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC nv{};
            nv.inputs.flags=NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BUILD_FLAG_PREFER_FAST_TRACE;
            nv.inputs.numOMMUsageCounts=uint32_t(nvHistogram.size());nv.inputs.pOMMUsageCounts=nvHistogram.data();
            nv.inputs.inputBuffer=raw->GetGPUVirtualAddress();nv.inputs.perOMMDescs=array.PerOmmDescs;
            NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO sizes{};
            NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS query{
                NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER,&nv.inputs,&sizes};
            CheckNv(NvAPI_D3D12_GetRaytracingOpacityMicromapArrayPrebuildInfo(device.Get(),&query));
            map.nvapiArray=allocate(sizes.resultDataMaxSizeInBytes,AsState);
            nv.destOpacityMicromapArrayData=map.nvapiArray->GetGPUVirtualAddress();
            nv.scratchOpacityMicromapArrayData=scratch(sizes.scratchDataSizeInBytes)->GetGPUVirtualAddress();
            NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS params{
                NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS_VER,&nv,0,nullptr};
            CheckNv(NvAPI_D3D12_BuildRaytracingOpacityMicromapArray(startup.list.Get(),&params));
            AsBarrier(startup.list.Get(),map.nvapiArray.Get());
            forestStatus.ommArrayBytes+=sizes.resultDataMaxSizeInBytes;
        }

        const auto& range=meshes.ranges[part.mesh];
        std::vector<uint32_t> indicesForOmm;
        indicesForOmm.reserve(range.indexCount/3u);
        for(uint32_t i=0;i<range.indexCount;i+=3){
            const auto p=range.firstIndex+i;
            indicesForOmm.push_back(map.Index(meshes.vertices.at(meshes.indices.at(p)),
                meshes.vertices.at(meshes.indices.at(p+1)),meshes.vertices.at(meshes.indices.at(p+2))));
        }
        const auto linkageIndices=host(indicesForOmm.data(),UINT64(indicesForOmm.size())*4u);
        D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC triangles{};
        triangles.VertexBuffer={vertices->GetGPUVirtualAddress(),sizeof(Vertex)};
        triangles.VertexCount=uint32_t(meshes.vertices.size());triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        triangles.IndexBuffer=indices->GetGPUVirtualAddress()+UINT64(range.firstIndex)*4u;
        triangles.IndexCount=range.indexCount;triangles.IndexFormat=DXGI_FORMAT_R32_UINT;
        D3D12_RAYTRACING_GEOMETRY_OMM_LINKAGE_DESC linkage{};
        linkage.OpacityMicromapIndexBuffer={linkageIndices->GetGPUVirtualAddress(),4u};
        linkage.OpacityMicromapIndexFormat=DXGI_FORMAT_R32_UINT;
        linkage.OpacityMicromapArray=map.nativeArray->GetGPUVirtualAddress();
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};
        geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES;
        geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometry.OmmTriangles={&triangles,&linkage};
        build={};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        build.Inputs.pGeometryDescs=&geometry;info={};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&build.Inputs,&info);
        blas[part.mesh]=allocate(info.ResultDataMaxSizeInBytes,AsState);
        build.DestAccelerationStructureData=blas[part.mesh]->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=scratch(info.ScratchDataSizeInBytes)->GetGPUVirtualAddress();
        startup.list->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        AsBarrier(startup.list.Get(),blas[part.mesh].Get());
        forestStatus.ommNativeTriangles+=uint32_t(indicesForOmm.size());
        foliageOmms.push_back(std::move(map));
    }
    forestStatus.ommMaterials=uint32_t(foliageOmms.size());
    startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);
}
} // namespace OASIS
