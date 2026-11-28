#include "GraphicsInternal.h"
#include <nvapi.h>
#include <omm.h>
#include <cmath>
#include <algorithm>
#include <string_view>

namespace OASIS {
namespace {
constexpr auto AsState=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr auto OmmScratchState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
constexpr auto UavFlag=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
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
using UvKey=std::array<uint32_t,6>;
// OMM 1.9.2 predates the CPU baker's degenerate-UV fix. Preserve exact
// texture alpha for points/lines rather than feeding them to its rasterizer.
constexpr uint32_t UnknownOmm=uint32_t(D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_OPAQUE);
bool ValidBakeUv(XMFLOAT2 a,XMFLOAT2 b,XMFLOAT2 c){
    if(!std::isfinite(a.x)||!std::isfinite(a.y)||!std::isfinite(b.x)||
       !std::isfinite(b.y)||!std::isfinite(c.x)||!std::isfinite(c.y))return false;
    const double x=double(b.x)-a.x,y=double(b.y)-a.y;
    const double u=double(c.x)-a.x,v=double(c.y)-a.y;
    const double scale=std::max(x*x+y*y,u*u+v*v);
    return scale>0.0&&std::abs(x*v-y*u)>std::max(1.e-12,scale*1.e-7);
}
UvKey TriangleKey(XMFLOAT2 a,XMFLOAT2 b,XMFLOAT2 c){
    auto bits=[](float v){return std::bit_cast<uint32_t>(v==0.f?0.f:v);};
    return {bits(a.x),bits(a.y),bits(b.x),bits(b.y),bits(c.x),bits(c.y)};
}
void AsBarrier(ID3D12GraphicsCommandList7* list,ID3D12Resource* resource){
    BBarrier(list,resource,D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
        D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE|D3D12_BARRIER_SYNC_RAYTRACING,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
        D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ);
}
}
uint32_t Graphics::Impl::FoliageOmm::Index(const Vertex& a,const Vertex& b,const Vertex& c)const{
    if(!ValidBakeUv(a.uv,b.uv,c.uv))return UnknownOmm;
    const auto it=indices.find(TriangleKey(a.uv,b.uv,c.uv));
    if(it==indices.end())Fail("OMM triangle absent from immutable cooked hierarchy");
    return it->second;
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
    StartupCheckpoint("omm_hierarchy_begin");
    // Bake every actual hierarchy UV triangle once before any CLAS can consume it.
    if(!forestHierarchy&&forestBake.valid()){
        forestHierarchy=std::make_shared<const Mg2Pages>(forestBake.get());
        forestStatus.sourcePages=uint32_t(forestHierarchy->PageCount());
    }
    if(!forestHierarchy)Fail("OMM hierarchy missing");
    StartupCheckpoint("omm_hierarchy_ready");
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
        auto resource=allocate(std::max<UINT64>(bytes,256u),OmmScratchState);
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
        const auto& range=meshes.ranges[part.mesh];
        std::vector<std::array<XMFLOAT2,3>> trianglesUv;
        trianglesUv.reserve(range.indexCount/3u);
        for(uint32_t i=0;i<range.indexCount;i+=3){
            const auto p=range.firstIndex+i;
            trianglesUv.push_back({meshes.vertices.at(meshes.indices.at(p)).uv,
                meshes.vertices.at(meshes.indices.at(p+1)).uv,meshes.vertices.at(meshes.indices.at(p+2)).uv});
        }
        forestHierarchy->AppendOpacityTriangles(part.mesh,trianglesUv);
        std::vector<XMFLOAT2> uv;
        std::vector<uint32_t> triplets;
        std::vector<UvKey> keys;
        std::map<UvKey,uint32_t> unique;
        uint32_t degenerate=0;
        for(const auto& triangle:trianglesUv){
            if(!ValidBakeUv(triangle[0],triangle[1],triangle[2])){++degenerate;continue;}
            const auto key=TriangleKey(triangle[0],triangle[1],triangle[2]);
            if(!unique.try_emplace(key,uint32_t(keys.size())).second)continue;
            if(uv.size()>UINT32_MAX-3u)Fail("OMM UV triangle capacity");
            const auto base=uint32_t(uv.size());
            uv.insert(uv.end(),triangle.begin(),triangle.end());
            triplets.insert(triplets.end(),{base,base+1u,base+2u});
            keys.push_back(key);
        }
        if(keys.empty())Fail("OMM foliage has no nondegenerate UV triangles");
        FoliageOmm map{};map.mesh=part.mesh;map.material=part.material;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        // Material identity covers the immutable alpha atlas and sampler.
        // Share an array only if every ordered UV key already exists; a
        // different hierarchy may introduce keys and needs its own bake.
        const auto shared=std::find_if(foliageOmms.begin(),foliageOmms.end(),[&](const auto& existing){
            return existing.material==part.material&&std::all_of(keys.begin(),keys.end(),
                [&](const auto& key){return existing.indices.contains(key);});
        });
        if(shared!=foliageOmms.end()){
            map.indices=shared->indices;
            map.nativeArray=shared->nativeArray;map.nvapiArray=shared->nvapiArray;
        }else{
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
            auto input=ommCpuBakeInputDescDefault();
            input.texture=bake.texture;input.bakeFlags=ommCpuBakeFlags_Force32BitIndices;
            input.alphaMode=ommAlphaMode_Test;input.runtimeSamplerDesc.addressingMode=ommTextureAddressMode_Clamp;
            input.runtimeSamplerDesc.filter=ommTextureFilterMode_Linear;
            input.texCoordFormat=ommTexCoordFormat_UV32_FLOAT;input.texCoords=uv.data();
            input.texCoordStrideInBytes=sizeof(XMFLOAT2);
            input.indexFormat=ommIndexFormat_UINT_32;input.indexBuffer=triplets.data();
            input.indexCount=uint32_t(triplets.size());input.alphaCutoff=threshold;
            input.format=ommFormat_OC1_4_State;input.maxSubdivisionLevel=8;
            char checkpoint[160];
            std::snprintf(checkpoint,sizeof(checkpoint),"omm_cpu_begin mesh=%u unique=%zu degenerate=%u",
                part.mesh,keys.size(),degenerate);
            StartupCheckpoint(checkpoint);
            CheckOmm(ommCpuBake(bake.baker,&input,&bake.result));
            StartupCheckpoint("omm_cpu_ready");
            const ommCpuBakeResultDesc* result{};
            CheckOmm(ommCpuGetBakeResultDesc(bake.result,&result));
            if(!result||result->indexCount!=keys.size()||result->indexFormat!=ommIndexFormat_UINT_32||
               !result->arrayDataSize||!result->descArrayCount||!result->descArrayHistogramCount)
                Fail("OMM foliage bake returned no usable maps");
    
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
            build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_OPACITY_MICROMAP_ARRAY;
            build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            build.Inputs.NumDescs=1;build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
            build.Inputs.pOpacityMicromapArrayDesc=&array;
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
        }

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
    StartupCheckpoint("omm_gpu_submit");
    startup.Submit(direct);WaitStartup(direct);residency.Submitted(direct.serial);
    StartupCheckpoint("omm_gpu_ready");
}
} // namespace OASIS
