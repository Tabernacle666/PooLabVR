#pragma once
#include <array>
#include <algorithm>
#include <cstdint>

namespace lab {
struct GpuResourceHandle {
    uint32_t slot{0xffffffffu},generation{};
    bool Valid()const{return slot!=0xffffffffu;}
};
// Copy, Compute, Direct. Values are comparable only within the same queue.
using GpuFenceValues=std::array<uint64_t,3>;
inline bool GpuFencesComplete(const GpuFenceValues& needed,const GpuFenceValues& completed){
    for(uint32_t q=0;q<needed.size();++q)if(completed[q]<needed[q])return false;
    return true;
}
inline void GpuFenceMerge(GpuFenceValues& destination,const GpuFenceValues& source){
    for(uint32_t q=0;q<destination.size();++q)destination[q]=std::max(destination[q],source[q]);
}
inline uint64_t GpuSubtract(uint64_t a,uint64_t b){return a>b?a-b:0;}

// Residency role is stable for the lifetime of an allocation. "required" remains
// a per-frame bit: cacheable data can be required while in use, but is the first
// class sacrificed when it becomes idle and memory pressure appears.
enum class GpuResidencyClass:uint8_t { Cacheable,Required,Pinned };
inline uint32_t GpuEvictionPriority(GpuResidencyClass value){
    return value==GpuResidencyClass::Cacheable?0u:value==GpuResidencyClass::Required?1u:2u;
}

// Persistent cache identity. Content says what the bytes/AS represent, generation
// says which revision produced them, and kind prevents unrelated equal byte streams
// from aliasing one another. A changed generation can never hit an older result.
struct GpuCacheKey {
    uint64_t content{},generation{};
    uint32_t kind{};
    constexpr bool Valid()const{return content&&generation&&kind;}
    friend constexpr bool operator==(const GpuCacheKey&,const GpuCacheKey&)=default;
};
inline constexpr bool GpuCacheMatch(const GpuCacheKey& a,const GpuCacheKey& b){return a.Valid()&&a==b;}

struct GpuBudgetPolicy {
    uint64_t budget{},usage{},managedResident{},previousExternal{},growth{};
    void Sample(uint64_t osBudget,uint64_t osUsage,uint64_t domainResident){
        if(!budget)previousExternal=GpuSubtract(osUsage,domainResident);
        budget=osBudget;usage=osUsage;managedResident=domainResident;
        uint64_t external=GpuSubtract(usage,managedResident);
        growth=std::max(GpuSubtract(external,previousExternal),growth-growth/8);previousExternal=external;
    }
    uint64_t Headroom()const{
        // Driver/XR/RT/AS/transients/CUDA remain in measured external usage.
        // Keep a proportional reserve plus a decaying allowance for their growth.
        uint64_t burst=std::min(growth,budget/4)*2;
        return std::min(budget,std::max(budget/10,budget/32+burst));
    }
    uint64_t DomainBudget()const{return GpuSubtract(GpuSubtract(budget,Headroom()),GpuSubtract(usage,managedResident));}
    bool CanAdmit(uint64_t bytes)const{return bytes<=GpuSubtract(GpuSubtract(budget,Headroom()),usage);}
};
}
