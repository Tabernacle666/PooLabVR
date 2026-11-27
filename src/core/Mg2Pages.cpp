#include "Mg2Pages.h"
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <bit>
#include <map>
#include <stdexcept>
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>

namespace OASIS {
Mg2Pages::Mg2Pages(const SceneData& asset){
    std::vector<Mg2ForestPart> parts;
    for(const auto& object:asset.objects)
        if(!(object.flags&(DynamicObject|DeformedObject)))parts.push_back({object.mesh,object.material});
    Bake(asset,parts);
}

Mg2Pages::Mg2Pages(const SceneData& asset,std::span<const Mg2ForestPart> parts){Bake(asset,parts);}

void Mg2Pages::Bake(const SceneData& asset,std::span<const Mg2ForestPart> parts){
    // NVIDIA RTXMG Cluster LOD defaults to 128 triangles / 128 vertices.
    // The earlier 64/64 pilot doubled the number of small groups, increasing
    // traversal, rebuild and upload overhead without improving scene content.
    constexpr uint32_t MaxTriangles=128,MaxVertices=128;
    auto config=clodDefaultConfigRT(MaxTriangles);
    config.max_vertices=MaxVertices;
    constexpr float weights[]{.35f,.35f,.35f,.7f,.7f,.7f,.7f};
    static_assert(offsetof(Vertex,normal)==12&&offsetof(Vertex,uv)==24&&offsetof(Vertex,uv1)==32);
    const auto& mesh=asset.meshes;
    uint32_t nextCluster=0;
    for(const auto& object:parts){
        const auto& range=mesh.ranges.at(object.mesh);
        const auto& surface=asset.materials.at(object.material);
        if(!range.indexCount||surface.alphaMode==2u)continue;
        // Small disconnected branches need the spatial simplifier at distance.
        // Keep the cutout silhouette and UV charts of the needles intact.
        config.simplify_fallback_sloppy=surface.alphaMode==0u;
        // Pack the indexed range into its own vertex domain. Passing the
        // global scene vertex count made every new tree scan the entire campus
        // repeatedly during CPU LOD construction.
        if(uint64_t(range.firstVertex)+range.vertexCount>mesh.vertices.size()||
           uint64_t(range.firstIndex)+range.indexCount>mesh.indices.size())
            Fail("MG2: invalid source mesh range");
        std::vector<uint32_t> localIndices(range.indexCount);
        for(uint32_t i=0;i<range.indexCount;++i){
            const uint32_t global=mesh.indices[range.firstIndex+i];
            if(global<range.firstVertex||global-range.firstVertex>=range.vertexCount)
                Fail("MG2: source mesh crosses vertex range");
            localIndices[i]=global-range.firstVertex;
        }
        clodMesh input{};
        input.indices=localIndices.data();
        input.index_count=range.indexCount;
        input.vertex_count=range.vertexCount;
        input.vertex_positions=&mesh.vertices[range.firstVertex].position.x;
        input.vertex_positions_stride=sizeof(Vertex);
        input.vertex_attributes=&mesh.vertices[range.firstVertex].normal.x;
        input.vertex_attributes_stride=sizeof(Vertex);
        input.attribute_weights=weights;
        input.attribute_count=std::size(weights);
        input.attribute_protect_mask=0x78u; // Keep the UV seams of both channels.
        clodBuild(config,input,[&](const clodGroup& group,const clodCluster* clusters,size_t count){
            const auto& bounds=group.simplified;
            Page page{{bounds.center[0],bounds.center[1],bounds.center[2]},bounds.radius,bounds.error,
                object.material,object.mesh,surface.alphaMode==0u,{}};
            page.clusters.reserve(count);
            for(size_t i=0;i<count;++i){
                const auto& source=clusters[i];
                std::array<unsigned,MaxVertices> remap{};
                Cluster packed{nextCluster++,uint32_t(source.vertex_count),source.refined,{},{}};
                packed.indices.resize(source.index_count);
                const size_t vertices=clodLocalIndices(remap.data(),packed.indices.data(),source.indices,source.index_count);
                if(vertices!=packed.vertexCount||vertices>MaxVertices)
                    Fail("MG2: invalid cluster source vertex count");
                packed.vertices.resize(vertices);
                for(size_t v=0;v<vertices;++v)
                    packed.vertices[v]=mesh.vertices[range.firstVertex+remap[v]];
                sourceBytes_+=uint64_t(packed.vertices.capacity())*sizeof(Vertex)+packed.indices.capacity();
                page.clusters.push_back(std::move(packed));
            }
            pages_.push_back(std::move(page));
            return int(pages_.size()-1);
        });
    }
    // Structural cuts share the original mesh/material routing and the
    // same 128/128 cluster ABI. Keep them separate from refinement edges.
    for(const auto& node:asset.canopy.lods){
        const auto source=std::find_if(parts.begin(),parts.end(),[&](const auto& part){return part.mesh==node.mesh;});
        if(source==parts.end()||node.indices.empty()||node.vertices.empty())continue;
        const auto& bounds=mesh.ranges.at(node.mesh).bounds;
        const auto& surface=asset.materials.at(source->material);
        if(surface.alphaMode==2u)continue;
        Page page{{bounds.x,bounds.y,bounds.z},bounds.w+node.error,node.error,
            source->material,node.mesh,surface.alphaMode==0u,{},true,node.indices.size()/3u};
        const auto capacity=meshopt_buildMeshletsBound(node.indices.size(),MaxVertices,MaxTriangles);
        std::vector<meshopt_Meshlet> meshlets(capacity);
        std::vector<uint32_t> vertices(capacity*MaxVertices);
        std::vector<unsigned char> triangles(capacity*MaxTriangles*3u);
        const size_t count=meshopt_buildMeshlets(meshlets.data(),vertices.data(),triangles.data(),
            node.indices.data(),node.indices.size(),&node.vertices.front().position.x,node.vertices.size(),
            sizeof(Vertex),MaxVertices,MaxTriangles,0.f);
        if(!count||count>capacity)Fail("MG2: invalid structural cut");
        page.clusters.reserve(count);
        for(size_t i=0;i<count;++i){
            const auto& m=meshlets[i];
            if(!m.vertex_count||m.vertex_count>MaxVertices||!m.triangle_count||m.triangle_count>MaxTriangles)
                Fail("MG2: invalid structural cluster");
            Cluster cluster{nextCluster++,uint32_t(m.vertex_count),-1,{},{}};
            cluster.vertices.reserve(m.vertex_count);
            for(size_t v=0;v<m.vertex_count;++v)
                cluster.vertices.push_back(node.vertices.at(vertices[m.vertex_offset+v]));
            const auto begin=triangles.begin()+m.triangle_offset;
            cluster.indices.assign(begin,begin+m.triangle_count*3u);
            sourceBytes_+=uint64_t(cluster.vertices.capacity())*sizeof(Vertex)+cluster.indices.capacity();
            page.clusters.push_back(std::move(cluster));
        }
        structuralPages_[node.mesh].push_back(pages_.size());
        pages_.push_back(std::move(page));
    }
    // Immutable indexes are prepared once, not rebuilt on each worker request.
    hasParent_.assign(pages_.size(),0u);
    for(auto& page:pages_)for(auto& cluster:page.clusters){
        if(cluster.vertices.size()>UINT32_MAX-vertexCount_||cluster.indices.size()>UINT32_MAX-indexCount_)
            Fail("MG2 immutable source ABI");
        cluster.sourceFirstVertex=vertexCount_;cluster.sourceFirstIndex=indexCount_;
        vertexCount_+=uint32_t(cluster.vertices.size());
        indexCount_+=uint32_t(cluster.indices.size());
    }
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=pages_[i];
        if(page.structural)continue;
        pagesByMesh_[page.mesh].push_back(i);
        for(const auto& cluster:page.clusters){
            if(cluster.refined<0)continue;
            const size_t child=size_t(cluster.refined);
            if(child>=i||pages_[child].mesh!=page.mesh)Fail("MG2: invalid refinement edge");
            hasParent_[child]=1u;
        }
    }
    BuildGpuCatalog();
}

