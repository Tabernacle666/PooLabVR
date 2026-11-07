#pragma once

#include "LaboVRServices.h"
#include "LaboVRGpuInteropService.h"
#include "LaboVRTerminal.h"
#include "LaboVRCapture.h"
#include "LaboVRRendering.h"
#include "LaboVRAtmosphere.h"
#include "LaboVROptical.h"

#define LABOVR_GPU_ABI_V1 1u
#define LABOVR_GPU_SERVICE_VERSION_V1 1u
#define LABOVR_GPU_DOMAIN_MESH_SOLID_V1 1u
#define LABOVR_GPU_MESH_METADATA_BYTES_V1 16u
#define LABOVR_GPU_MESH_INDIRECT_ARGS_BYTES_V1 32u
#define LABOVR_GPU_PROVIDER_REBASED_MATERIALS_V1 1u

typedef LabU64 LabGpuHandle;
enum LabGpuMeshBindingV1 {
    LABOVR_GPU_MESH_BINDING_INSTANCES_V1 = 0,
    LABOVR_GPU_MESH_BINDING_METADATA_V1 = 1,
    LABOVR_GPU_MESH_BINDING_INDIRECT_ARGS_V1 = 2,
    /* Optional 64-byte GPU Light; binding=LIGHT_BASE+index in activated scene.
       Reserve a point light (flags: SHADOW|GPU_POSE) in visual_v1 at that index. */
    LABOVR_GPU_MESH_BINDING_LIGHT_BASE_V1 = 0x100
};


enum LabGpuResourceKindV1 {
    LABOVR_GPU_RESOURCE_BUFFER_V1 = 0,
    LABOVR_GPU_RESOURCE_TEXTURE2D_V1 = 1,
    LABOVR_GPU_RESOURCE_TEXTURE3D_V1 = 2
};

typedef struct LabGpuResourceRefV1 {
    LabGpuHandle handle;
    LabU32 kind;
    LabU32 flags;
    /* Resource/fence objects remain owned by the host. */
    LabU32 binding;
    LabU32 initialLayout;
    LabU64 offset;
    LabU64 bytes;
    LabU64 generation;
    LabGpuInteropResourceHandleV1 resourceHandle;
} LabGpuResourceRefV1;

typedef struct LabGpuPublicationV1 {
    LabU32 gpuAbiVersion;
    LabU32 structSize;
    LabU32 domainId;
    LabU32 reserved;
    const LabGpuResourceRefV1* resources;
    LabU32 resourceCount;
    LabU32 reserved2;
    LabU64 sequence;
    LabGpuInteropFenceHandleV1 producerFence;
    LabU64 producerValue;
    LabGpuInteropFenceHandleV1 consumerFence;
    LabU64 consumerValue;
    void* token;
    LabU64 tokenGeneration;
} LabGpuPublicationV1;

typedef LabU32 (LABOVR_PLUGIN_CALL *LabAcquireGpuPublicationFnV1)(
    void* callbackContext,
    LabGpuPublicationV1* outPublication);
typedef void (LABOVR_PLUGIN_CALL *LabReleaseGpuPublicationFnV1)(
    void* callbackContext,
    void* token,
    LabU64 tokenGeneration);

typedef struct LabGpuPublicationProviderV1 {
    /* Acquire leases the descriptor/resources until Release. The console queues
       the consumer signal before Release; completion can still be pending.
       Before overwriting a returned slot, producers must GPU-wait for its last
       consumer value. Acquire=0 omits this stream, never the whole VR frame. */
    LabU32 gpuAbiVersion;
    LabU32 structSize;
    void* callbackContext;
    LabAcquireGpuPublicationFnV1 Acquire;
    LabReleaseGpuPublicationFnV1 Release;
    LabU32 domainId;
    /* Flags: REBASED_MATERIALS means streamMaterials already includes activation.materialBase.
       Zero retains the original package-local material summary contract. */
    LabU32 reserved;
    const LabU32* streamMaterials;
    LabU32 streamMaterialCount;
} LabGpuPublicationProviderV1;

typedef struct LabGpuServiceV1 {
    LabU32 version;
    LabU32 structSize;
    void* serviceContext;
    /* Provider descriptor is copied by the host. nullptr detaches it. */
    LabU32 (LABOVR_PLUGIN_CALL *SetPublicationProvider)(void* serviceContext, const LabGpuPublicationProviderV1* provider);
    /* Optional V1 tail: copied world-space terminal frame, rendered by Core.
       Empty frame detaches this client's terminal without touching other owners.
       Rejects invalid dimensions, coordinates, count or non-ASCII text. */
    LabU32 (LABOVR_PLUGIN_CALL *PublishTerminal)(void* serviceContext,const LabTerminalFrameV1*);
    /* Optional 1.1.4 tail. Settings are copied; enabled=0 detaches the volume.
       One active volume per owner, rendered before terminals. */
    LabU32 (LABOVR_PLUGIN_CALL *SetFogSettings)(void*,const LabFogSettingsV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetFogInput)(void*,const LabFogInputV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetFogWind)(void*,const LabFogWindV1*);
    /* Optional 1.1.7 tail: owner-scoped GPU population occultation of one light. */
    LabU32 (LABOVR_PLUGIN_CALL *SetPopulationOpticalSettings)(void*,const LabPopulationOpticalSettingsV1*);
    /* Optional 1.1.8 tail. Check structSize before accessing. V1 is unchanged. */
    LabU32 (LABOVR_PLUGIN_CALL *PublishTerminalV2)(void*,const LabTerminalFrameV2*);
    LabU32 (LABOVR_PLUGIN_CALL *HitTestTerminalV2)(void*,const LabTerminalHitQueryV2*,LabTerminalHitV2*);
    LabU32 (LABOVR_PLUGIN_CALL *RequestCapture)(void*,const LabCaptureRequestV1*,LabU64*);
    LabU32 (LABOVR_PLUGIN_CALL *GetCaptureStatus)(void*,LabCaptureStatusV1*);
    /* Optional 1.1.9 rendering tail; older callers retain their original size. */
    LabU32 (LABOVR_PLUGIN_CALL *SetAntialiasing)(void*,const LabAntialiasingV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetFogTransport)(void*,const LabFogTransportV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetSurfaceSettings)(void*,const LabSurfaceSettingsV1*);
    LabU32 (LABOVR_PLUGIN_CALL *SetMultisampling)(void*,const LabMultisamplingV1*);
} LabGpuServiceV1;
