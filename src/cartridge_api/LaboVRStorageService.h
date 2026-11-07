#pragma once
#include "LaboVRServices.h"
#define LABOVR_STORAGE_SERVICE_VERSION_V1 1u
enum LabProfileScopeV1 { LAB_PROFILE_SYSTEM_V1=0, LAB_PROFILE_USER_V1=1 };
enum LabStorageStatusV1 { LAB_STORAGE_OK_V1=0, LAB_STORAGE_MISSING_V1=1, LAB_STORAGE_UNAVAILABLE_V1=2, LAB_STORAGE_IO_V1=3, LAB_STORAGE_INVALID_V1=4, LAB_STORAGE_CAPACITY_V1=5 };
typedef struct LabStorageResultV1 { LabU32 structSize,status,bytes,reserved; char message[96]; } LabStorageResultV1;
/* Generic versioned cartridge profiles. Names are simple ASCII stems.
   System writes target the project source profile directory, never build/.
   User writes target a durable per-user directory. Write replaces atomically.
   Read with data=0 queries size. Capacity and errors are always explicit. */
typedef struct LabStorageServiceV1 {
    LabU32 version,structSize; void* serviceContext;
    LabU32 (LABOVR_PLUGIN_CALL *ReadProfile)(void*,LabU32,const char*,void*,LabU32,LabStorageResultV1*);
    LabU32 (LABOVR_PLUGIN_CALL *WriteProfile)(void*,LabU32,const char*,const void*,LabU32,LabStorageResultV1*);
} LabStorageServiceV1;
