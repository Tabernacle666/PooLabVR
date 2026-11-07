#pragma once
#include "LaboVRWorld.h"
#define LAB_AA_OFF_V1 0u
#define LAB_AA_LOW_V1 1u
#define LAB_AA_QUALITY_V1 2u
/* Spatial, per-eye filtering. Fine coverage and terminal domains compose after
   this pass. No temporal history, supersampling or change to MSAA allocation. */
typedef struct LabAntialiasingV1 {LabU32 structSize,mode;} LabAntialiasingV1;
/* Runtime scene MSAA: 1 (off) or 4 samples. */
typedef struct LabMultisamplingV1 {LabU32 structSize,samples;} LabMultisamplingV1;
/* Activated scene material index. Texture slots, geometry, alpha and emission
   remain authored. Procedural normals are evaluated in the existing PBR pass. */
typedef struct LabSurfaceSettingsV1 {
    LabU32 structSize,material;
    LabVec3V1 color;
    float roughness,normalAmplitude,normalFrequency,junctionWidth,junctionContrast;
} LabSurfaceSettingsV1;
