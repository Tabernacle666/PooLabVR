#pragma once
#include "LaboVRWorld.h"
#define LAB_ATMOSPHERE_DOMAIN_V1 0x464f4720u
#define LAB_POPULATION_PROXY_BINDING_V1 0x201u
#define LAB_POPULATION_SHADOW_BINDING_V1 0x200u
#define LAB_POPULATION_SHADOW_MAX_V1 4096u
/* World coordinates and velocities are meters and meters/second. Previous
   position is from the same uninterrupted tracking revision. */
typedef struct LabTrackedSweepV1 {
    LabVec3V1 position,previous,velocity;
    LabU32 tracked;
} LabTrackedSweepV1;
typedef struct LabPopulationHandsV1 {
    LabU32 structSize,reserved;
    LabTrackedSweepV1 hand[2];
} LabPopulationHandsV1;
typedef struct LabPopulationEffectsV1 {
    LabU32 structSize,reserved;
    float sweepRadius,impulse,fleeSeconds,avoidanceRadius,residualAversion;
    float shadowDensity,shadowIntensity;
} LabPopulationEffectsV1;
typedef struct LabFogSettingsV1 {
    LabU32 structSize,enabled,quality,lightIndex;
    LabVec3V1 center,halfExtent;
    float density,height,falloff,scattering,anisotropy,absorption;
    float turbulence,evolution,handResponse,lightIntensity;
} LabFogSettingsV1;
typedef struct LabFogInputV1 {
    LabU32 structSize,enabled;
    LabTrackedSweepV1 hand[2];
} LabFogInputV1;

/* Optional transport controls, independent of the V1 volume layout. Contrast
   describes actual world density structures; frequency is cycles per meter. */
typedef struct LabFogTransportV1 {
    LabU32 structSize,enabled;
    float contrast,frequency,sourceRate,reserved;
} LabFogTransportV1;

/* 1.1.5: dt=0 freezes transport/time while view-ray integration continues.
   Effective rotor speed (not its setpoint) determines wind speed. */
typedef struct LabFogWindV1 {
    LabU32 structSize,paused;
    LabVec3V1 position,direction;
    float speed,radius,length,dissipation;
} LabFogWindV1;
