#pragma once
#include "LaboVRServices.h"
#include "LaboVRWorld.h"
#include "LaboVRGpuService.h"
#include "LaboVRPopulation.h"
#include "LaboVREdit.h"

#define LABOVR_PHYSICS_SERVICE_VERSION_V1 1u
#define LABOVR_PHYSICS_MAX_PROBES_V1 128u

typedef LabU64 LabPhysicsSceneV1;
typedef LabU64 LabPhysicsBodyV1;
typedef LabU64 LabPhysicsJointV1;
enum LabPhysicsBodyKindV1 { LAB_PHYSICS_STATIC=0, LAB_PHYSICS_DYNAMIC=1, LAB_PHYSICS_KINEMATIC=2 };
enum LabPhysicsShapeKindV1 { LAB_PHYSICS_BOX=0, LAB_PHYSICS_SPHERE=1, LAB_PHYSICS_CAPSULE=2, LAB_PHYSICS_CONVEX=3 };
enum LabPhysicsBodyFlagsV1 { LAB_PHYSICS_BODY_SPECULATIVE_CCD=1u };
enum LabPhysicsMotionV1 { LAB_PHYSICS_LOCKED=0, LAB_PHYSICS_LIMITED=1, LAB_PHYSICS_FREE=2 };
enum LabPhysicsD6DriveKindV1 { LAB_PHYSICS_DRIVE_X=0, LAB_PHYSICS_DRIVE_Y=1, LAB_PHYSICS_DRIVE_Z=2, LAB_PHYSICS_DRIVE_SWING=3, LAB_PHYSICS_DRIVE_TWIST=4, LAB_PHYSICS_DRIVE_SLERP=5 };
enum LabPhysicsCommandKindV1 { LAB_PHYSICS_FORCE=0, LAB_PHYSICS_TORQUE=1, LAB_PHYSICS_IMPULSE=2, LAB_PHYSICS_ANGULAR_IMPULSE=3, LAB_PHYSICS_KINEMATIC_TARGET=4 };

typedef struct LabPhysicsSceneDescV1 {
    LabU32 structSize,maxBodies;
    LabVec3V1 gravity;
    float fixedStepSeconds;
    LabU32 maxSubsteps,reserved;
} LabPhysicsSceneDescV1;
typedef struct LabPhysicsBodyDescV1 {
    LabU32 structSize,kind,shape,layer;
    LabU32 collisionMask,collisionGroup,reserved[2]; /* reserved[0]: LabPhysicsBodyFlagsV1 */
    LabPoseV1 pose,shapePose;
    /* Box half extents; sphere radius in x; capsule radius/halfHeight in x/y (axis X). */
    LabVec3V1 dimensions;
    float mass;
    /* Positive diagonal inertia; zero selects PhysX geometry-based inertia. */
    LabVec3V1 inertia;
    float staticFriction,dynamicFriction,restitution,linearDamping,angularDamping;
    float contactOffset,restOffset,maxAngularVelocity;
    /* Borrowed during CreateBody only. GPU convex cooking is mandatory, <=64 vertices. */
    const LabVec3V1* convexVertices;
    LabU32 convexVertexCount,reserved2;
} LabPhysicsBodyDescV1;
typedef struct LabPhysicsD6DriveV1 { float stiffness,damping,maxForce; LabU32 acceleration; } LabPhysicsD6DriveV1;
typedef struct LabPhysicsD6DescV1 {
    LabU32 structSize,collideConnected;
    LabPhysicsBodyV1 bodyA,bodyB; /* A=0 anchors frameA in world space. */
    LabPoseV1 frameA,frameB;
    /* X,Y,Z,Twist,Swing1,Swing2, with stable ABI enum values above. */
    LabU32 motion[6];
    float linearLower[3],linearUpper[3],twistLower,twistUpper,swingY,swingZ;
    float limitStiffness,limitDamping;
    /* X,Y,Z,Swing,Twist,SLERP. */
    LabPhysicsD6DriveV1 drive[6];
} LabPhysicsD6DescV1;
typedef struct LabPhysicsMotorV1 {
    LabU32 structSize,reserved;
    LabPhysicsJointV1 joint;
    LabPoseV1 target;
    LabVec3V1 linearVelocity,angularVelocity;
    /* Optional physical D6 twist motor. Evaluated at fixed simulation ticks, never a visual animation. */
    float frequencyHz,amplitudeRadians,phaseRadians,reserved2;
} LabPhysicsMotorV1;
typedef struct LabPhysicsCommandV1 {
    LabU32 structSize,kind;
    LabPhysicsBodyV1 body;
    LabVec3V1 value;
    LabU32 reserved;
    LabPoseV1 target;
} LabPhysicsCommandV1;
typedef struct LabPhysicsAeroSurfaceV1 {
    LabU32 structSize,reserved;
    LabPhysicsBodyV1 wing,carrier;
    /* Simplified flapping-wing law: actual relative angular speed squared drives lift.
       No velocity or pose integration occurs here; all forces enter the PhysX solver. */
    LabVec3V1 liftAxis;
    float liftCoefficient,dragCoefficient,maxLift,maxDrag;
} LabPhysicsAeroSurfaceV1;
/* Rotor-driven 3D airflow. Speed follows the emitter's actual GPU angular
   velocity around localDirection; no CPU body or population pose readback. */