void Mg2Pages::BuildGpuCatalog(){
    if(pages_.size()>=UINT32_MAX)Fail("MG2 GPU page ABI");
    gpuPages_.reserve(pages_.size());
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=pages_[i];
        if(page.clusters.size()>UINT32_MAX||page.structuralTriangles>UINT32_MAX)
            Fail("MG2 GPU page range");
        gpuPages_.push_back({page.center,page.radius,page.error,uint32_t(!hasParent_[i]),
            uint32_t(page.clusters.size()),uint32_t(page.structuralTriangles),uint32_t(page.opaque),
            page.clusters.empty()?UINT32_MAX:page.clusters.front().id});
    }
    for(const auto& [mesh,groups]:pagesByMesh_){
        GpuMesh range{uint32_t(gpuClusters_.size()),0u,uint32_t(gpuStructural_.size()),0u};
        for(size_t index:groups){
            const auto& page=pages_[index];
            for(size_t local=0;local<page.clusters.size();++local){
                const auto& c=page.clusters[local];
                if(gpuClusters_.size()>=UINT32_MAX)Fail("MG2 GPU cluster ABI");
                gpuClusters_.push_back({uint32_t(index),c.refined<0?UINT32_MAX:uint32_t(c.refined),
                    uint32_t(c.indices.size()/3u),uint32_t(local)});
            }
        }
        range.count=uint32_t(gpuClusters_.size())-range.first;
        if(const auto structural=structuralPages_.find(mesh);structural!=structuralPages_.end()){
            for(size_t page:structural->second){
                if(gpuStructural_.size()>=UINT32_MAX)Fail("MG2 GPU structural ABI");
                gpuStructural_.push_back(uint32_t(page));
            }
        }
        range.structuralCount=uint32_t(gpuStructural_.size())-range.structuralFirst;
        gpuMeshes_.emplace(mesh,range);
    }
}

