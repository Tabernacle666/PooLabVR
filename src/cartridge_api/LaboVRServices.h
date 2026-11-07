#pragma once

#include "LaboVRPlugin.h"

#define LABOVR_SERVICE_INPUT_V1 0x00010001u
#define LABOVR_SERVICE_AUDIO_V1 0x00030001u
#define LABOVR_SERVICE_GPU_V1   0x00040001u
#define LABOVR_SERVICE_GPU_INTEROP_V1 0x00090001u
#define LABOVR_SERVICE_PHYSICS_V1 0x000B0001u
#define LABOVR_SERVICE_STORAGE_V1 0x000C0001u
#define LABOVR_SERVICE_VISUAL_V1 0x000A0001u

typedef const void* (LABOVR_PLUGIN_CALL *LabQueryServiceFn)(
    void* hostContext,
    LabU32 serviceId,
    LabU32 requestedVersion);

typedef struct LabHostApiV1 {
    LabU32 abiVersion;
    LabU32 structSize;
    void* hostContext;
    LabQueryServiceFn QueryService;
} LabHostApiV1;
