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
    float maxRelativeAreaError{},maxMergeSupportError{},maxMergeGeometricError{};
    float maxOriginalSphereRadius{},maxTightSphereRadius{};
    float maxProjectedCoverageError{},maxShadingNormalError{},maxUvFrameError{},maxOpticalError{};
    size_t opticalMerges{},opticalDominantMerges{};
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
// Return the distance to an actual point inside the rectangle. The edge
// projection is exact for orthogonal cards and remains a conservative
// candidate for slightly nonorthogonal input. No alpha/normal error here:
// A10 owns the optical criterion.
inline double PointCardDistanceSquared(XMFLOAT3 p,const Card& card){
    const auto a=card.vertices[0].position;
    const auto u=Sub(card.vertices[1].position,a),v=Sub(card.vertices[3].position,a);
    const auto d=Sub(p,a);
    const auto dot=[](XMFLOAT3 x,XMFLOAT3 y){
        return double(x.x)*y.x+double(x.y)*y.y+double(x.z)*y.z;
    };
    const double uu=dot(u,u),vv=dot(v,v);
    const double s=uu>0.?std::clamp(dot(d,u)/uu,0.,1.):0.;
    const double t=vv>0.?std::clamp(dot(d,v)/vv,0.,1.):0.;
    const double x=double(d.x)-s*u.x-t*v.x;
    const double y=double(d.y)-s*u.y-t*v.y;
    const double z=double(d.z)-s*u.z-t*v.z;
    return x*x+y*y+z*z;
}
inline double SurfaceError(const std::vector<Card>& cards,
                           std::span<const uint32_t> ids,const Card& proxy){
    double forward=0.,reverse=0.;
    // Distance to a convex rectangle is convex: corner maxima bound
    // the whole source card, not merely the sampled vertices.
    for(auto id:ids)for(const auto& v:cards[id].vertices)
        forward=std::max(forward,PointCardDistanceSquared(v.position,proxy));
    // The reverse target is a union of rectangles, not convex. Partition
    // the proxy into 4x4 convex patches. Each patch is bounded against one
    // source card (min over cards of max over corners); the union remains
    // a conservative bound, including the gaps between source cards.
    constexpr unsigned Grid=4;
    const auto a=proxy.vertices[0].position;
    const auto u=Sub(proxy.vertices[1].position,a),v=Sub(proxy.vertices[3].position,a);
    const auto point=[&](unsigned x,unsigned y){
        return Add(a,Add(Scale(u,float(x)/Grid),Scale(v,float(y)/Grid)));
    };
    for(unsigned y=0;y<Grid;++y)for(unsigned x=0;x<Grid;++x){
        const std::array<XMFLOAT3,4> corners{{point(x,y),point(x+1,y),
                                            point(x+1,y+1),point(x,y+1)}};
        double best=std::numeric_limits<double>::infinity();
        for(auto id:ids){
            double bound=0.;
            for(const auto& p:corners){
                bound=std::max(bound,PointCardDistanceSquared(p,cards[id]));
                if(bound>=best)break;
            }
            best=std::min(best,bound);
        }
        reverse=std::max(reverse,best);
    }
    return std::sqrt(std::max(forward,reverse));
}
// Fit a directional patch to its source support. Area and its centroid are
// conserved. If a dense patch cannot fit, preserve separate spatial layers.
// The original atlas means alpha-weighted area scales with geometric area;
// actual directional opacity, overlap and lighting are not exact invariants.
inline bool Fit(const std::vector<Card>& cards,std::span<const uint32_t> ids,
                XMFLOAT3 reference,Card& proxy,float& error,float& supportError){
    if(ids.size()==1u){proxy=cards[ids.front()];error=supportError=0.f;return true;}
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
    // Bound geometric displacement, not the diameter of the whole region.
    // The old support+radius bound is retained as a diagnostic reference.
    supportError=std::nextafter(support+radius,std::numeric_limits<float>::infinity());
    const double measured=SurfaceError(cards,ids,proxy);
    if(!std::isfinite(measured))return false;
    // Account for float-space corner construction and projection roundoff.
    const float scale=std::max({Length(center),support,radius});
    const double guard=64.*std::numeric_limits<float>::epsilon()*double(scale);
    error=std::nextafter(float(measured+guard),std::numeric_limits<float>::infinity());
    return std::isfinite(error)&&std::isfinite(supportError);
}
// A10: compare the optical attributes of a fitted patch against its
// original cards, not against an already simplified intermediate cut.
// Identical cutout UVs/material imply a common alpha-tested area fraction.
// Summed directional projected area is a coverage proxy, NOT union
// visibility/transmittance: overlap and exact mask silhouettes are not modeled.
// The square root converts the area difference to a length, compatible with
// the existing projected-error traversal; no runtime shader/ABI change.
struct OpticalError {
    float coverage{},normal{},uvFrame{};
    float LengthError() const {return std::max({coverage,normal,uvFrame});}
    void Include(const OpticalError& e){
        coverage=std::max(coverage,e.coverage);
        normal=std::max(normal,e.normal);
        uvFrame=std::max(uvFrame,e.uvFrame);
    }
};
inline OpticalError MeasureOpticalError(const std::vector<Card>& cards,
        std::span<const uint32_t> ids,const Card& proxy,float alphaCoverage){
    OpticalError out{};
    if(ids.size()==1u)return out;
    const auto side=Unit(Sub(proxy.vertices[1].position,proxy.vertices[0].position));
    // Fixed direction set plus the proxy's own grazing and frontal axes.
    // Sampling is a directional estimate, not a mathematical bound.
    const std::array<XMFLOAT3,16> axes{{
        {1,0,0},{0,1,0},{0,0,1},{1,1,0},{1,-1,0},{1,0,1},{1,0,-1},
        {0,1,1},{0,1,-1},{1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1},
        proxy.normal,proxy.axis,side
    }};
    double maxAreaDelta=0.;
    for(const auto& axis:axes){
        const auto direction=Unit(axis);
        double source=0.;
        for(auto id:ids){
            const auto& c=cards[id];
            source+=double(c.area)*std::abs(Dot(c.normal,direction));
        }
        const double projected=double(proxy.area)*std::abs(Dot(proxy.normal,direction));
        maxAreaDelta=std::max(maxAreaDelta,std::abs(source-projected));
    }
    out.coverage=float(std::sqrt(maxAreaDelta*double(alphaCoverage)));
    for(auto id:ids){
        const auto& c=cards[id];
        const auto u=Sub(c.vertices[1].position,c.vertices[0].position);
        const auto v=Sub(c.vertices[3].position,c.vertices[0].position);
        const float halfU=.5f*Length(u),halfV=.5f*Length(v);
        const float radius=std::hypot(halfU,halfV);
        // Two-sided shading: n and -n describe the same tangent plane.
        // Convert angular deviation to local feature displacement.
        const float cosine=std::clamp(std::abs(Dot(c.normal,proxy.normal)),0.f,1.f);
        out.normal=std::max(out.normal,radius*std::sqrt(std::max(0.f,1.f-cosine*cosine)));
        // UV axes are signed: flipping the root-to-tip pattern or mirroring
        // an asymmetric needle atlas is not equivalent to preserving it.
        const float du=halfU*Length(Sub(Unit(u),side));
        const float dv=halfV*Length(Sub(c.axis,proxy.axis));
        out.uvFrame=std::max(out.uvFrame,std::hypot(du,dv));
    }
    return out;
}
inline constexpr size_t CardsPerCluster=32; // 4 vertices/card, RTXMG 128/128
inline bool Proxy(const std::vector<Card>& cards,std::span<const uint32_t> ids,
                  std::vector<Card>& output,float& error,float& supportError,
                  float alphaCoverage,OpticalError& optical){
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
        Card card{};float e=0.f,s=0.f;
        if(Fit(cards,group,direction,card,e,s)){
            optical.Include(MeasureOpticalError(cards,group,card,alphaCoverage));
            output.push_back(std::move(card));error=std::max(error,e);
            supportError=std::max(supportError,s);return true;
        }
        if(group.size()<2u)return false;
        const auto mid=Split(cards,group);
        return self(self,group.first(mid),direction)&&self(self,group.subspan(mid),direction);
    };
    for(size_t b=0;b<bins.size();++b)
        if(!bins[b].empty()&&!emit(emit,std::span<uint32_t>(bins[b]),directions[b]))return false;
    return !output.empty();
}
inline bool Build(const MeshData& mesh,const MeshRange& range,Hierarchy& out,
                  float alphaCoverage=1.f){
    alphaCoverage=std::isfinite(alphaCoverage)?std::clamp(alphaCoverage,0.f,1.f):1.f;
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
        const auto originalCenter=g.center;
        const auto directRadius=[&](XMFLOAT3 center){
            float radius=0.f;
            const auto include=[&](const Card& card){
                for(const auto& v:card.vertices)
                    radius=std::max(radius,Length(Sub(v.position,center)));
            };
            for(auto id:ids)include(cards[id]);
            for(const auto& card:proxy)include(card);
            for(const auto& cluster:clusters)for(const auto& card:cluster.cards)include(card);
            return radius;
        };
        const auto enclosingRadius=[&](XMFLOAT3 center){
            float radius=directRadius(center);
            for(const auto& cluster:clusters)if(cluster.child>=0){
                const auto& child=out.groups.at(size_t(cluster.child));
                radius=std::max(radius,Length(Sub(child.center,center))+child.radius);
            }
            return radius;
        };
        const float originalRadius=enclosingRadius(originalCenter);
        // Merge child spheres by moving the enclosing center instead of
        // inflating around a fixed source AABB center at every DAG level.
        g.radius=directRadius(g.center);
        for(const auto& cluster:clusters)if(cluster.child>=0){
            const auto& child=out.groups.at(size_t(cluster.child));
            const auto delta=Sub(child.center,g.center);
            const float distance=Length(delta);
            if(distance+child.radius>g.radius){
                if(distance+g.radius<=child.radius){
                    g.center=child.center;g.radius=child.radius;
                }else if(distance>0.f){
                    const float radius=.5f*(distance+g.radius+child.radius);
                    g.center=Add(g.center,Scale(delta,(radius-g.radius)/distance));
                    g.radius=radius;
                }
            }
            g.error=std::max(g.error,child.error);
            g.depth=std::max(g.depth,child.depth+1u);
        }
        // Recompute at the final center: all source/representative vertices
        // and complete child spheres must be enclosed. Never enlarge a
        // sphere just to use the new heuristic. Monotone error is unchanged.
        g.radius=enclosingRadius(g.center);
        if(!std::isfinite(g.radius)||g.radius>originalRadius){
            g.center=originalCenter;g.radius=originalRadius;
        }
        out.maxOriginalSphereRadius=std::max(out.maxOriginalSphereRadius,originalRadius);
        const float guard=32.f*std::numeric_limits<float>::epsilon()*
            std::max(Length(g.center),g.radius);
        g.radius=std::nextafter(g.radius+guard,std::numeric_limits<float>::infinity());
        out.maxTightSphereRadius=std::max(out.maxTightSphereRadius,g.radius);
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
        Cluster merged{};float error=0.f,supportError=0.f;OpticalError optical{};
        if(!Proxy(cards,ids,merged.cards,error,supportError,alphaCoverage,optical)||
           merged.cards.size()>=previous)return frontier;
        const double area=Area(merged.cards);
        const double relative=std::abs(area/previousArea-1.);
        if(!std::isfinite(relative)||relative>1.e-4)return frontier;
        out.maxRelativeAreaError=std::max(out.maxRelativeAreaError,float(relative));
        out.maxMergeSupportError=std::max(out.maxMergeSupportError,supportError);
        out.maxMergeGeometricError=std::max(out.maxMergeGeometricError,error);
        const float opticalError=optical.LengthError();
        if(!std::isfinite(opticalError))return frontier;
        ++out.opticalMerges;
        out.opticalDominantMerges+=size_t(opticalError>error);
        out.maxProjectedCoverageError=std::max(out.maxProjectedCoverageError,optical.coverage);
        out.maxShadingNormalError=std::max(out.maxShadingNormalError,optical.normal);
        out.maxUvFrameError=std::max(out.maxUvFrameError,optical.uvFrame);
        out.maxOpticalError=std::max(out.maxOpticalError,opticalError);
        // Same monotone hierarchy and 1.1px consumer: only the local
        // error changes. Child errors are propagated by page() as before.
        merged.child=page(ids,std::move(frontier),merged.cards,std::max(error,opticalError));
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
