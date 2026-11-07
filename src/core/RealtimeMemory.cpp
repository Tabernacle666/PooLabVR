#include "RealtimeMemory.h"
#include <limits>
namespace lab {
namespace {
struct AllocationHeader { RealtimeMemory* owner; uint32_t offset,order; uint64_t cookie;void* dedicated;size_t bytes; };
constexpr uint64_t Cookie=0x4c56523132484541ull;
}
void RealtimeMemory::Initialize(uint32_t order) {
    if(memory_||order<MinOrder||order>MaxOrder)Fail("real-time memory capacity");
    order_=order;
    memory_=static_cast<std::byte*>(VirtualAlloc(nullptr,size_t(1)<<order,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    freeOrder_=static_cast<uint8_t*>(VirtualAlloc(nullptr,size_t(1)<<(order-MinOrder),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!memory_||!freeOrder_)Fail("real-time memory reservation");
    memset(freeOrder_,0xff,size_t(1)<<(order-MinOrder));
    for(size_t i=0;i<(size_t(1)<<order);i+=4096)memory_[i]=std::byte{};
    Insert(0,order);
}
void RealtimeMemory::Insert(uint32_t offset,uint32_t order) {
    auto* node=reinterpret_cast<FreeBlock*>(memory_+offset);
    node->previous=nullptr;node->next=lists_[order];
    if(node->next)node->next->previous=node;
    lists_[order]=node;freeOrder_[offset>>MinOrder]=uint8_t(order);
}
void RealtimeMemory::Remove(uint32_t offset,uint32_t order) {
    auto* node=reinterpret_cast<FreeBlock*>(memory_+offset);
    if(node->previous)node->previous->next=node->next;else lists_[order]=node->next;
    if(node->next)node->next->previous=node->previous;
    freeOrder_[offset>>MinOrder]=0xff;
}
void* RealtimeMemory::Allocate(size_t bytes,size_t alignment) {
    alignment=std::max(alignment,alignof(AllocationHeader));
    if(!std::has_single_bit(alignment)||bytes>SIZE_MAX-alignment-sizeof(AllocationHeader))Fail("real-time allocation alignment");
    size_t need=std::max(size_t(1)<<MinOrder,bytes+sizeof(AllocationHeader)+alignment-1);
    uint32_t order=uint32_t(std::bit_width(need-1));
    std::lock_guard lock(mutex_);
    uint32_t available=order;
    while(available<=order_&&!lists_[available])++available;
    if(available>order_){
        void* block=VirtualAlloc(nullptr,need,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);if(!block)Fail("Physical memory exhausted");
        uintptr_t address=(reinterpret_cast<uintptr_t>(block)+sizeof(AllocationHeader)+alignment-1)&~(alignment-1);
        auto* header=reinterpret_cast<AllocationHeader*>(address)-1;*header={this,0,0,Cookie,block,need};
        used_+=need;peak_=std::max(peak_,used_);return reinterpret_cast<void*>(address);
    }
    uint32_t offset=uint32_t(reinterpret_cast<std::byte*>(lists_[available])-memory_);
    Remove(offset,available);
    while(available>order){--available;Insert(offset+(1u<<available),available);}
    uintptr_t address=(reinterpret_cast<uintptr_t>(memory_+offset)+sizeof(AllocationHeader)+alignment-1)&~(alignment-1);
    auto* header=reinterpret_cast<AllocationHeader*>(address)-1;
    *header={this,offset,order,Cookie,nullptr,0};used_+=uint64_t(1)<<order;peak_=std::max(peak_,used_);
    
    return reinterpret_cast<void*>(address);
}
void RealtimeMemory::Release(void* pointer) {
    if(!pointer)return;
    auto* header=static_cast<AllocationHeader*>(pointer)-1;
    if(header->cookie!=Cookie)Fail("real-time memory ownership");
    auto& pool=*header->owner;std::lock_guard lock(pool.mutex_);
    if(header->dedicated){pool.used_-=header->bytes;void* block=header->dedicated;header->cookie=0;VirtualFree(block,0,MEM_RELEASE);return;}
    uint32_t offset=header->offset,order=header->order;header->cookie=0;
    pool.used_-=uint64_t(1)<<order;
    while(order<pool.order_){uint32_t buddy=offset^(1u<<order);if(pool.freeOrder_[buddy>>MinOrder]!=order)break;pool.Remove(buddy,order);offset=std::min(offset,buddy);++order;}
    pool.Insert(offset,order);
}
uint64_t RealtimeMemory::Peak(){std::lock_guard lock(mutex_);return peak_;}
RealtimeMemory::~RealtimeMemory(){if(memory_)VirtualFree(memory_,0,MEM_RELEASE);if(freeOrder_)VirtualFree(freeOrder_,0,MEM_RELEASE);}
}
