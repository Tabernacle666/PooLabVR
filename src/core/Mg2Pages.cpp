#include "Mg2Pages.h"
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
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
}

Mg2LodCut Mg2Pages::Select(std::span<const Mg2LodInstance> instances,float errorPixels,
    std::span<const uint32_t> previous,std::span<const Mg2InstanceRoute> previousRoutes)const{
    Mg2LodCut result;
    result.candidates=uint32_t(instances.size());
    std::unordered_map<uint32_t,std::vector<size_t>> pagesByMesh;
    for(size_t i=0;i<pages_.size();++i)pagesByMesh[pages_[i].mesh].push_back(i);
    std::vector<const Mg2LodInstance*> order;
    order.reserve(instances.size());
    for(const auto& instance:instances)order.push_back(&instance);
    std::sort(order.begin(),order.end(),[](const auto* a,const auto* b){
        if(a->visible!=b->visible)return a->visible>b->visible;
        if(a->distanceWorld!=b->distanceWorld)return a->distanceWorld<b->distanceWorld;
        return a->key<b->key;
    });
    struct Selected {const Page* page;const Cluster* cluster;};
    struct Variant {std::vector<Selected> clusters;uint32_t part{UINT32_MAX};};
    // Exact complete projected-error cuts only. No admission ceiling or degraded routes.
    std::map<std::vector<uint32_t>,Variant> variants;
    std::vector<std::pair<Mg2InstanceRoute,Variant*>> assignments;
    assignments.reserve(instances.size());
    std::vector<uint8_t> refine(pages_.size()),hasParent(pages_.size());
    for(size_t i=0;i<pages_.size();++i)for(const auto& cluster:pages_[i].clusters){
        if(cluster.refined<0)continue;
        const size_t child=size_t(cluster.refined);
        if(child>=i||pages_[child].mesh!=pages_[i].mesh)Fail("MG2: invalid refinement edge");
        hasParent[child]=1;
    }
    for(const auto* instance:order){
        if(!instance->eyeCount)continue;
        const auto found=pagesByMesh.find(instance->mesh);
        if(found==pagesByMesh.end())continue;
        ++result.eligible;
        const auto& groups=found->second;
        for(size_t index:groups){
            const auto& page=pages_[index];
            refine[index]=uint8_t(!hasParent[index]);
            for(uint32_t eye=0;eye<instance->eyeCount&&!refine[index];++eye){
                const auto& view=instance->eyes[eye];
                const auto depth=std::max(view.nearPlane,Length(Sub(page.center,view.position))-page.radius);
                if(page.error>errorPixels*depth/view.focalPixels)refine[index]=1;
            }
        }
        std::vector<uint32_t> ids;
        std::vector<Selected> selected;
        for(size_t index:groups){
            if(!refine[index])continue;
            const auto& page=pages_[index];
            for(const auto& cluster:page.clusters){
                if(cluster.refined>=0&&refine[size_t(cluster.refined)])continue;
                ids.push_back(cluster.id);selected.push_back({&page,&cluster});
            }
        }
        if(ids.empty())Fail("MG2: empty hierarchy cut");
        auto [it,inserted]=variants.try_emplace(std::move(ids));
        if(inserted)it->second.clusters=std::move(selected);
        assignments.emplace_back(instance->key,&it->second);
    }
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
                uint32_t(cluster.indices.size()),page->material,page->mesh,variant.part,page->opaque});
        }
    }
    return result;
}
}
