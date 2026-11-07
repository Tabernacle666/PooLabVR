#pragma once
#include "LaboVRWorld.h"
/* Explicit authoring operations; only allowed while the scene is paused.
   These never represent gameplay forces. Handles remain owner-scoped.
   A batch is validated before any pose is changed. Dynamic state is written
   through the Direct GPU API, with previous=current and zero edit velocity. */
typedef struct LabPhysicsEditBodyV1 { LabU64 body; LabPoseV1 pose; } LabPhysicsEditBodyV1;
typedef struct LabPhysicsEditJointV1 { LabU64 joint; LabPoseV1 frameA,frameB; } LabPhysicsEditJointV1;
typedef struct LabPhysicsEditBatchV1 {
    LabU32 structSize,bodyCount;
    const LabPhysicsEditBodyV1* bodies;
    LabU32 jointCount,reserved;
    const LabPhysicsEditJointV1* joints;
} LabPhysicsEditBatchV1;
typedef struct LabPhysicsEditSnapshotV1 {
    LabU32 structSize,bodyCapacity;
    LabPhysicsEditBodyV1* bodies;
    LabU32 jointCapacity,bodyCount;
    LabPhysicsEditJointV1* joints;
    LabU32 jointCount,reserved;
    double simulationSeconds;
} LabPhysicsEditSnapshotV1;
typedef struct LabPhysicsEditValidationV1 {
    LabU32 structSize,valid;
    LabU64 bodyA,bodyB;
    float penetration;
    LabU32 reserved;
} LabPhysicsEditValidationV1;
