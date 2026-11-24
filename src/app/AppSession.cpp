#include "AppSession.h"
#include "SessionLaunch.h"
#include "DiskBake.h"
#include "CampusBake.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "VrInputMapping.h"
#include "OrbitTool.h"
#include "BuildingTour.h"
#include "Mg2Catalog.h"
#include "BuildingCampus.h"
#include "WorldRegistry.h"
#include "DesktopInput.h"
#include "DesktopOutput.h"
#include "SpectacleSpace.h"
#include "SphereCalibration.h"
#include "Feedback.h"
#include "LocalProfile.h"
#include "MaterialProfiles.h"
#include "ShuttlePilot.h"
#include "ShuttleVisuals.h"
#include "SurfaceContact.h"
#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace OASIS {
namespace {
// Only explicit milestones on the render thread extend the MG2 startup deadline.
thread_local std::atomic<ULONGLONG>* mg2StartupProgress{};
}

void MarkDiagnosticStartupProgress() noexcept {
    if(mg2StartupProgress)
        mg2StartupProgress->store(GetTickCount64(),std::memory_order_release);
}

std::wstring diagnosticLogPath;
std::atomic<bool> diagnosticCaptured{};
void DiagnosticLine(const char* text) noexcept {
    if(diagnosticLogPath.empty())return;
    HANDLE file=CreateFileW(diagnosticLogPath.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                            nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD written=0;WriteFile(file,text,DWORD(std::strlen(text)),&written,nullptr);CloseHandle(file);
}
void DiagnosticFailure(const char* text) noexcept {
    char line[1024];std::snprintf(line,sizeof(line),"erreur=%s\r\n",text);DiagnosticLine(line);
    if(!diagnosticCaptured.load(std::memory_order_acquire))DiagnosticLine("capture=aucune\r\n");
}
bool StartDiagnostic(const std::wstring& directory){
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(directory),error);
    if(error)return false;
    const DWORD attributes=GetFileAttributesW(directory.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY))return false;
    const auto path=directory+L"\\OASIS.log";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                            nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    CloseHandle(file);diagnosticLogPath=path;failureSink=DiagnosticFailure;
    DiagnosticLine("OASIS\r\n");
    SYSTEMTIME now{};GetLocalTime(&now);
    char line[160];std::snprintf(line,sizeof(line),"date=%04u-%02u-%02u %02u:%02u:%02u\r\n",unsigned(now.wYear),unsigned(now.wMonth),unsigned(now.wDay),unsigned(now.wHour),unsigned(now.wMinute),unsigned(now.wSecond));DiagnosticLine(line);
    char revision[80]{};const DWORD n=GetEnvironmentVariableA("OASIS_SOURCE_REVISION",revision,DWORD(sizeof(revision)));
    std::snprintf(line,sizeof(line),"revision=%s\r\n",n&&n<sizeof(revision)?revision:"inconnue");DiagnosticLine(line);
    char dirty[8]{};const DWORD d=GetEnvironmentVariableA("OASIS_SOURCE_DIRTY",dirty,DWORD(sizeof(dirty)));
    std::snprintf(line,sizeof(line),"modifications_locales=%s\r\n",d&&d<sizeof(dirty)?dirty:"inconnu");DiagnosticLine(line);
    return true;
}
struct ComScope {
    ComScope(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}
    ~ComScope(){CoUninitialize();}
};
constexpr UINT OutputWindowMessage=WM_APP+1;
enum class LoopResult {Exit,SwitchOutput};
struct NavigationContinuity {Pose pose{};bool valid{},inside{};};

namespace {
// World-space poses fitted to the OASIS loop capture in 3d9fc54.
constexpr XRCaptureView OasisDiagnosticView{
    {{-.00265935f,1.69044050f,.06374757f},{.03100724f,.01551041f,-.00648089f,.99937779f}},
    {-.91241771f,.79412581f,.84459636f,-.87909295f}
};
constexpr Pose OasisDiagnosticLeftGrip{
    {.01152271f,1.49517336f,.49152076f},{-.61197879f,.55731763f,-.47334697f,-.30136633f}
};
constexpr Pose OasisDiagnosticRightGrip{
    {.27379403f,1.46544947f,.58451058f},{-.76814861f,-.07239576f,-.19798771f,.60457210f}
};
constexpr Pose OasisDiagnosticRightAim{
    {.28505885f,1.51523788f,.57460007f},{-.11566606f,-.19118999f,-.08880692f,.97066013f}
};

Pose OasisDiagnosticHead(){
    auto head=OasisDiagnosticView.eye;
    head.p=Transform({.032f,0.f,0.f},head);
    return head;
}

void ApplyOasisDiagnosticControllers(FrameInput& input,const VrInputActions& actions,bool followHead=false){
    auto atHead=[&](const Pose& pose){
        if(!followHead)return pose;
        auto local=FromMatrix(Matrix(pose)*XMMatrixInverse(nullptr,Matrix(OasisDiagnosticHead())));
        local.p=Scale(local.p,input.sceneUnitsPerMeter);
        return FromMatrix(Matrix(local)*Matrix(input.head));
    };
    auto& grips=input.State(actions.handPose);
    grips.active={true,true};
    grips.pose[0]={atHead(OasisDiagnosticLeftGrip),{},{},true};
    grips.pose[1]={atHead(OasisDiagnosticRightGrip),{},{},true};
    auto& aim=input.State(actions.aimPose);
    aim.active[1]=true;
    aim.pose[1]={atHead(OasisDiagnosticRightAim),{},{},true};
}

void WriteOasisCapturePoses(const XRFrame& frame){
    char line[256];
    std::snprintf(line,sizeof(line),"capture_image=%llu poses=reference_estimee\r\n",
        static_cast<unsigned long long>(frame.input.frameId));
    DiagnosticLine(line);
    auto writePose=[&](const char* name,const Pose& pose){
        std::snprintf(line,sizeof(line),"%s position_m_xyz=%.9f,%.9f,%.9f rotation_xyzw=%.9f,%.9f,%.9f,%.9f\r\n",
            name,pose.p.x,pose.p.y,pose.p.z,pose.q.x,pose.q.y,pose.q.z,pose.q.w);
        DiagnosticLine(line);
    };
    const auto& actions=StandardVrActions();
    writePose("manette_gauche",frame.input.PoseState(actions.handPose,0).pose);
    writePose("manette_droite",frame.input.PoseState(actions.handPose,1).pose);
    writePose("visee_droite",frame.input.PoseState(actions.aimPose,1).pose);
    writePose("oeil_gauche",frame.captureOnly?OasisDiagnosticView.eye:Pose{frame.eyes[0],frame.input.head.q});
    const auto& fov=frame.fov[0];
    std::snprintf(line,sizeof(line),"champ_oeil_gauche_rad_gauche_droite_haut_bas=%.9f,%.9f,%.9f,%.9f\r\n",
        fov.angleLeft,fov.angleRight,fov.angleUp,fov.angleDown);
    DiagnosticLine(line);
}
}

struct DiagnosticCapture {
    std::wstring path;
    bool interactive{},requested{},attempted{},captured{},dlssLoopCompleted{},sphereBaselineCompleted{},sunCompleted{},plafondCompleted{};
    int64_t firstFrame{};
    bool mg2{};
    std::optional<Pose> sessionForestSpawn;
    std::future<bool> captureWriter;
    void ResetPending(){if(!attempted){firstFrame=0;requested=false;}}
    void BeforeRender(Graphics& graphics,bool captureNow=false,bool nearWorktop=false){
        const double delay=nearWorktop?2.0:10.0;
        if(path.empty()||attempted||requested||!firstFrame||graphics.CapturePending()||
           (!captureNow&&QpcSeconds(QpcCounter()-firstFrame)<delay))return;
        DiagnosticLine("etape=demande_capture\r\n");graphics.RequestCapture(path);requested=true;
    }
    bool AfterRender(Graphics& graphics,bool submitted,bool desktop,std::atomic<bool>& frameSeen,
                     const XRFrame* captureFrame=nullptr){
        if(submitted)frameSeen.store(true,std::memory_order_release);
        if(path.empty()||attempted)return false;
        if(submitted&&!firstFrame){firstFrame=QpcCounter();DiagnosticLine(desktop?"etape=image_ecran\r\n":"etape=image_vr\r\n");}
        if(!captureWriter.valid()){
            if(!requested||!graphics.CaptureDone())return false;
            DiagnosticLine("etape=capture\r\n");
            if(mg2)captureWriter=graphics.FinishCaptureAsync();
            if(!captureWriter.valid()){
                if(!graphics.CaptureFailed())graphics.WaitIdle();
                captured=graphics.SaveCapture();
            }
        }
        if(captureWriter.valid()){
            if(captureWriter.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
            try{captured=captureWriter.get();}catch(...){captured=false;}
        }
        attempted=true;
        diagnosticCaptured.store(captured,std::memory_order_release);
        const auto metrics=graphics.GetMetrics();char line[320];
        if(metrics.fps>0)std::snprintf(line,sizeof(line),"fps_soumis=%.2f\r\nintervalle_image_ms=%.2f\r\n",metrics.fps,1000.0/double(metrics.fps));
        else std::snprintf(line,sizeof(line),"fps_soumis=indisponible\r\nintervalle_image_ms=indisponible\r\n");
        DiagnosticLine(line);
        constexpr double gibibyte=1024.0*1024.0*1024.0;
        if(metrics.vramBudget)std::snprintf(line,sizeof(line),"vram_gio=%.2f / %.2f\r\n",double(metrics.vramUsed)/gibibyte,double(metrics.vramBudget)/gibibyte);
        else std::snprintf(line,sizeof(line),"vram_gio=indisponible\r\n");
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),"vue=%s\r\nresolution=%u x %u\r\ncapture=%s\r\n",desktop?"ecran":"oeil_gauche",metrics.width,metrics.height,captured?"OASIS.png":"aucune");
        DiagnosticLine(line);
        if(captured&&captureFrame)WriteOasisCapturePoses(*captureFrame);
        return !interactive;
    }
};

struct DlssDiagnosticCapture {
    enum class Phase {Before,Active,After};
    enum Stage : size_t {Wait,Begin,Prepare,Acquire,Render,End,Update,StageCount};
    struct Sample {
        std::array<double,StageCount> ms{};
        bool wanted{},prepared{},submitted{};
    };
    bool enabled{};
    Phase phase{Phase::Before};
    std::array<Sample,12> window{};
    size_t count{},cursor{};
    int64_t phaseStart{};
    uint64_t firstFrame{1};
    double firstWaitMs{-1.0};
    bool reported{};

    int64_t Stamp()const{return enabled?QpcCounter():0;}
    double Elapsed(int64_t start)const{return start?1000.0*QpcSeconds(QpcCounter()-start):0.0;}

    void Write(const DlssDiagnosticState& state){
        if(!count||reported)return;
        const char* name=phase==Phase::Before?"natif_avant":
            phase==Phase::Active?"dlss_actif":"natif_apres_off";
        std::array<double,StageCount> mean{};
        double maxUpdate=0.0;
        unsigned wanted=0,prepared=0,submitted=0;
        for(size_t i=0;i<count;++i){
            const auto& sample=window[i];
            for(size_t stage=0;stage<StageCount;++stage)mean[stage]+=sample.ms[stage];
            maxUpdate=std::max(maxUpdate,sample.ms[Update]);
            wanted+=sample.wanted;
            prepared+=sample.prepared;
            submitted+=sample.submitted;
        }
        for(auto& value:mean)value/=double(count);
        const bool gpuSample=state.gpuFrame>=firstFrame;
        char line[1200];
        std::snprintf(line,sizeof(line), "dlss_phase=%s fenetre=%zu demande=%u preparees=%u soumises=%u "
            "xr_attente_ms=%.2f xr_premiere_attente_ms=%.2f xr_debut_ms=%.2f "
            "preparation_ms=%.2f xr_images_ms=%.2f "
            "rendu_cpu_ms=%.2f xr_fin_ms=%.2f option_max_ms=%.2f "
            "gpu_direct_ms=%.2f gpu_compute_ms=%.2f ngx_gpu_ms=%.2f ngx_cpu_ms=%.2f "
            "gpu_image=%llu gpu_direct_pic_ms=%.2f gpu_direct_lot=\"%s\" "
            "gpu_compute_pic_ms=%.2f gpu_compute_lot=\"%s\" "
            "echelle_gpu=%.3f echelle_demandee=%.3f ngx_initialise=%u ngx_fence_attente=%u "
            "ngx_release=0x%08x ngx_shutdown=0x%08x\r\n",
            name,count,wanted,prepared,submitted,mean[Wait],firstWaitMs,mean[Begin],mean[Prepare],mean[Acquire],
            mean[Render],mean[End],maxUpdate,gpuSample?state.directMs:-1.f,
            gpuSample?state.computeMs:-1.f,gpuSample?state.dlssGpuMs:-1.f,
            gpuSample?state.dlssCpuMs:-1.f,static_cast<unsigned long long>(gpuSample?state.gpuFrame:0),
            gpuSample?state.directPeakMs:-1.f,gpuSample?state.directPeakBatch.data():"-",
            gpuSample?state.computePeakMs:-1.f,gpuSample?state.computePeakBatch.data():"-",
            gpuSample?state.gpuScale:-1.f,state.requestedScale,
            unsigned(state.initialized),unsigned(state.fencePending),unsigned(state.releaseResult),
            unsigned(state.shutdownResult));
        DiagnosticLine(line);
        reported=true;
    }

    void Record(const Sample& sample,const DlssDiagnosticState& before,
                const DlssDiagnosticState& after,uint64_t frame){
        if(!count&&phase!=Phase::Before)firstWaitMs=sample.ms[Wait];
        window[cursor]=sample;
        cursor=(cursor+1)%window.size();
        count=std::min(count+1,window.size());
        const bool active=after.requestedScale>0.f;
        const auto now=QpcCounter();
        const double seconds=phaseStart?QpcSeconds(now-phaseStart):0.0;
        const bool enoughTime=seconds>=2.5||
            (seconds>=1.0&&count>=8&&after.gpuFrame>=firstFrame);
        if(phase==Phase::After){
            if(active||seconds>=2.5||(enoughTime&&!after.initialized)){
                Write(active?before:after);
                enabled=false;
            }
            return;
        }
        if(phase==Phase::Active&&active&&enoughTime)Write(after);
        if((phase==Phase::Before&&active)||(phase==Phase::Active&&!active)){
            Write(before);
            phase=active?Phase::Active:Phase::After;
            phaseStart=now;
            firstFrame=frame+1u;
            count=cursor=0;
            reported=false;
        }
    }

    void Finish(const DlssDiagnosticState& state,bool includeBefore){
        if(enabled&&(includeBefore||phase!=Phase::Before))Write(state);
    }
};

enum class SessionStage { Loop,Begin,Input,Prepare,Acquire,Render,Submit,Options,Capture,Shutdown };
const char* SessionStageName(SessionStage stage){
    constexpr const char* names[]{"boucle","xr_debut","entrees","preparation","xr_images",
        "rendu","xr_soumission","options","capture","fermeture"};
    return names[size_t(stage)];
}
std::atomic<SessionStage> sessionStage{SessionStage::Loop};
std::atomic<ULONGLONG> sessionProgress{};
std::atomic<uint64_t> sessionFrame{};
void MarkSession(SessionStage stage){
    sessionStage.store(stage,std::memory_order_relaxed);
    sessionProgress.store(GetTickCount64(),std::memory_order_release);
}

