#pragma once
#include <cstdint>

namespace lab {
// Compact per-scene counters; updates remain at the original measurement sites.
struct PhysicsDiagnosticProfile {
    uint64_t frames{},steps{},fetchBlocked{},droppedFrames{};
    uint64_t deviceZeroSteps{},hostUploadSteps{};
    uint64_t gpuStepSamples{},gpuFrameSamples{},untimedGpuSteps{};
    uint64_t motorWrites{},motorUnchangedSkips{};
    uint64_t fetchByOrdinal[2]{}, fetchBuckets[5]{};
    double fetchSumByOrdinalMs[2]{}, fetchPeakByOrdinalMs[2]{}, checkResultsMs{};
    double gpuStepSumMs{},gpuStepPeakMs{},gpuFrameSumMs{},gpuFramePeakMs{};
    double frameCpuMs{},stepCpuMs{},maxStepCpuMs{},contextLockMs{};
    double commandWaitMs{},fetchBlockedMs{},fetchReadyMs{},topologyWaitMs{};
    double simulateSubmitMs{},populationSubmitMs{},postFetchCpuMs{},forceInitSubmitMs{};
};

void LogPhysicsProfileCpu(const PhysicsDiagnosticProfile& profile,float fixedStepSeconds,
                          uint32_t maxSubsteps,double droppedSeconds);
void LogPhysicsProfileSolver(const PhysicsDiagnosticProfile& profile,uint32_t positionIterations,
                             uint32_t velocityIterations,uint32_t dynamicCount,
                             uint32_t activeJoints,bool sleepingDisabled);
void LogPhysicsProfileGpu(const PhysicsDiagnosticProfile& profile);
}
