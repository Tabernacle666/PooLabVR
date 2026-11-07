#pragma once
#include "../cartridge_api/LaboVRGpuInteropService.h"
#include <cuda_runtime_api.h>
#include <cstdint>

namespace lab::physics_detail {
struct SharedBuffer {
    LabGpuInteropResourceHandleV1 handle{};cudaExternalMemory_t memory{};void* pointer{};uint64_t bytes{};
    void Create(const LabGpuInteropServiceV1& interop,uint64_t size);
    void Destroy(const LabGpuInteropServiceV1& interop);
};
struct SharedFence {
    LabGpuInteropFenceHandleV1 handle{};cudaExternalSemaphore_t semaphore{};
    void Create(const LabGpuInteropServiceV1& interop);
    uint64_t Completed(const LabGpuInteropServiceV1& interop);
    void Wait(cudaStream_t stream,uint64_t value);
    void Signal(cudaStream_t stream,uint64_t value);
    void Destroy(const LabGpuInteropServiceV1& interop);
};
}
