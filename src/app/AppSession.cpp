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
    bool interactive{},requested{},attempted{},captured{},dlssLoopCompleted{},sphereBaselineCompleted{};
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
    unsigned frames{},wanted{},prepared{},submitted{};
    std::array<double,DlssDiagnosticCapture::StageCount> peaks{};
    void Record(const DlssDiagnosticCapture::Sample& sample,const DlssDiagnosticState& gpu,
                const GraphicsMetrics& metrics,uint64_t frame,const ShuttlePilot* shuttle=nullptr,double xrPeriodMs=0.){
        sessionFrame.store(frame,std::memory_order_relaxed);
        const auto now=GetTickCount64();
        if(!started)started=window=now;
        ++frames;
        wanted+=sample.wanted;prepared+=sample.prepared;submitted+=sample.submitted;
        for(size_t i=0;i<peaks.size();++i)peaks[i]=std::max(peaks[i],sample.ms[i]);
        if(now-window<2000)return;
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
            "clas_reserve_octets=%llu blas_reserve_octets=%llu dernier_echec=%s code=%d\r\n",
            static_cast<unsigned long long>(frame.input.frameId),forestFrames,forestRayFrames,forestClasFrames,forestNvapiFrames,
            forestPagesMin,forestPagesMax,forestClusters,forestTriangles,forestBytes,uint32_t(forestRequestFailed),
            forestDrawSamples,forestDrawPeak,forestRoutesVerified,forestRoutesFailed,cluster.publications,
            Mg2BackendName(cluster.lastPublished),cluster.clasMs,cluster.blasMs,
            static_cast<unsigned long long>(cluster.clasBytes),static_cast<unsigned long long>(cluster.blasBytes),
            cluster.lastFailure,cluster.buildCode);
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
    mappings.SetNavigationOptions((!automated||passageCalibration)&&!buildingLoop,!automated&&!buildingLoop);
    mappings.SetSpectaclePassage(logo&&!automated&&!scene.solarSystem,sphereCount);
    if(!automated&&!buildingLoop)mappings.SetOrbitTool(&orbit);
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
    if(sphereDiagnostic||diagnostic.mg2)graphics.EnableSphereBaselineTiming(diagnostic.mg2);
    if(diagnostic.mg2)graphics.EnableMg2Diagnostic();
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
        const bool liveInput=!automated&&!buildingTour&&!sessionTour&&!exitInitiated&&frame.input.focused;
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
            frame.headLockedDiagnostic=sphereDiagnostic||buildingTour.has_value();
            xr.LocateLate(frame,captureView);
            if(calibration)calibration->ApplyView(frame);
            const bool acquired=frame.tracked&&xr.AcquireSwapchains(frame);
            timing.ms[DlssDiagnosticCapture::Acquire]=dlssTrace.Elapsed(started);
            if(acquired){
                if(!sessionTour&&!buildingTour&&(!automated||captureLoopFrame||
                    (calibration&&calibration->CaptureReady()))&&!exitInitiated)
                    diagnostic.BeforeRender(graphics,automated,diagnostic.mg2&&building&&
                        frame.input.insideSphere==0u&&building->ResidentWorktops()>0u);
                if((feedback||sessionTour)&&!automated&&!exitInitiated)liveCapture.Prepare(graphics);
                started=stamp();
                if(sessionTraceEnabled)MarkSession(SessionStage::Render);
                if(sessionTraceEnabled)cutTrace.Collect(graphics);
                graphics.Render(frame);
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
            if(!sphereDiagnostic&&(!automatedDlss||dlssLoop.phase!=DlssLoop::Phase::Native))
                graphics.UpdateDlss(scaleStep);
            if(!sphereDiagnostic&&!exitInitiated&&!orbit.Apply(graphics,atmosphere))DiagnosticLine("orbit_profil=echec_ecriture\r\n");
            if(dlssTrace.enabled||sessionTraceEnabled){
                timing.ms[DlssDiagnosticCapture::Update]=dlssTrace.Elapsed(started);
                const auto state=graphics.GetDlssDiagnostic();
                if(dlssTrace.enabled)dlssTrace.Record(timing,before,state,frame.input.frameId);
                if(sessionTraceEnabled){
            
                    const auto metrics=graphics.GetMetrics();
                    sessionTrace.Record(timing,state,metrics,frame.input.frameId,
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
        if(buildingTour){
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
    if(sessionTraceEnabled)cutTrace.Collect(graphics,true);
    graphics.UnloadScene();
    if(!automated&&!shuttle.SaveSettings())DiagnosticLine("navette_profil=echec_ecriture\r\n");
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
            sessionTrace.Record(timing,graphics.GetDlssDiagnostic(),graphics.GetMetrics(),frame.input.frameId);
        }
        if(diagnostic.AfterRender(graphics,frame.submitted,true,frameSeen))break;
    }
    if(traceEnabled){
        MarkSession(SessionStage::Shutdown);
        graphics.WaitIdle();
        cutTrace.Collect(graphics,true);
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
    SceneData scene=LoadSceneGLTF(logo?AssetPath(L"oasis_logo.glb"):worldPath);
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
        }catch(const std::exception& error){
            DiagnosticFailure(error.what());
            if(mg2)return 1;
        }
    }
    if(!local.empty())materialProfiles.Install(scene);
    WorldRegistry world;
    std::optional<BuildingCampus> building;
    if(useCampus){
        const auto save=buildingLoop?std::filesystem::path{}:local.parent_path()/"buildings"/"oasis";
        try{building.emplace(AssetPath(L"buildings/oasis"),save);}
        catch(const std::exception& e){
            DiagnosticLine("megastructure=chargement_echoue\r\n");
            Fail(e.what());
        }
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
            }catch(const std::exception& error){
                DiagnosticFailure(error.what());
                if(mg2)return 1;
                mg2Catalog.reset();
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
        catch(const AssetImportError& error){const auto line=std::string("asset_unavailable source=navette_organes.glb reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());}
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
    if(LaunchSessionMode()==SessionMode::Forest&&!diagnostic.sessionForestSpawn){
        DiagnosticFailure("foret indisponible");return 1;
    }
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
                    mg2Catalog->PrepareGeometry();
                    DiagnosticLine("mg2_phase=pbr_begin\r\n");
                    pbr.emplace(mg2Catalog->AppendArchitecturalMaterials(*campusScene));
                    MarkDiagnosticStartupProgress();
                    DiagnosticLine("mg2_phase=worktop_begin\r\n");
                    worktop.emplace(mg2Catalog->AppendWorktop(*campusScene));
                    MarkDiagnosticStartupProgress();
                }
                mg2Catalog->ReleasePreparedGeometry();
                const auto assembled=GetTickCount64();
                const bool written=!restored&&CampusBake::Write(campusKey,*campusScene,*worktop,*pbr);
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