struct SessionTrace {
    ULONGLONG started{},window{};
    int64_t stallOriginQpc{},lastFrameQpc{},lastStallQpc{};
    uint32_t lastStallPublications{};
    std::string stallEvents;
    unsigned frames{},wanted{},prepared{},submitted{};
    std::array<double,DlssDiagnosticCapture::StageCount> peaks{};
    bool opticalConfigLogged{},depthEnabled{};
    std::optional<Graphics::DepthMode> lastDepthMode;
    void LogDepthMode(Graphics& graphics){
        const auto active=graphics.ActiveDepthMode();
        const auto name=[](Graphics::DepthMode mode){
            switch(mode){
            case Graphics::DepthMode::Baseline:return "baseline";
            case Graphics::DepthMode::Hybrid:return "hybrid";
            default:return "selective";
            }
        };
        if(!lastDepthMode||*lastDepthMode!=active){
            char line[128];std::snprintf(line,sizeof(line),"depth_mode_requested=%s depth_mode_active=%s\r\n",name(graphics.RequestedDepthMode()),name(active));
            DiagnosticLine(line);
            lastDepthMode=active;
        }
    }
    uint64_t depthCountSamples{},depthTimeSamples{},depthCutSamples{};
    uint64_t depthCountFirst{},depthCountLast{},depthTimeFirst{},depthTimeLast{};
    std::array<uint64_t,8> depthCounts{};
    double depthTotal{},depthRemaining{},depthCut{},depthOccluders{};
    void CollectDepth(Graphics& graphics,bool flush=false){
        if(!depthEnabled)return;
        std::array<DepthDiagnosticSample,6> counts{};
        const auto nc=graphics.ReadDepthDiagnostics(counts);
        for(size_t i=0;i<nc;++i){
            const auto& s=counts[i];
            if(!depthCountSamples)depthCountFirst=s.frame;
            depthCountFirst=std::min(depthCountFirst,s.frame);
            depthCountLast=std::max(depthCountLast,s.frame);
            ++depthCountSamples;
            for(size_t j=0;j<depthCounts.size();++j)depthCounts[j]+=s.counts[j];
        }
        std::array<SphereGpuTimings,12> timings{};
        const auto nt=graphics.ReadSphereGpuTimings(timings);
        for(size_t i=0;i<nt;++i){
            const auto& s=timings[i];
            const float total=s.PassMs(DirectGpuPass::CpuDepth),remaining=s.PassMs(DirectGpuPass::DepthRemaining);
            if(total<0.f||remaining<0.f)continue;
            if(!depthTimeSamples)depthTimeFirst=s.frame;
            depthTimeFirst=std::min(depthTimeFirst,s.frame);
            depthTimeLast=std::max(depthTimeLast,s.frame);
            ++depthTimeSamples;depthTotal+=total;depthRemaining+=remaining;
            const float seed=s.PassMs(DirectGpuPass::DepthOccluders);
            if(seed>=0.f)depthOccluders+=seed;
            const float cut=s.PassMs(DirectGpuPass::DepthCut);
            if(cut>=0.f){++depthCutSamples;depthCut+=cut;}
        }
        if(!flush||(!depthCountSamples&&!depthTimeSamples))return;
        const auto mean=[&](size_t j){return depthCountSamples?double(depthCounts[j])/double(depthCountSamples):-1.;};
        char line[1000];
        std::snprintf(line,sizeof(line),
            "depth_strategy=staged_current_frame depth_count_samples=%llu depth_count_first=%llu depth_count_last=%llu "
            "depth_time_samples=%llu depth_time_first=%llu depth_time_last=%llu depth_cut_samples=%llu "
            "depth_candidates=%.1f depth_occluders=%.1f depth_occluded=%.1f depth_submitted=%.1f "
            "depth_meshlets_native=%.1f depth_meshlets_mg2=%.1f depth_meshlets_rejected=%.1f depth_color_only=%.1f "
            "depth_occluders_ms=%.3f depth_remaining_ms=%.3f depth_cut_ms=%.3f depth_total_ms=%.3f\r\n",
            static_cast<unsigned long long>(depthCountSamples),static_cast<unsigned long long>(depthCountFirst),
            static_cast<unsigned long long>(depthCountLast),static_cast<unsigned long long>(depthTimeSamples),
            static_cast<unsigned long long>(depthTimeFirst),static_cast<unsigned long long>(depthTimeLast),
            static_cast<unsigned long long>(depthCutSamples),mean(0),mean(5),mean(6),mean(1),mean(2),mean(3),mean(4),mean(7),
            depthTimeSamples?depthOccluders/double(depthTimeSamples):-1.,
            depthTimeSamples?depthRemaining/double(depthTimeSamples):-1.,
            depthTimeSamples?depthCut/double(depthTimeSamples):-1.,
            depthTimeSamples?depthTotal/double(depthTimeSamples):-1.);
        DiagnosticLine(line);
        depthCountSamples=depthTimeSamples=depthCutSamples=0;
        depthCountFirst=depthCountLast=depthTimeFirst=depthTimeLast=0;
        depthCounts={};depthTotal=depthRemaining=depthCut=depthOccluders=0.;
    }
    uint64_t opticalSamples{};
    std::array<uint64_t,23> opticalTotals{};
    void CollectOptical(Graphics& graphics,bool finish=false){
        std::array<OpticalDiagnosticSample,6> samples{};
        const auto count=graphics.ReadOpticalDiagnostics(samples);
        std::array<uint64_t,23> windowCounts{};
        for(size_t i=0;i<count;++i){
            ++opticalSamples;
            for(size_t j=0;j<windowCounts.size();++j){
                windowCounts[j]+=samples[i].counts[j];
                opticalTotals[j]+=samples[i].counts[j];
            }
        }
        char line[1536];
        if(count){
            const double hit=windowCounts[0]?100.0*double(windowCounts[1])/double(windowCounts[0]):0.;
            std::snprintf(line,sizeof(line),
                "optique_image_debut=%llu optique_image=%llu optique_echantillons=%zu optique_echantillonnage=sampled_1_sur_256 "
                "soleil_demandes=%llu soleil_cache_hits=%llu soleil_cache_hit_pct=%.2f soleil_rayons_combines=%llu "
                "soleil_candidats_refuses=%llu reflet_demandes=%llu reflet_rayons_nouveaux=%llu "
                "soleil_rayons_statiques=%llu soleil_rayons_dynamiques=%llu soleil_zero_statique=%llu soleil_cache_misses=%llu reflet_partage_candidats=%llu reflet_partages=%llu reflet_partage_rejets=%llu reflet_historique_demandes=%llu reflet_historique_hits=%llu reflet_historique_rejets=%llu reflet_historique_bypass=%llu soleil_partage_candidats=%llu soleil_partages=%llu soleil_partage_rejets=%llu soleil_sans_ombres=%llu soleil_temoin_demandes=%llu reflet_temoin_borne=%llu\r\n",
                static_cast<unsigned long long>(samples[0].frame),static_cast<unsigned long long>(samples[count-1].frame),count,
                static_cast<unsigned long long>(windowCounts[0]),static_cast<unsigned long long>(windowCounts[1]),hit,
                static_cast<unsigned long long>(windowCounts[2]),static_cast<unsigned long long>(windowCounts[3]),
                static_cast<unsigned long long>(windowCounts[4]),static_cast<unsigned long long>(windowCounts[5]),
                static_cast<unsigned long long>(windowCounts[6]),static_cast<unsigned long long>(windowCounts[7]),
                static_cast<unsigned long long>(windowCounts[8]),static_cast<unsigned long long>(windowCounts[9]),
                static_cast<unsigned long long>(windowCounts[10]),static_cast<unsigned long long>(windowCounts[11]),
                static_cast<unsigned long long>(windowCounts[12]),
                static_cast<unsigned long long>(windowCounts[13]),static_cast<unsigned long long>(windowCounts[14]),
                static_cast<unsigned long long>(windowCounts[15]),static_cast<unsigned long long>(windowCounts[16]),
                static_cast<unsigned long long>(windowCounts[17]),static_cast<unsigned long long>(windowCounts[18]),
                static_cast<unsigned long long>(windowCounts[19]),static_cast<unsigned long long>(windowCounts[20]),
                static_cast<unsigned long long>(windowCounts[21]),static_cast<unsigned long long>(windowCounts[22]));
            DiagnosticLine(line);
        }else if(!finish)DiagnosticLine("optique_echantillonnage=indisponible\r\n");
        if(finish){
            const double hit=opticalTotals[0]?100.0*double(opticalTotals[1])/double(opticalTotals[0]):0.;
            std::snprintf(line,sizeof(line),
                "optique_synthese echantillons=%llu soleil_demandes=%llu soleil_cache_hits=%llu soleil_cache_hit_pct=%.2f "
                "soleil_rayons_combines=%llu soleil_candidats_refuses=%llu reflet_demandes=%llu reflet_rayons_nouveaux=%llu "
                "soleil_rayons_statiques=%llu soleil_rayons_dynamiques=%llu soleil_zero_statique=%llu soleil_cache_misses=%llu reflet_partage_candidats=%llu reflet_partages=%llu reflet_partage_rejets=%llu reflet_historique_demandes=%llu reflet_historique_hits=%llu reflet_historique_rejets=%llu reflet_historique_bypass=%llu soleil_partage_candidats=%llu soleil_partages=%llu soleil_partage_rejets=%llu soleil_sans_ombres=%llu soleil_temoin_demandes=%llu reflet_temoin_borne=%llu\r\n",
                static_cast<unsigned long long>(opticalSamples),static_cast<unsigned long long>(opticalTotals[0]),
                static_cast<unsigned long long>(opticalTotals[1]),hit,static_cast<unsigned long long>(opticalTotals[2]),
                static_cast<unsigned long long>(opticalTotals[3]),static_cast<unsigned long long>(opticalTotals[4]),
                static_cast<unsigned long long>(opticalTotals[5]),
                static_cast<unsigned long long>(opticalTotals[6]),static_cast<unsigned long long>(opticalTotals[7]),
                static_cast<unsigned long long>(opticalTotals[8]),static_cast<unsigned long long>(opticalTotals[9]),
                static_cast<unsigned long long>(opticalTotals[10]),static_cast<unsigned long long>(opticalTotals[11]),
                static_cast<unsigned long long>(opticalTotals[12]),
                static_cast<unsigned long long>(opticalTotals[13]),static_cast<unsigned long long>(opticalTotals[14]),
                static_cast<unsigned long long>(opticalTotals[15]),static_cast<unsigned long long>(opticalTotals[16]),
                static_cast<unsigned long long>(opticalTotals[17]),static_cast<unsigned long long>(opticalTotals[18]),
                static_cast<unsigned long long>(opticalTotals[19]),static_cast<unsigned long long>(opticalTotals[20]),
                static_cast<unsigned long long>(opticalTotals[21]),static_cast<unsigned long long>(opticalTotals[22]));
            DiagnosticLine(line);
        }
    }
    void Record(const DlssDiagnosticCapture::Sample& sample,const DlssDiagnosticState& gpu,
                const GraphicsMetrics& metrics,uint64_t frame,Graphics& graphics,uint32_t views,const ShuttlePilot* shuttle=nullptr,double xrPeriodMs=0.){
        sessionFrame.store(frame,std::memory_order_relaxed);
        const auto now=GetTickCount64();
        if(!started)started=window=now;
        // Record individual pauses on a monotonic clock, not a two-second peak.
        const int64_t eventQpc=QpcCounter();
        if(!stallOriginQpc)stallOriginQpc=eventQpc;
        const double gapMs=lastFrameQpc?1000.*QpcSeconds(eventQpc-lastFrameQpc):0.;
        const auto stallCluster=graphics.GetMg2ClusterStatus();
        const bool publication=stallCluster.publications!=lastStallPublications;
        const bool currentRender=stallCluster.traceRenderFrame==frame;
        const bool slow=gapMs>=60.||sample.ms[DlssDiagnosticCapture::Render]>=40.;
        if(slow||publication||(currentRender&&stallCluster.traceHudTick)){
            const double delta=slow&&lastStallQpc?1000.*QpcSeconds(eventQpc-lastStallQpc):-1.;
            char event[768];
            std::snprintf(event,sizeof(event),
                "cadence t_ms=%.3f image=%llu pause=%u intervalle_ms=%.3f depuis_pause_ms=%.3f "
                "xr_wait_ms=%.3f xr_begin_ms=%.3f prep_ms=%.3f acquire_ms=%.3f render_ms=%.3f end_ms=%.3f options_ms=%.3f "
                "resident_cpu_ms=%.3f mg2_cpu_ms=%.3f ray_cpu_ms=%.3f graph_cpu_ms=%.3f "
                "hud_1hz=%u publication=%u publications=%u clas_construits=%u clas_reutilises=%u soumis=%u\r\n",
                1000.*QpcSeconds(eventQpc-stallOriginQpc),static_cast<unsigned long long>(frame),unsigned(slow),gapMs,delta,
                sample.ms[DlssDiagnosticCapture::Wait],sample.ms[DlssDiagnosticCapture::Begin],
                sample.ms[DlssDiagnosticCapture::Prepare],sample.ms[DlssDiagnosticCapture::Acquire],
                sample.ms[DlssDiagnosticCapture::Render],sample.ms[DlssDiagnosticCapture::End],sample.ms[DlssDiagnosticCapture::Update],
                currentRender?stallCluster.traceResidentMs:-1.f,currentRender?stallCluster.traceForestMs:-1.f,
                currentRender?stallCluster.traceRayMs:-1.f,currentRender?stallCluster.traceGraphMs:-1.f,
                unsigned(currentRender&&stallCluster.traceHudTick),unsigned(publication),stallCluster.publications,
                stallCluster.clasBuilt,stallCluster.clasReused,unsigned(sample.submitted));
            stallEvents+=event;
            if(slow)lastStallQpc=eventQpc;
        }
        lastFrameQpc=eventQpc;lastStallPublications=stallCluster.publications;
        ++frames;
        wanted+=sample.wanted;prepared+=sample.prepared;submitted+=sample.submitted;
        for(size_t i=0;i<peaks.size();++i)peaks[i]=std::max(peaks[i],sample.ms[i]);
        CollectDepth(graphics);
        if(now-window<2000)return;
        if(!stallEvents.empty()){DiagnosticLine(stallEvents.c_str());stallEvents.clear();}
        CollectDepth(graphics,true);
        char line[820];
        std::snprintf(line,sizeof(line),
            "session_ms=%llu image=%llu fenetre_ms=%llu images=%u demandees=%u preparees=%u soumises=%u "
            "xr_periode_ms=%.2f xr_attente_pic_ms=%.2f xr_debut_pic_ms=%.2f preparation_pic_ms=%.2f acquisition_pic_ms=%.2f "
            "rendu_cpu_pic_ms=%.2f soumission_pic_ms=%.2f options_pic_ms=%.2f "
            "gpu_image=%llu gpu_direct_ms=%.2f gpu_compute_ms=%.2f gpu_direct_pic_ms=%.2f "
            "gpu_direct_lot=\"%s\" gpu_compute_pic_ms=%.2f gpu_compute_lot=\"%s\" "
            "ngx_gpu_ms=%.2f dlss=%.3f vram_mio=%llu budget_mio=%llu\r\n",
            now-started,static_cast<unsigned long long>(frame),now-window,frames,wanted,prepared,submitted,
            xrPeriodMs,peaks[0],peaks[1],peaks[2],peaks[3],peaks[4],peaks[5],peaks[6],
            static_cast<unsigned long long>(gpu.gpuFrame),gpu.directMs,gpu.computeMs,gpu.directPeakMs,
            gpu.directPeakBatch.data(),gpu.computePeakMs,gpu.computePeakBatch.data(),
            gpu.dlssGpuMs,gpu.requestedScale,static_cast<unsigned long long>(metrics.vramUsed>>20),
            static_cast<unsigned long long>(metrics.vramBudget>>20));
        DiagnosticLine(line);
        if(!opticalConfigLogged){
            const auto state=graphics.GetRenderingState();
            if(state.frame){
                const auto mode=state.applied.reflections;
                std::snprintf(line,sizeof(line),
                    "optique_version=6 soleil_mode=%s soleil_cache=off reflet_partage=wave_lobe_2x2_fast reflet_historique=post_trace_only sondes=local_fixed_time reflexions=%s msaa=%u resolution_oeil=%ux%u dlss=%u vrs=%u vues=%u cache_vram_mio=%u\r\n",
                    state.applied.solarVisibility==SceneSolarVisibility::Adaptive?"adaptive":state.applied.solarVisibility==SceneSolarVisibility::Exact?"exact":"no_shadows",
                    mode==SceneReflections::Full?"full":mode==SceneReflections::Selective?"selective":"ambient",
                    state.applied.antialiasing==SceneAntialiasing::Msaa4?4u:1u,state.width,state.height,
                    unsigned(state.dlss==DlssRenderStatus::Active),unsigned(state.applied.peripheral==PeripheralShading::Auto),
                    views,state.opticalCacheBytes>>20u);
                DiagnosticLine(line);opticalConfigLogged=true;
            }
        }
        // CPU-side publication counters; no GPU queries or waits in loop.
        const auto cluster=graphics.GetMg2ClusterStatus();
        std::snprintf(line,sizeof(line),
            "mg2_publication image=%llu api=%s publications=%u routes_seules=%u "
            "clas_construits_cumul=%u clas_reutilises_cumul=%u clas_partages_cumul=%u generations_reutilisees=%u "
            "variantes=%u instances_routees=%u clas_reserve_octets=%llu blas_reserve_octets=%llu "
            "mg2_select_worker_ms=%.3f mg2_pack_worker_ms=%.3f dernier_echec=%s code=%d\r\n",
            static_cast<unsigned long long>(frame),Mg2BackendName(cluster.lastPublished),
            cluster.publications,cluster.routeOnlyPublications,cluster.clasBuilt,cluster.clasReused,
            cluster.clasDuplicate,cluster.generationReusePublications,cluster.variants,cluster.routedInstances,
            static_cast<unsigned long long>(cluster.clasBytes),static_cast<unsigned long long>(cluster.blasBytes),
            cluster.selectWorkerMs,cluster.packWorkerMs,cluster.lastFailure,cluster.buildCode);
        DiagnosticLine(line);
        CollectOptical(graphics);
        if(shuttle)shuttle->LogSample(frame);
        window=now;frames=wanted=prepared=submitted=0;peaks={};
    }
};

