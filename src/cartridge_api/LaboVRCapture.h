#pragma once
#include "LaboVRWorld.h"
#define LAB_CAPTURE_IDLE_V1 0u
#define LAB_CAPTURE_QUEUED_V1 1u
#define LAB_CAPTURE_GPU_V1 2u
#define LAB_CAPTURE_ENCODING_V1 3u
#define LAB_CAPTURE_SAVED_V1 4u
#define LAB_CAPTURE_FAILED_V1 5u
typedef struct LabCaptureRequestV1 {LabU32 structSize,eye,includeInterface,reserved;} LabCaptureRequestV1;
typedef struct LabCaptureStatusV1 {
    LabU32 structSize,state;LabU64 requestId,frameId;
    char directoryUtf8[384],fileUtf8[512],message[128];
} LabCaptureStatusV1;
