#pragma once
#include "../cartridge_api/LaboVRPhysicsService.h"
#include "GpuMeshTypes.h"
#include <PxPhysicsAPI.h>
#include <cuda_runtime_api.h>
namespace lab {
inline constexpr uint32_t PhysicsInvalidIndex=0xffffffffu;
struct PhysicsVisualGpu {LabPhysicsVisualV1 visual;uint32_t index;physx::PxTransform initial;};
struct PhysicsLightGpu {LabPhysicsLightV1 light;uint32_t index;physx::PxTransform initial;};
struct PhysicsAeroGpu {LabPhysicsAeroSurfaceV1 surface;uint32_t wing,carrier;};
struct PhysicsAirflowGpu {LabPhysicsAirflowV1 field;uint32_t emitter;};
struct PhysicsDragGpu {uint32_t index;LabPhysicsBodyDescV1 geometry;};
struct PhysicsProbeGpu {LabPhysicsBodyV1 handle;uint64_t actor;uint32_t index,reserved;physx::PxTransform initial;};
struct PhysicsActorGpu {uint64_t actor;LabPhysicsBodyV1 handle;};
struct PhysicsGpuLight {GpuFloat4 positionRadius,colorPower,directionOuter,parameters;};
static_assert(sizeof(PhysicsGpuLight)==64);
void PhysicsApplyAero(cudaStream_t,const PhysicsAeroGpu*,uint32_t,const physx::PxTransform*,const physx::PxVec3*,const physx::PxVec3*,physx::PxVec3*,physx::PxVec3*);
void PhysicsApplyAirflow(cudaStream_t,const PhysicsDragGpu*,uint32_t,const PhysicsAirflowGpu*,uint32_t,const physx::PxTransform*,const physx::PxVec3*,const physx::PxVec3*,physx::PxVec3*,physx::PxVec3*,float);
void PhysicsPackVisuals(cudaStream_t,const PhysicsVisualGpu*,uint32_t,const PhysicsLightGpu*,uint32_t,const physx::PxTransform*,const physx::PxTransform*,float,bool,GpuInstance*,uint32_t*,PhysicsGpuLight*);
void PhysicsPackProbes(cudaStream_t,const PhysicsProbeGpu*,uint32_t,const physx::PxTransform*,const physx::PxVec3*,const physx::PxVec3*,const physx::PxGpuContactPair*,const uint32_t*,uint32_t,const PhysicsActorGpu*,uint32_t,LabPhysicsStateV1*);
}
