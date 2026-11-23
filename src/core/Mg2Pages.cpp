#include "Mg2Pages.h"
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
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
    constexpr uint32_t MaxTriangles=64,MaxVertices=64;
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
        clodMesh input{};
        input.indices=mesh.indices.data()+range.firstIndex;
        input.index_count=range.indexCount;
        input.vertex_count=mesh.vertices.size();
        input.vertex_positions=&mesh.vertices.front().position.x;
        input.vertex_positions_stride=sizeof(Vertex);
        input.vertex_attributes=&mesh.vertices.front().normal.x;
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
                for(size_t v=0;v<vertices;++v)pageVertices[v]=mesh.vertices[remap[v]];
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

Mg2LodCut Mg2Pages::Select(std::span<const Mg2LodView> views,float errorPixels,
    std::span<const uint32_t> previous)const{
    Mg2LodCut result;
    std::unordered_map<uint32_t,std::vector<Mg2LodEye>> eyesByMesh;
    std::unordered_map<uint32_t,float> priorities;
    for(const auto& view:views){
        eyesByMesh[view.mesh].push_back(view.eye);
        auto [it,newMesh]=priorities.try_emplace(view.mesh,view.distanceWorld);
        if(!newMesh)it->second=std::min(it->second,view.distanceWorld);
    }
    std::vector<bool> refine(pages_.size(),false);
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=pages_[i];
        auto found=eyesByMesh.find(page.mesh);
        if(found==eyesByMesh.end())continue;
        refine[i]=std::any_of(found->second.begin(),found->second.end(),[&](const Mg2LodEye& eye){
            const auto depth=std::max(eye.nearPlane,Length(Sub(page.center,eye.position))-page.radius);
            return page.error>errorPixels*depth/eye.focalPixels;
        });
    }
    std::unordered_map<uint32_t,uint32_t> triangleCounts;
    for(size_t i=0;i<pages_.size();++i){
        if(!refine[i])continue;
        const auto& page=pages_[i];
        for(const auto& cluster:page.clusters){
            if(cluster.refined>=0&&refine[size_t(cluster.refined)])continue;
            triangleCounts[page.mesh]+=uint32_t(cluster.indices.size()/3u);
        }
    }
    // A route only exists when every selected cluster of its mesh fits.
    // Unadmitted meshes remain on their original unmodified geometry.
    std::vector<std::pair<float,uint32_t>> order;
    order.reserve(triangleCounts.size());
    for(const auto& [mesh,count]:triangleCounts)order.push_back({priorities[mesh],mesh});
    std::sort(order.begin(),order.end());
    constexpr uint32_t TriangleBudget=65536u;
    uint32_t triangles=0u;
    std::unordered_set<uint32_t> accepted;
    for(const auto& [distance,mesh]:order){
        const uint32_t size=triangleCounts[mesh];
        if(size>TriangleBudget-triangles)continue;
        triangles+=size;accepted.insert(mesh);
    }
    struct Selection {const Page* page;const Cluster* cluster;};
    std::vector<Selection> selected;
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=pages_[i];
        if(!refine[i]||!accepted.contains(page.mesh))continue;
        for(const auto& cluster:page.clusters){
            if(cluster.refined>=0&&refine[size_t(cluster.refined)])continue;
            selected.push_back({&page,&cluster});
            result.selection.push_back(cluster.id);
        }
    }
    if(std::ranges::equal(result.selection,previous))return result;
    for(const auto& [page,packed]:selected){
        const auto& cluster=*packed;
        const auto vertex=uint32_t(result.vertices.size()),index=uint32_t(result.indices.size());
        result.vertices.resize(result.vertices.size()+cluster.vertexCount);
        if(meshopt_decodeVertexBuffer(result.vertices.data()+vertex,cluster.vertexCount,sizeof(Vertex),
                                     cluster.vertices.data(),cluster.vertices.size())!=0)
            Fail("MG2: cluster page decoding failed");
        result.indices.insert(result.indices.end(),cluster.indices.begin(),cluster.indices.end());
        result.clusters.push_back({cluster.id,vertex,cluster.vertexCount,index,uint32_t(cluster.indices.size()),
            page->material,page->mesh,page->opaque});
    }
    result.pages=accepted.size();
    return result;
}
}
