#pragma once
#include "../cartridge_api/LaboVRPhysicsService.h"
#include <memory>
#include <cstdint>
struct ID3D12Device;
namespace lab {
// Host-private description, not part of the cartridge publication ABI.
struct PhysicsPublicationGroupSync {
    uint32_t groupId{};
    LabGpuInteropFenceHandleV1 producerFence{},consumerFence{};
    uint64_t producerValue{},consumerValue{};
};

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
    // Only providers belonging to this runtime can reserve an unfinished frame.
    // The worker signals its producer fence after publishing the current state.
    uint32_t ReservePublication(uint64_t owner,const LabGpuPublicationProviderV1&,
                                LabGpuPublicationV1*);
    // Returns groups only for this runtime's own provider and current lease.
    uint32_t DescribePublicationGroups(uint64_t owner,const LabGpuPublicationProviderV1&,
        const LabGpuPublicationV1&,PhysicsPublicationGroupSync* out,uint32_t capacity)const;
    void Advance(double displaySeconds,uint64_t inputFrame);
};
}