struct Mg2Trace {
    ULONGLONG start{};
    uint64_t firstLiveFrame{},vramPeak{},budgetMin{};
    unsigned submitted{},wanted{},skyFrames{},sceneFrames{};
    uint32_t peakCameraTiles{},peakCameraInstances{};
    uint32_t forestFrames{},forestRayFrames{},forestClasFrames{},forestNvapiFrames{};
    uint32_t forestPagesMin{},forestPagesMax{},forestClusters{},forestTriangles{},forestBytes{};
    uint32_t forestDrawSamples{},forestDrawPeak{};
    uint32_t forestRoutesVerified{},forestRoutesFailed{};
    bool forestRequestFailed{};
    size_t peakWorktops{};
    enum Timing {Cpu,Direct,Compute,Campus,Count};
    std::array<std::vector<float>,Count> samples;
    std::array<std::vector<float>,static_cast<size_t>(DirectGpuPass::Count)> passSamples;

    void Record(Graphics& graphics,const XRFrame& frame,const DlssDiagnosticCapture::Sample& cpu,
                const GraphicsMetrics& metrics,const BuildingCampus& campus,const WorldRegistry& world,
                double campusMs){
        const auto now=GetTickCount64();
        if(!start)start=now;
        if(!firstLiveFrame&&frame.submitted)firstLiveFrame=frame.input.frameId;
        wanted+=cpu.wanted;
        submitted+=frame.submitted;
        if(frame.submitted){
            samples[Cpu].push_back(float(cpu.ms[DlssDiagnosticCapture::Prepare]+cpu.ms[DlssDiagnosticCapture::Render]));
            if(frame.sceneRaster)++sceneFrames;
            else ++skyFrames;
            peakCameraTiles=std::max(peakCameraTiles,frame.cameraTiles);
            peakCameraInstances=std::max(peakCameraInstances,frame.cameraInstances);
            if(frame.forestPages){
                forestPagesMin=forestFrames?std::min(forestPagesMin,frame.forestPages):frame.forestPages;
                ++forestFrames;
                forestPagesMax=std::max(forestPagesMax,frame.forestPages);
                forestClusters=frame.forestClusters;
                forestTriangles=frame.forestTriangles;
                forestBytes=frame.forestGeometryBytes;
            }
            forestRayFrames+=frame.forestRayPages!=0u;
            forestClasFrames+=frame.forestRayBackend!=Mg2RayBackend::Standard;
            forestNvapiFrames+=frame.forestRayBackend==Mg2RayBackend::Nvapi;
            forestRoutesVerified+=frame.forestRayRouteSample==1u;
            forestRoutesFailed+=frame.forestRayRouteSample==2u;
            forestRequestFailed|=frame.forestRequestFailed;
            if(frame.forestDrawSampleFrame){
                ++forestDrawSamples;
                forestDrawPeak=std::max(forestDrawPeak,frame.forestDrawClusters);
            }
        }
        samples[Campus].push_back(float(campusMs));
        std::array<SphereGpuTimings,12> gpu{};
        const auto count=graphics.ReadSphereGpuTimings(gpu);
        for(size_t i=0;i<count;++i){
            if(!firstLiveFrame||gpu[i].frame<firstLiveFrame)continue;
            if(gpu[i].directMs>=0.f)samples[Direct].push_back(gpu[i].directMs);
            if(gpu[i].computeMs>=0.f)samples[Compute].push_back(gpu[i].computeMs);
            for(size_t pass=0;pass<passSamples.size();++pass){
                const float ms=gpu[i].directPassMs[pass];
                if(ms>=0.f)passSamples[pass].push_back(ms);
            }
        }
        const auto worktops=campus.ResidentWorktops();
        peakWorktops=std::max(peakWorktops,worktops);
        vramPeak=std::max(vramPeak,metrics.vramUsed);
        if(metrics.vramBudget)budgetMin=budgetMin?std::min(budgetMin,metrics.vramBudget):metrics.vramBudget;
        const auto elapsed=now-start;
        if(elapsed<2000)return;
        for(auto& timings:samples)std::sort(timings.begin(),timings.end());
        for(auto& timings:passSamples)std::sort(timings.begin(),timings.end());
        const auto quantile=[](const std::vector<float>& values,size_t percentile){
            if(values.empty())return -1.f;
            return values[(values.size()*percentile+99u)/100u-1u];
        };
        const auto pass95=[&](DirectGpuPass pass){return quantile(passSamples[static_cast<size_t>(pass)],95);};
        char line[1024];
        std::snprintf(line,sizeof(line),
            "mg2 backend=%s frame=%llu campus_revision=%llu tuiles=%llu lumieres=%zu postes_residents=%zu postes_pic=%zu "
            "claviers_residents=%zu pieces_residentes=%zu crateres_publies=%zu "
            "soumises=%u demandees=%u fps_soumis=%.2f pixels_oeil=%ux%u "
            "cpu_p95_ms=%.3f cpu_p99_ms=%.3f gpu_echantillons=%zu direct_p95_ms=%.3f direct_p99_ms=%.3f "
            "compute_p95_ms=%.3f compute_p99_ms=%.3f campus_p95_ms=%.3f present_p95_ms=%.3f membrane_p95_ms=%.3f "
            "opaque_p95_ms=%.3f opaque_resident_p95_ms=%.3f opaque_tail_p95_ms=%.3f "
            "transparent_p95_ms=%.3f resolve_color_p95_ms=%.3f "
            "reflets_post_p95_ms=%.3f reflets_composite_p95_ms=%.3f "
            "tlas_build_p95_ms=%.3f tlas_update_p95_ms=%.3f "
            "ciel_frames=%u scene_frames=%u tuiles_camera_pic=%u instances_camera_pic=%u "
            "vram_pic_mio=%llu budget_min_mio=%llu\r\n",
            forestNvapiFrames?"nvapi_clas_foret":forestClasFrames?"dxr_clas_foret":"dxr_standard",
            static_cast<unsigned long long>(frame.input.frameId),static_cast<unsigned long long>(campus.Revision()),
            static_cast<unsigned long long>(world.TileCount()),campus.Lights().size(),worktops,peakWorktops,
            campus.ResidentKeyboards(),campus.ResidentRooms(),campus.GroundCraterCount(),submitted,wanted,
            double(submitted)*1000./double(elapsed),metrics.width,metrics.height,quantile(samples[Cpu],95),quantile(samples[Cpu],99),
            samples[Direct].size(),quantile(samples[Direct],95),quantile(samples[Direct],99),
            quantile(samples[Compute],95),quantile(samples[Compute],99),quantile(samples[Campus],95),
            pass95(DirectGpuPass::Present),pass95(DirectGpuPass::Membrane),pass95(DirectGpuPass::Opaque),
            pass95(DirectGpuPass::OpaqueResident),pass95(DirectGpuPass::OpaqueTail),
            pass95(DirectGpuPass::Transparent),pass95(DirectGpuPass::ResolveColor),
            pass95(DirectGpuPass::ReflectionPost),pass95(DirectGpuPass::ReflectionComposite),
            pass95(DirectGpuPass::TlasBuild),pass95(DirectGpuPass::TlasUpdate),
            skyFrames,sceneFrames,peakCameraTiles,peakCameraInstances,
            static_cast<unsigned long long>(vramPeak>>20),static_cast<unsigned long long>(budgetMin>>20));
        DiagnosticLine(line);
        const auto cluster=graphics.GetMg2ClusterStatus();
        std::snprintf(line,sizeof(line),"mg2_foret_live frame=%llu raster=%u tlas=%u clas=%u nvapi=%u "
            "pages_min=%u pages_max=%u clusters=%u triangles=%u octets_geometrie=%u requete_refusee=%u "
            "tirages_echantillons=%u clusters_soumis_pic=%u routes_tlas_ok=%u routes_tlas_erreur=%u "
            "publications=%u api_publication=%s clas_ms=%.3f blas_ms=%.3f "
            "clas_reserve_octets=%llu blas_reserve_octets=%llu dernier_echec=%s code=%d "
            "variantes=%u instances_routees=%u candidats=%u publications_routes_seules=%u "
            "clas_construits_cumul=%u clas_reutilises_cumul=%u clas_partages_cumul=%u generations_reutilisees=%u\r\n",
            static_cast<unsigned long long>(frame.input.frameId),forestFrames,forestRayFrames,forestClasFrames,forestNvapiFrames,
            forestPagesMin,forestPagesMax,forestClusters,forestTriangles,forestBytes,uint32_t(forestRequestFailed),
            forestDrawSamples,forestDrawPeak,forestRoutesVerified,forestRoutesFailed,cluster.publications,
            Mg2BackendName(cluster.lastPublished),cluster.clasMs,cluster.blasMs,
            static_cast<unsigned long long>(cluster.clasBytes),static_cast<unsigned long long>(cluster.blasBytes),
            cluster.lastFailure,cluster.buildCode,cluster.variants,cluster.routedInstances,
            cluster.selectionCandidates,cluster.routeOnlyPublications,
            cluster.clasBuilt,cluster.clasReused,cluster.clasDuplicate,cluster.generationReusePublications);
        DiagnosticLine(line);
        for(auto& timings:samples)timings.clear();
        for(auto& timings:passSamples)timings.clear();
        start=now;
        submitted=wanted=skyFrames=sceneFrames=0;
        peakCameraTiles=peakCameraInstances=0;
        forestFrames=forestRayFrames=forestClasFrames=forestNvapiFrames=0;
        forestPagesMin=forestPagesMax=forestClusters=forestTriangles=forestBytes=0;
        forestDrawSamples=forestDrawPeak=0;
        forestRoutesVerified=forestRoutesFailed=0;
        forestRequestFailed=false;
        peakWorktops=0;
        vramPeak=budgetMin=0;
    }
};

const char* CutFailure(const CutDiagnosticSample& sample){
    if(sample.flags&32u)return "historique_sature";
    if(sample.flags&64u)return "cache_non_pret";
    if(sample.flags&128u)return "publication_incoherente";
    if(sample.flags&1u)return "tampon_triangles";
    if(sample.flags&2u)return "bloc_source_invalide";
    if(sample.flags&4u)return "sections_laser";
    if(sample.flags&8u)return "fragments_paroi";
    if(sample.flags&16u)return "croisements_paroi";
    switch(sample.cacheError){
        case 1u:return "allocateur_invalide";
        case 2u:return "plage_contigue_indisponible";
        case 3u:return "compte_triangles_incoherent";
        case 4u:return "recyclage_plage";
        default:return "inconnu";
    }
}

struct CutSessionTrace {
    struct Tool {
        unsigned accepted{},missed{},rejected{},consecutive{};
        uint32_t lastFailure{};
        CutDiagnosticSample last{};
    };
    std::array<Tool,2> tools{};
    ULONGLONG window{};
    static const char* Name(bool laser){return laser?"laser":"projectile";}

    void Collect(Graphics& graphics,bool flush=false){
        std::array<CutDiagnosticSample,6> batch{};
        const size_t count=graphics.ReadCutDiagnostics(batch);
        for(size_t i=0;i<count;++i){
            const auto& sample=batch[i];
            auto& tool=tools[sample.laser?1u:0u];
            tool.last=sample;
            if(sample.status==2u){
                ++tool.accepted;
                tool.consecutive=tool.lastFailure=0;
                continue;
            }
            if(sample.flags==0u&&sample.cacheError==0u&&sample.removed==0u){
                ++tool.missed;
                continue;
            }
            ++tool.rejected;
            ++tool.consecutive;
            const uint32_t signature=(sample.cacheError<<16u)|sample.flags|0x80000000u;
            if(signature==tool.lastFailure)continue;
            tool.lastFailure=signature;
            char line[384];
            std::snprintf(line,sizeof(line),
                "decoupe_rejet=%s outil=%s image=%llu version=%u drapeaux=0x%x cache=%u "
                "triangles=%u surfaces_retirees=%u blocs=%u "
                "allocation_demandee=%u cache_capacite=%u cache_libre=%u cache_plage_max=%u\r\n",
                CutFailure(sample),Name(sample.laser),static_cast<unsigned long long>(sample.frame),
                sample.generation,sample.flags,sample.cacheError,sample.triangles,sample.removed,sample.blocks,
                sample.needed,sample.poolCapacity,sample.poolFree,sample.largestRange);
            DiagnosticLine(line);
        }
        const auto now=GetTickCount64();
        if(!flush&&now-window<2000)return;
        window=now;
        for(size_t i=0;i<tools.size();++i){
            auto& tool=tools[i];
            if(!(tool.accepted||tool.missed||tool.rejected))continue;
            char line[320];
            std::snprintf(line,sizeof(line),
                "decoupes=%s image=%llu version=%u validees=%u sans_matiere=%u rejetees=%u "
                "rejets_depuis_derniere_validation=%u\r\n",
                Name(i==1u),static_cast<unsigned long long>(tool.last.frame),tool.last.generation,
                tool.accepted,tool.missed,tool.rejected,tool.consecutive);
            DiagnosticLine(line);
            tool.accepted=tool.missed=tool.rejected=0;
        }
    }
};

struct DlssLoop {
    enum class Phase {Native,Active,Off,Capture};
    Phase phase{Phase::Native};
    int64_t started{};
    bool activeFrame{};

    bool CaptureReady()const{return phase==Phase::Capture;}

    void Update(Graphics& graphics,bool submitted,const DlssDiagnosticState& state){
        if(CaptureReady())return;
        const auto now=QpcCounter();
        if(!started){
            if(submitted)started=now;
            return;
        }

        const double elapsed=QpcSeconds(now-started);
        switch(phase){
        case Phase::Native:
            if(elapsed<5.0)return;
            if(!graphics.SetDlssScale(.5f)){
                DiagnosticLine("dlss_loop=activation_indisponible\r\n");
                phase=Phase::Capture;
                return;
            }
            DiagnosticLine("dlss_loop=dlss_actif\r\n");
            phase=Phase::Active;
            started=QpcCounter();
            return;
        case Phase::Active:
            if(state.requestedScale>0.f){
                activeFrame|=submitted&&state.reconstructed;
                if(elapsed<5.0)return;
            }else{
                activeFrame=false;
                DiagnosticLine("dlss_loop=evaluation_interrompue\r\n");
            }
            graphics.SetDlssScale(0.f);
            DiagnosticLine("dlss_loop=natif_apres_off\r\n");
            phase=Phase::Off;
            started=now;
            return;
        case Phase::Off:
            if(elapsed>=4.0)phase=Phase::Capture;
            return;
        case Phase::Capture:
            return;
        }
    }
};

AtmosphereSettings DefaultAtmosphere(bool logo){
    AtmosphereSettings atmosphere{};
    if(logo){
        atmosphere.center=SpectacleCenter;
        atmosphere.radius=SpectacleRadius;
        atmosphere.color={.32f,.55f,.25f};
        atmosphere.density=.19f;
        atmosphere.emissionReferencePower=20.f;
        atmosphere.membrane=true;
    }
    return atmosphere;
}

AtmosphereSettings CityAtmosphere(){
    auto profile=DefaultAtmosphere(true);
    profile.center=SpectacleCenters[1];profile.color={.21f,.45f,.76f};
    profile.film.tint={.55f,1.f,1.65f};
    return profile;
}

