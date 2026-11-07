#pragma once
#include "GpuSubresources.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lab {
enum class GraphQueue:uint8_t {Copy,Compute,Direct,Count};
enum class GraphMode:uint8_t {Read,Write,ReadWrite};
constexpr uint32_t GraphQueueCount=uint32_t(GraphQueue::Count);
constexpr uint32_t QI(GraphQueue q){return uint32_t(q);}

// Frame-local CONTENT versions, unrelated to allocation/descriptor generations.
// By default reads bind now, and every write advances the content version.
// Explicit ordinals allow a publication consumer to name a future producer.
// Version 0 is imported content; version N must have exactly one declared writer.
namespace GpuContentVersion {
inline constexpr uint32_t Imported=0,Current=UINT32_MAX;
}
enum class GraphHazard:uint8_t {RAW=1,WAR=2,WAW=4,Explicit=8};
struct GraphLogicalUse {
    uint32_t pass{},subresource{UINT32_MAX},inputVersion{},outputVersion{};
    const void* resource{};GraphMode mode{};
};
struct GraphVersionHazard {
    uint32_t from{},to{},subresource{UINT32_MAX},fromVersion{},toVersion{};
    const void* resource{};GraphHazard kind{};
};

struct GraphBatchSpan {
    uint32_t first{},count{};
    GraphQueue queue{GraphQueue::Direct};
};

