#ifndef OASIS_GPU_INSTANCE_VISUAL
#define OASIS_GPU_INSTANCE_VISUAL

// Shared by CPU producers and HLSL consumers of GpuInstance.
#define OASIS_INSTANCE_COCKPIT_DISPLAY (1u << 14u)
#define OASIS_INSTANCE_SPEED_DUST      (1u << 15u)
#define OASIS_INSTANCE_LUNAR_ACTOR     (1u << 17u)
#define OASIS_RESOURCE_ABI_VERSION    2u

#define OASIS_INSTANCE_BUILDING (1u << 18u)
#define OASIS_INSTANCE_FOREST (1u << 20u)
#define OASIS_LIGHT_BUILDING 8u
#endif
