#include "Mg2Pages.h"
#include "Mg2Canopy.h"
#include <meshoptimizer.h>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <limits>
#include <numeric>
#include <map>
#include <stdexcept>
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>

namespace OASIS {
void DiagnosticLine(const char*) noexcept;
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
    // RTXMG Cluster LOD bake defaults: 128/128, 32 clusters per group,
    // normal and texcoord weights 0.5. Use the RT spatial partitioner.
    constexpr uint32_t MaxTriangles=128,MaxVertices=128,GroupSize=32;
    auto baseConfig=clodDefaultConfigRT(MaxTriangles);
    baseConfig.max_vertices=MaxVertices;
    baseConfig.partition_size=GroupSize;
    // Keep the library's conservative error propagation and permissive
    // simplification with protected UV seams. Do not change source positions:
    // border dilation mutates the input and is not a generic surface policy.
    baseConfig.simplify_error_clamped=true;
    baseConfig.simplify_permissive=true;
    baseConfig.simplify_dilate_borders=false;
    struct BakeStats {
        uint64_t meshes{},sourceTriangles{},groups{},clusters{},hierarchyTriangles{},
            finestTriangles{},rootGroups{},rootTriangles{},foldMeshes{};
        unsigned maxDepth{};
    };
    std::array<BakeStats,2> stats{}; // opaque, alpha-tested; authored cuts excluded
    static_assert(offsetof(Vertex,normal)==12&&offsetof(Vertex,uv)==24&&offsetof(Vertex,uv1)==32);
    const auto& mesh=asset.meshes;
    uint32_t nextCluster=0;
    std::unordered_set<uint32_t> botanicalMeshes;
    // A10: base-level alpha-tested area of the canonical botanical atlas.
    // Cache by material; no texture read or additional work per XR frame.
    std::unordered_map<uint32_t,float> botanicalAlphaCoverage;
    const auto alphaCoverage=[&](uint32_t material){
        if(auto it=botanicalAlphaCoverage.find(material);it!=botanicalAlphaCoverage.end())
            return it->second;
        const auto& s=asset.materials.at(material);
        float coverage=1.f; // conservative support if atlas format is not recognized
        if(s.albedo<asset.textures.size()){
            const auto& t=asset.textures[s.albedo];
            const uint64_t texels=uint64_t(t.width)*t.height;
            if((t.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||t.format==DXGI_FORMAT_R8G8B8A8_UNORM)&&
               texels&&!t.offsets.empty()&&t.offsets[0]<=t.bytes.size()&&
               texels<=(t.bytes.size()-t.offsets[0])/4u){
                uint64_t passed=0;
                const size_t offset=size_t(t.offsets[0]);
                for(size_t i=0;i<size_t(texels);++i){
                    const float alpha=float(std::to_integer<unsigned char>(t.bytes[offset+4u*i+3u]))/255.f;
                    passed+=uint64_t(alpha*s.alpha>=s.cutoff);
                }
                coverage=float(double(passed)/double(texels));
            }
        }
        botanicalAlphaCoverage.emplace(material,coverage);
        return coverage;
    };
    for(const auto& object:parts){
        const auto& range=mesh.ranges.at(object.mesh);
        const auto& surface=asset.materials.at(object.material);
        if(!range.indexCount||surface.alphaMode==2u)continue;
        const bool masked=surface.alphaMode!=0u;
        auto config=baseConfig;
        // Sloppy fallback drops attribute-aware scoring. Keep it for opaque
        // geometry with the upstream error penalty, not for cutout coverage.
        config.simplify_fallback_sloppy=!masked;
        // Preserve folds on two-sided sheets; opaque closed solids retain the
        // RT defaults. Keep the existing stricter UV weight for alpha tests.
        config.simplify_preserve_folds=surface.doubleSided!=0u;
        constexpr float NormalWeight=.5f;
        const float uvWeight=masked?.7f:.5f;
        const float weights[]{NormalWeight,NormalWeight,NormalWeight,
            uvWeight,uvWeight,uvWeight,uvWeight};
        auto& report=stats[masked?1u:0u];
        ++report.meshes;
        report.sourceTriangles+=range.indexCount/3u;
        report.foldMeshes+=uint64_t(config.simplify_preserve_folds);
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
        // A08 replaces the generic decimator only for canonical tree foliage.
        // Same cluster/page ABI, same selection, same A06/A07 residency path.
        const bool botanical=masked&&std::any_of(asset.canopy.assemblies.begin(),
            asset.canopy.assemblies.end(),[&](const auto& a){
                return a.meshes[1]==object.mesh&&a.materials[1]==object.material;
            });
        if(botanical){
            Mg2Canopy::Hierarchy tree;
            const float atlasCoverage=alphaCoverage(object.material);
            if(Mg2Canopy::Build(mesh,range,tree,atlasCoverage)){
                const size_t base=pages_.size();
                if(base+tree.groups.size()>size_t(std::numeric_limits<int>::max()))
                    Fail("MG2 A08 page address");
                for(auto& group:tree.groups){
                    Page page{group.center,group.radius,group.error,object.material,object.mesh,false,{}};
                    page.clusters.reserve(group.clusters.size());
                    for(auto& source:group.clusters){
                        const int refined=source.child<0?-1:int(base+size_t(source.child));
                        Cluster packed{nextCluster++,uint32_t(source.cards.size()*4u),refined,{},{}};
                        packed.vertices.reserve(packed.vertexCount);
                        packed.indices.reserve(source.cards.size()*6u);
                        for(const auto& card:source.cards){
                            const auto at=static_cast<unsigned char>(packed.vertices.size());
                            packed.vertices.insert(packed.vertices.end(),card.vertices.begin(),card.vertices.end());
                            packed.indices.insert(packed.indices.end(),{at,
                                static_cast<unsigned char>(at+1u),static_cast<unsigned char>(at+2u),at,
                                static_cast<unsigned char>(at+2u),static_cast<unsigned char>(at+3u)});
                        }
                        if(!packed.vertexCount||packed.vertexCount>MaxVertices||
                            packed.indices.size()/3u>MaxTriangles)Fail("MG2 A08 cluster capacity");
                        sourceBytes_+=uint64_t(packed.vertices.capacity())*sizeof(Vertex)+packed.indices.capacity();
                        page.clusters.push_back(std::move(packed));
                    }
                    pages_.push_back(std::make_shared<Page>(std::move(page)));
                }
                botanicalMeshes.insert(object.mesh);
                // A05 counts only the generic decimator. Report A08 separately.
                --report.meshes;report.sourceTriangles-=range.indexCount/3u;
                report.foldMeshes-=uint64_t(config.simplify_preserve_folds);
                char line[512];
                std::snprintf(line,sizeof(line),
                    "mg2_a08_canopy validated=1 mesh=%u source_cards=%zu finest_cards=%zu root_cards=%zu "
                    "proxy_cards=%zu groups=%zu depth=%u root_area_ratio=%.6f max_merge_area_error=%.8f\r\n",
                    object.mesh,tree.sourceCards,tree.finestCards,tree.rootCards,tree.proxyCards,
                    tree.groups.size(),tree.maxDepth,tree.rootArea/tree.sourceArea,tree.maxRelativeAreaError);
                DiagnosticLine(line);
                std::snprintf(line,sizeof(line),
                    "mg2_a08_bounds mesh=%u support_error_max=%.9g geometric_error_max=%.9g "
                    "fixed_center_radius_max=%.9g tight_radius_max=%.9g\r\n",
                    object.mesh,double(tree.maxMergeSupportError),double(tree.maxMergeGeometricError),
                    double(tree.maxOriginalSphereRadius),double(tree.maxTightSphereRadius));
                DiagnosticLine(line);
                std::snprintf(line,sizeof(line),
                    "mg2_a10_optical mesh=%u atlas_coverage=%.6f merges=%zu optical_dominant=%zu "
                    "coverage_error_max=%.9g normal_error_max=%.9g uv_frame_error_max=%.9g "
                    "optical_error_max=%.9g geometric_error_max=%.9g\r\n",
                    object.mesh,double(atlasCoverage),tree.opticalMerges,tree.opticalDominantMerges,
                    double(tree.maxProjectedCoverageError),double(tree.maxShadingNormalError),
                    double(tree.maxUvFrameError),double(tree.maxOpticalError),double(tree.maxMergeGeometricError));
                DiagnosticLine(line);
                continue;
            }
            char line[160];
            std::snprintf(line,sizeof(line),"mg2_a08_canopy validated=0 mesh=%u fallback=source_contract\r\n",object.mesh);
            DiagnosticLine(line);
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
        // Both UV channels can feed material/optical shaders. Preserve their
        // discontinuities; material boundaries are already isolated by parts.
        input.attribute_protect_mask=0x78u;
        clodBuild(config,input,[&](const clodGroup& group,const clodCluster* clusters,size_t count){
            ++report.groups;
            report.clusters+=count;
            report.maxDepth=std::max(report.maxDepth,unsigned(group.depth));
            const auto& bounds=group.simplified;
            Page page{{bounds.center[0],bounds.center[1],bounds.center[2]},bounds.radius,bounds.error,
                object.material,object.mesh,surface.alphaMode==0u,{}};
            page.clusters.reserve(count);
            for(size_t i=0;i<count;++i){
                const auto& source=clusters[i];
                const auto triangles=source.index_count/3u;
                report.hierarchyTriangles+=triangles;
                if(source.refined<0)report.finestTriangles+=triangles;
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
            pages_.push_back(std::make_shared<Page>(std::move(page)));
            return int(pages_.size()-1);
        });
    }
    // Structural cuts share the original mesh/material routing and the
    // same 128/128 cluster ABI. Keep them separate from refinement edges.
    for(const auto& node:asset.canopy.lods){
        if(botanicalMeshes.contains(node.mesh))continue; // no competing whole-tree foliage cuts
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
        pages_.push_back(std::make_shared<Page>(std::move(page)));
    }
    nextClusterId_=nextCluster;
    // Immutable indexes are prepared once, not rebuilt on each worker request.
    hasParent_.assign(pages_.size(),0u);
    for(size_t p=0;p<pages_.size();++p)for(size_t c=0;c<pages_[p]->clusters.size();++c){
        auto& cluster=pages_[p]->clusters[c];
        sourceClusters_.emplace_back(p,c);
        if(cluster.vertices.size()>UINT32_MAX-vertexCount_||cluster.indices.size()>UINT32_MAX-indexCount_)
            Fail("MG2 immutable source ABI");
        cluster.sourceFirstVertex=vertexCount_;cluster.sourceFirstIndex=indexCount_;
        vertexCount_+=uint32_t(cluster.vertices.size());
        indexCount_+=uint32_t(cluster.indices.size());
    }
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=*pages_[i];
        if(page.structural)continue;
        pagesByMesh_[page.mesh].push_back(i);
        for(const auto& cluster:page.clusters){
            if(cluster.refined<0)continue;
            const size_t child=size_t(cluster.refined);
            if(child>=i||pages_[child]->mesh!=page.mesh)Fail("MG2: invalid refinement edge");
            hasParent_[child]=1u;
        }
    }
    // A root is a terminal group with no coarser parent. Count the complete
    // coarse cut, separately from the original triangles and authored canopy
    // alternatives, so a deeper hierarchy cannot masquerade as source loss.
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=*pages_[i];
        if(page.structural||hasParent_[i])continue;
        auto& report=stats[page.opaque?0u:1u];
        ++report.rootGroups;
        for(const auto& cluster:page.clusters)
            report.rootTriangles+=cluster.indices.size()/3u;
    }
    // A09: asset-local inventory, independent of territorial instance count.
    // Measure the actual DAG, including A08 foliage, not just generic bake
    // totals. Structural alternatives are counted separately from DAG cuts.
    std::vector<unsigned> hierarchyDepth(pages_.size());
    for(const auto& assembly:asset.canopy.assemblies){
        for(unsigned part=0;part<2u;++part){
            const uint32_t id=assembly.meshes[part];
            const auto found=pagesByMesh_.find(id);
            if(found==pagesByMesh_.end())continue;
            uint64_t finest=0,root=0,total=0,clusters=0,roots=0,bytes=0,alternatives=0;
            unsigned maxDepth=0;
            for(const auto index:found->second){
                const auto& page=*pages_[index];
                unsigned depth=0;
                if(!hasParent_[index])++roots;
                for(const auto& cluster:page.clusters){
                    const uint64_t triangles=cluster.indices.size()/3u;
                    ++clusters;total+=triangles;
                    bytes+=uint64_t(cluster.vertices.size())*sizeof(Vertex)+
                        uint64_t(cluster.indices.size())*sizeof(uint32_t);
                    if(cluster.refined<0)finest+=triangles;
                    else depth=std::max(depth,1u+hierarchyDepth[size_t(cluster.refined)]);
                    if(!hasParent_[index])root+=triangles;
                }
                hierarchyDepth[index]=depth;
                maxDepth=std::max(maxDepth,depth);
            }
            if(const auto cuts=structuralPages_.find(id);cuts!=structuralPages_.end())
                alternatives=cuts->second.size();
            const uint64_t source=mesh.ranges.at(id).indexCount/3u;
            char line[640];
            std::snprintf(line,sizeof(line),
                "mg2_a09_hierarchy mesh=%u part=%s source_triangles=%llu finest_triangles=%llu "
                "root_triangles=%llu hierarchy_triangles=%llu groups=%zu root_groups=%llu "
                "clusters=%llu refinement_depth=%u authored_cuts=%llu dag_payload_bytes=%llu validated=%u\r\n",
                id,part?"foliage":"bark",static_cast<unsigned long long>(source),
                static_cast<unsigned long long>(finest),static_cast<unsigned long long>(root),
                static_cast<unsigned long long>(total),found->second.size(),
                static_cast<unsigned long long>(roots),static_cast<unsigned long long>(clusters),maxDepth,
                static_cast<unsigned long long>(alternatives),static_cast<unsigned long long>(bytes),
                unsigned(source==finest&&roots>0&&root>0&&root<=finest));
            DiagnosticLine(line);
        }
    }
    sourceSegments_.push_back({{}, {SourceVirtualBytes(SourceStream::Vertices),SourceVirtualBytes(SourceStream::Indices)}});
    BuildGpuCatalog();
    for(size_t i=0;i<stats.size();++i){
        const auto& report=stats[i];
        char line[640];
        std::snprintf(line,sizeof(line),
            "mg2_a05_bake profile=%s cluster_triangles=%u cluster_vertices=%u group_target=%u "
            "normal_weight=0.5 uv_weight=%s uv_protect=0x78 sloppy=%u error_clamped=1 "
            "fold_meshes=%llu meshes=%llu source_triangles=%llu finest_triangles=%llu "
            "groups=%llu clusters=%llu hierarchy_triangles=%llu root_groups=%llu "
            "root_triangles=%llu max_depth=%u\r\n",
            i?"masked":"opaque",MaxTriangles,MaxVertices,GroupSize,i?"0.7":"0.5",unsigned(i==0),
            static_cast<unsigned long long>(report.foldMeshes),
            static_cast<unsigned long long>(report.meshes),
            static_cast<unsigned long long>(report.sourceTriangles),
            static_cast<unsigned long long>(report.finestTriangles),
            static_cast<unsigned long long>(report.groups),
            static_cast<unsigned long long>(report.clusters),
            static_cast<unsigned long long>(report.hierarchyTriangles),
            static_cast<unsigned long long>(report.rootGroups),
            static_cast<unsigned long long>(report.rootTriangles),report.maxDepth);
        DiagnosticLine(line);
    }
}

