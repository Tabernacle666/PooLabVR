#include "PhysicsRuntimeInternal.h"

namespace lab {
uint32_t PhysicsRuntime::Impl::Scene::ConfigurePopulation(const LabPopulationDescV1& d){
    if(desc.maxBodies>LAB_PHYSICS_POPULATION_COLLIDERS_V1||insectCapacity||d.structSize<sizeof(d)||d.layer==0||d.collisionMask==0||d.capacity<1||d.capacity>LAB_PHYSICS_POPULATION_MAX_V1||d.initialCount>d.capacity||!Valid(d.spawnCenter)||!Valid(d.spawnHalfExtent)||d.spawnHalfExtent.x<0||d.spawnHalfExtent.y<0||d.spawnHalfExtent.z<0||!ValidFlight(d.flight))return 0;
    for(auto mesh:{d.proxyMesh,d.bodyMesh,d.headMesh,d.abdomenMesh,d.leftWingMesh,d.rightWingMesh})if(mesh>=ConsoleSpec::MaxMeshes)return 0;
    for(auto mat:{d.bodyMaterial,d.headMaterial,d.wingMaterial})if(mat>=ConsoleSpec::MaxMaterials)return 0;
    PxScopedCudaLock lock(*runtime.cuda);population=d;populationEffects.fleeSeconds=d.flight.impactSeconds;descriptorsDirty=true;populationClockOrigin=lastTime;populationClockStarted=lastTime>0;Allocate(populationBodyPoses,desc.maxBodies);Cuda(cudaMemsetAsync(populationBodyPoses,0,desc.maxBodies*sizeof(PopulationBodyPoseGpu),stream),"initialize population collider history");Allocate(populationGrid,PopulationGridWords);insectCapacity=d.capacity;Allocate(insects,insectCapacity);insectCount=d.initialCount;
    if(descriptorsDirty)RefreshDescriptors();PopulationInitialize(stream,insects,0,insectCount,population,++birthGeneration,populationBodies,uint32_t(populationBodyHost.size()),started?current:nullptr);Cuda(cudaGetLastError(),"initialize GPU population");
    Allocate(particleField,GpuParticleFieldCount);
    GpuParticleFieldInitialize(stream,particleField,GpuParticleFieldCount,d.flight.seed);
    Cuda(cudaGetLastError(),"initialize 2000000 CUDA particles");
    LogEvent(0,"physics.particles","count=%u bytes=%llu backend=cuda integration_hz=120 render_instances=0 existing_flies=%u",GpuParticleFieldCount,static_cast<unsigned long long>(uint64_t(GpuParticleFieldCount)*sizeof(GpuParticleFieldState)),insectCount);
    auto v=visuals;auto l=lights;SetVisuals(v.data(),uint32_t(v.size()),l.data(),uint32_t(l.size()));return 1;
}
uint32_t PhysicsRuntime::Impl::Scene::ResizePopulation(uint32_t n){
    if(!insectCapacity||n>LAB_PHYSICS_POPULATION_MAX_V1)return 0;PxScopedCudaLock lock(*runtime.cuda);
    if(n>insectCapacity){uint32_t capacity=insectCapacity;while(capacity<n)capacity=std::min(LAB_PHYSICS_POPULATION_MAX_V1,capacity*2);
        PopulationStateGpu* next{};Allocate(next,capacity);Cuda(cudaMemcpyAsync(next,insects,insectCount*sizeof(PopulationStateGpu),cudaMemcpyDeviceToDevice,stream),"grow GPU population preserving survivors");Cuda(cudaStreamSynchronize(stream),"retire population capacity");cudaFree(insects);insects=next;insectCapacity=capacity;
        auto v=visuals;auto l=lights;SetVisuals(v.data(),uint32_t(v.size()),l.data(),uint32_t(l.size()));
    }
    if(n>insectCount){if(descriptorsDirty)RefreshDescriptors();PopulationInitialize(stream,insects,insectCount,n-insectCount,population,++birthGeneration,populationBodies,uint32_t(populationBodyHost.size()),started?current:nullptr);Cuda(cudaGetLastError(),"append GPU insect batch");}
    insectCount=n;return 1;
}
void PhysicsRuntime::Impl::Scene::StepPopulation(){
    const auto policy=PopulationPolicy(insectCapacity!=0,populationInput.enabled!=0,visible);
    if(!policy.simulate)return;
    insectAccumulator+=desc.fixedStepSeconds;
    constexpr double step=1./120.;
    while(insectAccumulator+1e-9>=step){insectAccumulator-=step;
    Cuda(cudaMemsetAsync(populationGrid,0,PopulationGridWords*sizeof(uint32_t),stream),"clear population broadphase");
    PopulationStep(stream,insects,insectCount,population,populationInput,populationEffects,populationHands,populationBodies,populationBodyPoses,populationGrid,uint32_t(populationBodyHost.size()),previous,current,velocity,angular,forces,torques,lightDevice,uint32_t(lightHost.size()),airflowDevice,uint32_t(airflowHost.size()),populationSpace,float(step),desc.fixedStepSeconds,simulationTime,float(populationSeconds),policy.observerPresent);Cuda(cudaGetLastError(),"advance compact GPU flight and contacts");
    if(particleField){
        auto* sample=BeginTiming(particleTimings,particleTimingCursor,tick+1);
        GpuParticleFieldAdvance(stream,particleField,GpuParticleFieldCount,float(step),float(populationSeconds));
        Cuda(cudaGetLastError(),"advance 2000000 CUDA particles");
        if(sample)Cuda(cudaEventRecord(sample->end,stream),"finish particle GPU timing");
    }}
}
void PhysicsRuntime::Impl::Scene::QueuePopulationReadback(){
    auto& r=populationReadbacks[populationReadCursor];if(r.pending){auto ready=cudaEventQuery(r.done);if(ready==cudaErrorNotReady)return;Cuda(ready,"population readback reuse");}
    Cuda(cudaMemcpyAsync(r.host,&populationSummary->snapshot,sizeof(LabPopulationSnapshotV1),cudaMemcpyDeviceToHost,stream),"copy bounded population diagnostics and voices");Cuda(cudaEventRecord(r.done,stream),"complete population readback");r.pending=true;r.sequence=++populationReadSequence;populationReadCursor=(populationReadCursor+1)%uint32_t(populationReadbacks.size());
}
uint32_t PhysicsRuntime::Impl::Scene::PollPopulation(LabPopulationSnapshotV1* out){
    if(!out||out->structSize<sizeof(*out))return 0;PxScopedCudaLock lock(*runtime.cuda);PopulationReadback* newest{};
    for(auto& r:populationReadbacks)if(r.pending&&r.sequence>populationPolledSequence){auto ready=cudaEventQuery(r.done);if(ready==cudaErrorNotReady)continue;Cuda(ready,"poll GPU population");if(!newest||r.sequence>newest->sequence)newest=&r;}
    if(!newest)return 0;*out=*newest->host;populationPolledSequence=newest->sequence;return 1;
}
} // namespace lab