// The production resolver. No DX12, OpenXR, CUDA or PhysX dependency: CTest
// exercises this exact implementation, rather than a copy of the algorithm.
class FrameGraphDependencies {
    static constexpr uint32_t Invalid=UINT32_MAX;
    struct Pass {std::string name;GraphQueue queue{};uint64_t key{};};
    struct Version {uint32_t number{},writer{Invalid};std::vector<uint32_t> readers;};
    struct Resource {
        const void* physical{};uint32_t physicalId{},subresource{Invalid},latest{};
        std::vector<Version> versions{{0,Invalid,{}}};
    };
    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<const void*> physicals_;
    std::vector<GraphLogicalUse> uses_;
    std::vector<std::pair<uint32_t,uint32_t>> explicitEdges_;
    std::vector<std::pair<uint32_t,const void*>> afterAll_;
    std::vector<GraphVersionHazard> hazards_;
    std::vector<uint8_t> edges_;
    std::vector<uint32_t> order_;
    std::string error_;
    bool Error(const char* message){error_=message;order_.clear();return false;}
    bool Less(uint32_t a,uint32_t b)const{
        const auto& x=passes_[a];const auto& y=passes_[b];
        if(x.key!=y.key)return x.key<y.key;
        if(x.name!=y.name)return x.name<y.name;
        return a<b;
    }
    uint32_t PhysicalId(const void* physical){
        for(uint32_t i=0;i<physicals_.size();++i)if(physicals_[i]==physical)return i;
        physicals_.push_back(physical);return uint32_t(physicals_.size()-1);
    }
    uint32_t PhysicalId(const void* physical)const{
        for(uint32_t i=0;i<physicals_.size();++i)if(physicals_[i]==physical)return i;
        return Invalid;
    }
    Resource& State(const void* physical,uint32_t subresource){
        for(auto& r:resources_)if(r.physical==physical&&r.subresource==subresource)return r;
        resources_.push_back({physical,PhysicalId(physical),subresource,0,{{0,Invalid,{}}}});return resources_.back();
    }
    static Version& Content(Resource& r,uint32_t number){
        for(auto& v:r.versions)if(v.number==number)return v;
        r.versions.push_back({number,Invalid,{}});return r.versions.back();
    }
    static void Reader(Version& v,uint32_t pass){
        if(std::find(v.readers.begin(),v.readers.end(),pass)==v.readers.end())v.readers.push_back(pass);
    }
    bool Access(uint32_t pass,const void* physical,uint32_t subresource,GraphMode mode,uint32_t requested){
        if(!error_.empty())return false;
        if(pass>=passes_.size())return Error("Frame graph access pass index");
        if(!physical)return true;
        auto& r=State(physical,subresource);
        if(mode==GraphMode::Read){
            const auto version=requested==GpuContentVersion::Current?r.latest:requested;
            Reader(Content(r,version),pass);uses_.push_back({pass,subresource,version,version,physical,mode});
        }else{
            if(requested==GpuContentVersion::Current&&r.latest==UINT32_MAX-1)return Error("Frame graph content version overflow");
            const auto version=requested==GpuContentVersion::Current?r.latest+1:requested;
            if(!version)return Error("Frame graph cannot write imported version 0");
            auto& output=Content(r,version);
            if(output.writer!=Invalid)return Error("Frame graph content version has multiple writers");
            output.writer=pass;
            // Create the predecessor even for a blind write: physical reuse still
            // needs WAR/WAW. ReadWrite additionally consumes that predecessor.
            auto& input=Content(r,version-1);
            if(mode==GraphMode::ReadWrite)Reader(input,pass);
            r.latest=std::max(r.latest,version);
            uses_.push_back({pass,subresource,version-1,version,physical,mode});
        }
        edges_.clear();order_.clear();return true;
    }
    void Edge(uint32_t from,uint32_t to,GraphHazard kind,const Resource* r=nullptr,uint32_t before=0,uint32_t after=0){
        if(from==Invalid||to==Invalid||from==to)return;
        edges_[size_t(from)*passes_.size()+to]|=uint8_t(kind);
        hazards_.push_back({from,to,r?r->subresource:Invalid,before,after,r?r->physical:nullptr,kind});
    }
    bool Path(uint32_t from,uint32_t to,uint32_t skipFrom=Invalid,uint32_t skipTo=Invalid)const{
        const auto n=uint32_t(passes_.size());if(from>=n||to>=n||edges_.size()!=size_t(n)*n)return false;
        std::vector<uint8_t> seen(n);std::vector<uint32_t> work{from};seen[from]=1;
        while(!work.empty()){
            auto a=work.back();work.pop_back();
            for(uint32_t b=0;b<n;++b){
                if((a==skipFrom&&b==skipTo)||!edges_[size_t(a)*n+b])continue;
                if(b==to)return true;
                if(!seen[b]){seen[b]=1;work.push_back(b);}
            }
        }
        return false;
    }
public:
    FrameGraphDependencies(){passes_.reserve(32);resources_.reserve(128);physicals_.reserve(64);uses_.reserve(128);order_.reserve(32);}
    void BeginFrame(){passes_.clear();resources_.clear();physicals_.clear();uses_.clear();explicitEdges_.clear();afterAll_.clear();hazards_.clear();edges_.clear();order_.clear();error_.clear();}
    uint32_t AddPass(const char* name,GraphQueue queue,uint64_t stableKey=0){
        if(!name||!*name||QI(queue)>=GraphQueueCount){Error("Frame graph pass name/queue");return Invalid;}
        passes_.push_back({name,queue,stableKey});edges_.clear();order_.clear();return uint32_t(passes_.size()-1);
    }
    bool Buffer(uint32_t pass,const void* physical,GraphMode mode,uint32_t version=GpuContentVersion::Current){return Access(pass,physical,Invalid,mode,version);}
    bool Texture(uint32_t pass,const void* physical,GraphMode mode,GpuSubresourceShape shape,GpuTextureSubresources requested={},uint32_t version=GpuContentVersion::Current){
        GpuTextureSubresources range;if(!GpuNormalizeSubresources(shape,requested,range))return Error("Frame graph texture range");
        bool ok=true;GpuForEachSubresource(shape,range,[&](uint32_t index,uint32_t,uint32_t,uint32_t){if(ok)ok=Access(pass,physical,index,mode,version);});return ok;
    }
    bool Dependency(uint32_t from,uint32_t to){
        if(from>=passes_.size()||to>=passes_.size())return Error("Frame graph dependency pass index");
        explicitEdges_.emplace_back(from,to);edges_.clear();order_.clear();return true;
    }
    bool AfterAllUsers(uint32_t sink,const void* physical){
        if(sink>=passes_.size())return Error("Frame graph sink pass index");
        if(physical)afterAll_.emplace_back(sink,physical);
        edges_.clear();order_.clear();return true;
    }
    bool Resolve(){
        if(!error_.empty())return false;
        const auto n=uint32_t(passes_.size());edges_.assign(size_t(n)*n,0);hazards_.clear();order_.clear();
        for(auto& r:resources_){
            std::sort(r.versions.begin(),r.versions.end(),[](const auto& a,const auto& b){return a.number<b.number;});
            for(size_t i=0;i<r.versions.size();++i){
                const auto& v=r.versions[i];
                if(v.number!=i||(v.number&&v.writer==Invalid))return Error("Frame graph content version has no producer");
                for(auto reader:v.readers)Edge(v.writer,reader,GraphHazard::RAW,&r,v.number,v.number);
                if(!i)continue;
                const auto& previous=r.versions[i-1];
                Edge(previous.writer,v.writer,GraphHazard::WAW,&r,previous.number,v.number);
                for(auto reader:previous.readers)Edge(reader,v.writer,GraphHazard::WAR,&r,previous.number,v.number);
            }
        }
        for(auto [from,to]:explicitEdges_){if(from==to)return Error("Frame graph dependency cycle");Edge(from,to,GraphHazard::Explicit);}
        for(auto [sink,physical]:afterAll_)for(const auto& use:uses_)if(use.resource==physical)Edge(use.pass,sink,GraphHazard::Explicit);
        std::vector<uint32_t> indegree(n);std::vector<uint8_t> emitted(n);
        for(uint32_t a=0;a<n;++a)for(uint32_t b=0;b<n;++b)if(edges_[size_t(a)*n+b])++indegree[b];
        for(uint32_t step=0;step<n;++step){
            uint32_t best=Invalid;
            for(uint32_t i=0;i<n;++i)if(!emitted[i]&&!indegree[i]&&(best==Invalid||Less(i,best)))best=i;
            if(best==Invalid)return Error("Frame graph dependency cycle");
            emitted[best]=1;order_.push_back(best);
            for(uint32_t b=0;b<n;++b)if(edges_[size_t(best)*n+b])--indegree[b];
        }
        // Keep the full typed hazard proof, but omit a WAW-only scheduling edge
        // already covered through the preceding readers. A->B->A->B is exactly
        // P1->P2->P3->P4->P5, without redundant P1->P3 / P2->P4 edges.
        for(uint32_t a=0;a<n;++a)for(uint32_t b=0;b<n;++b)
            if(edges_[size_t(a)*n+b]==uint8_t(GraphHazard::WAW)&&Path(a,b,a,b))edges_[size_t(a)*n+b]=0;
        return true;
    }
    // Exact, pointer-independent structural key. Physical resources are represented
    // by first-use ordinals so per-frame-slot allocations do not invalidate an
    // otherwise identical graph. Commands/constants never enter this key.
    void StructureKey(std::vector<uint64_t>& out)const{
        out.clear();out.reserve(16+passes_.size()*4+resources_.size()*8+uses_.size());
        auto pushString=[&](const std::string& value){out.push_back(value.size());uint64_t word=0;uint32_t shift=0;for(unsigned char c:value){word|=uint64_t(c)<<shift;shift+=8;if(shift==64){out.push_back(word);word=0;shift=0;}}if(shift)out.push_back(word);};
        out.push_back(0x4647524150483631ull);out.push_back(passes_.size());out.push_back(resources_.size());out.push_back(explicitEdges_.size());out.push_back(afterAll_.size());
        for(const auto& pass:passes_){out.push_back(uint64_t(QI(pass.queue)));out.push_back(pass.key);pushString(pass.name);}
        for(const auto& resource:resources_){out.push_back(0x52534f5552434531ull);out.push_back(resource.physicalId);out.push_back(resource.subresource);out.push_back(resource.latest);out.push_back(resource.versions.size());for(const auto& version:resource.versions){out.push_back(version.number);out.push_back(version.writer);out.push_back(version.readers.size());for(auto reader:version.readers)out.push_back(reader);}}
        out.push_back(0x4558504c49434954ull);for(auto edge:explicitEdges_){out.push_back(edge.first);out.push_back(edge.second);}
        out.push_back(0x4146544552414c4cull);for(auto item:afterAll_){out.push_back(item.first);out.push_back(PhysicalId(item.second));}
    }
    bool RestoreResolved(const std::vector<uint32_t>& order,const std::vector<uint8_t>& edges){
        const auto n=passes_.size();if(!error_.empty()||order.size()!=n||edges.size()!=n*n)return false;
        std::vector<uint8_t> seen(n);for(auto pass:order){if(pass>=n||seen[pass]++)return false;}
        order_=order;edges_=edges;hazards_.clear();return true;
    }
    const std::vector<uint8_t>& DependencyMatrix()const{return edges_;}
    uint32_t PassCount()const{return uint32_t(passes_.size());}
    GraphQueue PassQueue(uint32_t pass)const{return pass<passes_.size()?passes_[pass].queue:GraphQueue::Count;}
    bool HasCrossQueueInput(uint32_t pass)const{for(uint32_t from=0;from<passes_.size();++from)if(from!=pass&&PassQueue(from)!=PassQueue(pass)&&HasDependency(from,pass))return true;return false;}
    bool HasCrossQueueOutput(uint32_t pass)const{for(uint32_t to=0;to<passes_.size();++to)if(to!=pass&&PassQueue(to)!=PassQueue(pass)&&HasDependency(pass,to))return true;return false;}
    const char* ErrorMessage()const{return error_.c_str();}
    const std::vector<uint32_t>& ResolvedOrder()const{return order_;}
    const std::vector<GraphLogicalUse>& Uses()const{return uses_;}
    const std::vector<GraphVersionHazard>& Hazards()const{return hazards_;}
    bool HasDependency(uint32_t from,uint32_t to)const{
        auto n=passes_.size();return from<n&&to<n&&edges_.size()==n*n&&edges_[size_t(from)*n+to]!=0;
    }
    bool HasPath(uint32_t from,uint32_t to)const{return Path(from,to);}
    uint32_t LatestVersion(const void* physical,uint32_t subresource=Invalid)const{
        for(const auto& r:resources_)if(r.physical==physical&&r.subresource==subresource)return r.latest;
        return 0;
    }
};

// Consecutive same-queue nodes may share one command list only when doing so does
// not move an inter-queue synchronization point. External waits start a batch;
// external signals finish one. This preserves queue parallelism while removing
// pass-local submits that never carried synchronization value.
inline std::vector<GraphBatchSpan> CompileGraphBatches(const FrameGraphDependencies& graph,const std::vector<uint8_t>& waitBefore,const std::vector<uint8_t>& signalAfter){
    const auto& order=graph.ResolvedOrder();std::vector<GraphBatchSpan> batches;if(order.empty())return batches;
    if(waitBefore.size()!=graph.PassCount()||signalAfter.size()!=graph.PassCount())return batches;
    batches.reserve(order.size());uint32_t first=0;
    auto flush=[&](uint32_t end){auto pass=order[first];batches.push_back({first,end-first,graph.PassQueue(pass)});first=end;};
    for(uint32_t i=1;i<order.size();++i){auto previous=order[i-1],next=order[i];bool split=graph.PassQueue(previous)!=graph.PassQueue(next)||signalAfter[previous]||waitBefore[next]||graph.HasCrossQueueOutput(previous)||graph.HasCrossQueueInput(next);if(split)flush(i);}
    flush(uint32_t(order.size()));return batches;
}
}
