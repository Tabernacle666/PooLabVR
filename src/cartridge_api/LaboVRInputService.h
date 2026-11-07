#pragma once

#include "LaboVRServices.h"

#define LABOVR_INPUT_SERVICE_VERSION_V1 1u

/* Stable input_v1 action token.
   Handles are opaque runtime values: 0 is invalid and plugins must never derive,
   index, increment or otherwise interpret them. Resolve them by name. */
typedef LabU32 LabInputActionHandleV1;

enum LabInputActionTypeV1 {
    LABOVR_INPUT_ACTION_TYPE_BOOLEAN_V1 = 1,
    LABOVR_INPUT_ACTION_TYPE_FLOAT_V1 = 2,
    LABOVR_INPUT_ACTION_TYPE_VECTOR2_V1 = 3,
    LABOVR_INPUT_ACTION_TYPE_POSE_V1 = 4,
    LABOVR_INPUT_ACTION_TYPE_HAPTIC_V1 = 5
};

typedef struct LabInputActionInfoV1 {
    LabU32 structSize;
    LabInputActionHandleV1 handle;
    LabU32 type;
    LabU32 reserved;
    const char* nameUtf8;
    const char* displayNameUtf8;
    const char* actionSetNameUtf8;
} LabInputActionInfoV1;

enum LabInputSampleFlagsV1 {
    LABOVR_INPUT_SAMPLE_ACTIVE_V1  = 1u << 0,
    LABOVR_INPUT_SAMPLE_DOWN_V1    = 1u << 1,
    LABOVR_INPUT_SAMPLE_PRESSED_V1 = 1u << 2,
    LABOVR_INPUT_SAMPLE_TRACKED_V1 = 1u << 3
};

typedef struct LabInputSampleV1 {
    LabInputActionHandleV1 controlId;
    LabU32 sourceId;
    LabU32 flags;
    LabU32 valueCount;
    float value[4];
} LabInputSampleV1;

typedef struct LabInputPoseStateV1 {
    LabU32 structSize;
    LabInputActionHandleV1 controlId;
    LabU32 sourceId;
    LabU32 flags;
    float position[3];
    float orientation[4];
    float velocity[3];
    float angularVelocity[3];
} LabInputPoseStateV1;

typedef struct LabInputSnapshotV1 {
    LabU32 version;
    LabU32 structSize;
    LabU64 frameId;
    double timeSeconds;
    const LabInputSampleV1* samples;
    LabU32 sampleCount;
    LabU32 reserved;
    LabU64 trackingRevision;
    LabU32 focused;
    LabU32 reserved2;
    LabInputPoseStateV1 head;
    const LabInputPoseStateV1* poses;
    LabU32 poseCount;
    LabU32 reserved3;
} LabInputSnapshotV1;

typedef struct LabInputHapticV1 {
    LabU32 structSize;
    LabInputActionHandleV1 actionId;
    LabU32 sourceId;
    LabU32 reserved;
    float amplitude;
    float durationSeconds;
    LabU64 frameId;
} LabInputHapticV1;

/* Physical source tokens are opaque and valid for the frozen input registry.
   Availability follows the real active OpenXR profile, independently per hand. */
typedef struct LabInputPhysicalSourceV1 {
    LabU32 id,type,availableMask,activeMask,downMask,selectedMask;
    char name[64];
} LabInputPhysicalSourceV1;
typedef struct LabInputPhysicalSourcesV1 {
    LabU32 structSize,capacity,count,reserved;
    LabInputActionHandleV1 action;
    LabU32 reserved2;
    LabInputPhysicalSourceV1* sources;
} LabInputPhysicalSourcesV1;
typedef struct LabInputBooleanSourcesV1 {
    LabU32 structSize,handMask,count;
    LabInputActionHandleV1 action;
    const LabU32* sourceIds;
} LabInputBooleanSourcesV1;
typedef struct LabInputServiceV1 {
    LabU32 version;
    LabU32 structSize;
    void* serviceContext;
    /* Host-owned bulk snapshot. Borrowed pointers remain valid only until the
       next GetSnapshot call on this service. */
    LabU32 (LABOVR_PLUGIN_CALL *GetSnapshot)(void* serviceContext, LabInputSnapshotV1* outSnapshot);
    /* Stable input_v1 prefix. Existing fields and offsets are frozen. */
    LabU32 (LABOVR_PLUGIN_CALL *EmitHaptic)(void* serviceContext, const LabInputHapticV1* haptic);
    /* Stable input_v1 logical action registry. Handles are opaque to plugins. */
    LabU32 (LABOVR_PLUGIN_CALL *ResolveAction)(void* serviceContext, const char* nameUtf8, LabInputActionHandleV1* outHandle);
    LabU32 (LABOVR_PLUGIN_CALL *GetActionInfo)(void* serviceContext, LabInputActionHandleV1 handle, LabInputActionInfoV1* outInfo);
    LabU32 (LABOVR_PLUGIN_CALL *GetPhysicalSources)(void*,LabInputPhysicalSourcesV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetBooleanSources)(void*,const LabInputBooleanSourcesV1*);
} LabInputServiceV1;
