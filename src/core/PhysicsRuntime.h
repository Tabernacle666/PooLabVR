#pragma once
#include "../cartridge_api/LaboVRPhysicsService.h"
#include <memory>
#include <cstdint>
struct ID3D12Device;
namespace lab {
class PhysicsRuntime {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    PhysicsRuntime();~PhysicsRuntime();
    void Initialize(ID3D12Device*);
    void Shutdown();
    // Owner tokens are private to the host; a cartridge receives only its own
    // table/context. ResetOwner destroys precisely that client's scenes.
    uint64_t CreateOwner(const LabGpuInteropServiceV1&);
    void ResetOwner(uint64_t);
    LabPhysicsServiceV1 Service(uint64_t);
    void Advance(double displaySeconds,uint64_t inputFrame);
};
}
