#pragma once

#include <stdint.h>

#define LABOVR_PLUGIN_ABI_V1 1u

#if defined(_WIN32)
#  define LABOVR_PLUGIN_CALL __cdecl
#  define LABOVR_PLUGIN_EXPORT __declspec(dllexport)
#  define LABOVR_PLUGIN_IMPORT __declspec(dllimport)
#else
#  define LABOVR_PLUGIN_CALL
#  define LABOVR_PLUGIN_EXPORT __attribute__((visibility("default")))
#  define LABOVR_PLUGIN_IMPORT
#endif

#if defined(LABOVR_PLUGIN_BUILD)
#  define LABOVR_PLUGIN_API LABOVR_PLUGIN_EXPORT
#elif defined(LABOVR_PLUGIN_USE_DLL)
#  define LABOVR_PLUGIN_API LABOVR_PLUGIN_IMPORT
#else
#  define LABOVR_PLUGIN_API
#endif

typedef uint8_t  LabU8;
typedef uint16_t LabU16;
typedef uint32_t LabU32;
typedef uint64_t LabU64;
typedef int8_t   LabI8;
typedef int16_t  LabI16;
typedef int32_t  LabI32;
typedef int64_t  LabI64;

typedef struct LabHostApiV1 LabHostApiV1;
typedef struct LabWorldApiV1 LabWorldApiV1;
typedef void* LabPluginWorldHandle;

typedef struct LabWorldCreateInfoV1 {
    LabU32 version;
    LabU32 structSize;
    /* Optional UTF-8 scene override. Borrowed only for the CreateWorldEx call. */
    const char* scenePathUtf8;
    /* Append-only V1 extension. UTF-8 root directory of the selected world
       package. Borrowed only for CreateWorldEx; plugins copy it if retained. */
    const char* packageRootUtf8;
} LabWorldCreateInfoV1;

typedef struct LabPluginInfoV1 {
    LabU32 abiVersion;
    LabU32 structSize;
    const char* pluginId;
    const char* displayName;
    const char* pluginVersion;
} LabPluginInfoV1;

typedef struct LabPluginApiV1 {
    LabU32 abiVersion;
    LabU32 structSize;
    LabPluginInfoV1 info;
    /* The plugin owns every world instance it creates. The host must return
       the handle only through DestroyWorld; no allocation crosses modules. */
    LabPluginWorldHandle (LABOVR_PLUGIN_CALL *CreateWorld)(const LabHostApiV1* hostApi);
    void (LABOVR_PLUGIN_CALL *DestroyWorld)(LabPluginWorldHandle world);
    /* Append-only V1 extension. Read only when structSize reaches this field. */
    const LabWorldApiV1* worldApi;
    /* Append-only V1 creation extension. Create-info strings are borrowed only
       for the duration of this call; the plugin must copy data it retains. */
    LabPluginWorldHandle (LABOVR_PLUGIN_CALL *CreateWorldEx)(const LabHostApiV1* hostApi, const LabWorldCreateInfoV1* createInfo);
} LabPluginApiV1;

/* Metadata strings are plugin-owned, read-only, and must remain valid for the
   lifetime of the returned API table. The host never frees them. */
#ifdef __cplusplus
extern "C" {
#endif
LABOVR_PLUGIN_API const LabPluginApiV1* LABOVR_PLUGIN_CALL LaboVR_GetPluginApi(LabU32 requestedAbi);
#ifdef __cplusplus
}
#endif
