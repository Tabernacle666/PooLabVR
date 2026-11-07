#pragma once
#include "GpuLifetime.h"
#include <vector>
#include <cstdlib>
#include <utility>

namespace lab {
// Payload owns the resource as long as any shader can use the physical index.
// Logical slots may be reused immediately; physical slots only leave retirement
// after ALL queue frontiers complete, including the currently recording frame.
template<class Payload> class GpuDescriptorAllocator {
    static constexpr uint32_t InvalidSlot=0xffffffffu;
    struct Entry {uint32_t physical{InvalidSlot},generation{1};bool live{};Payload payload{};};
    struct Retired {uint32_t physical{};GpuFenceValues fences{};bool pendingSubmission{};Payload payload{};};
    std::vector<Entry> entries_;std::vector<uint32_t> freeLogical_,freePhysical_;
    std::vector<Retired> retired_;uint32_t nextPhysical_{},capacity_;uint64_t live_{},recycled_{};bool recording_{};
    void(*failure_)(const char*);
    [[noreturn]] void Error(const char* message)const{if(failure_)failure_(message);std::abort();}
public:
    explicit GpuDescriptorAllocator(uint32_t capacity,void(*failure)(const char*)=nullptr):capacity_(capacity),failure_(failure){}
    GpuResourceHandle Allocate(Payload payload={}){
        uint32_t physical;
        if(!freePhysical_.empty()){physical=freePhysical_.back();freePhysical_.pop_back();}
        else{if(nextPhysical_>=capacity_)Error("Descriptor physical pool exhausted (in-flight retirement included)");physical=nextPhysical_++;}
        uint32_t logical;if(!freeLogical_.empty()){logical=freeLogical_.back();freeLogical_.pop_back();}else{logical=uint32_t(entries_.size());entries_.push_back({});}
        auto& e=entries_[logical];if(e.live)Error("Descriptor logical slot already live");e.physical=physical;e.live=true;e.payload=std::move(payload);++live_;return {logical,e.generation};
    }
    bool IsLive(GpuResourceHandle h)const{return h.Valid()&&h.slot<entries_.size()&&entries_[h.slot].live&&entries_[h.slot].generation==h.generation;}
    uint32_t Resolve(GpuResourceHandle h)const{if(!IsLive(h))Error("Stale descriptor handle");return entries_[h.slot].physical;}
    uint32_t Generation(GpuResourceHandle h)const{Resolve(h);return h.generation;}
    void Release(GpuResourceHandle& h,const GpuFenceValues& submitted){
        if(!h.Valid())return;
        if(!IsLive(h))Error("Stale descriptor release");
        auto& e=entries_[h.slot];
        retired_.push_back({e.physical,submitted,recording_,std::move(e.payload)});
        e.physical=InvalidSlot;e.live=false;if(++e.generation==0)++e.generation;
        --live_;freeLogical_.push_back(h.slot);h={};
    }
    void BeginRecording(){if(recording_)Error("Nested descriptor recording epoch");recording_=true;}
    void EndRecording(const GpuFenceValues& submitted){
        if(!recording_)return;
        for(auto& r:retired_)if(r.pendingSubmission){GpuFenceMerge(r.fences,submitted);r.pendingSubmission=false;}
        recording_=false;
    }
    void Collect(const GpuFenceValues& completed){
        size_t write=0;for(size_t i=0;i<retired_.size();++i){auto& r=retired_[i];if(!r.pendingSubmission&&GpuFencesComplete(r.fences,completed)){freePhysical_.push_back(r.physical);++recycled_;}else{if(write!=i)retired_[write]=std::move(r);++write;}}retired_.resize(write);
    }
    uint64_t LiveCount()const{return live_;}
    uint64_t RetiredCount()const{return retired_.size();}
    uint64_t RecycledCount()const{return recycled_;}
};
}
