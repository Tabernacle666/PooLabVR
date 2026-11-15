#pragma once
#include "GpuPublication.h"

namespace OASIS {


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


constexpr uint32_t PhysicsInstances=0x400;
}
namespace GpuMeshPublicationGroupId {
inline constexpr uint32_t PhysicsVisual=1;
}


struct GpuMeshPhysicalRayMetadata {
    uint32_t sourceFirst{},physicalCount{};
    uint64_t storageGeneration{},rayRevision{},sceneIdentity{};
};
inline const GpuMeshPhysicalRayMetadata* PhysicalRayMetadata(const GpuPublication& publication){
    const auto* group=publication.FindGroup(GpuMeshPublicationGroupId::PhysicsVisual);
    return group&&group->metadata?
        static_cast<const GpuMeshPhysicalRayMetadata*>(group->metadata.get()):nullptr;
}

}
