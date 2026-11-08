#pragma once
#include <cuda_runtime.h>
#include <cstdint>

namespace lab {
// Independently simulated GPU micro-particles. They do not replace PhysX rigid
// bodies, the existing 500 flies, or the atmospheric rendering pipeline.
inline constexpr std::uint32_t GpuParticleFieldCount=2'000'000u;
struct alignas(16) GpuParticleFieldState {
    float4 position;  // xyz in metres, w = deterministic phase
    float4 velocity;  // xyz in metres/second, w = per-particle wind factor
};
static_assert(sizeof(GpuParticleFieldState)==32);
void GpuParticleFieldInitialize(cudaStream_t,GpuParticleFieldState*,std::uint32_t,std::uint32_t);
void GpuParticleFieldAdvance(cudaStream_t,GpuParticleFieldState*,std::uint32_t,float,float);
}
