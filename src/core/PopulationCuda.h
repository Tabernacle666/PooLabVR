#pragma once
#include "PhysicsCuda.h"
#include "../cartridge_api/LaboVRPopulation.h"
namespace lab {
struct PopulationBodyGpu {
    physx::PxTransform initial,local;
    physx::PxVec3 dimensions;
    uint32_t index,shape,enabled,hand,kinematic;
    float radius,restitution;
    uint64_t handle;
};
struct PopulationBodyPoseGpu {
    physx::PxTransform current,previous;
    uint64_t handle;
};
inline constexpr uint32_t PopulationGridWords=8*8*8*4;
struct PopulationStateGpu {
    physx::PxVec3 position,previous,velocity,heading,liftAxis;
    float impact,activity,wingPhase; // impact = expiry on the Core display clock
    uint32_t birth,chased;
};
static_assert(sizeof(PopulationStateGpu)==80);
struct PopulationSummaryGpu {
    uint32_t nearCount,detailedCount,targetKey,voiceKeys[8],detailKeys[64];
    LabPopulationSnapshotV1 snapshot;
};
void PopulationInitialize(cudaStream_t,PopulationStateGpu*,uint32_t,uint32_t,const LabPopulationDescV1&,uint32_t,const PopulationBodyGpu*,uint32_t,const physx::PxTransform*);
void PopulationStep(cudaStream_t,PopulationStateGpu*,uint32_t,const LabPopulationDescV1&,const LabPopulationInputV1&,const LabPopulationEffectsV1&,const LabPopulationHandsV1&,const PopulationBodyGpu*,PopulationBodyPoseGpu*,uint32_t*,uint32_t,const physx::PxTransform*,const physx::PxTransform*,const physx::PxVec3*,const physx::PxVec3*,physx::PxVec3*,physx::PxVec3*,const PhysicsLightGpu*,uint32_t,const PhysicsAirflowGpu*,uint32_t,const LabPopulationSpaceV1&,float,float,double,float,bool);
void PopulationPack(cudaStream_t,const PopulationStateGpu*,uint32_t,uint32_t,const LabPopulationDescV1&,const LabPopulationInputV1&,const LabPopulationEffectsV1&,const PopulationBodyGpu*,const PopulationBodyPoseGpu*,uint32_t,const PhysicsLightGpu*,uint32_t,const physx::PxTransform*,bool,GpuInstance*,uint32_t*,uint32_t,PopulationSummaryGpu*,uint64_t,uint64_t,float,float);
}