void TrackRightTool(XRFrame& frame,const VrInputActions& actions,bool liveInput){
    if(frame.rightTool==RightHandTool::Empty)return;
    constexpr uint32_t right=1u;
    const auto& grip=frame.input.PoseState(actions.handPose,right);
    if(!grip.tracked)return;

    // The equipped model is authored around its optic exit. Project that
    // exit onto the controller's aim axis so the model, laser and shot agree.
    const bool basalt=frame.rightTool==RightHandTool::Basalt;
    const float units=frame.input.sceneUnitsPerMeter;
    const auto opticFromGrip=Scale({0.f,.052f,basalt?.182f:.230f},units);
    const auto& aim=frame.input.PoseState(actions.aimPose,right);
    auto gripAligned=aim.tracked?aim.pose:
        FromMatrix(XMMatrixRotationX(XM_PIDIV2)*Matrix(grip.pose));
    gripAligned.p=grip.pose.p;
    const auto restExit=Transform(opticFromGrip,gripAligned);
    if(!aim.tracked){
        frame.toolExit=Pose{restExit,gripAligned.q};
        return;
    }
    const auto forward=Unit(Sub(Transform({0.f,0.f,1.f},aim.pose),aim.pose.p));
    const auto reach=std::max(0.f,Dot(Sub(restExit,aim.pose.p),forward));
    const auto exit=Add(aim.pose.p,Scale(forward,reach));
    frame.toolExit=Pose{exit,aim.pose.q};
    if(!liveInput)return;
    const bool switching=frame.input.Pressed(actions.equipRightTool,right);
    if(basalt){
        const auto owner=frame.input.insideSphere;
        const float length=owner<SpectacleSphereCount&&units<1.f?
            LaserReachToMembrane(exit,forward,SolarSceneCenter(owner,frame.input.solarTime),
                SolarSceneRadius(owner,frame.input.solarTime)):
            LaserReach*units;
        if(length>0.f){
            frame.basaltRay=LaserRay{exit,Add(exit,Scale(forward,length))};
            frame.laserHeld=!switching&&frame.input.Down(actions.secondary,right);
        }
        return;
    }
    auto& shot=frame.shotHands[right];
    shot.aim=aim;
    shot.aim.pose=*frame.toolExit;
    shot.fired=!switching&&frame.input.Pressed(actions.secondary,right);
}

struct BasaltStroke {
    struct Stamp { LaserRay ray; double time{}; };
    static constexpr double AfterimageSeconds=.12;
    std::array<Stamp,LaserAfterimageCount> stamps{};
    std::optional<LaserRay> previous;
    uint32_t next{},count{};
    XrTime lastDisplay{};
    uint64_t trackingRevision{};

    static XMMATRIX ReferenceToCosmos(const FrameInput& input){
        return input.solarTime>=0.&&input.insideSphere<SolarBodyCount?
            SolarBodyToCosmos(input.insideSphere,SolarBody(input.insideSphere,input.solarTime)):
            XMMatrixIdentity();
    }
    static LaserRay TransformRay(const LaserRay& ray,FXMMATRIX transform){
        return {TransformPoint(ray.origin,transform),TransformPoint(ray.end,transform)};
    }

    void Prepare(XRFrame& frame){
        if(frame.input.trackingRevision!=trackingRevision){
            previous.reset();
            next=count=0;
            trackingRevision=frame.input.trackingRevision;
        }
        if(!count&&!frame.laserHeld){previous.reset();return;}
        const auto referenceToCosmos=ReferenceToCosmos(frame.input);
        for(uint32_t i=0;i<count;++i){
            const auto& stamp=stamps[i];
            const auto age=frame.input.time-stamp.time;
            if(age<0.0||age>=AfterimageSeconds)continue;
            const float fade=1.f-float(age/AfterimageSeconds);
            frame.laserAfterimages[frame.laserAfterimageCount++]={TransformRay(stamp.ray,referenceToCosmos),fade*fade};
        }
        if(!frame.laserHeld||!frame.basaltRay){
            previous.reset();
            return;
        }
        if(frame.displayTime-lastDisplay>2*frame.displayPeriod)previous.reset();
        if(!previous){
            frame.laserCut=true;
            return;
        }
        frame.previousLaserRay=TransformRay(*previous,referenceToCosmos);
        const auto now=TransformRay(*frame.basaltRay,XMMatrixInverse(nullptr,referenceToCosmos));
        const auto originDelta=Sub(now.origin,previous->origin);
        // Compare motion at a fixed distance. A change in shell-exit distance
        // or an extended interior reach is independent of wrist motion.
        const auto aimDelta=Sub(Unit(Sub(now.end,now.origin)),Unit(Sub(previous->end,previous->origin)));
        const auto endDelta=Add(originDelta,Scale(aimDelta,LaserReach*frame.input.sceneUnitsPerMeter));
        const auto sceneDelta=Add(Scale(originDelta,.67f),Scale(endDelta,.33f));
        // Keep accumulating movement below the kerf scale while the aim holds.
        frame.laserCut=std::max(Length(originDelta),Length(sceneDelta))>=
                       .004f*frame.input.sceneUnitsPerMeter;
    }
    void EndGpuSweep(){previous.reset();}
    void Submitted(const XRFrame& frame){
        if(!frame.submitted||!frame.laserHeld||!frame.basaltRay)return;
        trackingRevision=frame.input.trackingRevision;
        lastDisplay=frame.displayTime;
        // Store the sweep and its afterimages with the rig when the planet
        // carries it, so the orbit alone cannot register as a new cut.
        const auto local=TransformRay(*frame.basaltRay,XMMatrixInverse(nullptr,ReferenceToCosmos(frame.input)));
        if(frame.laserCut)previous=local;
        stamps[next]={local,frame.input.time};
        next=(next+1u)%LaserAfterimageCount;
        count=std::min(count+1u,LaserAfterimageCount);
    }
};

void AppendSpectacleLights(RenderScene& render,const FrameInput& input,double time,bool paired){
    render.lights.push_back(SpectacleLight(0u,input.head.p,time));
    if(paired)render.lights.push_back(SpectacleLight(1u,input.head.p,time));
    render.lightCount=uint32_t(render.lights.size());
}

RenderContributions WorldContributions(WorldRegistry& world,bool logo,bool paired=false){
    auto result=world.Contributions();
    if(logo)result.appendContext=paired?+[](void*,RenderScene& scene,const FrameInput& input,double time){
        AppendSpectacleLights(scene,input,time,true);
    }:+[](void*,RenderScene& scene,const FrameInput& input,double time){
        AppendSpectacleLights(scene,input,time,false);
    };
    return result;
}

class LiveCapture {
    std::filesystem::path directory_;
    std::future<bool> saving_;
    uint32_t queued_{},sequence_{};
    bool armed_{};
public:
    LiveCapture(const std::wstring& config,const std::wstring& diagnosticPath,bool tour=false){
        namespace fs=std::filesystem;
        if(!diagnosticPath.empty()){
            directory_=fs::path(diagnosticPath).parent_path();
            if(!tour)directory_/="live";
            return;
        }
        for(auto folder=fs::path(config).parent_path();!folder.empty()&&folder!=folder.root_path();
            folder=folder.parent_path()){
            std::error_code error;
            if(fs::exists(folder/".git",error)){
                directory_=folder/"diagnostics"/"live";
                return;
            }
        }
        directory_=fs::path(L"diagnostics")/"live";
    }
    void Queue(){queued_=std::min(queued_+1u,4u);}
    void Prepare(Graphics& graphics){
        if(!queued_||armed_||saving_.valid()||graphics.CapturePending())return;
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        wchar_t name[112]{};
        swprintf_s(name,L"OASIS_%04u%02u%02uT%02u%02u%02u_%03uZ_%lu_%u.png",
            unsigned(utc.wYear),unsigned(utc.wMonth),unsigned(utc.wDay),unsigned(utc.wHour),
            unsigned(utc.wMinute),unsigned(utc.wSecond),unsigned(utc.wMilliseconds),
            GetCurrentProcessId(),++sequence_);
        graphics.RequestCapture((directory_/name).wstring());
        --queued_;
        armed_=true;
    }
    void Submitted(Graphics& graphics){
        if(!armed_||!graphics.CaptureDone())return;
        saving_=graphics.FinishCaptureAsync();
        if(saving_.valid())armed_=false;
    }
    std::optional<bool> Poll(){
        if(!saving_.valid()||saving_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)
            return std::nullopt;
        try{return saving_.get();}
        catch(const std::exception&){return false;}
    }
};

class MembranePassages {
    struct Pulse {XMFLOAT3 normal{};double time{};uint32_t owner{};};
    std::array<Pulse,MembranePassagePulseCount> pulses_{};
    uint32_t next_{},count_{};
public:
    void Update(XRFrame& frame,bool diagnostic){
        const auto& input=frame.input;
        for(uint32_t i=0;i<input.crossingCount;++i){
            const auto& crossing=input.crossings[i];
            const auto center=SolarSceneCenter(crossing.sphere,crossing.solarTime);
            auto normal=Unit(Sub(crossing.position,center));
            if(crossing.solarTime>=0.)
                XMStoreFloat3(&normal,XMVector3Rotate(XMLoadFloat3(&normal),
                    SolarInverseRotation(crossing.sphere,crossing.solarTime)));
            pulses_[next_]={normal,input.time,crossing.sphere};
            next_=(next_+1u)%MembranePassagePulseCount;
            count_=std::min(count_+1u,MembranePassagePulseCount);
            if(diagnostic){
                char line[240];
                const float units=crossing.solarTime>=0.?SolarInteriorUnitsPerMeter(crossing.sphere):
                    SpectacleInteriorUnitsPerMeter;
                std::snprintf(line,sizeof(line),
                    "sphere_passage=%s sphere=%u image=%llu position_scene=%.6f,%.6f,%.6f facteur=%.3f temps_orbite_s=%.6f\r\n",
                    crossing.entering?"entree":"sortie",crossing.sphere,static_cast<unsigned long long>(input.frameId),
                    crossing.position.x,crossing.position.y,crossing.position.z,
                    double(crossing.entering?1.f/units:1.f),crossing.solarTime);
                DiagnosticLine(line);
            }
        }
        uint32_t used=0u;
        for(uint32_t i=0;i<count_;++i){
            const auto& pulse=pulses_[(next_+MembranePassagePulseCount-1u-i)%MembranePassagePulseCount];
            const double age=input.time-pulse.time;
            if(age<0.||age>=1.)continue;
            if(!used)frame.membranePassageSphere=pulse.owner;
            if(pulse.owner==frame.membranePassageSphere)
                frame.membranePassagePulses[used++]={pulse.normal.x,pulse.normal.y,pulse.normal.z,1.f+float(age)};
        }
    }
};

struct SphereContactSurface {
    XMFLOAT3 center;
    float radius;
    float Distance(XMFLOAT3 point)const{return Length(Sub(point,center))-radius;}
    XMFLOAT3 Project(XMFLOAT3 point)const{return Add(center,Scale(Unit(Sub(point,center)),radius));}
};

class MembraneHands {
    struct Pulse {XMFLOAT3 normal{};double time{};bool active{};};
    struct Hand {
        SurfaceContact contact;
        std::array<Pulse,2> pulses{};
        uint32_t next{};
    };
    std::array<Hand,HandCount> hands_{};
    uint64_t revision_{};
public:
    void Update(XRFrame& frame,const AtmosphereSettings& atmosphere,
                Feedback& feedback,XR& xr,bool enabled){
        if(frame.input.trackingRevision!=revision_){
            hands_={};
            revision_=frame.input.trackingRevision;
        }
        if(!atmosphere.membrane||atmosphere.radius<=0.f)return;
        const auto& input=frame.input;
        const float units=input.sceneUnitsPerMeter;
        const SphereContactSurface surface{{},atmosphere.radius/units};
        const auto toLocal=SolarInverseRotation(frame.membraneHandSphere,input.solarTime);
        const auto toWorld=XMQuaternionConjugate(toLocal);
        const auto scenePoint=[&](XMFLOAT3 local){
            XMStoreFloat3(&local,XMVector3Rotate(XMLoadFloat3(&local),toWorld));
            return Add(atmosphere.center,Scale(local,units));
        };
        for(uint32_t hand=0;hand<HandCount;++hand){
            auto& state=hands_[hand];
            const auto& glove=frame.glovePoses[hand];
            if(enabled&&glove){
                const auto palm=Transform({0.f,0.f,.045f*units},*glove);
                auto localPalm=Scale(Sub(palm,atmosphere.center),1.f/units);
                XMStoreFloat3(&localPalm,XMVector3Rotate(XMLoadFloat3(&localPalm),toLocal));
                const auto sample=state.contact.Update(localPalm,input.time,surface);
                if(sample.crossing){
                    const auto& crossing=*sample.crossing;
                    state.pulses[state.next]={Unit(crossing.position),input.time,true};
                    state.next=(state.next+1u)%uint32_t(state.pulses.size());
                    feedback.Emit(xr,input.head,{crossing.entering?FeedbackCue::SurfaceEnter:FeedbackCue::SurfaceExit,
                        hand,input.time,input.frameId,1.f,scenePoint(crossing.position)},units);
                }
                if(sample.stroke){
                    const auto& stroke=*sample.stroke;
                    const auto first=Unit(stroke.start),last=Unit(stroke.end);
                    frame.membraneStrokeStart[hand]={first.x,first.y,first.z,std::clamp(.085f/surface.radius,.021f,.12f)};
                    frame.membraneStrokeEnd[hand]={last.x,last.y,last.z,stroke.strength};
                    feedback.Emit(xr,input.head,{FeedbackCue::SurfaceSlide,hand,input.time,input.frameId,
                                                 stroke.strength,scenePoint(stroke.end)},units);
                }
            }else state.contact.Reset();
            for(uint32_t i=0;i<state.pulses.size();++i){
                auto& pulse=state.pulses[i];
                const auto age=input.time-pulse.time;
                if(!pulse.active||age<0.||age>=atmosphere.film.waveDuration){
                    pulse.active=false;
                    continue;
                }
                frame.membraneHandPulses[hand*2u+i]={pulse.normal.x,pulse.normal.y,pulse.normal.z,1.f+float(age)};
            }
        }
    }
};

