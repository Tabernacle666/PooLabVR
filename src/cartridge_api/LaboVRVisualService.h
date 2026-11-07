#pragma once
#include "LaboVRServices.h"
#include "LaboVRWorld.h"

#define LABOVR_VISUAL_SERVICE_VERSION_V1 1u
#define LABOVR_VISUAL_PACKAGE_VERSION_V1 1u
#define LABOVR_VISUAL_BATCH_VERSION_V1 1u
#define LABOVR_VISUAL_INVALID_ID_V1 0xffffffffu

#define LABOVR_VISUAL_MAX_TEXTURES_V1 1024u
#define LABOVR_VISUAL_MAX_MATERIALS_V1 1024u
#define LABOVR_VISUAL_MAX_MESHES_V1 4096u
#define LABOVR_VISUAL_MAX_OBJECTS_V1 8192u
#define LABOVR_VISUAL_MAX_LIGHTS_V1 128u
#define LABOVR_VISUAL_MAX_STREAMS_V1 32u
#define LABOVR_VISUAL_MAX_EFFECTS_V1 32u
#define LABOVR_VISUAL_MAX_CAPTURES_V1 2u
#define LABOVR_VISUAL_MAX_DYNAMIC_VERTICES_V1 2000000u
#define LABOVR_VISUAL_MAX_CAPTURE_WIDTH_V1 1024u
#define LABOVR_VISUAL_MAX_CAPTURE_HEIGHT_V1 1024u
#define LABOVR_VISUAL_MAX_TEXTURE_EDGE_V1 8192u
#define LABOVR_VISUAL_MAX_HEIGHT_PARALLAX_STEPS_V1 96u
#define LABOVR_VISUAL_MAX_HEIGHT_TESSELLATION_V1 32u
#define LABOVR_VISUAL_FRAME_SLOTS_V1 3u
#define LABOVR_VISUAL_MAX_VIEWS_V1 4u
#define LABOVR_VISUAL_MAX_STATIC_GPU_BYTES_V1 (4ull*1024ull*1024ull*1024ull)
#define LABOVR_VISUAL_MAX_DYNAMIC_GPU_BYTES_V1 (512ull*1024ull*1024ull)


enum LabVisualTextureUseV1 {
    LABOVR_VISUAL_TEXTURE_COLOR_V1,
    LABOVR_VISUAL_TEXTURE_NORMAL_V1,
    LABOVR_VISUAL_TEXTURE_ORM_V1,
    LABOVR_VISUAL_TEXTURE_HEIGHT_V1,
    LABOVR_VISUAL_TEXTURE_DETAIL_V1,
    LABOVR_VISUAL_TEXTURE_LINEAR_V1
};

enum LabVisualMeshSourceV1 {
    LABOVR_VISUAL_MESH_FILE_V1,
    LABOVR_VISUAL_MESH_BOX_V1,
    LABOVR_VISUAL_MESH_SPHERE_V1,
    LABOVR_VISUAL_MESH_CYLINDER_V1,
    LABOVR_VISUAL_MESH_MEMORY_V1
};

enum LabVisualMaterialModelV1 {
    LABOVR_VISUAL_MATERIAL_PBR_V1,
    LABOVR_VISUAL_MATERIAL_HEIGHT_PBR_V1,
    LABOVR_VISUAL_MATERIAL_REFRACTIVE_GEL_V1,
    LABOVR_VISUAL_MATERIAL_VERTEX_COLOR_V1,
    LABOVR_VISUAL_MATERIAL_EMISSIVE_V1
};

enum LabVisualMaterialFlagsV1 {
    LABOVR_VISUAL_MATERIAL_OVERLAY_V1 = 1u<<0,
    LABOVR_VISUAL_MATERIAL_ALPHA_V1 = 1u<<1,
    LABOVR_VISUAL_MATERIAL_UNLIT_V1 = 1u<<2
};
/* Upper 16 flag bits carry the normalized metallic factor when no ORM texture exists. */
#define LABOVR_VISUAL_MATERIAL_METALLIC_SHIFT_V1 16u

enum LabVisualStreamPrimitiveV1 {
    LABOVR_VISUAL_STREAM_TRIANGLES_V1,
    LABOVR_VISUAL_STREAM_LINES_V1,
    LABOVR_VISUAL_STREAM_POINTS_V1
};

