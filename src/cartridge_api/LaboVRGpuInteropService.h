#pragma once
#include "LaboVRServices.h"
#define LABOVR_GPU_INTEROP_SERVICE_VERSION_V1 1u

typedef LabU64 LabGpuInteropResourceHandleV1;
typedef LabU64 LabGpuInteropFenceHandleV1;
typedef struct LabGpuInteropMemoryExportV1 {
    LabU32 structSize,reserved;
    void* win32Handle;
    LabU64 allocationBytes,resourceBytes;
} LabGpuInteropMemoryExportV1;
typedef struct LabGpuInteropFenceExportV1 {
    LabU32 structSize,reserved;
    void* win32Handle;
    LabU64 completedValue;
} LabGpuInteropFenceExportV1;
typedef struct LabGpuInteropServiceV1 {
    LabU32 version,structSize;
    void* serviceContext;
    /* Adapter identity for selecting the matching CUDA device, never a device pointer. */
    unsigned char adapterLuid[8];
    LabU32 (LABOVR_PLUGIN_CALL *CreateSharedBuffer)(void*,LabU64,LabGpuInteropResourceHandleV1*);
    LabU32 (LABOVR_PLUGIN_CALL *CreateSharedFence)(void*,LabU64,LabGpuInteropFenceHandleV1*);
    /* Exported Win32 handles are duplicates owned/closed by the caller. */
    LabU32 (LABOVR_PLUGIN_CALL *GetCudaMemoryHandle)(void*,LabGpuInteropResourceHandleV1,LabGpuInteropMemoryExportV1*);
    /* duplicateHandle=0 queries completion without allocating an OS handle. */
    LabU32 (LABOVR_PLUGIN_CALL *GetCudaFenceHandle)(void*,LabGpuInteropFenceHandleV1,LabU32 duplicateHandle,LabGpuInteropFenceExportV1*);
    LabU32 (LABOVR_PLUGIN_CALL *ReleaseResource)(void*,LabGpuInteropResourceHandleV1);
    LabU32 (LABOVR_PLUGIN_CALL *ReleaseFence)(void*,LabGpuInteropFenceHandleV1);
} LabGpuInteropServiceV1;
