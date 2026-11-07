#include "PhysicsDiagnostics.h"
#include "SessionLog.h"

namespace lab {
void LogPhysicsProfileCpu(const PhysicsDiagnosticProfile& p,float fixedStepSeconds,
                          uint32_t maxSubsteps,double droppedSeconds){
        const double invFrames=1./double(p.frames),invSteps=p.steps?1./double(p.steps):0.;
        LogEvent(0,"physics.profile.cpu",
            "frames=%llu steps=%llu steps_per_frame=%.3f fixed_dt_ms=%.3f max_substeps=%u dropped_frames=%llu dropped_s=%.6f frame_avg_ms=%.3f step_avg_ms=%.3f step_peak_ms=%.3f lock_avg_ms=%.3f",
            static_cast<unsigned long long>(p.frames),static_cast<unsigned long long>(p.steps),
            double(p.steps)*invFrames,double(fixedStepSeconds)*1000.,maxSubsteps,static_cast<unsigned long long>(p.droppedFrames),
            droppedSeconds,p.frameCpuMs*invFrames,p.stepCpuMs*invSteps,p.maxStepCpuMs,p.contextLockMs*invFrames);
        LogEvent(0,"physics.profile.wait",
            "fetch_blocked_steps=%llu fetch_blocked_total_ms=%.3f fetch_ready_total_ms=%.3f command_wait_total_ms=%.3f topology_wait_total_ms=%.3f check_avg_ms=%.4f first_count=%llu first_avg_ms=%.3f first_peak_ms=%.3f later_count=%llu later_avg_ms=%.3f later_peak_ms=%.3f fetch_lt_0_5ms=%llu fetch_0_5_to_1ms=%llu fetch_1_to_2ms=%llu fetch_2_to_4ms=%llu fetch_ge_4ms=%llu",
            static_cast<unsigned long long>(p.fetchBlocked),p.fetchBlockedMs,p.fetchReadyMs,p.commandWaitMs,p.topologyWaitMs,p.checkResultsMs*invSteps,
            static_cast<unsigned long long>(p.fetchByOrdinal[0]),p.fetchByOrdinal[0]?p.fetchSumByOrdinalMs[0]/double(p.fetchByOrdinal[0]):0.,p.fetchPeakByOrdinalMs[0],
            static_cast<unsigned long long>(p.fetchByOrdinal[1]),p.fetchByOrdinal[1]?p.fetchSumByOrdinalMs[1]/double(p.fetchByOrdinal[1]):0.,p.fetchPeakByOrdinalMs[1],
            static_cast<unsigned long long>(p.fetchBuckets[0]),static_cast<unsigned long long>(p.fetchBuckets[1]),static_cast<unsigned long long>(p.fetchBuckets[2]),static_cast<unsigned long long>(p.fetchBuckets[3]),static_cast<unsigned long long>(p.fetchBuckets[4]));
        LogEvent(0,"physics.profile.submit",
            "simulate_avg_ms=%.3f population_avg_ms=%.3f post_fetch_avg_ms=%.3f",
            p.simulateSubmitMs*invSteps,p.populationSubmitMs*invSteps,p.postFetchCpuMs*invSteps);
        LogEvent(0,"physics.profile.motor",
            "writes=%llu unchanged_skips=%llu total_considered=%llu",
            static_cast<unsigned long long>(p.motorWrites),
            static_cast<unsigned long long>(p.motorUnchangedSkips),
            static_cast<unsigned long long>(p.motorWrites+p.motorUnchangedSkips));
}
void LogPhysicsProfileSolver(const PhysicsDiagnosticProfile& p,uint32_t positionIterations,
                             uint32_t velocityIterations,uint32_t dynamicCount,
                             uint32_t activeJoints,bool sleepingDisabled){
        LogEvent(0,"physics.profile.solver",
            "position_iterations=%u velocity_iterations=%u dynamic_bodies=%u joint_count=%u sleeping_disabled=%u",
            unsigned(positionIterations),unsigned(velocityIterations),dynamicCount,
            activeJoints,unsigned(sleepingDisabled));
        const uint64_t forceSteps=p.deviceZeroSteps+p.hostUploadSteps;
        LogEvent(0,"physics.profile.transfer",
            "device_zero_steps=%llu host_upload_steps=%llu active_force_steps=%llu force_init_submit_avg_ms=%.4f",
            static_cast<unsigned long long>(p.deviceZeroSteps),
            static_cast<unsigned long long>(p.hostUploadSteps),
            static_cast<unsigned long long>(forceSteps),
            forceSteps?p.forceInitSubmitMs/double(forceSteps):0.);
}
void LogPhysicsProfileGpu(const PhysicsDiagnosticProfile& p){
        LogEvent(0,"physics.profile.gpu",
            "step_samples=%llu step_untimed=%llu step_avg_span_ms=%.3f step_peak_span_ms=%.3f frame_samples=%llu frame_avg_span_ms=%.3f frame_peak_span_ms=%.3f",
            static_cast<unsigned long long>(p.gpuStepSamples),
            static_cast<unsigned long long>(p.untimedGpuSteps),
            p.gpuStepSamples?p.gpuStepSumMs/double(p.gpuStepSamples):0.,
            p.gpuStepPeakMs,
            static_cast<unsigned long long>(p.gpuFrameSamples),
            p.gpuFrameSamples?p.gpuFrameSumMs/double(p.gpuFrameSamples):0.,
            p.gpuFramePeakMs);
}
}
