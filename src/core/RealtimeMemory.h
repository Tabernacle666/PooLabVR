#pragma once
#include "Core.h"
#include <bit>
#include <mutex>

namespace lab {
class RealtimeMemory {
    static constexpr uint32_t MinOrder=6, MaxOrder=29;
    struct FreeBlock { FreeBlock* next; FreeBlock* previous; };
    std::byte* memory_{};
    uint8_t* freeOrder_{};
    std::array<FreeBlock*,MaxOrder+1> lists_{};
    uint32_t order_{};
    uint64_t used_{}, peak_{};
    std::mutex mutex_;
    void Insert(uint32_t offset,uint32_t order);
    void Remove(uint32_t offset,uint32_t order);
public:
    RealtimeMemory()=default;
    RealtimeMemory(const RealtimeMemory&)=delete;
    ~RealtimeMemory();
    void Initialize(uint32_t order);
    void* Allocate(size_t bytes,size_t alignment);
    static void Release(void* pointer);
    uint64_t Peak();
};
}
