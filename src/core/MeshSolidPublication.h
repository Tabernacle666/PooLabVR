#pragma once
#include "GpuPublication.h"

namespace lab {

// Built-in mesh/solid domain identity and publication ABI stay outside the generic
// publication contract. Future domains define their own IDs and bindings locally.
namespace GpuDomainId {
inline constexpr GpuWorldDomain MeshSolid=1;
}

namespace GpuMeshPublicationAbi {
enum ResourceBinding:uint32_t { Instances=0,Metadata=1,IndirectArgs=2,LightBase=0x100 };
constexpr uint64_t MetadataBytes=16;
constexpr uint64_t IndirectBytes=32;
constexpr uint64_t ActiveDispatchOffset=0;
constexpr uint64_t CapacityDispatchOffset=16;
constexpr uint64_t ControlBytes=MetadataBytes+IndirectBytes;
// Reserved internal MeshSolid bindings. The legacy combined Instances/Metadata/
// IndirectArgs bindings and the cartridge ABI stay unchanged in step 1.
constexpr uint32_t PhysicsInstances=0x400;
constexpr uint32_t PhysicsControl=0x401;
constexpr uint32_t PopulationInstances=0x410;
constexpr uint32_t PopulationControl=0x411;
constexpr uint32_t SnowState=0x420;
}
namespace GpuMeshPublicationGroupId {
inline constexpr uint32_t PhysicsVisual=1;
inline constexpr uint32_t Population=2;
inline constexpr uint32_t Snow=3;
}

// Native PhysX visual group metadata. This range begins in the physical
// source buffer, not the combined GPU render publication. It does NOT yet
// change the ray descriptor traversal; that belongs to the next stage.
struct GpuMeshPhysicalRayMetadata {
    uint32_t sourceFirst{},physicalCount{};
    uint64_t storageGeneration{},rayRevision{};
};
inline const GpuMeshPhysicalRayMetadata* PhysicalRayMetadata(const GpuPublication& publication){
    const auto* group=publication.FindGroup(GpuMeshPublicationGroupId::PhysicsVisual);
    return group&&group->metadata?
        static_cast<const GpuMeshPhysicalRayMetadata*>(group->metadata.get()):nullptr;
}

}
