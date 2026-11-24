#include "Mg2Pages.h"
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <map>
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
                std::array<Vertex,MaxVertices> pageVertices{};
                for(size_t v=0;v<vertices;++v)pageVertices[v]=mesh.vertices[range.firstVertex+remap[v]];
                packed.vertices.resize(meshopt_encodeVertexBufferBound(vertices,sizeof(Vertex)));
                const auto bytes=meshopt_encodeVertexBuffer(packed.vertices.data(),packed.vertices.size(),
                    pageVertices.data(),vertices,sizeof(Vertex));
                if(!bytes)Fail("MG2: cluster page encoding failed");
                packed.vertices.resize(bytes);
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
    struct Variant {
        std::vector<Selected> clusters;
        uint32_t part{UINT32_MAX};
        bool admitted{};
    };
    // Exact ordered cuts share one raster range and one BLAS. Source
    // cluster IDs are globally unique; never merge cuts on a hash alone.
    std::map<std::vector<uint32_t>,Variant> variants;
    std::vector<std::pair<Mg2InstanceRoute,Variant*>> assignments;
    assignments.reserve(instances.size());
    std::vector<uint8_t> refine(pages_.size());
    constexpr uint32_t TriangleBudget=262144u;
    uint32_t triangles=0;
    for(const auto* instance:order){
        const auto found=pagesByMesh.find(instance->mesh);
        if(found==pagesByMesh.end()||!instance->eyeCount)continue;
        for(size_t i:found->second){
            const auto& page=pages_[i];
            refine[i]=0;
            for(uint32_t eye=0;eye<instance->eyeCount;++eye){
                const auto& view=instance->eyes[eye];
                const auto depth=std::max(view.nearPlane,Length(Sub(page.center,view.position))-page.radius);
                if(page.error>errorPixels*depth/view.focalPixels){refine[i]=1;break;}
            }
        }
        std::vector<uint32_t> ids;
        std::vector<Selected> selected;
        uint64_t size=0;
        for(size_t i:found->second){
            if(!refine[i])continue;
            const auto& page=pages_[i];
            for(const auto& cluster:page.clusters){
                if(cluster.refined>=0&&refine[size_t(cluster.refined)])continue;
                ids.push_back(cluster.id);
                selected.push_back({&page,&cluster});
                size+=cluster.indices.size()/3u;
            }
        }
        if(ids.empty())continue;
        auto [it,inserted]=variants.try_emplace(std::move(ids));
        auto& variant=it->second;
        if(inserted&&size<=TriangleBudget-triangles){
            triangles+=uint32_t(size);
            variant.admitted=true;
            variant.clusters=std::move(selected);
        }
        if(variant.admitted)assignments.emplace_back(instance->key,&variant);
    }
    // Stable ordinals independent of instance traversal order.
    std::unordered_set<uint32_t> acceptedMeshes;
    for(auto& [ids,variant]:variants){
        if(!variant.admitted)continue;
        variant.part=result.variants++;
        acceptedMeshes.insert(variant.clusters.front().page->mesh);
        result.selection.push_back(uint32_t(ids.size()));
        result.selection.insert(result.selection.end(),ids.begin(),ids.end());
    }
    result.pages=acceptedMeshes.size();
    result.routes.reserve(assignments.size());
    for(auto& [key,variant]:assignments){
        key[3]=variant->part;
        result.routes.push_back(key);
    }
    std::sort(result.routes.begin(),result.routes.end());
    const bool sameGeometry=std::ranges::equal(result.selection,previous);
    if(sameGeometry&&std::ranges::equal(result.routes,previousRoutes)){
        result.routes.clear();
        return result;
    }
    if(sameGeometry)return result;
    for(const auto& [ids,variant]:variants){
        if(!variant.admitted)continue;
        for(const auto& [page,packed]:variant.clusters){
            const auto& cluster=*packed;
            const auto vertex=uint32_t(result.vertices.size()),index=uint32_t(result.indices.size());
            result.vertices.resize(result.vertices.size()+cluster.vertexCount);
            if(meshopt_decodeVertexBuffer(result.vertices.data()+vertex,cluster.vertexCount,sizeof(Vertex),
                                         cluster.vertices.data(),cluster.vertices.size())!=0)
                Fail("MG2: cluster page decoding failed");
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
