#ifndef OASIS_GPU_INSTANCE_VISUAL
#define OASIS_GPU_INSTANCE_VISUAL

// Shared by CPU producers and HLSL consumers of GpuInstance.
#define OASIS_INSTANCE_COCKPIT_DISPLAY (1u << 14u)
#define OASIS_INSTANCE_SPEED_DUST      (1u << 15u)
#define OASIS_RESOURCE_ABI_VERSION    7u

#define OASIS_INSTANCE_BUILDING (1u << 18u)
#define OASIS_INSTANCE_FOREST (1u << 20u)
// A positive geometric contract, independent of campus/forest ownership.
#define OASIS_INSTANCE_UNIT_BOX (1u << 21u)
#define OASIS_INSTANCE_FOREST_INSECTS (1u << 22u)
#define OASIS_MICROFAUNA_COUNT 255u
#define OASIS_MICROFAUNA_MESHLET_SIZE 21u
#define OASIS_MICROFAUNA_SWARM (1u << 31u)
#define OASIS_LIGHT_BUILDING 8u
#endif
