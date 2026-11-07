#pragma once
#include "LaboVRWorld.h"
#include "LaboVRAtmosphere.h"
#define LAB_PHYSICS_POPULATION_COLLIDERS_V1 128u
#define LAB_PHYSICS_POPULATION_MAX_V1 1048576u
#define LAB_PHYSICS_POPULATION_DETAIL_MAX_V1 64u
#define LAB_PHYSICS_POPULATION_AUDIO_MAX_V1 8u
/* Compact flapping bodies use the Core CUDA solver and collide with the owning
   PhysX scene. No per-insect actor, host integration, or client-owned simulation. */
typedef struct LabFlightSettingsV1 {
    LabU32 structSize,seed;
    float nominalSpeed,maxSpeed,responseSeconds,turnIntensity,turnRadius;
    float lightAttraction,comfortDistance,anticipationDistance,wander,vertical;
    float avoidanceFailure,impactSeconds,impactMultiplier;
    float mass,wingArea,liftCoefficient,dragCoefficient,wingFrequency;
    float bodyRadius,detailDistance,nearDistance;
} LabFlightSettingsV1;
typedef struct LabPopulationDescV1 {
    LabU32 structSize,capacity,initialCount,layer;
    LabVec3V1 spawnCenter,spawnHalfExtent;
    LabU32 proxyMesh,bodyMesh,headMesh,abdomenMesh,leftWingMesh,rightWingMesh;
    LabU32 bodyMaterial,headMaterial,wingMaterial,collisionMask;
    LabFlightSettingsV1 flight;
} LabPopulationDescV1;
typedef struct LabPopulationInputV1 {
    LabU32 structSize,enabled;
    LabVec3V1 playerPosition,aimOrigin,aimDirection;
    LabU32 aimTracked;
    /* Explicit tracked-hand bodies mark real physical collisions as impacts. */
    LabU64 hands[2];
} LabPopulationInputV1;
typedef struct LabPopulationVoiceV1 {
    LabVec3V1 position,velocity;
    float activity,frequency;
    LabU32 insect,reserved;
} LabPopulationVoiceV1;
typedef struct LabPopulationSnapshotV1 {
    LabU32 structSize,count,nearCount,detailedCount;
    LabU32 simplifiedCount,targetId,voiceCount,capacity;
    LabU64 sourceFrame,simulationTick;
    float targetImpactRemaining,targetActivity;
    LabPopulationVoiceV1 voices[LAB_PHYSICS_POPULATION_AUDIO_MAX_V1];
} LabPopulationSnapshotV1;
