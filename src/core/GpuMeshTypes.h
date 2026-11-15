#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>
namespace OASIS {
struct GpuFloat4 {float x,y,z,w;};


struct GpuInstance {
    GpuFloat4 positionRadius,rotation,scale;
    uint32_t mesh,material,id,flags;
};
static_assert(std::is_standard_layout_v<GpuInstance>&&std::is_trivially_copyable_v<GpuInstance>);
static_assert(sizeof(GpuInstance)==64);
static_assert(offsetof(GpuInstance,positionRadius)==0&&offsetof(GpuInstance,rotation)==16&&offsetof(GpuInstance,scale)==32);
static_assert(offsetof(GpuInstance,mesh)==48&&offsetof(GpuInstance,material)==52&&offsetof(GpuInstance,id)==56&&offsetof(GpuInstance,flags)==60);
}
