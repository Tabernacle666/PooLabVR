#pragma once

#include "LaboVRServices.h"
#include "LaboVRWorld.h"

#define LABOVR_AUDIO_SERVICE_VERSION_V1 1u
#define LABOVR_AUDIO_EVENT_DETAILED_V1 1u
#define LABOVR_AUDIO_EMITTER_DIFFUSE_V1 1u

/* Stable wave IDs for both one-shot events and continuous emitters. Their
   numeric values and the V1 table/layout remain compatible with 1.1.0. */
enum LabAudioWaveV1 {
    LABOVR_AUDIO_WAVE_BUZZ_V1=0u,
    LABOVR_AUDIO_WAVE_SINE_V1=1u,
    LABOVR_AUDIO_WAVE_NOISE_V1=2u,
    LABOVR_AUDIO_WAVE_FIRE_V1=3u
};

typedef struct LabAudioEventV1 {
    LabU32 kind;
    LabU32 flags;
    LabU64 frameId;
    float position[3];
    float gain;
} LabAudioEventV1;

typedef struct LabAudioSoundEventV1 {
    LabU32 structSize;
    LabU32 kind;
    LabU64 frameId;
    float position[3];
    float gain;
    float frequency;
    float decay;
    LabU32 wave;
    LabU32 flags;
} LabAudioSoundEventV1;

typedef struct LabAudioEventBatchV1 {
    LabU32 version;
    LabU32 structSize;
    const LabAudioEventV1* events;
    LabU32 eventCount;
    LabU32 reserved;
    const LabAudioSoundEventV1* detailedEvents;
    LabU32 detailedEventCount;
    LabU32 reserved2;
} LabAudioEventBatchV1;

typedef struct LabAudioEmitterV1 {
    LabU32 structSize;
    LabU32 id;
    LabU32 wave;
    LabU32 flags;
    float position[3];
    float velocity[3];
    float gain;
    float frequency;
    float occlusion;
} LabAudioEmitterV1;

typedef struct LabAudioEmitterBatchV1 {
    LabU32 version;
    LabU32 structSize;
    LabU64 frameId;
    const LabAudioEmitterV1* emitters;
    LabU32 emitterCount;
    LabU32 reserved;
} LabAudioEmitterBatchV1;

typedef struct LabAcousticBoxV1 {LabPoseV1 pose;LabVec3V1 halfExtent;} LabAcousticBoxV1;

typedef struct LabAudioServiceV1 {
    LabU32 version;
    LabU32 structSize;
    void* serviceContext;
    /* Event/batch storage is borrowed only for the duration of each call. */
    LabU32 (LABOVR_PLUGIN_CALL *PublishEvents)(void* serviceContext, const LabAudioEventBatchV1* batch);
    LabU32 (LABOVR_PLUGIN_CALL *PublishEmitters)(void* serviceContext, const LabAudioEmitterBatchV1* batch);
    /* Optional 1.1.5 static acoustic geometry. Small authored batches only;
       no population positions. Copied and merged with the loaded world scene. */
    LabU32 (LABOVR_PLUGIN_CALL *SetAcousticBoxes)(void*,const LabAcousticBoxV1*,LabU32);
} LabAudioServiceV1;