void Mg2Pages::IndexClusterIds(){
    clusterRefsById_.assign(nextClusterId_,{SIZE_MAX,SIZE_MAX});
    for(const auto& [page,local]:sourceClusters_){
        const auto id=pages_[page]->clusters[local].id;
        if(id>=clusterRefsById_.size()||clusterRefsById_[id].first!=SIZE_MAX)
            Fail("MG2 source cluster identity");
        clusterRefsById_[id]={page,local};
    }
}

void Mg2Pages::BuildGpuCatalog(){
    IndexClusterIds();
    if(pages_.size()>=UINT32_MAX)Fail("MG2 GPU page ABI");
    gpuPages_.reserve(pages_.size());
    for(size_t i=0;i<pages_.size();++i){
        const auto& page=*pages_[i];
        if(page.clusters.size()>UINT32_MAX||page.structuralTriangles>UINT32_MAX)
            Fail("MG2 GPU page range");
        gpuPages_.push_back({page.center,page.radius,page.error,uint32_t(!hasParent_[i]),
            uint32_t(page.clusters.size()),uint32_t(page.structuralTriangles),uint32_t(page.opaque),
            page.clusters.empty()?UINT32_MAX:page.clusters.front().id});
    }
    for(const auto& [mesh,groups]:pagesByMesh_){
        GpuMesh range{uint32_t(gpuClusters_.size()),0u,uint32_t(gpuStructural_.size()),0u,0u,0u};
        for(size_t index:groups){
            const auto& page=*pages_[index];
            gpuPages_[index].firstCluster=uint32_t(gpuClusters_.size());
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
        // NVIDIA's group traversal uses a spatial tree above the LOD DAG.
        // A node sphere encloses every group sphere and its error bounds every
        // descendant error. Unlike a single DAG parent, this is conservative
        // even when a refined group is referenced by multiple parents.
        std::vector<size_t> spatial=groups;
        const auto build=[&](auto&& self,size_t begin,size_t end)->uint32_t{
            if(gpuNodes_.size()>=UINT32_MAX)Fail("MG2 GPU node ABI");
            const uint32_t index=uint32_t(gpuNodes_.size());
            gpuNodes_.emplace_back();
            Mg2GpuNode node{};node.page=UINT32_MAX;
            if(end-begin==1){
                const auto id=spatial[begin];const auto& p=gpuPages_[id];
                node.center=p.center;node.radius=p.radius;node.error=p.error;
                node.force=p.isRoot;node.page=uint32_t(id);
            }else{
                std::array<double,3> lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
                for(size_t j=begin;j<end;++j){
                    const auto& p=gpuPages_[spatial[j]];
                    const double v[]{p.center.x,p.center.y,p.center.z};
                    for(size_t axis=0;axis<3;++axis){
                        lo[axis]=std::min(lo[axis],v[axis]-double(p.radius));
                        hi[axis]=std::max(hi[axis],v[axis]+double(p.radius));
                    }
                    node.force|=p.isRoot||!std::isfinite(p.error)||!std::isfinite(p.radius);
                    node.error=std::max(node.error,p.error);
                }
                size_t axis=0;
                for(size_t k=1;k<3;++k)if(hi[k]-lo[k]>hi[axis]-lo[axis])axis=k;
                const auto coord=[&](size_t id){
                    const auto& v=gpuPages_[id].center;
                    return axis==0?v.x:axis==1?v.y:v.z;
                };
                const size_t mid=begin+(end-begin)/2;
                std::nth_element(spatial.begin()+begin,spatial.begin()+mid,spatial.begin()+end,
                    [&](size_t x,size_t y){return coord(x)==coord(y)?x<y:coord(x)<coord(y);});
                node.center={float((lo[0]+hi[0])*.5),float((lo[1]+hi[1])*.5),float((lo[2]+hi[2])*.5)};
                double radius=0.;
                for(size_t j=begin;j<end;++j){
                    const auto& p=gpuPages_[spatial[j]];
                    const double x=double(p.center.x)-node.center.x,y=double(p.center.y)-node.center.y,
                        z=double(p.center.z)-node.center.z;
                    radius=std::max(radius,std::sqrt(x*x+y*y+z*z)+double(p.radius));
                }
                node.radius=std::nextafter(float(radius),std::numeric_limits<float>::infinity());
                if(!std::isfinite(node.radius)||!std::isfinite(node.center.x)||
                   !std::isfinite(node.center.y)||!std::isfinite(node.center.z))node.force=1u;
                self(self,begin,mid);self(self,mid,end);
            }
            node.escape=uint32_t(gpuNodes_.size());gpuNodes_[index]=node;
            return index;
        };
        if(spatial.empty())Fail("MG2 GPU empty group tree");
        range.nodeRoot=build(build,0,spatial.size());
        range.nodeEnd=uint32_t(gpuNodes_.size());
        gpuMeshes_.emplace(mesh,range);
    }
}

Mg2Pages Mg2Pages::FromAuthoredTriangles(std::span<const AuthoredMesh> meshes){
    Mg2Pages result;
    // 42 unshared triangles / 126 vertices fit the native 128/128 ABI.
    // Keep the exact UVs, normals and primitive order of the authored proxy.
    constexpr size_t VerticesPerCluster=126u,ClustersPerPage=32u;
    std::unordered_set<uint32_t> ids;
    for(const auto& mesh:meshes){
        if(mesh.mesh>=Mg2ResidentKeyBit||!ids.insert(mesh.mesh).second||
           mesh.triangles.empty()||mesh.triangles.size()%3u)
            Fail("MG2 authored mesh identity or triangle range");
        for(size_t first=0;first<mesh.triangles.size();){
            auto page=std::make_shared<Page>();
            page->mesh=mesh.mesh;page->material=mesh.material;page->opaque=mesh.opaque;
            XMFLOAT3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
            const size_t end=first+std::min(mesh.triangles.size()-first,VerticesPerCluster*ClustersPerPage);
            if(result.pages_.size()>=size_t(INT32_MAX))Fail("MG2 authored page ABI");
            const auto pageIndex=result.pages_.size();
            for(;first<end;){
                const auto count=std::min(VerticesPerCluster,end-first);
                if(result.sourceClusters_.size()>=UINT32_MAX-1u||
                   count>UINT32_MAX-result.vertexCount_||count>UINT32_MAX-result.indexCount_)
                    Fail("MG2 authored source ABI");
                Cluster cluster{};
                cluster.id=uint32_t(result.sourceClusters_.size());
                cluster.refined=-1;cluster.vertexCount=uint32_t(count);
                cluster.sourceFirstVertex=result.vertexCount_;cluster.sourceFirstIndex=result.indexCount_;
                cluster.vertices.reserve(count);cluster.indices.reserve(count);
                for(size_t j=0;j<count;++j){
                    const auto& v=mesh.triangles[first+j];const auto& pos=v.position;
                    if(!std::isfinite(pos.x)||!std::isfinite(pos.y)||!std::isfinite(pos.z)||
                       !std::isfinite(v.normal.x)||!std::isfinite(v.normal.y)||!std::isfinite(v.normal.z)||
                       !std::isfinite(v.uv.x)||!std::isfinite(v.uv.y)||
                       !std::isfinite(v.uv1.x)||!std::isfinite(v.uv1.y))
                        Fail("MG2 authored vertex nonfinite");
                    lo={std::min(lo.x,pos.x),std::min(lo.y,pos.y),std::min(lo.z,pos.z)};
                    hi={std::max(hi.x,pos.x),std::max(hi.y,pos.y),std::max(hi.z,pos.z)};
                    cluster.vertices.push_back(v);cluster.indices.push_back(static_cast<unsigned char>(j));
                }
                result.sourceClusters_.emplace_back(pageIndex,page->clusters.size());
                result.vertexCount_+=uint32_t(count);result.indexCount_+=uint32_t(count);
                result.sourceBytes_+=count*(sizeof(Vertex)+sizeof(unsigned char));
                page->clusters.push_back(std::move(cluster));first+=count;
            }
            page->center={float((double(lo.x)+hi.x)*.5),float((double(lo.y)+hi.y)*.5),float((double(lo.z)+hi.z)*.5)};
            double radius=0.;
            for(const auto& cluster:page->clusters)for(const auto& vertex:cluster.vertices){
                const auto& v=vertex.position;
                const double x=double(v.x)-page->center.x,y=double(v.y)-page->center.y,z=double(v.z)-page->center.z;
                radius=std::max(radius,std::sqrt(x*x+y*y+z*z));
            }
            page->radius=std::nextafter(float(radius),std::numeric_limits<float>::infinity());
            if(!std::isfinite(page->radius))Fail("MG2 authored bounds");
            page->error=0.f; // exact relative to the authored cut, NOT its descendants
            result.pagesByMesh_[mesh.mesh].push_back(pageIndex);
            result.pages_.push_back(std::move(page));
        }
    }
    result.nextClusterId_=uint32_t(result.sourceClusters_.size());
    result.hasParent_.assign(result.pages_.size(),0u);
    result.sourceSegments_.push_back({{}, {result.SourceVirtualBytes(SourceStream::Vertices),result.SourceVirtualBytes(SourceStream::Indices)}});
    result.BuildGpuCatalog();
    return result;
}

bool Mg2Pages::IsPrefixOf(const Mg2Pages& next)const{
    return pages_.size()<=next.pages_.size()&&segmentIdentities_.size()<=next.segmentIdentities_.size()&&
        std::equal(segmentIdentities_.begin(),segmentIdentities_.end(),next.segmentIdentities_.begin());
}

std::shared_ptr<const Mg2Pages> Mg2Pages::Extend(const Mg2Pages& segment,
    std::span<const std::pair<uint32_t,uint32_t>> meshRemap)const{
    if(segment.pages_.empty()||segment.retiredClusters_)Fail("MG2 empty or retired catalog segment");
    const auto checked=[](uint64_t base,uint64_t count,uint64_t limit){
        if(base>limit||count>limit-base)Fail("MG2 catalog append ABI");
        return uint32_t(base+count);
    };
    // Seal the last tile of each existing stream, including non-power-of-two
    // Vertex strides. New data cannot change a tile mapped by an older cut.
    const auto alignedElements=[&](SourceStream stream,uint32_t count){
        const auto step=SourceTileBytes/std::gcd(SourceTileBytes,SourceStride(stream));
        return checked(0,((uint64_t(count)+step-1u)/step)*step,UINT32_MAX);
    };
    const uint32_t vertexBase=alignedElements(SourceStream::Vertices,vertexCount_);
    const uint32_t indexBase=alignedElements(SourceStream::Indices,indexCount_);
    const uint32_t vertexEnd=checked(vertexBase,segment.vertexCount_,UINT32_MAX);
    const uint32_t indexEnd=checked(indexBase,segment.indexCount_,UINT32_MAX);
    const uint32_t pageBase=checked(0,pages_.size(),INT32_MAX);
    checked(pageBase,segment.pages_.size(),INT32_MAX);
    const uint32_t idBase=nextClusterId_;
    const uint32_t idEnd=checked(idBase,segment.nextClusterId_,UINT32_MAX-1u);
    const uint32_t clusterBase=checked(0,gpuClusters_.size(),UINT32_MAX-1u);
    checked(clusterBase,segment.gpuClusters_.size(),UINT32_MAX-1u);
    const uint32_t nodeBase=checked(0,gpuNodes_.size(),UINT32_MAX-1u);
    checked(nodeBase,segment.gpuNodes_.size(),UINT32_MAX-1u);
    const uint32_t structuralBase=checked(0,gpuStructural_.size(),UINT32_MAX-1u);
    checked(structuralBase,segment.gpuStructural_.size(),UINT32_MAX-1u);
    if(sourceBytes_>UINT64_MAX-segment.sourceBytes_||revision_==UINT64_MAX)
        Fail("MG2 catalog size overflow");
    std::unordered_map<uint32_t,uint32_t> remap;
    for(const auto& [from,to]:meshRemap)
        if(to>=Mg2ResidentKeyBit||!remap.emplace(from,to).second)
            Fail("MG2 invalid segment mesh remap");
    const auto meshId=[&](uint32_t id){
        if(const auto found=remap.find(id);found!=remap.end())return found->second;
        return id;
    };
    std::unordered_set<uint32_t> used,localMeshes;
    for(const auto& page:segment.pages_)if(localMeshes.insert(page->mesh).second){
        const auto id=meshId(page->mesh);
        if(id>=Mg2ResidentKeyBit||gpuMeshes_.contains(id)||pagesByMesh_.contains(id)||
           structuralPages_.contains(id)||retiredMeshes_.contains(id)||!used.insert(id).second)
            Fail("MG2 representation ID already registered");
    }
    for(const auto& [from,to]:remap)if(!localMeshes.contains(from))
        Fail("MG2 unused segment mesh remap");
    // Copy the metadata snapshot, not the original vertex/index payloads.
    // All writes below affect only the new snapshot; a failed append cannot
    // change the live catalog or its CLAS/BLAS keys.
    auto next=std::shared_ptr<Mg2Pages>(new Mg2Pages(*this));
    next->pages_.reserve(pages_.size()+segment.pages_.size());
    for(const auto& old:segment.pages_){
        auto page=std::make_shared<Page>(*old);
        page->mesh=meshId(page->mesh);
        for(auto& cluster:page->clusters){
            cluster.id+=idBase;
            if(cluster.refined>=0)cluster.refined+=int(pageBase);
            cluster.sourceFirstVertex+=vertexBase;
            cluster.sourceFirstIndex+=indexBase;
        }
        next->pages_.push_back(std::move(page));
    }
    next->hasParent_.insert(next->hasParent_.end(),segment.hasParent_.begin(),segment.hasParent_.end());
    for(const auto& [mesh,groups]:segment.pagesByMesh_){
        auto& dst=next->pagesByMesh_[meshId(mesh)];
        for(const auto index:groups)dst.push_back(pageBase+index);
    }
    for(const auto& [mesh,groups]:segment.structuralPages_){
        auto& dst=next->structuralPages_[meshId(mesh)];
        for(const auto index:groups)dst.push_back(pageBase+index);
    }
    for(const auto& [page,local]:segment.sourceClusters_)
        next->sourceClusters_.emplace_back(pageBase+page,local);
    for(size_t j=0;j<segment.gpuPages_.size();++j){
        auto page=segment.gpuPages_[j];
        if(page.firstId!=UINT32_MAX)page.firstId+=idBase;
        if(!segment.pages_[j]->structural)page.firstCluster+=clusterBase;
        next->gpuPages_.push_back(page);
    }
    for(auto cluster:segment.gpuClusters_){
        cluster.page+=pageBase;
        if(cluster.refined!=UINT32_MAX)cluster.refined+=pageBase;
        next->gpuClusters_.push_back(cluster);
    }
    for(auto node:segment.gpuNodes_){
        if(node.page!=UINT32_MAX)node.page+=pageBase;
        node.escape+=nodeBase;
        next->gpuNodes_.push_back(node);
    }
    for(auto page:segment.gpuStructural_)next->gpuStructural_.push_back(page+pageBase);
    for(const auto& [mesh,old]:segment.gpuMeshes_){
        auto range=old;
        range.first+=clusterBase;
        range.structuralFirst+=structuralBase;
        range.nodeRoot+=nodeBase;range.nodeEnd+=nodeBase;
        next->gpuMeshes_.emplace(meshId(mesh),range);
    }
    next->vertexCount_=vertexEnd;next->indexCount_=indexEnd;
    next->sourceSegments_.push_back({
        {uint64_t(vertexBase)*sizeof(Vertex),uint64_t(indexBase)*sizeof(uint32_t)},
        {next->SourceVirtualBytes(SourceStream::Vertices),next->SourceVirtualBytes(SourceStream::Indices)}});

    next->nextClusterId_=idEnd;
    next->clusterRefsById_.resize(idEnd,{SIZE_MAX,SIZE_MAX});
    for(size_t id=0;id<segment.clusterRefsById_.size();++id){
        const auto [page,local]=segment.clusterRefsById_[id];
        if(page!=SIZE_MAX)next->clusterRefsById_[idBase+id]={pageBase+page,local};
    }
    next->segmentIdentities_.push_back(std::make_shared<const SegmentIdentity>());
    next->sourceBytes_+=segment.sourceBytes_;
    ++next->revision_;
    return next;
}

std::shared_ptr<const Mg2Pages> Mg2Pages::RetireMeshes(std::span<const uint32_t> ids)const{
    auto next=std::shared_ptr<Mg2Pages>(new Mg2Pages(*this));
    bool changed=false;
    for(const auto id:ids){
        if(next->retiredMeshes_.contains(id))continue;
        const auto retirePages=[&](auto& index){
            const auto found=index.find(id);
            if(found==index.end())return;
            for(const auto p:found->second){
                const auto& page=next->pages_.at(p);
                if(!page||page->mesh!=id)Fail("MG2 retirement page identity");
                for(const auto& cluster:page->clusters){
                    if(cluster.id>=next->nextClusterId_)Fail("MG2 retirement cluster identity");
                    if(cluster.id>=next->retiredClusterFlags_.size())
                        next->retiredClusterFlags_.resize(size_t(cluster.id)+1u,0u);
                    if(next->retiredClusterFlags_[cluster.id])Fail("MG2 duplicate retirement");
                    next->retiredClusterFlags_[cluster.id]=1u;
                    ++next->retiredClusters_;
                    const auto bytes=uint64_t(cluster.vertices.size())*sizeof(Vertex)+cluster.indices.size();
                    if(bytes>next->sourceBytes_)Fail("MG2 retirement source accounting");
                    next->sourceBytes_-=bytes;
                }
                next->pages_[p].reset();
            }
            index.erase(found);changed=true;
        };
        retirePages(next->pagesByMesh_);
        retirePages(next->structuralPages_);
        next->gpuMeshes_.erase(id);
        next->retiredMeshes_.insert(id); // never recycle a published identity
    }
    if(changed){
        // Keep address high-water marks and GPU indices stable. Only remove
        // packing references to dead source payload, preserving sorted order.
        std::erase_if(next->sourceClusters_,[&](const auto& ref){return !next->pages_[ref.first];});
        if(next->revision_==UINT64_MAX)Fail("MG2 catalog revision overflow");
        ++next->revision_;
    }
    return next;
}

bool Mg2Pages::SourceRangeLive(SourceStream stream,uint64_t first,uint64_t bytes)const{
    if(!bytes||first>=SourceBytes(stream))return false;
    const bool indices=stream==SourceStream::Indices;
    const auto start=[&](const auto& ref){
        const auto& c=pages_[ref.first]->clusters[ref.second];
        return indices?uint64_t(c.sourceFirstIndex)*sizeof(uint32_t):
            uint64_t(c.sourceFirstVertex)*sizeof(Vertex);
    };
    const auto end=[&](const auto& ref){
        const auto& c=pages_[ref.first]->clusters[ref.second];
        return start(ref)+(indices?uint64_t(c.indices.size())*sizeof(uint32_t):
            uint64_t(c.vertices.size())*sizeof(Vertex));
    };
    const auto it=std::partition_point(sourceClusters_.begin(),sourceClusters_.end(),
        [&](const auto& ref){return end(ref)<=first;});
    return it!=sourceClusters_.end()&&start(*it)-std::min(start(*it),first)<bytes;
}

void Mg2Pages::PackSourceTile(SourceStream stream,uint64_t byteOffset,std::span<std::byte> destination)const{
    if(destination.size()!=SourceTileBytes||byteOffset%SourceTileBytes||
       byteOffset>=SourceVirtualBytes(stream))Fail("MG2 source tile range");
    std::fill(destination.begin(),destination.end(),std::byte{});
    const bool indices=stream==SourceStream::Indices;
    const uint64_t first=byteOffset;
    const uint64_t last=first+destination.size();
    const auto get=[&](const auto& ref)->const Cluster&{
        return pages_[ref.first]->clusters[ref.second];
    };
    const auto start=[&](const Cluster& c)->uint64_t{
        return indices?uint64_t(c.sourceFirstIndex)*sizeof(uint32_t):
            uint64_t(c.sourceFirstVertex)*sizeof(Vertex);
    };
    const auto bytes=[&](const Cluster& c)->uint64_t{
        return indices?uint64_t(c.indices.size())*sizeof(uint32_t):
            uint64_t(c.vertices.size())*sizeof(Vertex);
    };
    // Each physical page is initialized completely, including neighbours
    // not yet demanded. A later cut never needs to modify a resident tile.
    auto it=std::partition_point(sourceClusters_.begin(),sourceClusters_.end(),
        [&](const auto& ref){const auto& c=get(ref);return start(c)+bytes(c)<=first;});
    for(;it!=sourceClusters_.end();++it){
        const auto& c=get(*it);const auto base=start(c);
        if(base>=last)break;
        const auto lo=std::max(base,first),hi=std::min(base+bytes(c),last);
        if(lo>=hi)continue;
        auto* dst=destination.data()+size_t(lo-first);
        if(indices){
            for(uint64_t offset=lo;offset<hi;offset+=sizeof(uint32_t)){
                const uint32_t value=c.indices[size_t((offset-base)/sizeof(uint32_t))];
                std::memcpy(dst+size_t(offset-lo),&value,sizeof(value));
            }
        }else{
            const auto* src=reinterpret_cast<const std::byte*>(c.vertices.data());
            std::memcpy(dst,src+size_t(lo-base),size_t(hi-lo));
        }
    }
}
void Mg2Pages::AppendOpacityTriangles(uint32_t mesh,std::vector<std::array<XMFLOAT2,3>>& out)const{
    for(const auto& page:pages_)if(page&&page->mesh==mesh&&!page->opaque)
        for(const auto& cluster:page->clusters)
            for(size_t i=0;i<cluster.indices.size();i+=3)
                out.push_back({cluster.vertices[cluster.indices[i]].uv,
                    cluster.vertices[cluster.indices[i+1]].uv,cluster.vertices[cluster.indices[i+2]].uv});
}

bool Mg2Pages::RegistryMesh(uint32_t mesh,Mg2GpuRegistryEntry& entry)const{
    const auto found=gpuMeshes_.find(mesh);
    if(found==gpuMeshes_.end())return false;
    const auto& source=found->second;
    if(!source.count)Fail("MG2 GPU empty mesh");
    entry.first=source.first;entry.count=source.count;
    entry.structuralFirst=source.structuralFirst;entry.structuralCount=source.structuralCount;
    entry.nodeRoot=source.nodeRoot;entry.nodeEnd=source.nodeEnd;
    return true;
}
Mg2LodCut Mg2Pages::Select(std::span<const Mg2GpuRegistryEntry> requests,uint64_t scratchWords,float errorPixels,
    const Mg2GpuDispatch& dispatch,std::span<const uint32_t> previous,std::span<const Mg2InstanceRoute> previousRoutes,
    std::span<const Mg2InstanceRoute> frozenRoutes)const{
    Mg2LodCut result;
    if(scratchWords+requests.size()+4u>UINT32_MAX||requests.size()>(UINT32_MAX-4u)/2u)
        throw std::length_error("MG2 GPU selection address range");
    result.candidates=result.eligible=uint32_t(requests.size());
    struct Selected {const Page* page;const Cluster* cluster;};
    struct Variant {std::vector<Selected> clusters;uint32_t part{UINT32_MAX};};
    std::map<std::vector<uint32_t>,Variant> variants;
    std::vector<std::pair<Mg2InstanceRoute,Variant*>> assignments;
    assignments.reserve(requests.size()+frozenRoutes.size());
    // Decode compact cuts against persistent camera-independent metadata.
    if(!requests.empty()){
        const size_t headerWords=4u+2u*requests.size();
        std::vector<uint32_t> words(4u+requests.size()+scratchWords);
        dispatch(words,errorPixels);
        const size_t used=words[0];
        if(used<headerWords||used>words.size()||words[1]>requests.size())
            Fail("MG2 GPU compact header");
        std::vector<Variant*> representatives(requests.size(),nullptr);
        uint32_t unique=0;
        for(size_t i=0;i<requests.size();++i){
            const size_t rep=words[4u+2u*i];
            if(rep>=requests.size()||words[4u+2u*rep]!=rep||
               requests[rep].first!=requests[i].first||requests[rep].count!=requests[i].count)
                Fail("MG2 GPU representative route");
            if(!representatives[rep]){
                const auto& request=requests[rep];
                const size_t offset=words[5u+2u*rep];
                if(offset<headerWords||offset>=used||used-offset<2u)Fail("MG2 GPU compact offset");
                const uint32_t structural=words[offset],encoding=words[offset+1u];
                std::vector<uint32_t> ids;
                std::vector<Selected> selected;
                const auto append=[&](const Page& page,const Cluster& cluster){
                    ids.push_back(cluster.id);selected.push_back({&page,&cluster});
                };
                const auto appendLocal=[&](uint32_t local){
                    if(local>=request.count)Fail("MG2 GPU selection cluster range");
                    const auto& c=gpuClusters_.at(size_t(request.first)+local);
                    if(!pages_.at(c.page))Fail("MG2 stale retired cluster selection");
                    const auto& page=*pages_[c.page];append(page,page.clusters[c.local]);
                };
                if(structural){
                    const size_t index=size_t(structural-1u);
                    const auto rootPage=gpuClusters_.at(request.first).page;
                    if(encoding||index>=pages_.size()||!pages_[index]||!pages_[index]->structural||
                       !pages_.at(rootPage)||pages_[index]->mesh!=pages_[rootPage]->mesh)
                        Fail("MG2 GPU structural selection");
                    for(const auto& cluster:pages_[index]->clusters)append(*pages_[index],cluster);
                }else{
                    const size_t count=encoding&0x7fffffffu;
                    if(count>used-offset-2u)Fail("MG2 GPU compact payload");
                    if(encoding&0x80000000u){
                        const size_t expected=request.count/32u+uint32_t(request.count%32u!=0u);
                        if(count!=expected)Fail("MG2 GPU membership range");
                        for(size_t word=0;word<count;++word){
                            uint32_t bits=words[offset+2u+word];
                            while(bits){
                                appendLocal(uint32_t(word)*32u+uint32_t(std::countr_zero(bits)));
                                bits&=bits-1u;
                            }
                        }
                    }else{
                        uint32_t last=0;
                        for(size_t j=0;j<count;++j){
                            const uint32_t local=words[offset+2u+j];
                            if(j&&local<=last)Fail("MG2 GPU compact order");
                            appendLocal(local);last=local;
                        }
                    }
                }
                if(ids.empty())Fail("MG2 GPU empty hierarchy cut");
                auto [it,inserted]=variants.try_emplace(std::move(ids));
                // GPU dedup is exact over the full snapshot. This ordered
                // container exists only to retain stable downstream ordinals.
                if(!inserted)Fail("MG2 GPU duplicate representative");
                it->second.clusters=std::move(selected);
                representatives[rep]=&it->second;++unique;
            }
            assignments.emplace_back(requests[i].key,representatives[rep]);
        }
        if(unique!=words[1])Fail("MG2 GPU unique count");
    }
    // Covered descendants retain the last published variant for immediate
    // fallback. They have no GPU registry entry, no DAG visit and no
    // compact readback row. Rejoin their immutable variant by cluster ID,
    // once per distinct frozen variant, not once per descendant geometry.
    if(!frozenRoutes.empty()){
        std::vector<std::span<const uint32_t>> prior;
        for(size_t cursor=0;cursor<previous.size();){
            const auto count=previous[cursor++];
            if(!count||count>previous.size()-cursor)Fail("MG2 frozen prior variant");
            prior.push_back(previous.subspan(cursor,count));cursor+=count;
        }
        std::vector<Variant*> resolved(prior.size());
        for(const auto& route:frozenRoutes){
            if(route[3]>=prior.size()||Mg2IsCollectiveKey(route))Fail("MG2 frozen route identity");
            auto*& variant=resolved[route[3]];
            if(!variant){
                const auto ids=prior[route[3]];
                auto [it,inserted]=variants.try_emplace(std::vector<uint32_t>(ids.begin(),ids.end()));
                variant=&it->second;
                if(inserted)for(const auto id:ids){
                    if(!ClusterLive(id)||id>=clusterRefsById_.size())Fail("MG2 frozen retired cluster");
                    const auto [page,local]=clusterRefsById_[id];
                    if(page>=pages_.size()||!pages_[page]||
                       local>=pages_[page]->clusters.size())Fail("MG2 frozen source cluster");
                    const auto& source=*pages_[page];
                    variant->clusters.push_back({&source,&source.clusters[local]});
                }
            }
            if(variant->clusters.empty()||variant->clusters.front().page->mesh!=(route[2]&~Mg2ResidentKeyBit))
                Fail("MG2 frozen mesh mismatch");
            assignments.emplace_back(route,variant);
        }
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
    if(result.routes.size()!=uint64_t(result.eligible)+frozenRoutes.size())Fail("MG2: incomplete coverage");
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