enum LabVisualSortModeV1 {
    LABOVR_VISUAL_SORT_NONE_V1,
    LABOVR_VISUAL_SORT_BACK_TO_FRONT_V1
};

enum LabVisualVisibilityV1 {
    LABOVR_VISUAL_MAIN_V1=1u<<0,
    LABOVR_VISUAL_CAPTURE_V1=1u<<1
};

enum LabVisualEffectTypeV1 {
    LABOVR_VISUAL_EFFECT_COSMOS_V1,
    LABOVR_VISUAL_EFFECT_LIQUID_ORB_V1,
    LABOVR_VISUAL_EFFECT_BLOOM_V1
};

enum LabVisualDeformModeV1 {
    LABOVR_VISUAL_DEFORM_NONE_V1,
    LABOVR_VISUAL_DEFORM_ORGANIC_V1
};

typedef struct LabVisualTextureV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 use;
    LabU32 reserved;
    const char* pathUtf8;
} LabVisualTextureV1;

typedef struct LabVisualMeshVertexV1 {
    LabVec3V1 position;
    LabVec3V1 normal;
    LabVec3V1 tangent;
    float uv[2];
} LabVisualMeshVertexV1;

/* Borrowed until Core has consumed the declaration; Core copies the arrays. */
typedef struct LabVisualMeshDataV1 {
    LabU32 structSize;
    LabU32 vertexCount;
    const LabVisualMeshVertexV1* vertices;
    LabU32 indexCount;
    const LabU32* indices;
} LabVisualMeshDataV1;

typedef struct LabVisualMeshV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 source;
    LabU32 primitive;
    union {
        const char* pathUtf8;
        const LabVisualMeshDataV1* data;
    };
} LabVisualMeshV1;

typedef struct LabVisualMaterialV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 model;
    LabU32 flags;

    LabU32 albedoTexture;
    LabU32 normalTexture;
    LabU32 ormTexture;
    LabU32 heightTexture;
    LabU32 detailTexture;

    float tint[4];

    float uvScale;
    float normalScale;
    float roughnessBias;
    float heightScale;

    float detailScale;
    float detailNormalScale;
    float wetness;
    float emission;

    float refractionStrength;
    float thickness;
    float tessFactor;
    float reserved0;

    LabU32 parallaxMinSteps;
    LabU32 parallaxMaxSteps;
    LabU32 reserved1;
    LabU32 reserved2;
} LabVisualMaterialV1;

enum LabVisualObjectUpdateV1 {
    LABOVR_VISUAL_OBJECT_STATIC_V1=0,
    LABOVR_VISUAL_OBJECT_TRANSFORM_V1=1u<<0,
    LABOVR_VISUAL_OBJECT_SCALE_V1=1u<<1,
    LABOVR_VISUAL_OBJECT_VISIBILITY_V1=1u<<2
};
typedef struct LabVisualObjectV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 mesh;
    LabU32 material;
    LabU32 visibilityMask;
    LabU32 deformMode;

    LabPoseV1 pose;
    LabVec3V1 scale;

    float deformAmplitude;
    float deformFrequency;
    float deformPhase;
    float emissionScale;
    LabU32 updateMask;
    LabU32 reservedUpdate[3];
} LabVisualObjectV1;

typedef struct LabVisualStreamVertexV1 {
    float position[3];
    float color[4];
} LabVisualStreamVertexV1;

typedef struct LabVisualStreamV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 primitive;
    LabU32 material;
    LabU32 capacity;
    LabU32 visibilityMask;
    LabU32 sortMode;
    float pointSizePixels;
} LabVisualStreamV1;

typedef struct LabVisualEffectV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 type;
    LabU32 visibilityMask;
    float values[8];
} LabVisualEffectV1;

typedef struct LabVisualCaptureV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 width;
    LabU32 height;
} LabVisualCaptureV1;

typedef struct LabVisualObjectStateV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 visible;
    LabU32 reserved;
    LabPoseV1 previous;
    LabPoseV1 current;
    LabVec3V1 scale;
} LabVisualObjectStateV1;

typedef struct LabVisualStreamFrameV1 {
    LabU32 structSize;
    LabU32 stream;
    const LabVisualStreamVertexV1* vertices;
    LabU32 vertexCount;
    float pointSizePixels;
} LabVisualStreamFrameV1;

