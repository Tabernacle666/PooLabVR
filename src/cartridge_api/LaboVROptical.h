#pragma once
#include "LaboVRWorld.h"
#define LAB_POPULATION_OPTICAL_DOMAIN_V1 0x4f505443u
#define LAB_POPULATION_OPTICAL_BINDING_V1 0x202u
/* Standalone optical records have the same first 48 bytes and stride as a
   MeshSolid transform, so an existing GPU transform range can be aliased. */
typedef struct LabPopulationOpticalInstanceV1 {
    LabVec3V1 position;float radius; /* <= 0 disables this occluder */
    LabQuatV1 orientation;
    LabVec3V1 scale;float representedWings;
    LabU32 reserved[4];
} LabPopulationOpticalInstanceV1;
/* Binding: 64-byte optical records, one per real occluder. Position and
   quaternion are world-space, scale.xyz scales the optical half extents;
   representedWings (MeshSolid scale.w) is geometric wing coverage (0..1).
   Optical participants use the existing MeshSolid NoShadow flag to avoid
   individual shadow duplication; normal visibility meshes are retained.
   Range is active count, never allocation capacity; flags bit 0 disables it.
   Settings are owner-scoped and select publications from that same owner.
   Source bodies are already covered by producer meshes/population proxies;
   emission receives only wing membrane absent from that rasterized coverage.
   sourceId identifies the emitting MeshSolid instance, not its material.
   Small-occluder bounds: at most 1,048,576 active records per owner; finite
   normalized quaternions, scaled half extents in (0, .05] metres, scaled wing
   area <= .01 square metres. Invalid GPU records are ignored. Depth: .01..16 m.
   quality: 0/1/2 = 96x96x48 / 192x192x64 / 256x256x96;
   visualization: 0 normal, 1 optical depth, 2 transmission. lightIndex is the
   composed scene index (activation.lightBase + package-local light index).
   Optional gpu_v1 tail; consumers check structSize before calling it. */
typedef struct LabPopulationOpticalSettingsV1 {
    LabU32 structSize,enabled,quality,lightIndex;
    LabU32 domainId,binding,sourceId,visualization;
    LabVec3V1 bodyHalfExtent;
    float wingArea;
    float wingOpacity,sourceRadius,coverage,stabilizationSeconds;
} LabPopulationOpticalSettingsV1;
