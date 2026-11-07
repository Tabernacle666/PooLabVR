#pragma once

#include "LaboVRPlugin.h"

#define LABOVR_WORLD_API_VERSION_V1 1u

#ifndef LABOVR_POSE_TYPES_V1
#define LABOVR_POSE_TYPES_V1
typedef struct LabVec3V1 { float x, y, z; } LabVec3V1;
typedef struct LabQuatV1 { float x, y, z, w; } LabQuatV1;
typedef struct LabPoseV1 { LabVec3V1 position; LabQuatV1 orientation; } LabPoseV1;
#endif


typedef struct LabScenePackageV1 LabScenePackageV1;
typedef struct LabVisualPackageV1 LabVisualPackageV1;

typedef struct LabWorldActivationV1 {
    LabU32 version;
    LabU32 structSize;
    LabU32 meshBase;
    LabU32 materialBase;
    /* Append-only: inspect structSize before reading. Base of this package's
       lights in the composed scene, including the persistent Home if retained.
       physics_v1 GPU light publications use lightBase + local light index. */
    LabU32 lightBase;
    LabU32 reserved;
} LabWorldActivationV1;

typedef struct LabWorldFrameV1 {
    LabU32 version;
    LabU32 structSize;
    LabU64 frameId;
    double timeSeconds;
} LabWorldFrameV1;

typedef struct LabWorldApiV1 {
    LabU32 version;
    LabU32 structSize;
    /* Historical ABI slot; never queried by visual_v1 hosts. */
    LabU32 (LABOVR_PLUGIN_CALL *DeclareScene)(LabPluginWorldHandle world, const LabScenePackageV1** outScene);
    LabU32 (LABOVR_PLUGIN_CALL *Activate)(LabPluginWorldHandle world, const LabWorldActivationV1* activation);
    void (LABOVR_PLUGIN_CALL *Deactivate)(LabPluginWorldHandle world);
    void (LABOVR_PLUGIN_CALL *Frame)(LabPluginWorldHandle world, const LabWorldFrameV1* frame);
    LabU32 (LABOVR_PLUGIN_CALL *DeclareVisual)(LabPluginWorldHandle world, const LabVisualPackageV1** outVisual);
} LabWorldApiV1;