typedef struct LabVisualEffectStateV1 {
    LabU32 structSize;
    LabU32 effect;
    LabU32 enabled;
    LabU32 reserved;
    LabVec3V1 position;
    float radius;
    float values[8];
} LabVisualEffectStateV1;

typedef struct LabVisualCaptureRequestV1 {
    LabU32 structSize;
    LabU32 capture;
    LabU32 enabled;
    LabU32 reserved;
    LabPoseV1 pose;
    float verticalFov;
    float nearZ;
    float farZ;
} LabVisualCaptureRequestV1;

typedef struct LabVisualLightModifierV1 {
    LabU32 structSize;
    LabU32 matchFlags;
    float powerScale;
    float reserved;
} LabVisualLightModifierV1;

typedef struct LabVisualBatchV1 LabVisualBatchV1;

typedef LabU32
(LABOVR_PLUGIN_CALL *LabBuildVisualBatchFnV1)(
    void* callbackContext,
    const LabWorldFrameV1* frame,
    LabVisualBatchV1* outBatch);

typedef struct LabVisualContributionV1 {
    LabU32 version;
    LabU32 structSize;
    void* callbackContext;
    LabBuildVisualBatchFnV1 BuildBatch;
} LabVisualContributionV1;

enum LabVisualLightFlagsV1 { LABOVR_VISUAL_LIGHT_SHADOW_V1=1u, LABOVR_VISUAL_LIGHT_DIRECTIONAL_V1=2u, LABOVR_VISUAL_LIGHT_GPU_POSE_V1=4u };

typedef struct LabVisualLightV1 {
    LabU32 structSize;
    LabU32 id;
    LabVec3V1 position;
    float radius;
    LabVec3V1 color;
    float power;
    LabVec3V1 direction;
    float outerCos;
    float innerCos;
    LabU32 flags;
} LabVisualLightV1;

typedef struct LabVisualPackageV1 {
    LabU32 version;
    LabU32 structSize;
    const LabVisualTextureV1* textures;
    LabU32 textureCount;
    const LabVisualMeshV1* meshes;
    LabU32 meshCount;
    const LabVisualMaterialV1* materials;
    LabU32 materialCount;
    const LabVisualObjectV1* objects;
    LabU32 objectCount;
    const LabVisualLightV1* lights;
    LabU32 lightCount;
    const LabVisualStreamV1* streams;
    LabU32 streamCount;
    const LabVisualEffectV1* effects;
    LabU32 effectCount;
    const LabVisualCaptureV1* captures;
    LabU32 captureCount;
    const char* externalScenePathUtf8;
} LabVisualPackageV1;

/* BuildBatch arrays are borrowed through the next World Frame() call. */
struct LabVisualBatchV1 {
    LabU32 version;
    LabU32 structSize;
    const LabVisualObjectStateV1* objectStates;
    LabU32 objectStateCount;
    const LabVisualStreamFrameV1* streams;
    LabU32 streamCount;
    const LabVisualEffectStateV1* effects;
    LabU32 effectCount;
    const LabVisualLightModifierV1* lightModifiers;
    LabU32 lightModifierCount;
    const LabVisualCaptureRequestV1* captures;
    LabU32 captureCount;
};

typedef LabU32 LabVisualCaptureHandleV1;
enum LabVisualCaptureStatusV1 {
    LABOVR_VISUAL_CAPTURE_PENDING_V1 = 0,
    LABOVR_VISUAL_CAPTURE_READY_V1 = 1,
    LABOVR_VISUAL_CAPTURE_FAILED_V1 = 2
};
typedef struct LabVisualCaptureResultV1 {
    LabU32 structSize;
    LabU32 status;
    LabU32 width;
    LabU32 height;
    LabU32 rowPitch;
    LabU32 tightRowBytes;
    LabU64 byteCount;
    LabU64 frameId;
    const void* bytes;
} LabVisualCaptureResultV1;

typedef struct LabVisualServiceV1 {
    LabU32 version;
    LabU32 structSize;
    void* serviceContext;
    LabU32 (LABOVR_PLUGIN_CALL *SetContribution)(void*, const LabVisualContributionV1*);
    LabU32 (LABOVR_PLUGIN_CALL *PollCapture)(void*, LabVisualCaptureHandleV1, LabVisualCaptureResultV1*);
    void (LABOVR_PLUGIN_CALL *ReleaseCapture)(void*, LabVisualCaptureHandleV1);
} LabVisualServiceV1;