// One process, one fixed stereo camera, DXR ON versus DXR OFF. GPU samples
// are assigned by their producing frame, not by readback arrival time.
struct SunBenchmark {
    struct Case {
        const char* name;
        SceneSolarVisibility sun;
        SceneReflections reflections;
        bool dxr;
    };
    static constexpr std::array<Case,2> Cases{{
        {"A_dxr_on",SceneSolarVisibility::Exact,SceneReflections::Full,true},
        {"B_dxr_off",SceneSolarVisibility::NoShadows,SceneReflections::Ambient,false}
    }};
    RenderingOptions options{};
    size_t index{};
    int64_t phaseStart{},measureStart{};
    uint64_t firstFrame{},submitted{},gpuSamples{},opticalSamples{};
    double directTotal{},computeTotal{},opaqueTotal{},residentTotal{},tailTotal{},reflectionTotal{};
    uint64_t opaqueN{},residentN{},tailN{},reflectionN{};
    std::array<uint64_t,23> optical{};
    std::array<double,Cases.size()> fps{},direct{};
    uint64_t rayFrames{},tlasFrames{},blasFrames{},probeDispatches{},maxRaySlots{};
    uint32_t mg2Pages{},mg2Clusters{},mg2Triangles{};
    bool complete{};
    void Apply(Graphics& graphics){
        graphics.SetDiagnosticRayTracing(Cases[index].dxr);
        options.solarVisibility=Cases[index].sun;
        options.reflections=Cases[index].reflections;
        if(!graphics.SetRenderingOptions(options))Fail("Sun benchmark rendering options");
        char line[160];
        std::snprintf(line,sizeof(line),"sun_phase=%s dxr=%u warmup_s=%u mesure_s=5\r\n",
            Cases[index].name,unsigned(Cases[index].dxr),index==0?6u:2u);
        DiagnosticLine(line);
    }
    void Start(Graphics& graphics){
        options=graphics.GetRenderingOptions();
        options.antialiasing=SceneAntialiasing::Native;
        options.peripheral=PeripheralShading::Auto;
        graphics.SetDlssScale(0.f);
        DiagnosticLine("sun_version=2 test=dxr_total_on_off camera=loop_8s soleil_temps=8s msaa=1 dlss=off vrs=auto mg2_selection=fixed\r\n");
        Apply(graphics);
    }
    bool Update(Graphics& graphics,const XRFrame& frame){
        if(complete)return true;
        const auto now=QpcCounter();
        if(!phaseStart&&frame.submitted)phaseStart=now;
        std::array<SphereGpuTimings,12> timings{};
        const auto count=graphics.ReadSphereGpuTimings(timings);
        std::array<OpticalDiagnosticSample,6> samples{};
        const auto sampleCount=graphics.ReadOpticalDiagnostics(samples);
        if(!phaseStart)return false;
        const auto applied=graphics.GetRenderingState();
        const double warmup=index==0?6.:2.;
        if(!measureStart){
            if(QpcSeconds(now-phaseStart)>60.)Fail("Sun benchmark warmup timeout or MG2 not resident");
            if(!frame.submitted||QpcSeconds(now-phaseStart)<warmup||
               applied.dxrForestBuildPending||!frame.forestPages)return false;
            if(applied.applied.solarVisibility!=Cases[index].sun||
               applied.applied.reflections!=Cases[index].reflections||
               applied.applied.antialiasing!=SceneAntialiasing::Native||
               applied.dlss==DlssRenderStatus::Active||applied.dxrEnabled!=Cases[index].dxr)
                Fail("Sun benchmark settings not applied");
            if(index==0){mg2Pages=frame.forestPages;mg2Clusters=frame.forestClusters;mg2Triangles=frame.forestTriangles;}
            measureStart=now;
            firstFrame=frame.input.frameId+1u;
            char line[256];
            std::snprintf(line,sizeof(line),"sun_mesure_debut=%s image=%llu resolution=%ux%u yeux=%u\r\n",
                Cases[index].name,static_cast<unsigned long long>(firstFrame),applied.width,applied.height,frame.viewCount);
            DiagnosticLine(line);
            return false;
        }
        if(frame.submitted){
            ++submitted;
            if(frame.forestPages!=mg2Pages||frame.forestClusters!=mg2Clusters||frame.forestTriangles!=mg2Triangles||
               applied.dxrForestBuildPending)Fail("Sun benchmark MG2 geometry changed");
            rayFrames+=applied.dxrQueries?1u:0u;
            tlasFrames+=applied.dxrTlasWork?1u:0u;
            blasFrames+=applied.dxrDynamicBlasWork?1u:0u;
            probeDispatches+=applied.dxrProbeDispatches;
            maxRaySlots=std::max(maxRaySlots,uint64_t(applied.dxrRaySlots));
            if(!Cases[index].dxr&&(applied.dxrEnabled||applied.dxrQueries||applied.dxrTlasWork||
               applied.dxrDynamicBlasWork||applied.dxrRaySlots||applied.dxrProbeDispatches||frame.forestRayPages))
                Fail("Sun benchmark DXR off invariant violated");
        }
        for(size_t i=0;i<count;++i){
            const auto& t=timings[i];
            if(t.frame<firstFrame||t.frame>frame.input.frameId||t.directMs<0.f)continue;
            ++gpuSamples;directTotal+=t.directMs;computeTotal+=std::max(0.f,t.computeMs);
            auto add=[&](DirectGpuPass pass,double& total,uint64_t& n){
                const float ms=t.PassMs(pass);if(ms>=0.f){total+=ms;++n;}
            };
            add(DirectGpuPass::Opaque,opaqueTotal,opaqueN);
            add(DirectGpuPass::OpaqueResident,residentTotal,residentN);
            add(DirectGpuPass::OpaqueTail,tailTotal,tailN);
            add(DirectGpuPass::ReflectionPost,reflectionTotal,reflectionN);
        }
        for(size_t i=0;i<sampleCount;++i){
            if(samples[i].frame<firstFrame||samples[i].frame>frame.input.frameId)continue;
            ++opticalSamples;
            for(size_t j=0;j<optical.size();++j)optical[j]+=samples[i].counts[j];
        }
        const double elapsed=QpcSeconds(now-measureStart);
        if(elapsed<5.)return false;
        const auto avg=[](double total,uint64_t n){return n?total/double(n):-1.;};
        fps[index]=double(submitted)/elapsed;direct[index]=avg(directTotal,gpuSamples);
        char line[1024];
        std::snprintf(line,sizeof(line),
            "sun_resultat=%s duree_s=%.3f soumises=%llu fps=%.2f intervalle_ms=%.2f gpu_echantillons=%llu "
            "gpu_direct_ms=%.3f gpu_compute_ms=%.3f opaque_ms=%.3f opaque_resident_ms=%.3f opaque_tail_ms=%.3f reflection_post_ms=%.3f "
            "optique_echantillons=%llu soleil_demandes=%llu soleil_rayons=%llu soleil_partages=%llu soleil_cache_hits=%llu "
            "soleil_sans_ombres=%llu reflet_rayons=%llu\r\n",
            Cases[index].name,elapsed,static_cast<unsigned long long>(submitted),fps[index],
            fps[index]>0.?1000./fps[index]:-1.,static_cast<unsigned long long>(gpuSamples),
            direct[index],avg(computeTotal,gpuSamples),avg(opaqueTotal,opaqueN),avg(residentTotal,residentN),
            avg(tailTotal,tailN),avg(reflectionTotal,reflectionN),static_cast<unsigned long long>(opticalSamples),
            static_cast<unsigned long long>(optical[0]),static_cast<unsigned long long>(optical[2]),
            static_cast<unsigned long long>(optical[18]),static_cast<unsigned long long>(optical[1]),
            static_cast<unsigned long long>(optical[20]),static_cast<unsigned long long>(optical[5]));
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "sun_dxr_preuve=%s ray_frames=%llu tlas_frames=%llu dynamic_blas_frames=%llu room_probe_dispatches=%llu "
            "max_ray_slots=%llu mg2_pages=%u mg2_clusters=%u mg2_triangles=%u\r\n",
            Cases[index].name,static_cast<unsigned long long>(rayFrames),static_cast<unsigned long long>(tlasFrames),
            static_cast<unsigned long long>(blasFrames),static_cast<unsigned long long>(probeDispatches),
            static_cast<unsigned long long>(maxRaySlots),mg2Pages,mg2Clusters,mg2Triangles);
        DiagnosticLine(line);
        if(!submitted||!gpuSamples)Fail("Sun benchmark missing frame or GPU samples");
        if(Cases[index].dxr&&(!rayFrames||!maxRaySlots))Fail("Sun benchmark DXR on not active");
        if(!Cases[index].dxr&&(optical[2]||optical[5]||rayFrames||tlasFrames||blasFrames||probeDispatches||maxRaySlots))
            Fail("Sun benchmark DXR off counters nonzero");
        if(++index==Cases.size()){
            std::snprintf(line,sizeof(line),
                "sun_synthese fps_dxr_on=%.2f fps_dxr_off=%.2f "
                "gpu_dxr_on_ms=%.3f gpu_dxr_off_ms=%.3f "
                "cout_dxr_total_ms=%.3f\r\n",
                fps[0],fps[1],direct[0],direct[1],direct[0]-direct[1]);
            DiagnosticLine(line);complete=true;return true;
        }
        phaseStart=measureStart=0;firstFrame=submitted=gpuSamples=opticalSamples=0;
        directTotal=computeTotal=opaqueTotal=residentTotal=tailTotal=reflectionTotal=0.;
        opaqueN=residentN=tailN=reflectionN=0;optical={};
        rayFrames=tlasFrames=blasFrames=probeDispatches=maxRaySlots=0;
        Apply(graphics);
        return false;
    }
};

// Fixed stereo camera and solar time for each comparison. The reference is
// repeated at the end of each pose to expose thermal/background drift.
struct Mg2BudgetBenchmark {
    static constexpr uint32_t Base=RuntimeConfig::Mg2ReferenceTriangles;
    static constexpr std::array<uint32_t,8> Budgets{
        RuntimeConfig::Mg2DefaultTriangleBudget,10u*Base,12u*Base,16u*Base,
        20u*Base,24u*Base,RuntimeConfig::Mg2DiagnosticMaxTriangleBudget,
        RuntimeConfig::Mg2DefaultTriangleBudget};
    static constexpr std::array<double,2> Poses{8.,16.};
    size_t pose{},index{};
    int64_t phaseStart{},readyStart{},measureStart{};
    uint64_t firstFrame{},submitted{},vramPeak{},vramBudget{};
    uint32_t publicationsBefore{},routeOnlyBefore{},residentPublication{},measured{},refused{};
    uint32_t triangles{},clusters{},pages{},variants{},routed{},candidates{},bytes{};
    uint32_t buildPublications{},routePublications{};
    double readySeconds{};
    bool complete{};
    std::vector<float> direct,compute,depth,opaque;
    static double Mean(const std::vector<float>& samples){
        if(samples.empty())return -1.;
        double sum=0.;for(float value:samples)sum+=value;
        return sum/double(samples.size());
    }
    static double P95(std::vector<float>& samples){
        if(samples.empty())return -1.;
        std::sort(samples.begin(),samples.end());
        return samples[(95u*samples.size()+99u)/100u-1u];
    }
    void Apply(Graphics& graphics,StandardVrInput& mappings){
        mappings.EnableFixedSessionTour(Poses[pose]);
        const auto status=graphics.GetMg2ClusterStatus();
        publicationsBefore=status.publications;
        routeOnlyBefore=status.routeOnlyPublications;
        graphics.SetDiagnosticMg2Budget(Budgets[index]);
        phaseStart=readyStart=measureStart=0;
        firstFrame=submitted=vramPeak=vramBudget=0;
        direct.clear();compute.clear();depth.clear();opaque.clear();
        char line[192];
        std::snprintf(line,sizeof(line),
            "plafond_phase pose_s=%.0f cas=%zu plafond=%u repetition=%u\r\n",
            Poses[pose],index+1u,Budgets[index],unsigned(index+1u==Budgets.size()));
        DiagnosticLine(line);
    }
    void Start(Graphics& graphics,StandardVrInput& mappings){
        auto options=graphics.GetRenderingOptions();
        options.antialiasing=SceneAntialiasing::Native;
        options.peripheral=PeripheralShading::Auto;
        options.solarVisibility=SceneSolarVisibility::Exact;
        options.reflections=SceneReflections::Full;
        graphics.SetDlssScale(0.f);
        graphics.SetDiagnosticRayTracing(true);
        if(!graphics.SetRenderingOptions(options))Fail("MG2 budget benchmark rendering options");
        char line[320];
        std::snprintf(line,sizeof(line),
            "plafond_version=2 poses=8,16 msaa=1 dlss=off vrs=auto dxr=on seuil=1.1 "
            "reference_triangles=%u construction_production_mio=%u construction_mio=%u mesure_s=5 stabilisation_s=2\r\n",
            RuntimeConfig::Mg2DefaultTriangleBudget,RuntimeConfig::Mg2DefaultBuildBudgetMiB,
            RuntimeConfig::Mg2DiagnosticBuildBudgetMiB);
        DiagnosticLine(line);
        Apply(graphics,mappings);
    }
    bool Advance(Graphics& graphics,StandardVrInput& mappings){
        if(++index==Budgets.size()){
            index=0;
            if(++pose==Poses.size()){
                char line[160];
                std::snprintf(line,sizeof(line),"plafond_synthese mesures=%u refuses=%u cas=%zu\r\n",
                    measured,refused,Budgets.size()*Poses.size());
                DiagnosticLine(line);
                complete=true;
                if(!measured)Fail("MG2 budget benchmark: no valid measurement");
                return true;
            }
        }
        Apply(graphics,mappings);return false;
    }
    bool Update(Graphics& graphics,StandardVrInput& mappings,const XRFrame& frame){
        if(complete)return true;
        const auto now=QpcCounter();
        if(!phaseStart&&frame.submitted)phaseStart=now;
        std::array<SphereGpuTimings,12> timings{};
        const auto count=graphics.ReadSphereGpuTimings(timings);
        std::array<OpticalDiagnosticSample,6> optical{};
        graphics.ReadOpticalDiagnostics(optical); // Drain the existing asynchronous ring.
        if(!phaseStart)return false;
        const auto status=graphics.GetMg2ClusterStatus();
        const auto applied=graphics.GetRenderingState();
        const double phaseSeconds=QpcSeconds(now-phaseStart);
        if(status.requestedTriangleBudget!=Budgets[index])
            Fail("MG2 budget benchmark: mismatched request");
        if(!measureStart){
            if(status.budgetResolution==2u){
                char line[512];
                std::snprintf(line,sizeof(line),
                    "plafond_refuse pose_s=%.0f cas=%zu plafond=%u raison=%s code=%d candidats=%u attente_s=%.3f "
                    "requis_octets=%llu disponible_octets=%llu\r\n",
                    Poses[pose],index+1u,Budgets[index],status.lastFailure,status.buildCode,
                    status.selectionCandidates,phaseSeconds,
                    static_cast<unsigned long long>(status.buildRequiredBytes),
                    static_cast<unsigned long long>(status.buildAvailableBytes));
                DiagnosticLine(line);++refused;return Advance(graphics,mappings);
            }
            if(phaseSeconds>120.){
                DiagnosticLine("plafond_erreur=timeout_publication\r\n");
                Fail("MG2 budget benchmark: publication timeout");
            }
            if(status.budgetResolution!=1u||applied.dxrForestBuildPending||!frame.submitted||
               !frame.forestPages||!frame.forestRayPages||!applied.dxrQueries)return false;
            if(!readyStart){readyStart=now;readySeconds=phaseSeconds;}
            if(QpcSeconds(now-readyStart)<2.||phaseSeconds<6.)return false;
            if(applied.applied.antialiasing!=SceneAntialiasing::Native||
               applied.applied.solarVisibility!=SceneSolarVisibility::Exact||
               applied.applied.reflections!=SceneReflections::Full||
               applied.applied.peripheral!=PeripheralShading::Auto||
               applied.dlss==DlssRenderStatus::Active||!applied.dxrEnabled)
                Fail("MG2 budget benchmark: rendering options not applied");
            if(frame.forestTriangles>Budgets[index])Fail("MG2 budget benchmark: triangle ceiling violated");
            residentPublication=status.publications;
            buildPublications=status.publications-publicationsBefore-
                (status.routeOnlyPublications-routeOnlyBefore);
            routePublications=status.routeOnlyPublications-routeOnlyBefore;
            triangles=frame.forestTriangles;clusters=frame.forestClusters;pages=frame.forestPages;
            variants=status.variants;routed=status.routedInstances;candidates=status.selectionCandidates;
            bytes=frame.forestGeometryBytes;
            measureStart=now;firstFrame=frame.input.frameId+1u;
            char line[320];
            std::snprintf(line,sizeof(line),
                "plafond_mesure_debut pose_s=%.0f cas=%zu plafond=%u image=%llu resolution=%ux%u yeux=%u\r\n",
                Poses[pose],index+1u,Budgets[index],static_cast<unsigned long long>(firstFrame),
                applied.width,applied.height,frame.viewCount);
            DiagnosticLine(line);return false;
        }
        if(frame.submitted){
            if(status.publications!=residentPublication||status.budgetResolution!=1u||
               applied.dxrForestBuildPending||!applied.dxrQueries||!frame.forestRayPages||
               frame.forestTriangles!=triangles||frame.forestClusters!=clusters||frame.forestPages!=pages)
                Fail("MG2 budget benchmark: geometry changed during measurement");
            ++submitted;
            const auto metrics=graphics.GetMetrics();
            vramPeak=std::max(vramPeak,metrics.vramUsed);
            vramBudget=metrics.vramBudget;
        }
        for(size_t i=0;i<count;++i){
            const auto& t=timings[i];
            if(t.frame<firstFrame||t.frame>frame.input.frameId||t.directMs<0.f)continue;
            direct.push_back(t.directMs);
            if(t.computeMs>=0.f)compute.push_back(t.computeMs);
            const float d=t.PassMs(DirectGpuPass::CpuDepth);
            const float o=t.PassMs(DirectGpuPass::OpaqueResident);
            if(d>=0.f)depth.push_back(d);
            if(o>=0.f)opaque.push_back(o);
        }
        const double elapsed=QpcSeconds(now-measureStart);
        if(elapsed<5.)return false;
        if(!submitted||direct.empty()||depth.empty()||opaque.empty())
            Fail("MG2 budget benchmark: missing GPU samples");
        const double fps=double(submitted)/elapsed;
        const double directMean=Mean(direct),direct95=P95(direct);
        const double depthMean=Mean(depth),depth95=P95(depth);
        const double opaqueMean=Mean(opaque),opaque95=P95(opaque);
        char line[1024];
        std::snprintf(line,sizeof(line),
            "plafond_resultat pose_s=%.0f cas=%zu plafond=%u repetition=%u "
            "duree_s=%.3f soumises=%llu fps=%.2f gpu_echantillons=%zu gpu_direct_ms=%.3f gpu_direct_p95_ms=%.3f "
            "gpu_compute_ms=%.3f cpu_scene_depth_ms=%.3f cpu_scene_depth_p95_ms=%.3f "
            "opaque_resident_ms=%.3f opaque_resident_p95_ms=%.3f vram_mio=%llu budget_mio=%llu "
            "triangles=%u clusters=%u pages=%u variantes=%u instances_routees=%u candidats=%u "
            "geometrie_octets=%u clas_octets=%llu blas_octets=%llu api=%s "
            "constructions=%u routes_seules=%u clas_ms=%.3f blas_ms=%.3f publication_s=%.3f\r\n",
            Poses[pose],index+1u,Budgets[index],unsigned(index+1u==Budgets.size()),elapsed,
            static_cast<unsigned long long>(submitted),fps,direct.size(),directMean,direct95,Mean(compute),
            depthMean,depth95,opaqueMean,opaque95,
            static_cast<unsigned long long>(vramPeak>>20),static_cast<unsigned long long>(vramBudget>>20),
            triangles,clusters,pages,variants,routed,candidates,bytes,
            static_cast<unsigned long long>(status.clasBytes),static_cast<unsigned long long>(status.blasBytes),
            Mg2BackendName(status.lastPublished),buildPublications,routePublications,
            buildPublications?status.clasMs:-1.f,buildPublications?status.blasMs:-1.f,readySeconds);
        DiagnosticLine(line);++measured;
        return Advance(graphics,mappings);
    }
};

