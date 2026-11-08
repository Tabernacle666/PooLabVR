#include "PhysicsRuntimeInternal.h"

namespace lab {

uint32_t PhysicsRuntime::Impl::Scene::ConfigureSnowfall(uint32_t seed){
    // CreateSnowfall is independent of insect population and its flight seed.
    // Visual publications must not already exist: their resource references
    // are assembled after the weather field is configured.
    if(snowfallReady||!slots[0].refs.empty())return 0;
    PxScopedCudaLock lock(*runtime.cuda);
    constexpr uint64_t bytes=uint64_t(GpuParticleFieldCount)*sizeof(GpuParticleFieldState);
    for(auto& state:snowfallStates)state.Create(owner.interop,bytes);
    snowfallCurrentSlot=0;
    snowfallPendingSeconds=0.;
    snowfallTimeSeconds=0.;
    GpuParticleFieldInitialize(stream,
        static_cast<GpuParticleFieldState*>(snowfallStates[0].pointer),
        GpuParticleFieldCount,seed);
    Cuda(cudaGetLastError(),"initialize shared CUDA weather snowfall");
    snowfallReady=true;
    LogEvent(0,"physics.particles",
        "count=%u bytes=%llu backend=cuda update_clock=display_frame visible_snow=1 storage=triple_shared state_copy_per_frame=0 independent_of_insects=1 seed=%u",
        GpuParticleFieldCount,static_cast<unsigned long long>(bytes),seed);
    return 1;
}

} // namespace lab
