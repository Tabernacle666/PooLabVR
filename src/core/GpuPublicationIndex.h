#pragma once
#include <vector>
#include <cstdint>
#include <utility>
#include <span>
namespace lab {
// Borrowed read-only frame snapshot. Pointers stay valid until EndFrame only.
// A domain can own multiple stream publications: ordinal is explicit and defaults
// to the first. Coupled simulation domains normally publish one named binding set.
template<class Publication> class GpuPublicationIndex {
    std::vector<const Publication*> entries_;
public:
    void Clear(){entries_.clear();}
    std::span<const Publication* const> Entries()const{return entries_;}
    void Add(const Publication& publication){entries_.push_back(&publication);}
    const Publication* FindPublication(uint32_t domain,uint32_t ordinal=0)const{
        for(auto* p:entries_)if(p->domain==domain){if(!ordinal)return p;--ordinal;}
        return nullptr;
    }
    auto FindResource(uint32_t domain,uint32_t binding,uint32_t ordinal=0)const
        -> decltype(std::declval<const Publication&>().Find(binding)){
        auto* p=FindPublication(domain,ordinal);return p?p->Find(binding):nullptr;
    }
};
}
