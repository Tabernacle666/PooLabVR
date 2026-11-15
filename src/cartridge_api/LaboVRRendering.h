#pragma once
#include "LaboVRWorld.h"
#define LAB_AA_OFF_V1 0u
#define LAB_AA_LOW_V1 1u
#define LAB_AA_QUALITY_V1 2u


typedef struct LabAntialiasingV1 {LabU32 structSize,mode;} LabAntialiasingV1;

typedef struct LabMultisamplingV1 {LabU32 structSize,samples;} LabMultisamplingV1;


typedef struct LabSurfaceSettingsV1 {
    LabU32 structSize,material;
    LabVec3V1 color;
    float roughness,normalAmplitude,normalFrequency,junctionWidth,junctionContrast;
} LabSurfaceSettingsV1;