typedef struct LabPhysicsAirflowV1 {
    LabU32 structSize,enabled;
    LabPhysicsBodyV1 emitter;
    LabVec3V1 localDirection;
    float radius,length,speedAtReferenceRpm,referenceRpm,turbulence;
} LabPhysicsAirflowV1;
/* Soft steering toward an authored flight volume; never teleports insects. */
typedef struct LabPopulationSpaceV1 {
    LabU32 structSize,enabled;
    LabVec3V1 center,halfExtent;
    float returnStrength;
} LabPopulationSpaceV1;
typedef struct LabPhysicsVisualV1 {
    LabU32 structSize,mesh,material,id;
    LabU32 flags,reserved;
    LabPhysicsBodyV1 body;
    LabPoseV1 localPose;
    LabVec3V1 scale;
    float boundingRadius;
} LabPhysicsVisualV1;
typedef struct LabPhysicsLightV1 {
    /* Index in the activated composed scene, including activation.lightBase. */
    LabU32 structSize,sceneLightIndex;
    LabPhysicsBodyV1 body;
    LabVec3V1 localPosition;
    float radius;
    LabVec3V1 color;
    float power;
    LabVec3V1 localDirection;
    float outerCos,innerCos;
    LabU32 reserved[3];
} LabPhysicsLightV1;
typedef struct LabPhysicsStateV1 {
    LabPhysicsBodyV1 body;
    LabPoseV1 pose;
    LabVec3V1 linearVelocity,angularVelocity;
    LabU32 contactCount,reserved;
    LabVec3V1 contactNormal;
    float normalImpulse;
    LabPhysicsBodyV1 contactBody;
} LabPhysicsStateV1;
typedef struct LabPhysicsSnapshotV1 {
    LabU32 structSize,stateCapacity;
    LabPhysicsStateV1* states; /* Caller-owned; copied only during PollSnapshot. */
    LabU32 stateCount,contactOverflow;
    LabU64 simulationTick,sourceFrame;
    double simulationSeconds;
} LabPhysicsSnapshotV1;

/* A table is scoped to one host/client generation. Handles are opaque, never reused
   across owners. Calls run on the console frame thread between simulation steps.
   The Core advances every scene automatically after clients issue commands.
   Force/torque batches last one display frame and apply at every fixed substep;
   impulse batches apply once. Kinematic targets are tracked input, not dynamics.
   MaxSubsteps bounds catch-up; time beyond that budget is discarded on suspension.
   Visual provider uses three slots; CUDA waits on the consumer fence before reuse.
   PollSnapshot is nonblocking: 0=no completed sample; 1=copied, possibly old (inspect
   sourceFrame/tick). Only explicitly selected probes are read back, never visuals.
   Scene destruction retires GPU work and invalidates all its body/joint handles. */
