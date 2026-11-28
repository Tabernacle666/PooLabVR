#pragma once
#include "Assets.h"
#include "GpuSceneCache.h"
#include "DiskBake.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace OASIS {
// LOD-21: CPU-only, immutable source geometry and territorial cuts. No mesh
// is appended to SceneData; no additional WorldObject, GPU route or BLAS.
// A patch is a triangulated directional surface voxel, not a solid box.
// The representative triangle retains its material and original UV frame.
// Coverage is summed alpha-weighted projected area, not exact transmittance.
// Consumers must use the stored error/support to decide whether to refine.
class TerritorialPatrimony {
public:
    enum Kind : uint32_t { GroundFlora=0, Deadwood=1, TreeWood=2, TreeFoliage=3 };
    static constexpr unsigned Levels=3;
    struct Key {
        int64_t x{},z{}; // aligned origins, in existing 10m cells
        uint32_t level{};
        bool operator<(const Key& b)const noexcept{
            return std::tie(level,x,z)<std::tie(b.level,b.x,b.z);
        }
        bool operator==(const Key&)const=default;
    };
    struct Patch {
        XMFLOAT3 center{},u{},v{}; // representative offsets from centroid; third is -u-v
        XMFLOAT3 lo{},hi{},projected{},color{};
        std::array<XMFLOAT2,3> uv{};
        float area{},covered{},repArea{},repAlpha{},priority{},fit{},sourceError{};
        uint32_t material{},kind{},twoSided{};
        std::array<Vertex,3> Triangle()const{
            const auto a=Add(center,Scale(u,fit)),b=Add(center,Scale(v,fit));
            const auto c=Sub(center,Scale(Add(u,v),fit));
            const auto n=Normal(u,v);
            return {{{a,n,uv[0],{}},{b,n,uv[1],{}},{c,n,uv[2],{}}}};
        }
        double ProxyCovered()const{return double(repArea)*fit*fit*repAlpha;}
        float GeometricError()const{return Length(Sub(hi,lo))+sourceError;}
        float OpticalError()const{
            const auto n=Normal(u,v);
            const auto a=float(ProxyCovered());
            return std::sqrt(std::max({std::abs(projected.x-a*std::abs(n.x)),
                std::abs(projected.y-a*std::abs(n.y)),std::abs(projected.z-a*std::abs(n.z))}));
        }
    };
    struct Node {
        Key key{};
        std::vector<uint64_t> routes; // leaves reference authoritative object identities
        std::vector<Key> children;   // parents reference collective cuts, not flattened routes
        std::vector<Patch> patches;
        uint64_t sourceTriangles{},referenceTriangles{},sourceRoutes{};
        bool valid{true};
        // Already-fitted triangles: no geometry simplification or source
        // traversal remains to be done by the future MG2 uploader.
        template<class Emit> void ForEachTriangle(Emit&& emit)const{
            for(const auto& p:patches)emit(p.material,p.kind,p.Triangle());
        }
    };
    struct Audit {
        uint64_t nodes{},routes{},sourceTriangles{},referenceTriangles{},triangles{},bytes{};
        uint64_t wood{},foliage{},invalid{};
        double covered{},proxyCovered{};
        float geometricError{},opticalError{};
    };
    struct Delta {
        std::vector<Node> leaves;
        uint64_t routes{},hits{},misses{},writes{};
        double prepareMs{};
    };
    struct Report {
        std::array<Audit,Levels> changed{};
        std::array<uint64_t,Levels> totalNodes{};
        uint64_t routes{},hits{},misses{},writes{},sourceHits{},sourceMisses{};
        double prepareMs{},publishMs{};
    };
private:
    using Clock=std::chrono::steady_clock;
    struct Source {
        uint64_t triangles{},referenceTriangles{};
        std::string signature;
        std::vector<Patch> patches;
    };
    struct Placement {
        uint64_t id{};
        uint32_t mesh{},material{};
        Pose pose{};
        XMFLOAT3 scale{};
    };
    struct Bin {
        int64_t x{},y{},z{};
        uint32_t material{},kind{},orientation{};
        bool operator<(const Bin& b)const noexcept{
            return std::tie(material,kind,orientation,x,y,z)<
                std::tie(b.material,b.kind,b.orientation,b.x,b.y,b.z);
        }
    };
    using Bins=std::map<Bin,Patch>;
    struct Builder {Node node;Bins bins;};
    std::map<uint64_t,Source> sources_;
    std::map<uint32_t,std::string> materials_;
    std::map<Key,Node> nodes_;
    std::unordered_map<uint64_t,Key> owners_;
    std::set<Key> invalid_;
    std::array<uint64_t,Levels> counts_{};
    uint64_t sourceHits_{},sourceMisses_{};
    static double Ms(Clock::time_point start){
        return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    }
    static XMFLOAT3 Cross(XMFLOAT3 a,XMFLOAT3 b){
        return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
    }
    static XMFLOAT3 Normal(XMFLOAT3 u,XMFLOAT3 v){
        const auto n=Cross(Sub(v,u),Sub(Scale(Add(u,v),-1.f),u));
        const float length=Length(n);return length>0.f?Scale(n,1.f/length):XMFLOAT3{0,1,0};
    }
    static float Area(XMFLOAT3 u,XMFLOAT3 v){
        return .5f*Length(Cross(Sub(v,u),Sub(Scale(Add(u,v),-1.f),u)));
    }
    static XMFLOAT3 Min(XMFLOAT3 a,XMFLOAT3 b){return {std::min(a.x,b.x),std::min(a.y,b.y),std::min(a.z,b.z)};}
    static XMFLOAT3 Max(XMFLOAT3 a,XMFLOAT3 b){return {std::max(a.x,b.x),std::max(a.y,b.y),std::max(a.z,b.z)};}
    static bool Finite(XMFLOAT3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
    static XMFLOAT3 Directional(XMFLOAT3 n,float area){
        return {std::abs(n.x)*area,std::abs(n.y)*area,std::abs(n.z)*area};
    }
    static uint64_t SourceKey(uint32_t mesh,uint32_t material){return (uint64_t(mesh)<<32)|material;}
    static int64_t Floor(double p,double width){return int64_t(std::floor(p/width));}
    static int64_t Align(int64_t v,int64_t side){const auto r=v%side;return v-(r<0?r+side:r);}
    static int64_t Side(unsigned level){return int64_t(1)<<(2u*level);}
    static Key Leaf(XMFLOAT3 p){return {Floor(p.x,TerritorialTileMeters),Floor(p.z,TerritorialTileMeters),0};}
    static Key Parent(Key k){++k.level;const auto side=Side(k.level);k.x=Align(k.x,side);k.z=Align(k.z,side);return k;}
    static uint32_t Orientation(const Patch& p){
        const auto n=Normal(p.u,p.v);
        const auto a=Directional(n,1.f);
        const unsigned axis=a.x>=a.y&&a.x>=a.z?0u:a.y>=a.z?1u:2u;
        const float sign=axis==0?n.x:axis==1?n.y:n.z;
        return axis*2u+uint32_t(!p.twoSided&&sign<0.f);
    }
    static Bin At(const Patch& p,double spacing){
        return {Floor(p.center.x,spacing),Floor(p.center.y,spacing),Floor(p.center.z,spacing),
            p.material,p.kind,Orientation(p)};
    }
    static void Fit(Patch& p){
        // Fit area into actual spatial support; excess density is retained
        // as metadata and measured error, never silently reported as conserved.
        float factor=std::sqrt(p.area/p.repArea);
        const std::array<XMFLOAT3,3> offsets{p.u,p.v,Scale(Add(p.u,p.v),-1.f)};
        for(const auto d:offsets){
            const auto cap=[&](float c,float off,float lo,float hi){
                const float eps=8.f*std::numeric_limits<float>::epsilon()*std::max(1.f,std::abs(c));
                if(off>0.f)factor=std::min(factor,(hi-c+eps)/off);
                if(off<0.f)factor=std::min(factor,(lo-c-eps)/off);
            };
            cap(p.center.x,d.x,p.lo.x,p.hi.x);
            cap(p.center.y,d.y,p.lo.y,p.hi.y);
            cap(p.center.z,d.z,p.lo.z,p.hi.z);
        }
        p.fit=std::max(1.e-6f,factor);
    }
    static void Merge(Patch& a,const Patch& b){
        const float area=a.area+b.area,covered=a.covered+b.covered;
        a.center=Add(a.center,Scale(Sub(b.center,a.center),b.area/area));
        if(covered>0.f)a.color=Add(a.color,Scale(Sub(b.color,a.color),b.covered/covered));
        a.lo=Min(a.lo,b.lo);a.hi=Max(a.hi,b.hi);
        a.projected=Add(a.projected,b.projected);
        a.area=area;a.covered=covered;a.sourceError=std::max(a.sourceError,b.sourceError);
        if(b.priority>a.priority){
            a.u=b.u;a.v=b.v;a.uv=b.uv;a.repArea=b.repArea;
            a.repAlpha=b.repAlpha;a.priority=b.priority;
        }
    }
    static void Insert(Bins& bins,const Bin& key,const Patch& p){
        const auto [it,inserted]=bins.try_emplace(key,p);
        if(!inserted)Merge(it->second,p);
    }
    static void Finish(Builder& b){
        b.node.patches.reserve(b.bins.size());
        for(auto& [key,p]:b.bins){Fit(p);b.node.patches.push_back(p);}
    }
    static bool Valid(const Patch& p){
        return Finite(p.center)&&Finite(p.lo)&&Finite(p.hi)&&Finite(p.u)&&Finite(p.v)&&
            Finite(p.projected)&&Finite(p.color)&&p.lo.x<=p.hi.x&&p.lo.y<=p.hi.y&&p.lo.z<=p.hi.z&&
            std::isfinite(p.area)&&p.area>0.f&&std::isfinite(p.repArea)&&p.repArea>0.f&&
            std::isfinite(p.covered)&&p.covered>=0.f&&p.covered<=p.area*1.0001f&&
            std::isfinite(p.fit)&&p.fit>0.f&&std::isfinite(p.sourceError)&&p.sourceError>=0.f&&
            p.repAlpha>=0.f&&p.repAlpha<=1.f&&p.kind<=TreeFoliage;
    }
    static std::string Digest(DiskBake::Hash& h){
        std::array<unsigned char,32> bytes{};
        if(!h.Finish(bytes))return {};
        static constexpr char hex[]="0123456789abcdef";
        std::string out;out.reserve(64);
        for(auto b:bytes){out+=hex[b>>4];out+=hex[b&15];}return out;
    }
    template<class T> static void HashValue(DiskBake::Hash& h,const T& x){h.Add(&x,sizeof(x));}
    std::string MaterialSignature(const SceneData& scene,uint32_t id){
        if(auto it=materials_.find(id);it!=materials_.end())return it->second;
        const auto& m=scene.materials.at(id);
        DiskBake::Hash h;HashValue(h,m.colorRoughness);HashValue(h,m.alphaMode);
        HashValue(h,m.alpha);HashValue(h,m.cutoff);HashValue(h,m.doubleSided);
        HashValue(h,m.mapping[0]);
        if(m.samplers[0]<scene.samplers.size())HashValue(h,scene.samplers[m.samplers[0]]);
        if(m.albedo<scene.textures.size()){
            const auto& t=scene.textures[m.albedo];
            HashValue(h,t.width);HashValue(h,t.height);HashValue(h,t.format);
            h.Add(t.bytes.data(),t.bytes.size());
        }
        auto sig=Digest(h);materials_.emplace(id,sig);return sig;
    }
    static XMFLOAT3 LinearColor(const TextureData& tex,size_t i){
        static const auto table=[]{
            std::array<float,256> v{};
            for(unsigned j=0;j<v.size();++j){
                const float x=float(j)/255.f;
                v[j]=x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f);
            }
            return v;
        }();
        const auto byte=[&](size_t n){return std::to_integer<unsigned char>(tex.bytes[i+n]);};
        if(tex.format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)return {table[byte(0)],table[byte(1)],table[byte(2)]};
        return {byte(0)/255.f,byte(1)/255.f,byte(2)/255.f};
    }
    struct Optical {XMFLOAT3 color{};float alpha{};};
    static Optical Sample(const SceneData& scene,uint32_t material,const std::array<XMFLOAT2,3>& uv){
        const auto& m=scene.materials.at(material);
        const XMFLOAT3 base{m.colorRoughness.x,m.colorRoughness.y,m.colorRoughness.z};
        const TextureData* t=m.albedo<scene.textures.size()?&scene.textures[m.albedo]:nullptr;
        const bool rgba=t&&t->width&&t->height&&
            (t->format==DXGI_FORMAT_R8G8B8A8_UNORM_SRGB||t->format==DXGI_FORMAT_R8G8B8A8_UNORM)&&
            uint64_t(t->width)*t->height<=t->bytes.size()/4u;
        const SamplerData sampler=m.samplers[0]<scene.samplers.size()?scene.samplers[m.samplers[0]]:SamplerData{};
        const auto coord=[](float u,uint32_t wrap,uint32_t size){
            if(wrap==33071u)u=std::clamp(u,0.f,1.f);
            else u-=std::floor(u);
            return std::min(size-1u,uint32_t(u*float(size)));
        };
        static constexpr std::array<std::array<float,3>,4> weights{{
            {{1.f/3,1.f/3,1.f/3}},{{.6f,.2f,.2f}},{{.2f,.6f,.2f}},{{.2f,.2f,.6f}}
        }};
        Optical out{};
        for(const auto& w:weights){
            auto c=base;float alpha=m.alpha;
            if(rgba){
                const float u=uv[0].x*w[0]+uv[1].x*w[1]+uv[2].x*w[2];
                const float v=uv[0].y*w[0]+uv[1].y*w[1]+uv[2].y*w[2];
                const auto x=coord(u,sampler.wrapS,t->width),y=coord(v,sampler.wrapT,t->height);
                const auto i=4u*(size_t(y)*t->width+x);
                const auto rgb=LinearColor(*t,i);c={c.x*rgb.x,c.y*rgb.y,c.z*rgb.z};
                alpha*=std::to_integer<unsigned char>(t->bytes[i+3])/255.f;
            }
            const float coverage=m.alphaMode==1u?float(alpha>=m.cutoff):1.f;
            out.alpha+=coverage*.25f;out.color=Add(out.color,Scale(c,coverage*.25f));
        }
        out.color=out.alpha>0.f?Scale(out.color,1.f/out.alpha):base;
        return out;
    }
    // Botanical sprigs use the two triangles of the complete OMM atlas.
    // Integrate texel-centre alpha and linear RGB separately on each half,
    // once per material, instead of re-sampling every placed leaf card.
    static std::array<Optical,2> FoliageOptics(const SceneData& scene,uint32_t material){
        const auto& m=scene.materials.at(material);
        const XMFLOAT3 base{m.colorRoughness.x,m.colorRoughness.y,m.colorRoughness.z};
        std::array<Optical,2> result{{{base,1.f},{base,1.f}}};
        if(m.albedo>=scene.textures.size())return result;
        const auto& t=scene.textures[m.albedo];
        const uint64_t texels=uint64_t(t.width)*t.height;
        if(!texels||t.offsets.empty()||t.offsets[0]>t.bytes.size()||
            texels>(t.bytes.size()-t.offsets[0])/4u||
            (t.format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB&&t.format!=DXGI_FORMAT_R8G8B8A8_UNORM))return result;
        std::array<double,2> support{},passed{},red{},green{},blue{};
        for(uint32_t y=0;y<t.height;++y)for(uint32_t x=0;x<t.width;++x){
            const auto i=size_t(t.offsets[0])+4u*(size_t(y)*t.width+x);
            const auto u=(double(x)+.5)/t.width,v=(double(y)+.5)/t.height;
            const auto alpha=float(std::to_integer<unsigned char>(t.bytes[i+3]))/255.f*m.alpha;
            const bool visible=m.alphaMode!=1u||alpha>=m.cutoff;
            const auto rgb=LinearColor(t,i);
            for(unsigned half=0;half<2;++half){
                const double weight=u==v?.5:double(half==unsigned(u<v));
                support[half]+=weight;
                if(visible){
                    passed[half]+=weight;
                    red[half]+=weight*rgb.x;green[half]+=weight*rgb.y;blue[half]+=weight*rgb.z;
                }
            }
        }
        for(unsigned i=0;i<2;++i)if(support[i]>0.){
            result[i].alpha=float(passed[i]/support[i]);
            if(passed[i]>0.)result[i].color={base.x*float(red[i]/passed[i]),base.y*float(green[i]/passed[i]),base.z*float(blue[i]/passed[i])};
        }
        return result;
    }
    static int CanonicalAtlasHalf(const std::array<XMFLOAT2,3>& uv){
        bool lower=false,upper=false,right=false,left=false;
        for(const auto p:uv){
            if(p.x==0.f&&p.y==0.f)lower=true;
            else if(p.x==1.f&&p.y==1.f)upper=true;
            else if(p.x==1.f&&p.y==0.f)right=true;
            else if(p.x==0.f&&p.y==1.f)left=true;
            else return -1;
        }
        return lower&&upper&&right!=left?(right?0:1):-1;
    }
    std::map<uint32_t,std::array<Optical,2>> foliageOptics_;
    static Patch FromTriangle(const SceneData& scene,uint32_t material,uint32_t kind,
                              const Vertex& a,const Vertex& b,const Vertex& c,float sourceError,
                              const std::array<Optical,2>* foliage){
        Patch p{};
        p.center=Scale(Add(Add(a.position,b.position),c.position),1.f/3.f);
        p.u=Sub(a.position,p.center);p.v=Sub(b.position,p.center);
        p.lo=Min(a.position,Min(b.position,c.position));p.hi=Max(a.position,Max(b.position,c.position));
        p.uv={a.uv,b.uv,c.uv};p.material=material;p.kind=kind;
        p.twoSided=scene.materials.at(material).doubleSided;
        p.area=p.repArea=Area(p.u,p.v);p.sourceError=sourceError;p.fit=1.f;
        if(!(p.area>1.e-14f))return p;
        const int half=foliage?CanonicalAtlasHalf(p.uv):-1;
        const auto optical=half>=0?(*foliage)[size_t(half)]:Sample(scene,material,p.uv);
        p.repAlpha=optical.alpha;p.covered=p.area*optical.alpha;p.color=optical.color;
        p.projected=Directional(Normal(p.u,p.v),p.covered);
        p.priority=p.covered+p.area*.001f;
        return p;
    }
    static Patch Place(Patch p,const Placement& o){
        const auto matrix=Matrix(o.pose);
        const auto point=[&](XMFLOAT3 a){return TransformPoint({a.x*o.scale.x,a.y*o.scale.y,a.z*o.scale.z},matrix);};
        const auto dir=[&](XMFLOAT3 a){return TransformDirection({a.x*o.scale.x,a.y*o.scale.y,a.z*o.scale.z},matrix);};
        const auto lo=p.lo,hi=p.hi;
        p.center=point(p.center);p.u=dir(p.u);p.v=dir(p.v);
        p.lo={std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};p.hi={-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
        for(unsigned k=0;k<8;++k){
            const auto c=point({k&1u?hi.x:lo.x,k&2u?hi.y:lo.y,k&4u?hi.z:lo.z});
            p.lo=Min(p.lo,c);p.hi=Max(p.hi,c);
        }
        const float a=Area(p.u,p.v),ratio=a/p.repArea;
        p.repArea=a;p.area*=ratio;p.covered*=ratio;p.priority*=ratio;
        p.projected=Directional(Normal(p.u,p.v),p.covered);
        p.sourceError*=std::max({std::abs(o.scale.x),std::abs(o.scale.y),std::abs(o.scale.z)});
        Fit(p);return p;
    }
    static std::filesystem::path Cache(const char* type,const std::string& sig){
        if(sig.empty())return {};
        return DiskBake::Key(L"territorial-patrimony",("lod21-v1/"+std::string(type)+"/"+sig).c_str(),{});
    }
    static bool ReadLeaf(DiskBake::Reader& io,Node& n){
        return io.Pod(n.key.x)&&io.Pod(n.key.z)&&io.Pod(n.key.level)&&
            io.Pod(n.sourceTriangles)&&io.Pod(n.referenceTriangles)&&io.Pod(n.sourceRoutes)&&
            io.Vec(n.routes)&&io.Vec(n.patches);
    }
    static bool WriteLeaf(DiskBake::Writer& io,const Node& n){
        return io.Pod(n.key.x)&&io.Pod(n.key.z)&&io.Pod(n.key.level)&&
            io.Pod(n.sourceTriangles)&&io.Pod(n.referenceTriangles)&&io.Pod(n.sourceRoutes)&&
            io.Vec(n.routes)&&io.Vec(n.patches);
    }
    bool ReadDelta(const std::filesystem::path& path,Delta& d,const std::vector<Placement>& input)const{
        if(path.empty())return false;
        DiskBake::Reader io(path);uint64_t count{};
        if(!io.Count(count,input.size()))return false;
        Delta candidate;candidate.leaves.resize(size_t(count));
        std::vector<uint64_t> routes;routes.reserve(input.size());
        std::set<Key> keys;
        for(auto& n:candidate.leaves){
            if(!ReadLeaf(io,n)||n.key.level||n.routes.empty()||n.routes.size()!=n.sourceRoutes||
                n.patches.empty()||!keys.insert(n.key).second||
                !std::all_of(n.patches.begin(),n.patches.end(),Valid))return false;
            routes.insert(routes.end(),n.routes.begin(),n.routes.end());
        }
        if(!io.Finish()||routes.size()!=input.size())return false;
        std::sort(routes.begin(),routes.end());
        for(size_t i=0;i<input.size();++i)if(routes[i]!=input[i].id)return false;
        d.leaves=std::move(candidate.leaves);return true;
    }
    static bool WriteDelta(const std::filesystem::path& path,const Delta& d){
        if(path.empty())return false;
        DiskBake::Writer io(path);
        if(!io.Pod(uint64_t(d.leaves.size())))return false;
        for(const auto& n:d.leaves)if(!WriteLeaf(io,n))return false;
        return io.Finish();
    }
    Delta PreparePlacements(std::vector<Placement> input)const{
        const auto start=Clock::now();Delta d;
        if(input.empty())return d;
        std::sort(input.begin(),input.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        DiskBake::Hash hash;std::set<uint64_t> used;
        for(const auto& p:input){
            if(!used.insert(SourceKey(p.mesh,p.material)).second)continue;
            const auto& s=sources_.at(SourceKey(p.mesh,p.material));
            hash.Add(s.signature.data(),s.signature.size());
        }
        for(const auto& p:input){
            HashValue(hash,p.id);HashValue(hash,p.mesh);HashValue(hash,p.material);
            HashValue(hash,p.pose.p);HashValue(hash,p.pose.q);HashValue(hash,p.scale);
        }
        const auto path=Cache("placements",Digest(hash));d.routes=input.size();
        if(ReadDelta(path,d,input)){d.hits=1;d.prepareMs=Ms(start);return d;}
        d.misses=1;
        std::map<Key,Builder> builders;
        for(const auto& o:input){
            const auto key=Leaf(o.pose.p);
            auto& b=builders[key];b.node.key=key;b.node.routes.push_back(o.id);
            const auto& s=sources_.at(SourceKey(o.mesh,o.material));
            ++b.node.sourceRoutes;b.node.sourceTriangles+=s.triangles;b.node.referenceTriangles+=s.referenceTriangles;
            for(const auto& source:s.patches){
                const auto p=Place(source,o);
                if(!Valid(p))Fail("LOD-21 invalid placed surface");
                Insert(b.bins,At(p,2.5),p);
            }
        }
        d.leaves.reserve(builders.size());
        for(auto& [key,b]:builders){Finish(b);d.leaves.push_back(std::move(b.node));}
        if(WriteDelta(path,d))d.writes=1;
        d.prepareMs=Ms(start);return d;
    }
    void Store(Node n){
        n.valid=!invalid_.contains(n.key);
        const auto key=n.key;
        if(!nodes_.contains(key))++counts_[key.level];
        nodes_.insert_or_assign(key,std::move(n));
    }
    static void Include(Audit& a,const Node& n){
        ++a.nodes;a.routes+=n.sourceRoutes;a.sourceTriangles+=n.sourceTriangles;
        a.referenceTriangles+=n.referenceTriangles;a.triangles+=n.patches.size();
        a.invalid+=!n.valid;
        a.bytes+=n.patches.size()*sizeof(Patch)+n.routes.size()*sizeof(uint64_t)+n.children.size()*sizeof(Key);
        for(const auto& p:n.patches){
            a.wood+=p.kind==TreeWood;a.foliage+=p.kind==TreeFoliage;
            a.covered+=p.covered;a.proxyCovered+=p.ProxyCovered();
            a.geometricError=std::max(a.geometricError,p.GeometricError());
            a.opticalError=std::max(a.opticalError,p.OpticalError());
        }
    }
    void InvalidateKey(Key key){
        for(;;){
            invalid_.insert(key);
            if(auto it=nodes_.find(key);it!=nodes_.end())it->second.valid=false;
            if(key.level+1u==Levels)break;
            key=Parent(key);
        }
    }
public:
    void Reset(){
        sources_.clear();materials_.clear();foliageOptics_.clear();nodes_.clear();owners_.clear();invalid_.clear();
        counts_.fill(0);sourceHits_=sourceMisses_=0;
    }
    // Called at scene setup only, before the expansion worker can read sources.
    void Register(const SceneData& scene,uint32_t mesh,uint32_t material,Kind kind,
                  unsigned horizontal=2,unsigned vertical=2,const CanopyLodNode* cut=nullptr){
        const auto key=SourceKey(mesh,material);
        if(sources_.contains(key))return;
        const auto& m=scene.meshes;const auto& r=m.ranges.at(mesh);
        if(!r.vertexCount||!r.indexCount||r.indexCount%3u)Fail("LOD-21 invalid source mesh");
        const auto verts=cut?std::span<const Vertex>(cut->vertices):
            std::span<const Vertex>(m.vertices).subspan(r.firstVertex,r.vertexCount);
        const auto indices=cut?std::span<const uint32_t>(cut->indices):
            std::span<const uint32_t>(m.indices).subspan(r.firstIndex,r.indexCount);
        const uint32_t offset=cut?0u:r.firstVertex;
        const float error=cut?cut->error:0.f;
        DiskBake::Hash hash;
        HashValue(hash,mesh);HashValue(hash,material);HashValue(hash,kind);
        HashValue(hash,horizontal);HashValue(hash,vertical);HashValue(hash,error);
        HashValue(hash,r.indexCount);HashValue(hash,offset);
        hash.Add(verts.data(),verts.size_bytes());hash.Add(indices.data(),indices.size_bytes());
        const auto mat=MaterialSignature(scene,material);hash.Add(mat.data(),mat.size());
        Source s;s.signature=Digest(hash);s.triangles=r.indexCount/3u;s.referenceTriangles=indices.size()/3u;
        const auto path=Cache("source",s.signature);
        if(!path.empty()){
            DiskBake::Reader io(path);
            if(io.Vec(s.patches)&&io.Finish()&&!s.patches.empty()&&
                std::all_of(s.patches.begin(),s.patches.end(),[&](const auto& p){
                    return Valid(p)&&p.material==material&&p.kind==kind;
                })){
                ++sourceHits_;sources_.emplace(key,std::move(s));return;
            }
            s.patches.clear();
        }
        ++sourceMisses_;
        XMFLOAT3 lo{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},hi{-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()};
        for(const auto& v:verts){lo=Min(lo,v.position);hi=Max(hi,v.position);}
        const auto extent=Sub(hi,lo);
        const auto bin=[](float p,float origin,float width,unsigned n){
            return std::min(int64_t(n-1u),std::max(int64_t(0),Floor(p-origin,std::max(1.e-6,double(width)/n))));
        };
        Bins bins;
        const std::array<Optical,2>* foliage=nullptr;
        if(kind==TreeFoliage){
            auto it=foliageOptics_.find(material);
            if(it==foliageOptics_.end())it=foliageOptics_.emplace(material,FoliageOptics(scene,material)).first;
            foliage=&it->second;
        }
        for(size_t i=0;i<indices.size();i+=3u){
            const auto index=[&](size_t j)->const Vertex&{
                if(indices[j]<offset||uint64_t(indices[j]-offset)>=verts.size())Fail("LOD-21 source index");
                return verts[indices[j]-offset];
            };
            const auto p=FromTriangle(scene,material,kind,index(i),index(i+1),index(i+2),error,foliage);
            if(!(p.area>1.e-14f))continue;
            Insert(bins,{bin(p.center.x,lo.x,extent.x,horizontal),bin(p.center.y,lo.y,extent.y,vertical),
                bin(p.center.z,lo.z,extent.z,horizontal),material,kind,Orientation(p)},p);
        }
        s.patches.reserve(bins.size());
        for(auto& [b,p]:bins){Fit(p);if(!Valid(p))Fail("LOD-21 source surface");s.patches.push_back(p);}
        if(s.patches.empty())Fail("LOD-21 empty source surface");
        if(!path.empty()){DiskBake::Writer io(path);if(io.Vec(s.patches))io.Finish();}
        sources_.emplace(key,std::move(s));
    }
    Delta PrepareInitial(const GpuSceneCache& cache)const{
        std::vector<Placement> input;
        for(const auto& [key,tile]:cache.ResidentTiles())for(const auto& o:tile.objects){
            if(!o.enabled||!sources_.contains(SourceKey(o.mesh,o.material)))continue;
            input.push_back({o.identity,o.mesh,o.material,
                {{o.position.x,o.position.y,o.position.z},{o.rotation.x,o.rotation.y,o.rotation.z,o.rotation.w}},
                {o.scale.x,o.scale.y,o.scale.z}});
        }
        return PreparePlacements(std::move(input));
    }
    template<class Object> Delta Prepare(const std::vector<Object>& objects,const std::vector<uint64_t>& ids)const{
        if(objects.size()!=ids.size())Fail("LOD-21 placement identity coverage");
        std::vector<Placement> input;input.reserve(objects.size());
        for(size_t i=0;i<objects.size();++i){
            const auto& o=objects[i];
            if(!o.enabled||!sources_.contains(SourceKey(o.mesh,o.material)))continue;
            input.push_back({ids[i],o.mesh,o.material,o.current,o.scale});
        }
        return PreparePlacements(std::move(input));
    }
    // Session-thread publication: only changed leaves and their ancestors.
    // The persistent cache stores leaf products; parents are regenerated
    // exclusively from their already-cooked child geometry, not source meshes.
    Report Publish(Delta d){
        const auto start=Clock::now();Report report{};
        report.hits=d.hits;report.misses=d.misses;report.writes=d.writes;report.prepareMs=d.prepareMs;
        report.sourceHits=sourceHits_;report.sourceMisses=sourceMisses_;
        std::set<Key> parents;
        for(auto& n:d.leaves){
            const auto key=n.key;
            for(auto id:n.routes)if(!owners_.emplace(id,key).second)Fail("LOD-21 duplicate source identity");
            if(auto old=nodes_.find(key);old!=nodes_.end()){
                Builder b;b.node=std::move(n);
                const auto& p=old->second;
                b.node.routes.insert(b.node.routes.end(),p.routes.begin(),p.routes.end());
                std::sort(b.node.routes.begin(),b.node.routes.end());
                b.node.sourceRoutes+=p.sourceRoutes;b.node.sourceTriangles+=p.sourceTriangles;
                b.node.referenceTriangles+=p.referenceTriangles;
                for(const auto& s:b.node.patches)Insert(b.bins,At(s,2.5),s);
                for(const auto& s:p.patches)Insert(b.bins,At(s,2.5),s);
                b.node.patches.clear();Finish(b);n=std::move(b.node);
            }
            Store(std::move(n));Include(report.changed[0],nodes_.at(key));parents.insert(Parent(key));
        }
        for(unsigned level=1;level<Levels;++level){
            std::set<Key> next;
            for(const auto key:parents){
                Builder b;b.node.key=key;const auto side=Side(level-1);
                for(int64_t z=0;z<4;++z)for(int64_t x=0;x<4;++x){
                    const Key child{key.x+x*side,key.z+z*side,level-1};
                    const auto found=nodes_.find(child);if(found==nodes_.end())continue;
                    const auto& n=found->second;b.node.children.push_back(child);
                    b.node.sourceRoutes+=n.sourceRoutes;b.node.sourceTriangles+=n.sourceTriangles;
                    b.node.referenceTriangles+=n.referenceTriangles;
                    for(const auto& p:n.patches)Insert(b.bins,At(p,2.5*double(Side(level))),p);
                }
                if(b.node.children.empty())Fail("LOD-21 missing child");
                Finish(b);Store(std::move(b.node));Include(report.changed[level],nodes_.at(key));
                if(level+1u<Levels)next.insert(Parent(key));
            }
            parents=std::move(next);
        }
        report.totalNodes=counts_;report.routes=owners_.size();report.publishMs=Ms(start);
        return report;
    }
    void Invalidate(uint64_t identity){
        const auto it=owners_.find(identity);if(it!=owners_.end())InvalidateKey(it->second);
    }
    void InvalidateAt(uint64_t identity,uint32_t mesh,uint32_t material,XMFLOAT3 position){
        const auto it=owners_.find(identity);
        if(it!=owners_.end()){
            InvalidateKey(it->second);
            if(sources_.contains(SourceKey(mesh,material)))InvalidateKey(Leaf(position));
        }
    }
    const std::map<Key,Node>& Nodes()const noexcept{return nodes_;}
};
}
