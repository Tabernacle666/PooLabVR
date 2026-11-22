#include "Mg2Pages.h"
#include <meshoptimizer.h>
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>

namespace OASIS {
Mg2Pages::Mg2Pages(const SceneData& asset){
    constexpr uint32_t MaxTriangles=64,MaxVertices=128;
    auto config=clodDefaultConfigRT(MaxTriangles);
    config.max_vertices=MaxVertices;
    config.simplify_fallback_sloppy=false;
    constexpr float weights[]{.35f,.35f,.35f,.7f,.7f,.7f,.7f};
    static_assert(offsetof(Vertex,normal)==12&&offsetof(Vertex,uv)==24&&offsetof(Vertex,uv1)==32);
    const auto& mesh=asset.meshes;
    uint32_t nextCluster=0;
    for(const auto& object:asset.objects){
        if(object.flags&(DynamicObject|DeformedObject))continue;
        const auto& range=mesh.ranges.at(object.mesh);
        const auto& surface=asset.materials.at(object.material);
        if(!range.indexCount||surface.alphaMode==2u)continue;
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
                object.material,surface.alphaMode==0u,{}};
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

Mg2LodCut Mg2Pages::Select(std::span<const Mg2LodEye> eyes,float errorPixels)const{
    Mg2LodCut result;
    std::vector<bool> refine;
    refine.reserve(pages_.size());
    for(const auto& page:pages_){
        refine.push_back(std::any_of(eyes.begin(),eyes.end(),[&](const Mg2LodEye& eye){
            const auto depth=std::max(eye.nearPlane,Length(Sub(page.center,eye.position))-page.radius);
            return page.error>errorPixels*depth/eye.focalPixels;
        }));
    }
    for(size_t i=0;i<pages_.size();++i){
        if(!refine[i])continue;
        const auto& page=pages_[i];
        const auto first=result.clusters.size();
        for(const auto& cluster:page.clusters){
            if(cluster.refined>=0&&refine[size_t(cluster.refined)])continue;
            const auto vertex=uint32_t(result.vertices.size()),index=uint32_t(result.indices.size());
            result.vertices.resize(result.vertices.size()+cluster.vertexCount);
            if(meshopt_decodeVertexBuffer(result.vertices.data()+vertex,cluster.vertexCount,sizeof(Vertex),
                                         cluster.vertices.data(),cluster.vertices.size())!=0)
                Fail("MG2: cluster page decoding failed");
            result.indices.insert(result.indices.end(),cluster.indices.begin(),cluster.indices.end());
            result.clusters.push_back({cluster.id,vertex,cluster.vertexCount,index,uint32_t(cluster.indices.size()),
                page.material,page.opaque});
        }
        result.pages+=result.clusters.size()!=first;
    }
    return result;
}
}