void Mg2Pages::PackImmutableGeometry(std::vector<Vertex>& vertices,std::vector<uint32_t>& indices)const{
    vertices.reserve(vertexCount_);indices.reserve(indexCount_);
    for(const auto& page:pages_)for(const auto& cluster:page.clusters){
        if(vertices.size()!=cluster.sourceFirstVertex||indices.size()!=cluster.sourceFirstIndex)
            Fail("MG2 immutable source offsets");
        vertices.insert(vertices.end(),cluster.vertices.begin(),cluster.vertices.end());
        indices.insert(indices.end(),cluster.indices.begin(),cluster.indices.end());
    }
}
void Mg2Pages::AppendOpacityTriangles(uint32_t mesh,std::vector<std::array<XMFLOAT2,3>>& out)const{
    for(const auto& page:pages_)if(page.mesh==mesh&&!page.opaque)
        for(const auto& cluster:page.clusters)
            for(size_t i=0;i<cluster.indices.size();i+=3)
                out.push_back({cluster.vertices[cluster.indices[i]].uv,
                    cluster.vertices[cluster.indices[i+1]].uv,cluster.vertices[cluster.indices[i+2]].uv});
}

Mg2LodCut Mg2Pages::Select(std::span<const Mg2LodInstance> instances,float errorPixels,
    const Mg2GpuDispatch& dispatch,std::span<const uint32_t> previous,std::span<const Mg2InstanceRoute> previousRoutes)const{
    Mg2LodCut result;
    result.candidates=uint32_t(instances.size());
    // First cluster ID uniquely identifies a structural cut. Decode the
    // previous publication's variant ordinals once for hysteresis.
    std::vector<uint32_t> previousFirst;
    for(size_t i=0;i<previous.size();){
        const auto count=previous[i++];
        if(!count||count>previous.size()-i)Fail("MG2: invalid previous selection");
        previousFirst.push_back(previous[i]);
        i+=count;
    }
    struct Selected {const Page* page;const Cluster* cluster;};
    struct Variant {std::vector<Selected> clusters;uint32_t part{UINT32_MAX};};
    std::map<std::vector<uint32_t>,Variant> variants;
    std::vector<std::pair<Mg2InstanceRoute,Variant*>> assignments;
    assignments.reserve(instances.size());
    // One workgroup per instance, compact membership bits in source order.
    // Batch size bounds transient transfer memory, never scene admission or LOD.
    constexpr size_t TransferTarget=16u*1024u*1024u;
    constexpr size_t DispatchLimit=65535u; // D3D12 dispatch dimension.
    std::vector<Mg2GpuInstance> requests;
    std::vector<Mg2InstanceRoute> keys;
    std::vector<uint32_t> words;
    size_t outputWords=0;
    const auto flush=[&]{
        if(requests.empty())return;
        words.resize(outputWords);
        dispatch(requests,words,errorPixels);
        for(size_t i=0;i<requests.size();++i){
            const auto& request=requests[i];
            std::vector<uint32_t> ids;
            std::vector<Selected> selected;
            const auto append=[&](const Page& page,const Cluster& cluster){
                ids.push_back(cluster.id);selected.push_back({&page,&cluster});
            };
            const uint32_t structural=words.at(request.outputOffset);
            if(structural){
                const size_t index=size_t(structural-1u);
                if(index>=pages_.size()||!pages_[index].structural||
                   pages_[index].mesh!=pages_[gpuClusters_[request.first].page].mesh)
                    Fail("MG2 GPU structural selection");
                const auto& page=pages_[index];
                for(const auto& cluster:page.clusters)append(page,cluster);
            }else{
                const uint32_t count=request.count/32u+uint32_t(request.count%32u!=0u);
                for(uint32_t word=0;word<count;++word){
                    uint32_t bits=words.at(size_t(request.outputOffset)+1u+word);
                    while(bits){
                        const uint32_t local=word*32u+uint32_t(std::countr_zero(bits));
                        if(local>=request.count)Fail("MG2 GPU selection bit range");
                        const auto& c=gpuClusters_.at(size_t(request.first)+local);
                        const auto& page=pages_[c.page];append(page,page.clusters[c.local]);
                        bits&=bits-1u;
                    }
                }
            }
            if(ids.empty())Fail("MG2 GPU empty hierarchy cut");
            auto [it,inserted]=variants.try_emplace(std::move(ids));
            if(inserted)it->second.clusters=std::move(selected);
            assignments.emplace_back(keys[i],&it->second);
        }
        requests.clear();keys.clear();outputWords=0;
    };
    for(const auto& instance:instances){
        if(!instance.eyeCount)continue;
        const auto found=gpuMeshes_.find(instance.mesh);
        if(found==gpuMeshes_.end())continue;
        const auto& mesh=found->second;
        if(!mesh.count)Fail("MG2 GPU empty mesh");
        const size_t needed=1u+size_t(mesh.count/32u)+size_t(mesh.count%32u!=0u);
        if(!requests.empty()&&(requests.size()==DispatchLimit||
           (outputWords+needed)*sizeof(uint32_t)+(requests.size()+1u)*sizeof(Mg2GpuInstance)>TransferTarget||
           outputWords+needed>UINT32_MAX))flush();
        Mg2GpuInstance request{};
        request.first=mesh.first;request.count=mesh.count;
        request.structuralFirst=mesh.structuralFirst;request.structuralCount=mesh.structuralCount;
        request.outputOffset=uint32_t(outputWords);request.eyeCount=instance.eyeCount;
        request.visible=uint32_t(instance.visible);request.previousCluster=Invalid;
        if(instance.visible&&mesh.structuralCount){
            auto key=instance.key;key[3]=0u;
            const auto route=std::lower_bound(previousRoutes.begin(),previousRoutes.end(),key);
            if(route!=previousRoutes.end()&&(*route)[0]==key[0]&&(*route)[1]==key[1]&&
               (*route)[2]==key[2]&&(*route)[3]<previousFirst.size())
                request.previousCluster=previousFirst[(*route)[3]];
        }
        for(uint32_t eye=0;eye<instance.eyeCount;++eye){
            const auto& v=instance.eyes[eye];
            request.eyes[eye]={{v.position.x,v.position.y,v.position.z,v.focalPixels},
                {v.motion.x,v.motion.y,v.motion.z,v.nearPlane}};
        }
        requests.push_back(request);keys.push_back(instance.key);
        outputWords+=needed;++result.eligible;
    }
    flush();
    // Stable ordinals independent of instance traversal order.
    std::unordered_set<uint32_t> acceptedMeshes;
    for(auto& [ids,variant]:variants){
        variant.part=result.variants++;
        acceptedMeshes.insert(variant.clusters.front().page->mesh);
        result.selection.push_back(uint32_t(ids.size()));
        result.selection.insert(result.selection.end(),ids.begin(),ids.end());
    }
    result.pages=acceptedMeshes.size();
    result.routes.reserve(assignments.size());
    for(auto& [key,variant]:assignments){
        key[3]=variant->part;result.routes.push_back(key);
    }
    if(result.routes.size()!=result.eligible)Fail("MG2: incomplete coverage");
    result.selectedRoutes=uint32_t(result.routes.size());
    std::sort(result.routes.begin(),result.routes.end());
    const bool sameGeometry=std::ranges::equal(result.selection,previous);
    if(sameGeometry&&std::ranges::equal(result.routes,previousRoutes)){
        result.routes.clear();
        return result;
    }
    if(sameGeometry)return result;
    for(const auto& [ids,variant]:variants){
        for(const auto& [page,packed]:variant.clusters){
            const auto& cluster=*packed;
            // Offsets and counts in the existing geometry ABI are uint32_t.
            // Reject an unrepresentable generation, not an arbitrary LOD cap.
            if(result.vertices.size()>UINT32_MAX-cluster.vertexCount||
               result.indices.size()>UINT32_MAX-cluster.indices.size()||
               result.clusters.size()>=UINT32_MAX)
                throw std::length_error("MG2: geometry ABI range");
            const auto vertex=uint32_t(result.vertices.size()),index=uint32_t(result.indices.size());
            result.vertices.insert(result.vertices.end(),cluster.vertices.begin(),cluster.vertices.end());
            result.indices.insert(result.indices.end(),cluster.indices.begin(),cluster.indices.end());
            // Unique build IDs even when a source cluster is used by
            // several variants with different primitive offsets.
            result.clusters.push_back({uint32_t(result.clusters.size()),vertex,cluster.vertexCount,index,
                uint32_t(cluster.indices.size()),page->material,page->mesh,variant.part,page->opaque,
                cluster.sourceFirstVertex,cluster.sourceFirstIndex});
        }
    }
    return result;
}
}