typedef struct LabPhysicsServiceV1 {
    LabU32 version,structSize;
    void* serviceContext;
    LabU32 (LABOVR_PLUGIN_CALL *CreateScene)(void*,const LabPhysicsSceneDescV1*,LabPhysicsSceneV1*);
    LabU32 (LABOVR_PLUGIN_CALL *DestroyScene)(void*,LabPhysicsSceneV1);
    LabU32 (LABOVR_PLUGIN_CALL *CreateBody)(void*,LabPhysicsSceneV1,const LabPhysicsBodyDescV1*,LabPhysicsBodyV1*);
    LabU32 (LABOVR_PLUGIN_CALL *DestroyBody)(void*,LabPhysicsSceneV1,LabPhysicsBodyV1);
    LabU32 (LABOVR_PLUGIN_CALL *CreateD6)(void*,LabPhysicsSceneV1,const LabPhysicsD6DescV1*,LabPhysicsJointV1*);
    LabU32 (LABOVR_PLUGIN_CALL *ConfigureD6)(void*,LabPhysicsSceneV1,LabPhysicsJointV1,const LabPhysicsD6DescV1*);
    LabU32 (LABOVR_PLUGIN_CALL *DestroyJoint)(void*,LabPhysicsSceneV1,LabPhysicsJointV1);
    LabU32 (LABOVR_PLUGIN_CALL *SetMotors)(void*,LabPhysicsSceneV1,const LabPhysicsMotorV1*,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *SubmitCommands)(void*,LabPhysicsSceneV1,const LabPhysicsCommandV1*,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *SetAeroSurfaces)(void*,LabPhysicsSceneV1,const LabPhysicsAeroSurfaceV1*,LabU32);
    /* Mesh/material IDs are already rebased to the activated visual package.
       Configure before registering the provider. If the material set or buffer
       layout changes, fetch/register the provider again before rendering. */
    LabU32 (LABOVR_PLUGIN_CALL *SetVisuals)(void*,LabPhysicsSceneV1,const LabPhysicsVisualV1*,LabU32,const LabPhysicsLightV1*,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *GetVisualProvider)(void*,LabPhysicsSceneV1,LabGpuPublicationProviderV1*);
    /* Controls visual publication and local observer interaction. It does not
       pause the population; SetPopulationInput.enabled controls simulation. */
    LabU32 (LABOVR_PLUGIN_CALL *SetVisible)(void*,LabPhysicsSceneV1,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *SetLightPower)(void*,LabPhysicsSceneV1,LabU32,float);
    LabU32 (LABOVR_PLUGIN_CALL *SetProbes)(void*,LabPhysicsSceneV1,const LabPhysicsBodyV1*,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *PollSnapshot)(void*,LabPhysicsSceneV1,LabPhysicsSnapshotV1*);
    /* Optional V1 tail: disable/restore the body's declared collision mask.
       Does not recreate the actor, change its pose or remove its constraints.
       Older V1 tables end immediately before this field; inspect structSize. */
    LabU32 (LABOVR_PLUGIN_CALL *SetBodyCollisionEnabled)(void*,LabPhysicsSceneV1,LabPhysicsBodyV1,LabU32);
    /* Optional V1 population tail (1.1.3). Configure once, then batch count,
       settings and tracked input edits. GPU allocation grows geometrically;
       count changes preserve existing state. Poll is nonblocking and returns
       bounded diagnostics/audio representatives, never the population array.
       Convex PhysX colliders use a conservative bounding sphere for insect
       avoidance; scene capacity above 128 is rejected. */
    LabU32 (LABOVR_PLUGIN_CALL *CreatePopulation)(void*,LabPhysicsSceneV1,const LabPopulationDescV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationCount)(void*,LabPhysicsSceneV1,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *SetFlightSettings)(void*,LabPhysicsSceneV1,const LabFlightSettingsV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationInput)(void*,LabPhysicsSceneV1,const LabPopulationInputV1*);
    LabU32 (LABOVR_PLUGIN_CALL *PollPopulation)(void*,LabPhysicsSceneV1,LabPopulationSnapshotV1*);
    /* Optional 1.1.4 tail. Does not recreate or clear existing insect state. */
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationEffects)(void*,LabPhysicsSceneV1,const LabPopulationEffectsV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationHands)(void*,LabPhysicsSceneV1,const LabPopulationHandsV1*);
    /* Optional 1.1.5 authoring tail. Resume rejects strong solid penetration.
       GetEditSnapshot is a bounded explicit editor read, never population readback. */
    LabU32 (LABOVR_PLUGIN_CALL *SetPaused)(void*,LabPhysicsSceneV1,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *GetEditSnapshot)(void*,LabPhysicsSceneV1,LabPhysicsEditSnapshotV1*);
    LabU32 (LABOVR_PLUGIN_CALL *ApplyEdit)(void*,LabPhysicsSceneV1,const LabPhysicsEditBatchV1*);
    LabU32 (LABOVR_PLUGIN_CALL *TranslateBodies)(void*,LabPhysicsSceneV1,const LabPhysicsBodyV1*,LabU32,LabVec3V1);
    LabU32 (LABOVR_PLUGIN_CALL *ValidateEdit)(void*,LabPhysicsSceneV1,LabPhysicsEditValidationV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetAirflows)(void*,LabPhysicsSceneV1,const LabPhysicsAirflowV1*,LabU32);
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationSpace)(void*,LabPhysicsSceneV1,const LabPopulationSpaceV1*);
    /* Optional tail: toggle speculative CCD on one dynamic body at runtime. */
    LabU32 (LABOVR_PLUGIN_CALL *SetBodySpeculativeCCD)(void*,LabPhysicsSceneV1,LabPhysicsBodyV1,LabU32);
} LabPhysicsServiceV1;
