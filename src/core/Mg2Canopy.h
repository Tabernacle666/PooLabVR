#pragma once
#include "Assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace OASIS::Mg2Canopy {
// A08: an asset-local, spatial tree of directional foliage aggregates.
// Cards remain triangles with the canonical OMM UVs. This is a volumetric
// distribution of surfaces, not a voxel intersection shader or an impostor.
struct Card {
    std::array<Vertex,4> vertices;
    XMFLOAT3 center,normal,axis;
    float area{};
};
struct Cluster {
    std::vector<Card> cards;
    int child=-1;
};
struct Group {
    XMFLOAT3 center{};
    float radius{},error{};
    unsigned depth{};
    std::vector<Cluster> clusters;
};
struct Hierarchy {
    std::vector<Group> groups;
    size_t sourceCards{},finestCards{},rootCards{},proxyCards{};
    double sourceArea{},rootArea{};
    float maxRelativeAreaError{};
    unsigned maxDepth{};
};
inline XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){
    return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
}
inline bool Finite(XMFLOAT3 p){
    return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
}
inline Card Describe(const std::array<Vertex,4>& vertices){
    Card c{};c.vertices=vertices;
    const auto along=Sub(vertices[3].position,vertices[0].position);
    const auto across=Sub(vertices[1].position,vertices[0].position);
    c.area=Length(along)*Length(across);
    c.center=Scale(Add(vertices[0].position,vertices[2].position),.5f);
    if(c.area>0.f){
        c.axis=Unit(along);
        c.normal=Unit(Cross(across,along));
    }
    return c;
}
inline double Area(const std::vector<Card>& cards){
    double sum=0.;for(const auto& c:cards)sum+=c.area;return sum;
}
inline size_t Split(const std::vector<Card>& cards,std::span<uint32_t> ids){
    auto lo=cards[ids.front()].center,hi=lo;
    for(auto i:ids){
        const auto p=cards[i].center;
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    const auto d=Sub(hi,lo);
    const unsigned axis=d.x>=d.y&&d.x>=d.z?0u:d.y>=d.z?1u:2u;
    const auto coordinate=[&](uint32_t i){
        const auto p=cards[i].center;return axis==0u?p.x:axis==1u?p.y:p.z;
    };
    const size_t middle=ids.size()/2u;
    std::nth_element(ids.begin(),ids.begin()+middle,ids.end(),[&](auto a,auto b){
        const float x=coordinate(a),y=coordinate(b);return x==y?a<b:x<y;
    });
    return middle;
}
inline XMFLOAT3 Center(const std::vector<Card>& cards,std::span<const uint32_t> ids){
    auto lo=cards[ids.front()].vertices[0].position,hi=lo;
    for(auto i:ids)for(const auto& v:cards[i].vertices){
        const auto p=v.position;
        lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
        hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
    }
    return Scale(Add(lo,hi),.5f);
}
// Fit a directional patch to its source support. Area and its centroid are
// conserved. If a dense patch cannot fit, preserve separate spatial layers.
// The original atlas means alpha-weighted area scales with geometric area;
// actual directional opacity, overlap and lighting are not exact invariants.
inline bool Fit(const std::vector<Card>& cards,std::span<const uint32_t> ids,
                XMFLOAT3 reference,Card& proxy,float& error){
    if(ids.size()==1u){proxy=cards[ids.front()];error=0.f;return true;}
    double area=0.;XMFLOAT3 center{},normal{};
    uint32_t largest=ids.front();
    for(auto i:ids){
        const auto& c=cards[i];area+=c.area;
        center=Add(center,Scale(c.center,c.area));
        normal=Add(normal,Scale(c.normal,Dot(c.normal,reference)<0.f?-c.area:c.area));
        if(c.area>cards[largest].area)largest=i;
    }
    if(!(area>0.)||!std::isfinite(area)||!Finite(normal)||Dot(normal,normal)<=0.f)return false;
    center=Scale(center,float(1./area));normal=Unit(normal);
    auto axis=cards[largest].axis;
    axis=Sub(axis,Scale(normal,Dot(axis,normal)));
    if(Dot(axis,axis)<=1.e-12f)return false;
    axis=Unit(axis);const auto side=Unit(Cross(axis,normal));
    float along=0.f,across=0.f,support=0.f;
    for(auto i:ids)for(const auto& v:cards[i].vertices){
        const auto d=Sub(v.position,center);
        along=std::max(along,std::abs(Dot(d,axis)));
        across=std::max(across,std::abs(Dot(d,side)));
        support=std::max(support,Length(d));
    }
    if(!(along>0.f&&across>0.f&&support>0.f))return false;
    const float h=float(std::sqrt(area*double(along)/(4.*double(across))));
    const float w=float(area/(4.*double(h)));
    const float radius=std::hypot(h,w);
    if(!(h>0.f&&w>0.f)||!std::isfinite(radius)||
        h>along||w>across||radius>support)return false;
    const auto a=Sub(center,Scale(axis,h)),b=Add(center,Scale(axis,h));
    const auto s=Scale(side,w);
    proxy=Describe({{
        {Sub(a,s),normal,{0,0},{}},{Add(a,s),normal,{1,0},{}},
        {Add(b,s),normal,{1,1},{}},{Sub(b,s),normal,{0,1},{}}
    }});
    if(!(proxy.area>0.f)||!std::isfinite(proxy.area)||
        std::abs(double(proxy.area)/area-1.)>1.e-4)return false;
    // Conservative geometric support bound, not an optical error metric.
    error=std::nextafter(support+radius,std::numeric_limits<float>::infinity());
    return std::isfinite(error);
}
inline constexpr size_t CardsPerCluster=32; // 4 vertices/card, RTXMG 128/128
inline bool Proxy(const std::vector<Card>& cards,std::span<const uint32_t> ids,
                  std::vector<Card>& output,float& error){
    const std::array<XMFLOAT3,13> axes{{
        {1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,-1,0},{1,0,1},{1,0,-1},
        {0,1,1},{0,1,-1},{1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1}
    }};
    std::array<XMFLOAT3,13> directions{};
    std::array<std::vector<uint32_t>,13> bins;
    for(size_t b=0;b<axes.size();++b)directions[b]=Unit(axes[b]);
    for(auto i:ids){
        size_t bin=0;
        for(size_t b=1;b<axes.size();++b)
            if(std::abs(Dot(cards[i].normal,directions[b]))>
               std::abs(Dot(cards[i].normal,directions[bin])))bin=b;
        bins[bin].push_back(i);
    }
    const auto emit=[&](auto&& self,std::span<uint32_t> group,XMFLOAT3 direction)->bool{
        if(output.size()>=CardsPerCluster)return false;
        Card card{};float e=0.f;
        if(Fit(cards,group,direction,card,e)){
            output.push_back(std::move(card));error=std::max(error,e);return true;
        }
        if(group.size()<2u)return false;
        const auto mid=Split(cards,group);
        return self(self,group.first(mid),direction)&&self(self,group.subspan(mid),direction);
    };
    for(size_t b=0;b<bins.size();++b)
        if(!bins[b].empty()&&!emit(emit,std::span<uint32_t>(bins[b]),directions[b]))return false;
    return !output.empty();
}
inline bool Build(const MeshData& mesh,const MeshRange& range,Hierarchy& out){
    // Only the canonical four-vertex/two-triangle botanical source qualifies.
    // Other alpha-tested meshes still use the A05 general-purpose builder.
    if(!range.vertexCount||range.vertexCount%4u||
        uint64_t(range.vertexCount/4u)*6u!=range.indexCount)return false;
    const size_t count=range.vertexCount/4u;
    std::vector<Card> cards;cards.reserve(count);
    constexpr std::array<unsigned,6> pattern{0,1,2,0,2,3};
    constexpr std::array<XMFLOAT2,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    for(size_t i=0;i<count;++i){
        const size_t base=size_t(range.firstVertex)+i*4u;
        for(size_t j=0;j<pattern.size();++j)
            if(mesh.indices.at(size_t(range.firstIndex)+i*6u+j)!=base+pattern[j])return false;
        std::array<Vertex,4> v{};
        for(size_t j=0;j<v.size();++j){
            v[j]=mesh.vertices.at(base+j);
            if(!Finite(v[j].position)||v[j].uv.x!=uv[j].x||v[j].uv.y!=uv[j].y)return false;
        }
        auto c=Describe(v);
        if(!(c.area>0.f)||!std::isfinite(c.area)||!Finite(c.normal)||!Finite(c.axis))return false;
        cards.push_back(std::move(c));
    }
    out.sourceCards=count;out.sourceArea=Area(cards);
    std::vector<uint32_t> order(count);std::iota(order.begin(),order.end(),0u);
    // Append a page only when it has a parent proxy, or is the root. Child
    // pages precede parents. Unmergeable regions remain full-resolution
    // clusters in the frontier; there are no duplicated full-tree LOD cuts.
    const auto page=[&](std::span<const uint32_t> ids,std::vector<Cluster>&& clusters,
                        const std::vector<Card>& proxy,float error)->int{
        Group g{};g.center=Center(cards,ids);g.error=error;
        const auto include=[&](const Card& card){
            for(const auto& v:card.vertices)
                g.radius=std::max(g.radius,Length(Sub(v.position,g.center)));
        };
        for(auto id:ids)include(cards[id]);
        for(const auto& card:proxy)include(card);
        for(const auto& cluster:clusters){
            for(const auto& card:cluster.cards)include(card);
            if(cluster.child>=0){
                const auto& child=out.groups.at(size_t(cluster.child));
                g.radius=std::max(g.radius,Length(Sub(child.center,g.center))+child.radius);
                g.error=std::max(g.error,child.error);
                g.depth=std::max(g.depth,child.depth+1u);
            }
        }
        // Parent bounds enclose descendants and errors are monotone, which
        // is required by A01's independent spatial traversal of DAG groups.
        g.radius=std::nextafter(g.radius,std::numeric_limits<float>::infinity());
        g.clusters=std::move(clusters);
        const int id=int(out.groups.size());
        out.maxDepth=std::max(out.maxDepth,g.depth);
        out.groups.push_back(std::move(g));return id;
    };
    const auto build=[&](auto&& self,std::span<uint32_t> ids)->std::vector<Cluster>{
        std::vector<Cluster> frontier;
        if(ids.size()<=CardsPerCluster){
            Cluster c{};c.cards.reserve(ids.size());
            for(auto i:ids)c.cards.push_back(cards[i]);
            frontier.push_back(std::move(c));
        }else{
            const auto mid=Split(cards,ids);
            frontier=self(self,ids.first(mid));
            auto right=self(self,ids.subspan(mid));
            for(auto& c:right)frontier.push_back(std::move(c));
        }
        size_t previous=0;double previousArea=0.;
        for(const auto& c:frontier){previous+=c.cards.size();previousArea+=Area(c.cards);}
        if(previous<=1u)return frontier;
        Cluster merged{};float error=0.f;
        if(!Proxy(cards,ids,merged.cards,error)||merged.cards.size()>=previous)return frontier;
        const double area=Area(merged.cards);
        const double relative=std::abs(area/previousArea-1.);
        if(!std::isfinite(relative)||relative>1.e-4)return frontier;
        out.maxRelativeAreaError=std::max(out.maxRelativeAreaError,float(relative));
        merged.child=page(ids,std::move(frontier),merged.cards,error);
        out.proxyCards+=merged.cards.size();
        std::vector<Cluster> result;result.push_back(std::move(merged));return result;
    };
    auto frontier=build(build,std::span<uint32_t>(order));
    for(const auto& c:frontier){out.rootCards+=c.cards.size();out.rootArea+=Area(c.cards);}
    page(std::span<const uint32_t>(order),std::move(frontier),{},0.f);
    for(const auto& g:out.groups)for(const auto& c:g.clusters)
        if(c.child<0)out.finestCards+=c.cards.size();
    return out.finestCards==out.sourceCards&&out.rootCards>0u&&
        std::abs(out.rootArea/out.sourceArea-1.)<=1.e-4;
}
}
