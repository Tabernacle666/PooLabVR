#include "Assets.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <map>
#include <numeric>
#include <tuple>

namespace OASIS {
namespace {
// The size controls publication granularity, not the total scene complexity.
constexpr uint32_t TrianglesPerRegion=512;
uint32_t SpreadBits(uint32_t value) {
    value&=1023u;
    value=(value|(value<<16))&0x030000ffu;
    value=(value|(value<<8))&0x0300f00fu;
    value=(value|(value<<4))&0x030c30c3u;
    return (value|(value<<2))&0x09249249u;
}
uint32_t SpatialKey(XMFLOAT3 p,XMFLOAT3 lo,XMFLOAT3 hi) {
    auto q=[](float x,float a,float b){return b>a?uint32_t(std::clamp((x-a)/(b-a),0.f,1.f)*1023.f):0u;};
    return SpreadBits(q(p.x,lo.x,hi.x))|(SpreadBits(q(p.y,lo.y,hi.y))<<1)|(SpreadBits(q(p.z,lo.z,hi.z))<<2);
}
struct OrderedTriangle {uint32_t first{},key{},solid{Invalid};};
struct WeldPoint {
    int64_t x{},y{},z{};
    bool operator<(const WeldPoint& b)const noexcept{return std::tie(x,y,z)<std::tie(b.x,b.y,b.z);}
    bool operator==(const WeldPoint& b)const noexcept{return x==b.x&&y==b.y&&z==b.z;}
};
struct WeldEdge {
    WeldPoint a,b;
    bool operator<(const WeldEdge& e)const noexcept{return a<e.a||(!(e.a<a)&&b<e.b);}
};
struct EdgeUse {uint32_t triangle{},count{};int32_t orientation{};};
WeldPoint Weld(XMFLOAT3 p){
    constexpr double precision=1000000.;
    return {int64_t(std::llround(double(p.x)*precision)),int64_t(std::llround(double(p.y)*precision)),int64_t(std::llround(double(p.z)*precision))};
}
std::vector<uint32_t> ClosedSolids(const SceneData& scene){
    const auto& mesh=scene.meshes;
    std::vector<uint32_t> parent(mesh.indices.size()/3,Invalid);
    std::map<WeldEdge,EdgeUse> edges;
    auto root=[&](uint32_t id){while(parent[id]!=id){parent[id]=parent[parent[id]];id=parent[id];}return id;};
    for(const auto& object:scene.objects){
        const auto& range=mesh.ranges[object.mesh];
        if(range.firstIndex%3||range.indexCount%3||uint64_t(range.firstIndex)+range.indexCount>mesh.indices.size())Fail("spectacle solid index range");
        for(uint32_t i=0;i<range.indexCount;i+=3){
            const uint32_t id=(range.firstIndex+i)/3;
            if(parent[id]!=Invalid)Fail("spectacle overlapping solid index ranges");
            parent[id]=id;WeldPoint p[3]{};
            for(uint32_t j=0;j<3;j++){
                uint32_t index=mesh.indices[range.firstIndex+i+j];
                if(index>=mesh.vertices.size())Fail("spectacle solid vertex index");
                p[j]=Weld(mesh.vertices[index].position);
            }
            for(uint32_t j=0;j<3;j++){
                auto a=p[j],b=p[(j+1)%3];if(a==b)continue;
                bool forward=a<b;WeldEdge key{forward?a:b,forward?b:a};
                auto [it,inserted]=edges.try_emplace(key,EdgeUse{id,0,0});
                auto& use=it->second;++use.count;use.orientation+=forward?1:-1;
                if(!inserted){auto x=root(id),y=root(use.triangle);if(x!=y)parent[x]=y;}
            }
        }
    }
    std::vector<uint8_t> closed(parent.size(),1);
    for(const auto& [edge,use]:edges){(void)edge;if(use.count!=2||use.orientation!=0)closed[root(use.triangle)]=0;}
    std::vector<uint32_t> result(parent.size(),Invalid);
    for(uint32_t i=0;i<parent.size();++i)if(parent[i]!=Invalid){auto r=root(i);if(closed[r])result[i]=r;}
    return result;
}
}

void PrepareSpectacleGeometry(SceneData& scene) {
    // The prepared path is for static opaque geometry; keep other scenes unchanged.
    if(scene.objects.empty())return;
    const auto& original=scene.meshes;
    for(const auto& object:scene.objects){
        if(object.mesh>=original.ranges.size()||object.material>=scene.materials.size())Fail("spectacle object mesh/material");
        if(object.mesh>=original.motions.size()||object.mesh>=original.deformations.size())Fail("spectacle mesh metadata");
        if(original.motions[object.mesh].node!=Invalid||original.deformations[object.mesh].axisAmplitude.w!=0 ||
           scene.materials[object.material].alphaMode==2 || (object.flags&(DynamicObject|DeformedObject|AlphaBlendObject)))return;
    }
    const auto sourceSolids=ClosedSolids(scene);
    MeshData source=std::move(scene.meshes), result;
    result.vertices=std::move(source.vertices);result.skin=std::move(source.skin);result.morphs=std::move(source.morphs);
    scene.triangleSolids.clear();scene.triangleSolids.reserve(source.indices.size()/3);
    std::vector<RenderObject> objects;
    for(const auto& object:scene.objects){
        const auto& src=source.ranges[object.mesh];
        if(src.indexCount%3||uint64_t(src.firstIndex)+src.indexCount>source.indices.size())Fail("spectacle index range");
        std::vector<OrderedTriangle> order;order.reserve(src.indexCount/3);
        XMFLOAT3 lo{FLT_MAX,FLT_MAX,FLT_MAX},hi{-FLT_MAX,-FLT_MAX,-FLT_MAX};
        for(uint32_t i=0;i<src.indexCount;i++){
            const auto index=source.indices[src.firstIndex+i];if(index>=result.vertices.size())Fail("spectacle vertex index");
            const auto& p=result.vertices[index].position;
            lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
            hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
        }
        for(uint32_t i=0;i<src.indexCount;i+=3){
            auto a=result.vertices[source.indices[src.firstIndex+i]].position;
            auto b=result.vertices[source.indices[src.firstIndex+i+1]].position;
            auto c=result.vertices[source.indices[src.firstIndex+i+2]].position;
            order.push_back({src.firstIndex+i,SpatialKey(Scale(Add(Add(a,b),c),1.f/3.f),lo,hi),sourceSolids[(src.firstIndex+i)/3]});
        }
        std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.key<b.key;});
        for(size_t begin=0;begin<order.size();begin+=TrianglesPerRegion){
            const size_t end=std::min(order.size(),begin+TrianglesPerRegion);
            MeshRange range{};range.firstVertex=src.firstVertex;range.vertexCount=src.vertexCount;
            range.firstIndex=uint32_t(result.indices.size());range.firstMeshlet=uint32_t(result.meshlets.size());
            XMFLOAT3 min{FLT_MAX,FLT_MAX,FLT_MAX},max{-FLT_MAX,-FLT_MAX,-FLT_MAX};
            std::array<uint32_t,64> remap{};uint32_t vertices=0,triangles=0;
            Meshlet current{uint32_t(result.remap.size()),uint32_t(result.triangles.size()),0,0};
            auto flush=[&]{if(!triangles)return;current.vertexCount=vertices;current.triangleCount=triangles;result.meshlets.push_back(current);result.remap.insert(result.remap.end(),remap.begin(),remap.begin()+vertices);vertices=triangles=0;current={uint32_t(result.remap.size()),uint32_t(result.triangles.size()),0,0};};
            for(size_t k=begin;k<end;k++){
                scene.triangleSolids.push_back(order[k].solid);
                uint32_t ids[3]={source.indices[order[k].first],source.indices[order[k].first+1],source.indices[order[k].first+2]};
                uint32_t needed=0;for(auto id:ids)if(std::find(remap.begin(),remap.begin()+vertices,id)==remap.begin()+vertices)++needed;
                if(vertices+needed>64||triangles==124)flush();
                std::array<uint32_t,3> local{};
                for(uint32_t j=0;j<3;j++){
                    const uint32_t id=ids[j];auto found=std::find(remap.begin(),remap.begin()+vertices,id);
                    if(found==remap.begin()+vertices)remap[vertices++]=id;
                    local[j]=uint32_t(found-remap.begin());result.indices.push_back(id);
                    const auto& p=result.vertices[id].position;
                    min={std::min(min.x,p.x),std::min(min.y,p.y),std::min(min.z,p.z)};
                    max={std::max(max.x,p.x),std::max(max.y,p.y),std::max(max.z,p.z)};
                }
                result.triangles.push_back(local);++triangles;
            }
            flush();range.indexCount=uint32_t(result.indices.size())-range.firstIndex;
            range.meshletCount=uint32_t(result.meshlets.size())-range.firstMeshlet;
            auto center=Scale(Add(min,max),.5f);float radius=0;
            for(uint32_t i=range.firstIndex;i<range.firstIndex+range.indexCount;i++)radius=std::max(radius,Length(Sub(result.vertices[result.indices[i]].position,center)));
            range.bounds={center.x,center.y,center.z,radius};
            RenderObject part=object;part.mesh=uint32_t(result.ranges.size());part.id=uint32_t(objects.size());
            result.ranges.push_back(range);result.deformations.push_back(source.deformations[object.mesh]);result.motions.push_back(source.motions[object.mesh]);objects.push_back(part);
        }
    }
    scene.meshes=std::move(result);scene.objects=std::move(objects);scene.writableStaticGeometry=true;
}
}
