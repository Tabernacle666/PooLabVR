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
// Bark UV0 is authored in local metres and follows uniform instance scale.
#define OASIS_INSTANCE_METRIC_BARK    (1u << 23u)
// LOD-00 diagnostic flag is added only to floor/understory instances at install.
#define OASIS_INSTANCE_LOD_VEGETATION (1u << 24u)
#define OASIS_LOD_DIAGNOSTIC_MASK 16u
#define OASIS_PRISM_CATEGORY(flags) (((flags)&OASIS_INSTANCE_FOREST)!=0u?((((flags)&(OASIS_INSTANCE_LOD_VEGETATION|OASIS_INSTANCE_FOREST_INSECTS))!=0u)?8u:1u):(((flags)&OASIS_INSTANCE_BUILDING)!=0u?2u:4u))
#define OASIS_PRISM_VISIBLE(mask,category) ((mask)==0u||(((mask)&OASIS_LOD_DIAGNOSTIC_MASK)!=0u?((mask)&(category))!=0u:((mask)&((category)==8u?1u:(category)))!=0u))
#define OASIS_LIGHT_BUILDING 8u
#endif
