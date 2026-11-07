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
}

}