LoopResult RunVr(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                 const std::wstring& config,const SceneData& scene,WorldRegistry& world,JobSystem& jobs,
                 RenderContributions contributions,bool logo,NavigationContinuity& navigation,
                 DiagnosticCapture& diagnostic,bool automatedDlss,
                 bool solarCalibration,bool shuttleCalibration,uint32_t passageOwner,OrbitProfile& profile,MaterialProfiles& materialProfiles,
                 const ShuttleAssets* shuttleAssets,BuildingCampus* building,bool buildingLoop,bool buildingPilot){
    StandardVrInput mappings;
    ShuttlePilot shuttle;
    shuttle.SetMaterialProfiles(materialProfiles);
    std::optional<ShuttleAssets::Contribution> shuttleContribution;
    std::optional<SphereCalibration> calibration;
    const bool passageCalibration=passageOwner!=Invalid;
    if(solarCalibration||shuttleCalibration||passageCalibration)
        calibration.emplace(solarCalibration||shuttleCalibration,
                            passageCalibration?&mappings:nullptr,passageOwner);
    if(shuttleCalibration)calibration->SetShuttle(shuttle);
    const bool sphereDiagnostic=calibration.has_value();
    const bool automated=automatedDlss||sphereDiagnostic;
    const bool plafondTest=LaunchSessionMode()==SessionMode::Plafond;
    // Both benchmarks use the fixed, head-locked VR session path.
    const bool sunTest=LaunchSessionMode()==SessionMode::Sun||plafondTest;
    if(sunTest&&(!scene.solarSystem||!building||automated))Fail("Fixed benchmark requires live OASIS campus");
    SunBenchmark sunBenchmark;
    Mg2BudgetBenchmark budgetBenchmark;
    const uint32_t sphereCount=scene.sphereObjectOffsets.empty()?1u:uint32_t(scene.sphereObjectOffsets.size()-1u);
    const bool paired=sphereCount>1u;
    PostMessageW(window,OutputWindowMessage,0,0);
    auto atmosphere=DefaultAtmosphere(logo);
    const auto city=CityAtmosphere();
    OrbitTool orbit(profile,logo,atmosphere.density);
    if(building)orbit.SetBuilding(&building->Pilot());
    mappings.SetBuilding(buildingLoop?nullptr:building);
    std::optional<BuildingTour> buildingTour;
    if(buildingLoop)buildingTour.emplace(std::filesystem::path(diagnostic.path).parent_path());
    mappings.SetNavigationOptions((!automated||passageCalibration)&&!buildingLoop&&!sunTest,!automated&&!buildingLoop&&!sunTest);
    mappings.SetSpectaclePassage(logo&&!automated&&!scene.solarSystem,sphereCount);
    if(!automated&&!buildingLoop&&!sunTest)mappings.SetOrbitTool(&orbit);
    const auto profileFolder=profile.LocalFile().empty()?std::filesystem::path(config).parent_path()/".oasis-local":
        profile.LocalFile().parent_path();
    if(!passageCalibration&&!mappings.LoadConfiguration(config,automated?std::filesystem::path{}:profileFolder/"controls.ini"))
        DiagnosticLine("controls_ini=locomotion_usine\r\n");
    if(!automated)shuttle.LoadSettings(profileFolder/"shuttle.ini");
    if(shuttleAssets){
        mappings.SetShuttlePilot(&shuttle);
        shuttle.SetCanopy(shuttleAssets->Canopy(shuttle.Craft()));
        shuttleContribution.emplace(ShuttleAssets::Contribution{contributions,*shuttleAssets,shuttle});
        contributions=shuttleContribution->Bind();
    }
    const bool fixedDiagnosticFraming=automated&&logo;
    if(building&&!navigation.valid&&!scene.solarSystem)mappings.SetInitialPose(building->Pilot().Spawn());
    if(fixedDiagnosticFraming)mappings.SetInitialPose(OasisDiagnosticHead());
    else if(navigation.valid)mappings.SetInitialPose(navigation.pose);
    XR xr;Graphics graphics;
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=openxr\r\n");xr.InitializeInstance({"OASIS","OASIS",1},input,
        calibration?calibration->Mapping():mappings.Mapping());
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=dx12\r\n");graphics.Initialize(xr.GraphicsRequirements());
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=session_vr\r\n");xr.InitializeSession(graphics.Device(),graphics.Queue());
    if(sphereDiagnostic||diagnostic.mg2||sunTest)graphics.EnableSphereBaselineTiming(diagnostic.mg2||sunTest);
    if(diagnostic.mg2||plafondTest)graphics.EnableMg2Diagnostic();
    if(paired)graphics.EnableSpherePair(city,sphereCount);
    graphics.SetMaterialTuning(materialProfiles.Values());
    if(building)graphics.SetBuildingRevision(building->Revision(),building->Lights());
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=chargement\r\n");graphics.Configure(xr,scene,jobs,contributions);
    if(diagnostic.mg2){
        const auto cluster=graphics.GetMg2ClusterStatus();
        char line[256];
        std::snprintf(line,sizeof(line),
            "mg2_cluster_gpu demande=%s api=%s raison=%s code=%d pilote=%u adaptateur=%04x:%04x parties=%u pages_source=%u\r\n",
            cluster.request,Mg2BackendName(cluster.configured),cluster.capability,cluster.code,cluster.driver,
            cluster.adapterVendor,cluster.adapterDevice,cluster.meshes,cluster.sourcePages);
        DiagnosticLine(line);
    }
    graphics.SetDeveloperMode(true);
    orbit.Restore(graphics,atmosphere);
    if(plafondTest)budgetBenchmark.Start(graphics,mappings);
    else if(sunTest)sunBenchmark.Start(graphics);
    DlssLoop dlssLoop;
    const bool sessionTraceEnabled=!automated&&!diagnosticLogPath.empty();
    SessionTrace sessionTrace;
    Mg2Trace mg2Trace;
    CutSessionTrace cutTrace;
    graphics.SetCutDiagnostic(sessionTraceEnabled);
    DlssDiagnosticCapture dlssTrace{!sphereDiagnostic&&!diagnosticLogPath.empty()};
    graphics.SetDlssDiagnostic(dlssTrace.enabled);
    if(automatedDlss){
        DiagnosticLine("dlss_loop=preparation_sdk\r\n");
        const int64_t started=QpcCounter();
        const bool ready=graphics.PrepareDlss(DlssPreset::E);
        char line[120];
        std::snprintf(line,sizeof(line),"dlss_preparation_ms=%.2f pret=%u\r\n",
            1000.0*QpcSeconds(QpcCounter()-started),unsigned(ready));
        DiagnosticLine(line);
        DiagnosticLine("dlss_loop=natif_avant\r\n");
    }
    std::optional<Feedback> feedback;
    if(!automated)feedback.emplace(profileFolder/"audio",shuttle.Profiles(),
                                  StandardVrActions().feedback,shuttleAssets!=nullptr,
                                  std::filesystem::path(config).parent_path()/"audio");
    if(solarCalibration||shuttleCalibration||passageCalibration)calibration->StartSolarSystem();
    const auto sessionMode=LaunchSessionMode();
    const bool sessionTour=sessionMode==SessionMode::Loop&&scene.solarSystem&&!automated&&building;
    if(sessionTour)mappings.EnableSessionTour();
    // Standard loop only: preserve independent sun/plafond/MG2 benchmark rings.
    if(sessionTour&&sessionTraceEnabled&&!diagnostic.mg2&&!plafondTest&&!sunTest){
        sessionTrace.depthEnabled=true;
        graphics.EnableDepthDiagnostics();
        DiagnosticLine("depth_telemetry=v3 scope=CpuScene.Depth count_every_frames=8 counts=mean_per_sample rejected=instance_frustum_or_occluded_meshlets_before_dispatch timings=mean_per_frame\r\n");
    }
    if(sunTest)mappings.EnableFixedSessionTour(8.);
    if(scene.solarSystem&&!automated){
        if(!navigation.valid){
            const auto worktop=building&&diagnostic.mg2?building->Pilot().WorktopSpawn():std::nullopt;
            if(diagnostic.mg2)DiagnosticLine(worktop?"mg2_depart=bureau\r\n":"mg2_depart=parvis\r\n");
            const auto spawn=building&&(buildingPilot||diagnostic.mg2)?
                FromMatrix(Matrix(worktop.value_or(building->Pilot().Spawn()))*
                    SolarBodyToCosmos(0u,SolarBody(0u,0.))):SolarOasisApproach(0.);
            auto sessionSpawn=spawn;
            const auto body=SolarBodyToCosmos(0u,SolarBody(0u,0.));
            if(sessionMode==SessionMode::Cosmos||sessionTour)sessionSpawn=SessionTourPose(0.,0.);
            if(sessionMode==SessionMode::Hall&&building){
                const auto& pilot=building->Pilot();
                sessionSpawn=FromMatrix(Matrix(SolarLookAt(pilot.Scene({7.75f,1.52f,4.7f}),
                    pilot.Scene({3.f,1.53f,3.4f})))*body);
            }
            if(sessionMode==SessionMode::Forest&&diagnostic.sessionForestSpawn)
                sessionSpawn=FromMatrix(Matrix(*diagnostic.sessionForestSpawn)*body);
            mappings.SetInitialPose(sessionSpawn);
        }
        mappings.StartSolarSystem();
    }
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=attente_image\r\n");
    int64_t menuClick=0;bool finishCapture=false,exitInitiated=false;
    ULONGLONG exitRequestedAt=0;
    bool pageUpHeld=false,pageDownHeld=false,wasAboard=false,hadAvionics=false;
    BasaltStroke laser;
    LiveCapture liveCapture(config,diagnostic.path,sessionTour);
    size_t tourCheckpoint=0,tourCaptureCompleted=0,tourCaptureSaved=0;
    std::array<MembraneHands,SpectacleSphereCount> membraneHands;
    MembranePassages passages;
    uint64_t materialRevision=materialProfiles.Revision();
    double previousBuildingTime=0.;
    auto stamp=[&]{return sphereDiagnostic||sessionTraceEnabled?QpcCounter():dlssTrace.Stamp();};
    while(!xr.ExitRequested()){
        if(sessionTraceEnabled)MarkSession(SessionStage::Loop);
        xr.PollEvents();if(xr.ExitRequested())break;
        if(stop.load(std::memory_order_relaxed)||finishCapture){
            if(!xr.Running())break;
            if(!exitInitiated){mappings.SetOrbitTool(nullptr);xr.RequestExit();exitInitiated=true;exitRequestedAt=GetTickCount64();}
            if(GetTickCount64()-exitRequestedAt>=2500)break;
        }
        if(!xr.Running()){Sleep(5);continue;}
        DlssDiagnosticCapture::Sample timing;
        auto started=stamp();
        XRFrame frame;
        if(sessionTraceEnabled)MarkSession(SessionStage::Begin);
        if(!xr.BeginFrame(frame,(dlssTrace.enabled||sessionTraceEnabled||sphereDiagnostic)?&timing.ms[DlssDiagnosticCapture::Wait]:nullptr,
                          !fixedDiagnosticFraming))continue;
        timing.ms[DlssDiagnosticCapture::Begin]=dlssTrace.Elapsed(started);
        if(sessionTraceEnabled)MarkSession(SessionStage::Input);
        timing.wanted=frame.render;
        int scaleStep=0;
        const auto& actions=StandardVrActions();
        const bool aboard=shuttle.Active();
        if(fixedDiagnosticFraming&&!(shuttleCalibration&&aboard))
            ApplyOasisDiagnosticControllers(frame.input,actions,sphereDiagnostic);
        if(sunTest)ApplyOasisDiagnosticControllers(frame.input,actions,true);
        passages.Update(frame,sessionTraceEnabled||passageCalibration||shuttleCalibration);
        if(buildingTour){
            buildingTour->PoseForFrame(frame,building->Pilot());
            ApplyOasisDiagnosticControllers(frame.input,actions,true);
        }
        double mg2CampusMs=0.;
        if(building){
            const auto started=diagnostic.mg2?QpcCounter():0;
            const double dt=previousBuildingTime?frame.input.time-previousBuildingTime:0.;
            previousBuildingTime=frame.input.time;
            auto local=frame.input;
            if(scene.solarSystem){
                const auto toLocal=XMMatrixInverse(nullptr,SolarBodyToCosmos(0,SolarBody(0,local.solarTime)));
                local.head=FromMatrix(Matrix(local.head)*toLocal);
                auto& aim=local.State(actions.aimPose).pose[1];
                if(aim.tracked)aim.pose=FromMatrix(Matrix(aim.pose)*toLocal);
            }
            building->Projectiles(graphics.DrainBuildingImpacts());
            building->Tick(world,local.head,float(std::clamp(dt,0.,.075)),
                local.farClipMeters);
            graphics.SetBuildingRevision(building->Revision(),building->Lights());
            auto& selected=building->Select(local.head.p);
            orbit.SetBuilding(&selected);
            if(buildingTour&&!exitInitiated)buildingTour->Before(graphics,building->Pilot(),local.head.p);
            if(local.insideSphere==0u)orbit.BuildingContext(local);
            else orbit.ClearBuildingContext();
            if(started)mg2CampusMs=dlssTrace.Elapsed(started);
        }
        frame.orbit=orbit.Display();
        if(buildingTour)frame.orbit.buildingPage=true;
        if(!aboard){
            frame.glovePoses[0]=VrGlovePose(frame.input.PoseState(actions.handPose,0),0);
            frame.glovePoses[1]=VrGlovePose(frame.input.PoseState(actions.handPose,1),1);
            frame.rightTool=fixedDiagnosticFraming?RightHandTool::Basalt:orbit.EquippedTool();
        }
        const bool liveInput=!automated&&!buildingTour&&!sessionTour&&!sunTest&&!exitInitiated&&frame.input.focused;
        const bool ignition=shuttle.ConsumeIgnition();
        const bool booting=shuttle.AvionicsOn()&&!hadAvionics;
        hadAvionics=shuttle.AvionicsOn();
        if((sessionTraceEnabled||shuttleCalibration)&&(aboard!=wasAboard||ignition)){
            char line[120];
            std::snprintf(line,sizeof(line),"navette_evenement=%s image=%llu\r\n",
                ignition?"allumage":aboard?"embarquement":"sortie",static_cast<unsigned long long>(frame.input.frameId));
            DiagnosticLine(line);
        }
        wasAboard=aboard;
        const auto reversals=shuttle.ConsumeRotorReversals();
        TrackRightTool(frame,actions,liveInput);
        laser.Prepare(frame);
        if(building&&liveInput&&frame.basaltRay&&frame.laserHeld&&
           (frame.input.insideSphere==0u||frame.input.insideSphere==Invalid)){
            if(building->Laser(*frame.basaltRay,frame.input.solarTime)){
                frame.laserCut=false;
                laser.EndGpuSweep();
            }
        }else if(building)building->EndLaser();
        if(!automated&&!exitInitiated){
            const bool pageUp=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0;
            const bool pageDown=(GetAsyncKeyState(VK_NEXT)&0x8000)!=0;
            if(frame.input.focused)scaleStep=int(pageUp&&!pageUpHeld)-int(pageDown&&!pageDownHeld);
            pageUpHeld=pageUp;
            pageDownHeld=pageDown;
            if(liveInput&&(frame.input.Pressed(actions.capture)||frame.input.Pressed(actions.next)))
                liveCapture.Queue();
            if(frame.input.Pressed(actions.quit,0)){
                const int64_t now=QpcCounter();
                if(menuClick&&now-menuClick<=2*QpcFrequency()){stop.store(true,std::memory_order_relaxed);menuClick=0;}
                else menuClick=now;
            }
        }
        if(feedback&&!automated)feedback->Tick(xr,frame.input);
        if(feedback&&!automated){
            feedback->UpdateShuttle(xr,shuttle,frame.input,aboard&&liveInput,booting,ignition,reversals);
            const auto owner=NearestSpectacle(frame.input.head.p,sphereCount,frame.input.solarTime);
            auto local=owner==0u?atmosphere:city;
            local.center=SolarSceneCenter(owner,frame.input.solarTime);
            local.radius=SolarSceneRadius(owner,frame.input.solarTime);
            frame.membraneHandSphere=owner;
            membraneHands[owner].Update(frame,local,*feedback,xr,liveInput&&!aboard);
        }
        if(const auto saved=liveCapture.Poll()){
            if(sessionTour){
                ++tourCaptureCompleted;
                if(*saved)++tourCaptureSaved;
            }
            if(*saved&&feedback&&!exitInitiated)
                feedback->Emit(xr,frame.input.head,{FeedbackCue::CaptureSaved,0u,frame.input.time,frame.input.frameId});
            else if(!*saved)DiagnosticLine("capture_live=echec_ecriture\r\n");
        }
        const double tourTime=mappings.SessionTourElapsed();
        if(sessionTour&&!exitInitiated&&tourCheckpoint<SessionTourCaptureTimes.size()&&tourTime>=SessionTourCaptureTimes[tourCheckpoint]){
            char line[256];
            std::snprintf(line,sizeof(line),"tour_seconde=%.3f image=%llu sphere=%u xyz=%.5f,%.5f,%.5f\r\n",
                tourTime,static_cast<unsigned long long>(frame.input.frameId),frame.input.insideSphere,
                frame.input.head.p.x,frame.input.head.p.y,frame.input.head.p.z);
            DiagnosticLine(line);
            liveCapture.Queue();++tourCheckpoint;
        }
        const bool captureLoopFrame=automatedDlss&&dlssLoop.CaptureReady()&&!diagnostic.attempted&&!exitInitiated;
        const auto* captureView=fixedDiagnosticFraming&&captureLoopFrame?&OasisDiagnosticView:nullptr;
        if(captureView){
            frame.input.head=OasisDiagnosticHead();
            ++frame.input.trackingRevision;
        }
        if(scene.solarSystem)ExpandSolarClip(frame.input);
        if(sessionTraceEnabled)MarkSession(SessionStage::Prepare);
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        if(materialRevision!=materialProfiles.Revision()){
            graphics.SetMaterialTuning(materialProfiles.Values());
            materialRevision=materialProfiles.Revision();
        }
        started=stamp();
        timing.prepared=frame.render&&graphics.Prepare(frame.input);
        timing.ms[DlssDiagnosticCapture::Prepare]=dlssTrace.Elapsed(started);
        if(timing.prepared){
            started=stamp();
            if(sessionTraceEnabled)MarkSession(SessionStage::Acquire);
            frame.headLockedDiagnostic=sphereDiagnostic||buildingTour.has_value()||sunTest;
            xr.LocateLate(frame,captureView);
            if(calibration)calibration->ApplyView(frame);
            const bool acquired=frame.tracked&&xr.AcquireSwapchains(frame);
            timing.ms[DlssDiagnosticCapture::Acquire]=dlssTrace.Elapsed(started);
            if(acquired){
                if(!sunTest&&!sessionTour&&!buildingTour&&(!automated||captureLoopFrame||
                    (calibration&&calibration->CaptureReady()))&&!exitInitiated)
                    diagnostic.BeforeRender(graphics,automated,diagnostic.mg2&&building&&
                        frame.input.insideSphere==0u&&building->ResidentWorktops()>0u);
                if((feedback||sessionTour)&&!automated&&!exitInitiated)liveCapture.Prepare(graphics);
                started=stamp();
                if(sessionTraceEnabled)MarkSession(SessionStage::Render);
                if(sessionTraceEnabled)cutTrace.Collect(graphics);
                graphics.Render(frame);
                if(sessionTraceEnabled)sessionTrace.LogDepthMode(graphics);
                if((feedback||sessionTour)&&!automated)liveCapture.Submitted(graphics);
                timing.ms[DlssDiagnosticCapture::Render]=dlssTrace.Elapsed(started);
            }
        }
        started=stamp();
        if(sessionTraceEnabled)MarkSession(SessionStage::Submit);
        xr.EndFrame(frame);
        laser.Submitted(frame);
        timing.ms[DlssDiagnosticCapture::End]=dlssTrace.Elapsed(started);
        timing.submitted=frame.submitted;
        {
            if(sessionTraceEnabled)MarkSession(SessionStage::Options);
            const auto before=dlssTrace.enabled||automatedDlss?graphics.GetDlssDiagnostic():DlssDiagnosticState{};
            started=stamp();
            if(automatedDlss&&!exitInitiated){
                dlssLoop.Update(graphics,frame.submitted,before);
                if(dlssLoop.CaptureReady())diagnostic.dlssLoopCompleted=dlssLoop.activeFrame;
            }
            if(!sunTest&&!sphereDiagnostic&&(!automatedDlss||dlssLoop.phase!=DlssLoop::Phase::Native))
                graphics.UpdateDlss(scaleStep);
            if(!sunTest&&!sphereDiagnostic&&!exitInitiated&&!orbit.Apply(graphics,atmosphere))DiagnosticLine("orbit_profil=echec_ecriture\r\n");
            if(dlssTrace.enabled||sessionTraceEnabled){
                timing.ms[DlssDiagnosticCapture::Update]=dlssTrace.Elapsed(started);
                const auto state=graphics.GetDlssDiagnostic();
                if(dlssTrace.enabled)dlssTrace.Record(timing,before,state,frame.input.frameId);
                if(sessionTraceEnabled&&!sunTest){
            
                    const auto metrics=graphics.GetMetrics();
                    sessionTrace.Record(timing,state,metrics,frame.input.frameId,graphics,frame.viewCount,
                        shuttleAssets?&shuttle:nullptr,double(frame.displayPeriod)*1.e-6);
                    if(diagnostic.mg2&&building)mg2Trace.Record(graphics,frame,timing,metrics,*building,world,mg2CampusMs);
                }
                if(!dlssTrace.enabled&&!sessionTraceEnabled)graphics.SetDlssDiagnostic(false);
            }
        }
        if(calibration&&!exitInitiated){
            calibration->Record(frame,timing.ms[DlssDiagnosticCapture::Prepare]+timing.ms[DlssDiagnosticCapture::Render]);
            calibration->Collect(graphics);
        }
        if(frame.submitted&&sessionTour)mappings.StartSessionTour();
        if(frame.submitted)
            navigation={frame.input.head,true,frame.input.sceneUnitsPerMeter!=1.f};
        if(sessionTraceEnabled)MarkSession(SessionStage::Capture);
        if(sunTest){
            sessionFrame.store(frame.input.frameId,std::memory_order_relaxed);
            if(frame.submitted)frameSeen.store(true,std::memory_order_release);
            if(!exitInitiated){
                if(plafondTest){
                    if(budgetBenchmark.Update(graphics,mappings,frame)){
                        diagnostic.plafondCompleted=true;finishCapture=true;
                    }
                }else if(sunBenchmark.Update(graphics,frame)){
                    diagnostic.sunCompleted=true;finishCapture=true;
                }
            }
        }else if(buildingTour){
            if(frame.submitted)frameSeen.store(true,std::memory_order_release);
            if(!exitInitiated&&!buildingTour->Finished())buildingTour->After(graphics,frame);
            if(buildingTour->Finished()){
                diagnostic.captured=buildingTour->Complete();
                diagnosticCaptured.store(diagnostic.captured,std::memory_order_release);
                finishCapture=true;
            }
        }else if(diagnostic.AfterRender(graphics,frame.submitted,false,frameSeen,
            frame.captureOnly||passageCalibration||shuttleCalibration?&frame:nullptr))
            finishCapture=true;
        if(sessionTour&&!exitInitiated&&tourTime>=SessionTourSeconds&&
           tourCaptureCompleted==SessionTourCaptureTimes.size()){
            diagnostic.attempted=true;
            diagnostic.captured=tourCaptureSaved==SessionTourCaptureTimes.size();
            diagnosticCaptured.store(diagnostic.captured,std::memory_order_release);
            const auto metrics=graphics.GetMetrics();
            char line[384];
            std::snprintf(line,sizeof(line),
                "capture_loop=%zu/%zu\r\nfps_soumis=%.2f\r\nintervalle_image_ms=%.2f\r\n"
                "vram_gio=%.2f / %.2f\r\nvue=oeil_gauche\r\nresolution=%u x %u\r\n",
                tourCaptureSaved,SessionTourCaptureTimes.size(),metrics.fps,
                metrics.fps>0?1000.0/double(metrics.fps):0.,
                double(metrics.vramUsed)/1073741824.,double(metrics.vramBudget)/1073741824.,
                metrics.width,metrics.height);
            DiagnosticLine(line);
            finishCapture=true;
        }
    }
    if(dlssTrace.enabled)dlssTrace.Finish(graphics.GetDlssDiagnostic(),automatedDlss);
    if(sessionTraceEnabled)MarkSession(SessionStage::Shutdown);
    graphics.WaitIdle();
    if(calibration){calibration->Collect(graphics);diagnostic.sphereBaselineCompleted=calibration->Write();}
    if(sessionTraceEnabled){cutTrace.Collect(graphics,true);if(!sunTest){sessionTrace.CollectOptical(graphics,true);sessionTrace.CollectDepth(graphics,true);}}
    graphics.UnloadScene();
    if(!automated&&!sunTest&&!shuttle.SaveSettings())DiagnosticLine("navette_profil=echec_ecriture\r\n");
    return LoopResult::Exit;
}

LoopResult RunDesktop(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                      const SceneData& scene,WorldRegistry& world,JobSystem& jobs,RenderContributions contributions,
                      bool logo,bool& developer,NavigationContinuity& navigation,DiagnosticCapture& diagnostic,
                      const OrbitProfile& profile,const MaterialProfiles& materialProfiles,BuildingCampus* building){
    PostMessageW(window,OutputWindowMessage,1,0);
    Graphics graphics;DiagnosticLine("etape=dx12_ecran\r\n");graphics.InitializeDesktop();
    RECT rectangle{};if(!GetClientRect(window,&rectangle))Fail("Desktop client area");
    uint32_t width=uint32_t(std::max<LONG>(1,rectangle.right-rectangle.left));
    uint32_t height=uint32_t(std::max<LONG>(1,rectangle.bottom-rectangle.top));
    graphics.SetMaterialTuning(materialProfiles.Values());
    DesktopOutput desktop;desktop.Create(window,graphics.Device(),graphics.Queue(),width,height);
    auto configure=[&]{
        OutputSurface output{};output.width=desktop.Width();output.height=desktop.Height();output.viewCount=1;
        output.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;output.desktop=true;
        for(uint32_t i=0;i<DesktopOutput::BufferCount;++i)output.colors.push_back(desktop.Color(i));
        output.depths.push_back(desktop.Depth());graphics.ConfigureOutput(output,scene,jobs,contributions);
        auto atmosphere=DefaultAtmosphere(logo);
        profile.RestoreAtmosphere(atmosphere);
        profile.RestoreRendering(graphics);
        graphics.SetAtmosphere(atmosphere);graphics.SetDeveloperMode(developer);
    };
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=chargement_ecran\r\n");configure();
    const bool traceEnabled=!diagnosticLogPath.empty();
    graphics.SetDlssDiagnostic(traceEnabled);
    graphics.SetCutDiagnostic(traceEnabled);
    SessionTrace sessionTrace;
    CutSessionTrace cutTrace;
    DesktopInput controller;
    controller.SetSolarSystem(scene.solarSystem);
    controller.SetSpectaclePassage(logo);
    if(navigation.valid)controller.SetPose(navigation.pose,navigation.inside);
    MembranePassages passages;
    MarkDiagnosticStartupProgress();
    uint64_t frameId=0;bool switchOutput=false;int64_t previousFrameQpc=0;
    while(!stop.load(std::memory_order_relaxed)){
        if(!GetClientRect(window,&rectangle))break;
        const LONG w=rectangle.right-rectangle.left,h=rectangle.bottom-rectangle.top;
        if(w<=0||h<=0||IsIconic(window)){Sleep(10);continue;}
        width=uint32_t(w);height=uint32_t(h);
        if(width!=desktop.Width()||height!=desktop.Height()){graphics.ReleaseOutput();desktop.Resize(width,height);configure();}
        const auto controls=controller.Poll(++frameId,width,height,input.ActionCount(),GetForegroundWindow()==window);
        navigation={controls.input.head,true,controls.input.sceneUnitsPerMeter!=1.f};
        if(controls.toggleDeveloper){developer=!developer;graphics.SetDeveloperMode(developer);}
        if(controls.quit){stop.store(true,std::memory_order_relaxed);break;}
        if(controls.toggleOutput){switchOutput=true;break;}
        XRFrame frame{};frame.begun=frame.render=frame.ready=frame.tracked=true;frame.viewCount=1;
        frame.color=desktop.CurrentIndex();frame.depth=0;frame.input=controls.input;
        passages.Update(frame,traceEnabled);
        frame.vp[0]=controls.vp;frame.inverseVP[0]=controls.inverseVP;frame.eyes[0]=controls.eye;
        if(previousFrameQpc){
            const double elapsed=QpcSeconds(controls.input.targetDisplayQpc-previousFrameQpc);
            if(elapsed>0.0)frame.displayPeriod=XrDuration(std::clamp(elapsed,.00001,.05)*1e9);
        }
        previousFrameQpc=controls.input.targetDisplayQpc;
        if(building){
            building->Projectiles(graphics.DrainBuildingImpacts());
            building->Tick(world,frame.input.head,float(double(frame.displayPeriod)*1.e-9),
                frame.input.farClipMeters);
            graphics.SetBuildingRevision(building->Revision(),building->Lights());
        }
        const auto& actions=StandardVrActions();
        for(uint32_t hand=0;hand<2;++hand){
            auto& shot=frame.shotHands[hand];
            shot.aim.pose=frame.input.head;shot.aim.tracked=frame.input.focused;
            shot.fired=frame.input.Pressed(hand==0?actions.primary:actions.secondary,hand);
        }
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        DlssDiagnosticCapture::Sample timing{};
        timing.wanted=true;
        if(traceEnabled)MarkSession(SessionStage::Prepare);
        auto started=traceEnabled?QpcCounter():0;
        timing.prepared=graphics.Prepare(frame.input);
        if(traceEnabled)timing.ms[DlssDiagnosticCapture::Prepare]=1000.0*QpcSeconds(QpcCounter()-started);
        if(timing.prepared){
            diagnostic.BeforeRender(graphics);
            if(traceEnabled){
                MarkSession(SessionStage::Render);
                cutTrace.Collect(graphics);
                started=QpcCounter();
            }
            graphics.Render(frame);
            if(traceEnabled)timing.ms[DlssDiagnosticCapture::Render]=1000.0*QpcSeconds(QpcCounter()-started);
        }
        if(traceEnabled){MarkSession(SessionStage::Submit);started=QpcCounter();}
        if(frame.submitted)desktop.Present();else SwitchToThread();
        if(traceEnabled){
            timing.submitted=frame.submitted;
            timing.ms[DlssDiagnosticCapture::End]=1000.0*QpcSeconds(QpcCounter()-started);
            sessionTrace.Record(timing,graphics.GetDlssDiagnostic(),graphics.GetMetrics(),frame.input.frameId,graphics,frame.viewCount);
        }
        if(diagnostic.AfterRender(graphics,frame.submitted,true,frameSeen))break;
    }
    if(traceEnabled){
        MarkSession(SessionStage::Shutdown);
        graphics.WaitIdle();
        cutTrace.Collect(graphics,true);
        sessionTrace.CollectOptical(graphics,true);
    }
    graphics.ReleaseOutput();
    return switchOutput&&!stop.load(std::memory_order_relaxed)?LoopResult::SwitchOutput:LoopResult::Exit;
}

int Run(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,std::atomic<ULONGLONG>& startupProgress,HWND window,const std::wstring& worldPath,
        const std::wstring& capturePath,bool interactive,bool desktop,bool automatedDlss,
        bool solarCalibration,bool shuttleCalibration,uint32_t passageOwner,
        bool buildingPilot,bool buildingLoop,bool mg2){
    mg2StartupProgress=(mg2||LaunchSessionMode()!=SessionMode::Default)?&startupProgress:nullptr;
    const auto launchStarted=GetTickCount64();
    ComScope com;JobSystem jobs;jobs.Start();
    InputSystem input;DeclareStandardVrInputActions(input);
    const auto config=AssetPath(L"input.ini");
    const bool sphereDiagnostic=solarCalibration||shuttleCalibration||passageOwner!=Invalid;
    OrbitProfile orbitProfile(automatedDlss||sphereDiagnostic?std::wstring{}:config);
    const auto controlProfile=orbitProfile.LocalFile().parent_path()/"controls.ini";
    if(!input.LoadConfiguration(config,automatedDlss||sphereDiagnostic?std::wstring{}:controlProfile.wstring()))
        DiagnosticLine("controls_ini=repli_usine\r\n");
    const bool logo=worldPath.empty();
    MaterialProfiles materialProfiles;
    const OrbitProfile diagnosticFolder(shuttleCalibration?config:std::wstring{});
    const auto& local=shuttleCalibration?diagnosticFolder.LocalFile():orbitProfile.LocalFile();
    materialProfiles.Load(local.parent_path());
    SceneData scene;
    try{scene=LoadSceneGLTF(logo?AssetPath(L"oasis_logo.glb"):worldPath);if(scene.objects.empty())throw AssetImportError("empty world");}
    catch(const AssetImportError& error){
        const auto line=std::string("asset_unavailable source=world reason=")+error.what()+" action=emergency_site\r\n";DiagnosticLine(line.c_str());
        scene=BuildEmergencySite();
    }
    std::optional<Mg2Catalog> mg2Catalog;
    const bool useCampus=logo&&!sphereDiagnostic&&!automatedDlss;
    const auto pack=std::filesystem::path(AssetPath(L"packs/oasis_mg2_v1"));
    char density[4]{};
    const auto densityLength=GetEnvironmentVariableA("OASIS_MG2_FOREST",density,DWORD(sizeof(density)));
    const std::string_view densitySetting(density,densityLength<sizeof(density)?densityLength:0u);
    const unsigned multiplier=densitySetting=="80"?1u:densitySetting=="320"?4u:2u;
    std::filesystem::path campusKey;
    const auto campusStart=GetTickCount64();
    if(mg2||(useCampus&&!sphereDiagnostic&&!buildingLoop&&std::filesystem::exists(pack/"pack.json"))){
        if(useCampus&&!buildingLoop)
            campusKey=CampusBake::Key(pack,local.parent_path(),desktop,multiplier);
        try{
            mg2Catalog.emplace(pack,CampusBake::Exists(campusKey));
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){
            const auto line=std::string("asset_unavailable source=mg2_catalog reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());
        }
    }
    if(!local.empty())materialProfiles.Install(scene);
    WorldRegistry world;
    std::optional<BuildingCampus> building;
    if(useCampus){
        const auto save=buildingLoop?std::filesystem::path{}:local.parent_path()/"buildings"/"oasis";
        try{building.emplace(AssetPath(L"buildings/oasis"),save);}
        catch(const std::bad_alloc&){throw;}
        catch(const std::exception& e){
            const auto line=std::string("asset_unavailable source=building_recipe reason=")+e.what()+"\r\n";DiagnosticLine(line.c_str());
        }
    }
    if(building){
        std::vector<BuildingFootprint> sites;
        for(const auto& letter:building->Letters()){
            const auto& d=letter->Definition();
            for(const auto band:Buildings::GlyphBands(d,0)){
                if(band.left>=band.right)continue;
                const float units=SpectacleInteriorUnitsPerMeter;
                sites.push_back({{d.origin.x+band.left*d.xScale*units,d.origin.y,d.origin.z},
                    (band.right-band.left)*d.xScale*units,d.depth*units});
            }
        }
        auto terrain=PrepareBuildingSite(scene,sites);
        if(mg2Catalog){
            try{
                if(mg2Catalog->PrepareForest(terrain,sites,multiplier)){
                    std::erase_if(scene.objects,[&](const RenderObject& object){
                        return scene.materialNames[object.material]=="Low vegetation";
                    });
                }
            }catch(const std::bad_alloc&){throw;}
            catch(const std::exception& error){
                const auto line=std::string("asset_unavailable source=forest_recipe reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());
            }
        }
        terrain.soil.insert(terrain.soil.end(),terrain.banks.begin(),terrain.banks.end());
        building->SetGround(terrain.soil);
    }
    if(mg2Catalog)mg2Catalog->WriteSummary();
    bool campusKeyDesktop=desktop;
    std::optional<SceneData> pair;
    std::optional<ShuttleAssets> shuttleAssets;
    if(logo&&!automatedDlss&&!buildingLoop){
        pair.emplace(scene);
        AppendSolarPlanets(*pair);
        PrepareSpectacleGeometry(*pair);
        if(pair->solarSystem&&(!sphereDiagnostic||shuttleCalibration))try{shuttleAssets.emplace(*pair);}
        catch(const AssetImportError& error){pair->assetDegraded=true;const auto line=std::string("asset_unavailable source=navette_organes.glb reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());}
        if(!local.empty())materialProfiles.Install(*pair);
        if(!pair->solarSystem)
            InstantiateSpectacleCities(*pair,SpectacleSphereCount);
    }
    if(logo){
        PrepareSpectacleGeometry(scene);
    }
    // Keep the logo's source instances in SceneData: the GPU cut source
    // is built from these same instances during Graphics::Configure.
    // Moving them to WorldRegistry clears scene.objects before that happens.
    if(!logo&&!scene.objects.empty()){
        std::vector<WorldObject> objects;objects.reserve(scene.objects.size());
        for(const auto& source:scene.objects){
            if(source.mesh>=scene.meshes.ranges.size()||source.material>=scene.materials.size())Fail("World references unavailable geometry or material");
            WorldObject object{};object.current=source.current;object.previous=source.previous;object.scale=source.scale;
            object.mesh=source.mesh;object.material=source.material;object.flags=source.flags;
            object.boundRadius=std::max(.001f,scene.meshes.ranges[source.mesh].bounds.w);objects.push_back(object);
        }
        world.CreateBatch(objects);scene.objects.clear();
    }
    auto contributions=WorldContributions(world,logo);bool developer=false;NavigationContinuity navigation;
    DiagnosticCapture diagnostic{capturePath,interactive};
    diagnostic.mg2=mg2;
    if(mg2Catalog)diagnostic.sessionForestSpawn=mg2Catalog->ForestSpawn();
    if(LaunchSessionMode()==SessionMode::Forest&&!diagnostic.sessionForestSpawn)
        DiagnosticLine("asset_unavailable source=forest_spawn action=default_spawn\r\n");
    if(building&&desktop)navigation={building->Pilot().Spawn(),true,true};
    while(!stop.load(std::memory_order_relaxed)){
        startupProgress.store(GetTickCount64(),std::memory_order_release);
        frameSeen.store(false,std::memory_order_release);
        const auto& source=desktop||!pair?scene:*pair;
        std::optional<SceneData> campusScene;
        std::optional<Mg2Worktop> worktop;
        std::optional<Mg2ArchitecturalPalette> pbr;
        if(building){
            world=WorldRegistry{};
            if(mg2Catalog){
                if(campusKeyDesktop!=desktop){
                    campusKey=CampusBake::Key(pack,local.parent_path(),desktop,multiplier);
                    campusKeyDesktop=desktop;
                }
                const auto bakeStart=GetTickCount64();
                CampusBake::Data baked;
                const bool restored=CampusBake::Read(campusKey,baked);
                if(restored){
                    campusScene.emplace(std::move(baked.scene));
                    pbr.emplace(baked.palette);
                    worktop.emplace(std::move(baked.worktop));
                }else{
                    campusScene.emplace(source);
                    try{
                        mg2Catalog->PrepareGeometry();
                        DiagnosticLine("mg2_phase=pbr_begin\r\n");
                        pbr.emplace(mg2Catalog->AppendArchitecturalMaterials(*campusScene));
                        MarkDiagnosticStartupProgress();
                    }catch(const std::bad_alloc&){throw;}
                    catch(const std::exception& error){
                        campusScene->assetDegraded=true;
                        const auto line=std::string("asset_unavailable source=mg2_architectural_recipe reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());
                    }
                    try{
                        DiagnosticLine("mg2_phase=worktop_begin\r\n");
                        worktop.emplace(mg2Catalog->AppendWorktop(*campusScene));
                        MarkDiagnosticStartupProgress();
                    }catch(const std::bad_alloc&){throw;}
                    catch(const std::exception& error){
                        campusScene->assetDegraded=true;
                        const auto line=std::string("asset_unavailable source=mg2_placement_recipe reason=")+error.what()+" action=retain_structural_recipe\r\n";DiagnosticLine(line.c_str());
                    }
                }
                mg2Catalog->ReleasePreparedGeometry();
                const auto assembled=GetTickCount64();
                const bool written=!restored&&worktop&&pbr&&!mg2Catalog->Degraded()&&!campusScene->assetDegraded&&CampusBake::Write(campusKey,*campusScene,*worktop,*pbr);
                char bakeLine[256];
                std::snprintf(bakeLine,sizeof(bakeLine),
                    "cache_campus=%s assemblage_ms=%llu ecriture_ms=%llu total_avant_foret_ms=%llu\r\n",
                    restored?"hit":written?"miss_written":"miss_uncached",
                    static_cast<unsigned long long>(assembled-bakeStart),
                    static_cast<unsigned long long>(GetTickCount64()-assembled),
                    static_cast<unsigned long long>(GetTickCount64()-campusStart));
                DiagnosticLine(bakeLine);
                MarkDiagnosticStartupProgress();
                DiagnosticLine("mg2_phase=forest_install_begin\r\n");
                mg2Catalog->InstallForest(*campusScene,world);
                DiagnosticLine("mg2_phase=forest_install_ready\r\n");
            }else campusScene.emplace(source);
            MarkDiagnosticStartupProgress();
            DiagnosticLine("mg2_phase=building_install_begin\r\n");
            building->Install(*campusScene,world,worktop?&*worktop:nullptr,pbr?&*pbr:nullptr);
            MarkDiagnosticStartupProgress();
            DiagnosticLine("mg2_phase=building_install_ready\r\n");
            DiagnosticLine("mg2_phase=static_meshlets_ready\r\n");
        }
        if(LaunchSessionMode()!=SessionMode::Default){
            char line[320];
            std::snprintf(line,sizeof(line),
                "cache_cpu glb_hits=%llu glb_misses=%llu glb_writes=%llu secteurs_hits=%llu secteurs_misses=%llu secteurs_writes=%llu preparation_cpu_ms=%llu\r\n",
                static_cast<unsigned long long>(DiskBake::glb.hits.load()),static_cast<unsigned long long>(DiskBake::glb.misses.load()),
                static_cast<unsigned long long>(DiskBake::glb.writes.load()),static_cast<unsigned long long>(DiskBake::building.hits.load()),
                static_cast<unsigned long long>(DiskBake::building.misses.load()),static_cast<unsigned long long>(DiskBake::building.writes.load()),
                static_cast<unsigned long long>(GetTickCount64()-launchStarted));
            DiagnosticLine(line);
        }
        const auto& activeScene=campusScene?*campusScene:source;
        const auto result=desktop?RunDesktop(stop,frameSeen,window,input,activeScene,world,jobs,contributions,
                  logo,developer,navigation,diagnostic,orbitProfile,materialProfiles,building?&*building:nullptr):
            RunVr(stop,frameSeen,window,input,config,activeScene,world,jobs,
                  pair?WorldContributions(world,false,true):contributions,logo,navigation,diagnostic,
                  automatedDlss,solarCalibration,shuttleCalibration,
                  passageOwner,orbitProfile,materialProfiles,
                  shuttleAssets?&*shuttleAssets:nullptr,building?&*building:nullptr,buildingLoop,buildingPilot);
        if(result!=LoopResult::SwitchOutput)break;
        desktop=!desktop;diagnostic.ResetPending();
    }
    jobs.Stop();
    if(!orbitProfile.LocalFile().empty()){
        std::error_code error;
        if(!std::filesystem::exists(controlProfile,error)&&!error){
            if(!WriteLocalProfile(controlProfile,[&](std::ostream& out){
                out<<"# Remove # from the entries you want to customise.\n";
                std::ifstream defaults{std::filesystem::path(config)};
                for(std::string line;std::getline(defaults,line);)
                    if(line.starts_with("bind.")||line.starts_with("oasis."))out<<"# "<<line<<'\n';
            }))DiagnosticLine("controls_ini=echec_ecriture\r\n");
        }
    }
    if(!automatedDlss&&!sphereDiagnostic&&!materialProfiles.Save())DiagnosticLine("materials_ini=echec_ecriture\r\n");
    if(!orbitProfile.Flush())DiagnosticLine("orbit_profil=echec_ecriture\r\n");
    if(LaunchSessionMode()==SessionMode::Plafond){
        DiagnosticLine(diagnostic.plafondCompleted?"resultat=termine\r\n":"resultat=plafond_incomplet\r\n");
        return diagnostic.plafondCompleted?0:1;
    }
    if(LaunchSessionMode()==SessionMode::Sun){
        DiagnosticLine(diagnostic.sunCompleted?"resultat=termine\r\n":"resultat=sun_incomplet\r\n");
        return diagnostic.sunCompleted?0:1;
    }
    if(!capturePath.empty()){
        if(sphereDiagnostic&&!diagnostic.sphereBaselineCompleted){
            DiagnosticLine("resultat=reference_incomplete\r\n");
            return 1;
        }
        if(!diagnostic.captured){
            DiagnosticLine("capture=aucune\r\nresultat=sans_capture\r\n");
            return 1;
        }
        if(automatedDlss&&!diagnostic.dlssLoopCompleted){
            DiagnosticLine("resultat=sequence_dlss_incomplete\r\n");
            return 1;
        }
        DiagnosticLine("resultat=termine\r\n");
    }
    return 0;
}
void ReportSessionStall(ULONGLONG& lastStallReport,bool& stalled){
    if(!diagnosticLogPath.empty()){
        const auto now=GetTickCount64();
        const auto progress=sessionProgress.load(std::memory_order_acquire);
        const auto delay=progress&&now>=progress?now-progress:0;
        if(delay>=2000&&(!stalled||now-lastStallReport>=5000)){
            char line[192];
            std::snprintf(line,sizeof(line),"blocage_ms=%llu etape=%s derniere_image=%llu\r\n",delay,
                SessionStageName(sessionStage.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(sessionFrame.load(std::memory_order_relaxed)));
            DiagnosticLine(line);
            stalled=true;lastStallReport=now;
        }else if(stalled&&delay<2000){
            DiagnosticLine("reprise=progression\r\n");
            stalled=false;
        }
    }
}

LRESULT CALLBACK OutputWindowProcedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==OutputWindowMessage){ShowWindow(window,wparam?SW_SHOW:SW_HIDE);if(wparam)SetForegroundWindow(window);return 0;}
    if(message==WM_CLOSE){PostQuitMessage(0);return 0;}
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,wparam,lparam);
}
HWND CreateOutputWindow(HINSTANCE instance){
    WNDCLASSW windowClass{};windowClass.lpfnWndProc=OutputWindowProcedure;windowClass.hInstance=instance;
    windowClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);windowClass.lpszClassName=L"OASIS.Output";
    if(!RegisterClassW(&windowClass))Fail("Desktop window class");
    RECT area{0,0,1280,720};if(!AdjustWindowRect(&area,WS_OVERLAPPEDWINDOW,FALSE))Fail("Desktop window size");
    HWND window=CreateWindowExW(0,windowClass.lpszClassName,L"OASIS",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,area.right-area.left,area.bottom-area.top,nullptr,nullptr,instance,nullptr);
    if(!window)Fail("Desktop window");return window;
}
}
