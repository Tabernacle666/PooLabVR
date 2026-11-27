#include "AppSession.h"
#include "OasisMetric.h"
#include "SessionLaunch.h"
#include "TargetCalibration.h"
#include "DiskBake.h"
#include "CampusBake.h"
#include "ForestFloor.h"
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
#include <psapi.h>
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
namespace {
    // Fixed RAM journal: never wait or allocate to record a loop message.
    std::array<char,8u*1024u*1024u> loopLog{};
    // Walk uses the same records over a much longer route. Allocate its
    // journal once, before rendering, without changing loop's buffer.
    std::vector<char> walkLog;
    std::span<char> ForwardTourJournal()noexcept{
        if(!walkLog.empty())return std::span<char>(walkLog);
        return std::span<char>(loopLog);
    }
    SRWLOCK loopLogLock=SRWLOCK_INIT;
    size_t loopLogSize{};
    std::atomic<bool> loopLogLost{};
    bool bufferLoopLog{};
}
void DiagnosticLine(const char* text) noexcept {
    if(diagnosticLogPath.empty())return;
    if(bufferLoopLog){
        const size_t n=std::strlen(text);
        if(!TryAcquireSRWLockExclusive(&loopLogLock)){
            loopLogLost.store(true,std::memory_order_relaxed);return;
        }
        const auto journal=ForwardTourJournal();
        if(n<=journal.size()-loopLogSize){
            std::memcpy(journal.data()+loopLogSize,text,n);loopLogSize+=n;
        }else loopLogLost.store(true,std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&loopLogLock);
        return;
    }
    HANDLE file=CreateFileW(diagnosticLogPath.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,
                            nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return;
    DWORD written=0;WriteFile(file,text,DWORD(std::strlen(text)),&written,nullptr);CloseHandle(file);
}
bool FlushDiagnostic() noexcept {
    if(!bufferLoopLog||diagnosticLogPath.empty())return true;
    // Called after render.join(), or on forced termination. Never wait for a stalled writer.
    if(!TryAcquireSRWLockExclusive(&loopLogLock))return false;
    HANDLE file=CreateFileW(diagnosticLogPath.c_str(),GENERIC_WRITE,FILE_SHARE_READ,
                            nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    bool ok=false;
    if(file!=INVALID_HANDLE_VALUE){
        DWORD written=0;
        ok=WriteFile(file,ForwardTourJournal().data(),DWORD(loopLogSize),&written,nullptr)&&written==loopLogSize;
        if(loopLogLost.load(std::memory_order_relaxed)){
            constexpr char invalid[]="loop_journal=perte\r\n";
            DWORD ignored=0;WriteFile(file,invalid,DWORD(sizeof(invalid)-1),&ignored,nullptr);
            ok=false;
        }
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&loopLogLock);
    return ok;
}
void DiagnosticFailure(const char* text) noexcept {
    char line[1024];std::snprintf(line,sizeof(line),"erreur=%s\r\n",text);DiagnosticLine(line);
    if(!diagnosticCaptured.load(std::memory_order_acquire))DiagnosticLine("capture=aucune\r\n");
    FlushDiagnostic();
}
void DiagnosticStartupCheckpoint(const char* text) noexcept {
    char line[256];std::snprintf(line,sizeof(line),"startup_checkpoint=%.220s\r\n",text);
    DiagnosticLine(line);
    FlushDiagnostic();
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
    CloseHandle(file);diagnosticLogPath=path;failureSink=DiagnosticFailure;startupCheckpointSink=DiagnosticStartupCheckpoint;
    const auto mode=LaunchSessionMode();
    bufferLoopLog=IsSessionForwardTour(mode)||mode==SessionMode::Prism;
    if(mode==SessionMode::Walk)walkLog.resize(128u*1024u*1024u);
    if(mode==SessionMode::Prism)walkLog.resize(32u*1024u*1024u);
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
namespace {
// The Win32 message pump continues receiving keys while OpenXR blocks or
// the hidden output window has no focus. Only PageUP is retained.
std::atomic<unsigned> continentKeyPresses{};
bool continentRawKeyboard=false; // initialized before the render thread
bool continentKeyDown=false;     // window thread only
}
enum class LoopResult {Exit,SwitchOutput};
struct NavigationContinuity {Pose pose{};bool valid{},inside{};};

namespace {
// World-space poses fitted to the OASIS loop capture in 3d9fc54.
constexpr XRCaptureView OasisDiagnosticView{
    {{-0.07745679603f,49.23613101f,1.856725338f},{.03100724f,.01551041f,-.00648089f,.99937779f}},
    {-.91241771f,.79412581f,.84459636f,-.87909295f}
};
constexpr Pose OasisDiagnosticLeftGrip{
    {-0.06327473603f,49.04086387f,2.284498528f},{-.61197879f,.55731763f,-.47334697f,-.30136633f}
};
constexpr Pose OasisDiagnosticRightGrip{
    {0.198996584f,49.01113998f,2.377488348f},{-.76814861f,-.07239576f,-.19798771f,.60457210f}
};
constexpr Pose OasisDiagnosticRightAim{
    {0.210261404f,49.06092839f,2.367577838f},{-.11566606f,-.19118999f,-.08880692f,.97066013f}
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
    bool interactive{},requested{},attempted{},captured{},dlssLoopCompleted{},sphereBaselineCompleted{},sunCompleted{};
    int64_t firstFrame{};
    bool mg2{},loopCompleted{},prismCompleted{};
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
    bool opticalConfigLogged{},depthEnabled{},loopTiming{};
    // Only loop allocates this bounded capture buffer, before the first frame.
    // No formatting, sorting, allocation or new GPU work is needed per record.
    static constexpr size_t LoopCapacity=8192,WalkCapacity=65536;
    static constexpr size_t LoopPassCount=static_cast<size_t>(DirectGpuPass::Count)+2;
    struct LoopFrame {
        uint64_t id{};
        int64_t completed{};
        double route{},interval{-1.};
        unsigned phase{};
        bool projection{},details{},gpuMatched{},drawMatched{},depthMatched{};
        uint32_t sphere{Invalid},cameraTiles{},cameraInstances{},forestPages{},forestClusters{},
            forestTriangles{},forestBytes{},forestRayPages{},forestDrawClusters{},forestBackend{},
            candidates{},routed{},triangleBudget{},publications{},raySlots{},probeDispatches{};
        bool sceneRaster{},sphereInView{},forestFailed{},dxrEnabled{},dxrQueries{},tlasWork{},
            dynamicBlasWork{},forestPending{};
        uint32_t width{},height{},outputWidth{},outputHeight{},views{},reflections{},solar{},aa{},vrs{},dlss{};
        uint64_t vramUsed{},vramBudget{};
        std::array<double,DlssDiagnosticCapture::StageCount> cpu{};
        float residentMs{-1.f},mg2Ms{-1.f},rayMs{-1.f},graphMs{-1.f},publishMs{-1.f};
        SphereGpuTimings gpu;
        std::array<uint64_t,14> depth{};
    };
    std::vector<LoopFrame> loopRecords;
    uint64_t loopFirstFrame{},loopLastFrame{},loopSubmissions{},loopBegun{};
    uint64_t loopGpuDuplicates{},loopGpuUnexpected{},loopGpuNonProjection{},loopDrawDuplicates{},loopDepthDuplicates{};
    int64_t loopFirstQpc{},loopLastQpc{},loopEndQpc{};
    bool loopIntervalOverflow{},loopRouteFinished{},loopSawInterior{};
    double loopDuration{SessionTourSeconds};
    void BeginLoop(SessionMode mode){
        loopDuration=SessionForwardDuration(mode);
        loopRecords.resize(mode==SessionMode::Walk?WalkCapacity:LoopCapacity);
        loopTiming=true;
    }
    LoopFrame* LoopLookup(uint64_t id){
        if(!loopTiming||!loopFirstFrame||id<loopFirstFrame||id>loopLastFrame||
           id-loopFirstFrame>=loopRecords.size())return nullptr;
        auto& r=loopRecords[size_t(id-loopFirstFrame)];
        return r.id==id?&r:nullptr;
    }
    void LoopSubmitted(const XRFrame& frame,int64_t completed,double routeSeconds){
        if(!loopTiming||loopRouteFinished)return;
        const bool projection=frame.begun&&frame.ready&&frame.tracked&&frame.submitted&&!frame.captureOnly;
        if(!loopFirstQpc&&!projection)return;
        ++loopBegun;
        const auto id=frame.input.frameId;
        if(!loopFirstQpc){loopFirstQpc=completed;loopFirstFrame=id;}
        if(id<loopFirstFrame||id-loopFirstFrame>=loopRecords.size()||
           (loopLastFrame&&id<=loopLastFrame)){
            loopIntervalOverflow=true;
        }else{
            loopLastFrame=id;
            auto& r=loopRecords[size_t(id-loopFirstFrame)];
            r.id=id;r.completed=completed;r.route=routeSeconds;r.projection=projection;
            r.sphere=frame.input.insideSphere;
            r.phase=SessionTourPhase(routeSeconds,loopDuration);
            if(projection){
                if(loopLastQpc)r.interval=1000.*QpcSeconds(completed-loopLastQpc);
                r.cameraTiles=frame.cameraTiles;r.cameraInstances=frame.cameraInstances;
                r.forestPages=frame.forestPages;r.forestClusters=frame.forestClusters;
                r.forestTriangles=frame.forestTriangles;r.forestBytes=frame.forestGeometryBytes;
                r.forestRayPages=frame.forestRayPages;r.forestBackend=uint32_t(frame.forestRayBackend);
                r.sceneRaster=frame.sceneRaster;r.sphereInView=frame.sphereInView;
                r.forestFailed=frame.forestRequestFailed;
            }
            // Delayed draw count belongs to its original image, not the current one.
            if(frame.forestDrawSampleFrame){
                if(auto* owner=LoopLookup(frame.forestDrawSampleFrame)){
                    if(owner->drawMatched)++loopDrawDuplicates;
                    else{owner->drawMatched=true;owner->forestDrawClusters=frame.forestDrawClusters;}
                }
            }
        }
        if(projection){loopLastQpc=completed;++loopSubmissions;}
        if(routeSeconds>=loopDuration){
            loopRouteFinished=true;loopEndQpc=completed;
        }
    }
    void LoopDetails(uint64_t frame,const DlssDiagnosticCapture::Sample& cpu,
                     const GraphicsMetrics& metrics,const Mg2ClusterStatus& cluster,
                     const RenderingState& state,uint32_t views){
        auto* r=LoopLookup(frame);
        if(!r||!r->projection||r->details||state.frame!=frame||cluster.traceRenderFrame!=frame)return;
        r->details=true;r->cpu=cpu.ms;
        r->residentMs=cluster.traceResidentMs;r->mg2Ms=cluster.traceForestMs;
        r->rayMs=cluster.traceRayMs;r->graphMs=cluster.traceGraphMs;r->publishMs=cluster.traceForestPublishMs;
        r->candidates=cluster.selectionCandidates;r->routed=cluster.routedInstances;
        r->publications=cluster.publications;
        r->raySlots=state.dxrRaySlots;r->probeDispatches=state.dxrProbeDispatches;
        r->dxrEnabled=state.dxrEnabled;r->dxrQueries=state.dxrQueries;r->tlasWork=state.dxrTlasWork;
        r->dynamicBlasWork=state.dxrDynamicBlasWork;r->forestPending=state.dxrForestBuildPending;
        r->width=state.width;r->height=state.height;r->outputWidth=state.outputWidth;r->outputHeight=state.outputHeight;
        r->views=views;r->reflections=uint32_t(state.applied.reflections);r->solar=uint32_t(state.applied.solarVisibility);
        r->aa=uint32_t(state.applied.antialiasing);r->vrs=uint32_t(state.applied.peripheral);
        r->dlss=uint32_t(state.dlss);r->vramUsed=metrics.vramUsed;r->vramBudget=metrics.vramBudget;
    }
    void CollectLoopGpu(const SphereGpuTimings& s){
        if(!loopTiming||!loopFirstFrame||s.frame<loopFirstFrame||s.frame>loopLastFrame)return;
        auto* r=LoopLookup(s.frame);
        if(!r){++loopGpuUnexpected;return;}
        if(!r->projection){++loopGpuNonProjection;return;}
        if(r->gpuMatched){++loopGpuDuplicates;return;}
        r->gpuMatched=true;r->gpu=s;
    }
    void CollectLoopDepth(const DepthDiagnosticSample& s){
        if(auto* r=LoopLookup(s.frame)){
            if(r->depthMatched)++loopDepthDuplicates;
            else{r->depthMatched=true;r->depth=s.counts;}
        }
    }
    bool FinishLoop(){
        struct PassStats {
            double sum{},peak{};
            uint64_t n{};
            void Add(double value){
                if(value<0.||!std::isfinite(value))return;
                sum+=value;peak=std::max(peak,value);++n;
            }
            double Mean()const{return n?sum/double(n):-1.;}
        };
        struct PhaseStats {
            uint64_t frames{},submitted{},matched{},details{},draws{},depths{},tlas{},dynamicBlas{},queries{},pending{},probes{};
            uint64_t first{},last{};
            double duration{};
            std::vector<double> intervals;
            std::array<PassStats,LoopPassCount> passes{};
            PassStats directSpan,computeSpan,resident,render,ray,mg2,graph;
            std::array<PassStats,10> geometry{};
        };
        std::array<PhaseStats,4> phases;
        const char* phaseNames[]={"approche","interieur","sortie","total"};
        constexpr const char* names[]={"Present","Membrane.Field","Opaque.Tail","Transparent","ResolveColor",
            "ReflectionPost","ReflectionComposite","TLAS.Build","TLAS.Update","Opaque","Opaque.Resident",
            "CpuScene.Depth","Depth.Remaining","Depth.Cut","Depth.Occluders","Solar.Visibility","Raster.Cull","Raster.DirectTotal","DXR.DynamicBLAS","Direct.Total","Compute.Total"};
        static_assert(std::size(names)==LoopPassCount);
        for(const auto& r:loopRecords){
            if(!r.id)continue;
            for(unsigned phase:{r.phase,3u}){
                auto& p=phases[phase];
                if(!p.first)p.first=r.id;
                p.last=r.id;++p.frames;
                if(!r.projection)continue;
                ++p.submitted;
                if(r.interval>=0.){p.intervals.push_back(r.interval);p.duration+=r.interval;}
                if(r.gpuMatched&&r.gpu.directMs>=0.f&&std::isfinite(r.gpu.directMs)){
                    ++p.matched;
                    for(size_t i=0;i<r.gpu.directPassMs.size();++i)p.passes[i].Add(r.gpu.directPassMs[i]);
                    p.passes[LoopPassCount-2].Add(r.gpu.directMs);
                    // No compute batches is distinct from a measured zero.
                    if(r.gpu.computeSpanMs>=0.f)p.passes[LoopPassCount-1].Add(r.gpu.computeMs);
                    p.directSpan.Add(r.gpu.directSpanMs);p.computeSpan.Add(r.gpu.computeSpanMs);
                }
                if(r.drawMatched){++p.draws;p.geometry[9].Add(r.forestDrawClusters);}
                if(r.depthMatched)++p.depths;
                if(!r.details)continue;
                ++p.details;p.tlas+=r.tlasWork;p.dynamicBlas+=r.dynamicBlasWork;p.queries+=r.dxrQueries;
                p.pending+=r.forestPending;p.probes+=r.probeDispatches;
                p.resident.Add(r.residentMs);p.render.Add(r.cpu[DlssDiagnosticCapture::Render]);
                p.ray.Add(r.rayMs);p.mg2.Add(r.mg2Ms);p.graph.Add(r.graphMs);
                const double values[]={double(r.cameraTiles),double(r.cameraInstances),double(r.forestPages),
                    double(r.forestClusters),double(r.forestTriangles),double(r.forestBytes),double(r.candidates),
                    double(r.routed),double(r.raySlots)};
                for(size_t i=0;i<std::size(values);++i)p.geometry[i].Add(values[i]);
            }
        }
        // The terminal unsubmitted interval, if any, still consumes real time.
        const double tail=loopLastQpc&&loopEndQpc>loopLastQpc?1000.*QpcSeconds(loopEndQpc-loopLastQpc):0.;
        if(auto* last=LoopLookup(loopLastFrame))phases[last->phase].duration+=tail;
        phases[3].duration+=tail;
        auto& total=phases[3];
        const double duration=loopFirstQpc&&loopEndQpc>loopFirstQpc?QpcSeconds(loopEndQpc-loopFirstQpc):0.;
        const uint64_t intervals=loopSubmissions?loopSubmissions-1u:0u;
        const auto coverage=[](const PhaseStats& p){return p.submitted?double(p.matched)/double(p.submitted):0.;};
        const auto quantile=[](const std::vector<double>& values,size_t pct){
            return values.empty()?-1.:values[(values.size()*pct+99u)/100u-1u];
        };
        for(auto& p:phases)std::sort(p.intervals.begin(),p.intervals.end());
        const bool phaseCoverage=std::all_of(phases.begin(),phases.end(),[&](const PhaseStats& p){
            return p.submitted&&p.details==p.submitted&&coverage(p)>=.90;
        });
        const bool valid=loopRouteFinished&&duration>0.&&intervals>0&&!loopIntervalOverflow&&
            total.intervals.size()==intervals&&total.submitted==loopSubmissions&&total.frames==loopBegun&&
            phaseCoverage&&!loopGpuDuplicates&&!loopGpuUnexpected&&!loopDrawDuplicates&&!loopDepthDuplicates&&
            total.passes[static_cast<size_t>(DirectGpuPass::OpaqueResident)].n>0&&
            total.passes[static_cast<size_t>(DirectGpuPass::CpuDepth)].n>0;
        char line[2048];
        std::snprintf(line,sizeof(line),
            "loop_resultat valide=%u duree_reelle_s=%.6f debuts=%llu soumises=%llu intervalles=%llu fps_soumis=%.3f moyenne_ms=%.3f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f gpu_couverture=%.4f premiere_image=%llu derniere_image=%llu\r\n",
            unsigned(valid),duration,(unsigned long long)loopBegun,(unsigned long long)loopSubmissions,
            (unsigned long long)intervals,duration>0.?double(intervals)/duration:0.,
            duration>0.&&intervals?1000.*duration/double(intervals):-1.,quantile(total.intervals,50),
            quantile(total.intervals,95),quantile(total.intervals,99),quantile(total.intervals,100),
            coverage(total),(unsigned long long)loopFirstFrame,(unsigned long long)loopLastFrame);
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "loop_integrite gpu_associes=%llu gpu_manquants=%llu gpu_dupliques=%llu gpu_sans_image=%llu gpu_non_soumis=%llu cpu_manquants=%llu draw_dupliques=%llu depth_dupliques=%llu tampon_depasse=%u phases_valides=%u queue_fin_ms=%.3f\r\n",
            (unsigned long long)total.matched,(unsigned long long)(total.submitted-total.matched),
            (unsigned long long)loopGpuDuplicates,(unsigned long long)loopGpuUnexpected,(unsigned long long)loopGpuNonProjection,
            (unsigned long long)(total.submitted-total.details),(unsigned long long)loopDrawDuplicates,
            (unsigned long long)loopDepthDuplicates,unsigned(loopIntervalOverflow),unsigned(phaseCoverage),tail);
        DiagnosticLine(line);
        for(unsigned phase=0;phase<phases.size();++phase){
            const auto& p=phases[phase];const auto n=p.intervals.size();
            std::snprintf(line,sizeof(line),
                "loop_phase=%s premiere_image=%llu derniere_image=%llu debuts=%llu soumises=%llu intervalles=%zu duree_ms=%.3f fps_soumis=%.3f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f gpu_couverture=%.4f draw_echantillons=%llu depth_echantillons=%llu\r\n",
                phaseNames[phase],(unsigned long long)p.first,(unsigned long long)p.last,(unsigned long long)p.frames,
                (unsigned long long)p.submitted,n,p.duration,p.duration>0.?1000.*double(n)/p.duration:0.,
                quantile(p.intervals,50),quantile(p.intervals,95),quantile(p.intervals,99),quantile(p.intervals,100),
                coverage(p),(unsigned long long)p.draws,(unsigned long long)p.depths);
            DiagnosticLine(line);
            for(size_t i=0;i<LoopPassCount;++i){
                const auto& s=p.passes[i];
                if(phase==3u)std::snprintf(line,sizeof(line),"loop_gpu passe=%s echantillons=%llu moyenne_ms=%.4f pic_ms=%.4f\r\n",
                    names[i],(unsigned long long)s.n,s.Mean(),s.n?s.peak:-1.);
                else std::snprintf(line,sizeof(line),"loop_phase_gpu phase=%s passe=%s echantillons=%llu moyenne_ms=%.4f pic_ms=%.4f\r\n",
                    phaseNames[phase],names[i],(unsigned long long)s.n,s.Mean(),s.n?s.peak:-1.);
                DiagnosticLine(line);
            }
            std::snprintf(line,sizeof(line),
                "loop_phase_cpu phase=%s render_moy_ms=%.3f render_pic_ms=%.3f resident_moy_ms=%.3f resident_pic_ms=%.3f mg2_moy_ms=%.3f mg2_pic_ms=%.3f ray_moy_ms=%.3f ray_pic_ms=%.3f graph_moy_ms=%.3f graph_pic_ms=%.3f direct_span_moy_ms=%.3f direct_span_pic_ms=%.3f compute_span_moy_ms=%.3f compute_span_pic_ms=%.3f\r\n",
                phaseNames[phase],p.render.Mean(),p.render.n?p.render.peak:-1.,p.resident.Mean(),p.resident.n?p.resident.peak:-1.,
                p.mg2.Mean(),p.mg2.n?p.mg2.peak:-1.,p.ray.Mean(),p.ray.n?p.ray.peak:-1.,
                p.graph.Mean(),p.graph.n?p.graph.peak:-1.,p.directSpan.Mean(),p.directSpan.n?p.directSpan.peak:-1.,
                p.computeSpan.Mean(),p.computeSpan.n?p.computeSpan.peak:-1.);
            DiagnosticLine(line);
            std::snprintf(line,sizeof(line),
                "loop_phase_dxr phase=%s frames=%llu queries=%llu tlas_work=%llu dynamic_blas_work=%llu mg2_pending=%llu probes_dispatches=%llu\r\n",
                phaseNames[phase],(unsigned long long)p.details,(unsigned long long)p.queries,(unsigned long long)p.tlas,
                (unsigned long long)p.dynamicBlas,(unsigned long long)p.pending,(unsigned long long)p.probes);
            DiagnosticLine(line);
            constexpr const char* geometryNames[]={"camera_tiles","camera_instances","mg2_pages","mg2_clusters","mg2_triangles",
                "mg2_geometry_bytes","mg2_selection_candidates","mg2_routed_instances","dxr_ray_slots","mg2_draw_clusters_sampled"};
            for(size_t i=0;i<std::size(geometryNames);++i){
                const auto& s=p.geometry[i];
                std::snprintf(line,sizeof(line),"loop_phase_geometry phase=%s compteur=%s echantillons=%llu moyenne=%.2f pic=%.0f\r\n",
                    phaseNames[phase],geometryNames[i],(unsigned long long)s.n,s.Mean(),s.n?s.peak:-1.);
                DiagnosticLine(line);
            }
        }
        // Raw joined rows enable later analysis without changing this benchmark.
        for(const auto& r:loopRecords){
            if(!r.id)continue;
            const bool cpu=r.details,gpu=r.gpuMatched;
            const auto iv=[&](uint64_t v){return cpu?static_cast<long long>(v):-1ll;};
            const auto cv=[&](size_t i){return cpu?r.cpu[i]:-1.;};
            std::snprintf(line,sizeof(line),
                "loop_image=%llu phase=%s trajet_s=%.6f t_ms=%.3f sphere=%u soumis=%u cpu_valide=%u gpu_associe=%u intervalle_ms=%.3f "
                "xr_wait_ms=%.3f xr_begin_ms=%.3f prep_ms=%.3f acquire_ms=%.3f render_ms=%.3f end_ms=%.3f options_ms=%.3f resident_cpu_ms=%.3f mg2_cpu_ms=%.3f ray_cpu_ms=%.3f graph_cpu_ms=%.3f mg2_publish_cpu_ms=%.3f "
                "direct_actif_ms=%.4f direct_span_ms=%.4f compute_actif_ms=%.4f compute_span_ms=%.4f opaque_resident_ms=%.4f depth_ms=%.4f tlas_build_ms=%.4f tlas_update_ms=%.4f reflection_post_ms=%.4f "
                "scene_raster=%lld sphere_visible=%lld camera_tiles=%lld camera_instances=%lld mg2_pages=%lld mg2_clusters=%lld mg2_triangles=%lld mg2_geometry_bytes=%lld mg2_ray_pages=%lld mg2_backend=%lld mg2_request_failed=%lld mg2_selection_candidates=%lld mg2_routed_instances=%lld mg2_triangle_budget=%lld mg2_publications=%lld "
                "dxr_enabled=%lld dxr_queries=%lld dxr_ray_slots=%lld dxr_tlas_work=%lld dxr_dynamic_blas_work=%lld dxr_probes_dispatches=%lld mg2_pending=%lld mg2_draw_clusters_sampled=%lld "
                "width=%lld height=%lld output_width=%lld output_height=%lld views=%lld reflections=%lld solar=%lld aa=%lld vrs=%lld dlss_status=%lld vram_mio=%lld budget_mio=%lld\r\n",
                (unsigned long long)r.id,phaseNames[r.phase],r.route,1000.*QpcSeconds(r.completed-loopFirstQpc),
                r.sphere,unsigned(r.projection),unsigned(cpu),unsigned(gpu),r.interval,
                cv(0),cv(1),cv(2),cv(3),cv(4),cv(5),cv(6),r.residentMs,r.mg2Ms,r.rayMs,r.graphMs,r.publishMs,
                r.gpu.directMs,r.gpu.directSpanMs,r.gpu.computeSpanMs>=0.f?r.gpu.computeMs:-1.f,r.gpu.computeSpanMs,
                r.gpu.PassMs(DirectGpuPass::OpaqueResident),r.gpu.PassMs(DirectGpuPass::CpuDepth),
                r.gpu.PassMs(DirectGpuPass::TlasBuild),r.gpu.PassMs(DirectGpuPass::TlasUpdate),r.gpu.PassMs(DirectGpuPass::ReflectionPost),
                iv(r.sceneRaster),iv(r.sphereInView),iv(r.cameraTiles),iv(r.cameraInstances),iv(r.forestPages),iv(r.forestClusters),
                iv(r.forestTriangles),iv(r.forestBytes),iv(r.forestRayPages),iv(r.forestBackend),iv(r.forestFailed),
                iv(r.candidates),iv(r.routed),iv(r.triangleBudget),iv(r.publications),iv(r.dxrEnabled),iv(r.dxrQueries),iv(r.raySlots),
                iv(r.tlasWork),iv(r.dynamicBlasWork),iv(r.probeDispatches),iv(r.forestPending),
                r.drawMatched?static_cast<long long>(r.forestDrawClusters):-1ll,
                iv(r.width),iv(r.height),iv(r.outputWidth),iv(r.outputHeight),iv(r.views),iv(r.reflections),iv(r.solar),
                iv(r.aa),iv(r.vrs),iv(r.dlss),iv(r.vramUsed>>20),iv(r.vramBudget>>20));
            DiagnosticLine(line);
            if(r.depthMatched){
                std::snprintf(line,sizeof(line),
                    "loop_depth_image=%llu candidates=%llu occluders=%llu occluded=%llu submitted=%llu meshlets_native=%llu meshlets_mg2=%llu meshlets_rejected=%llu color_only=%llu\r\n",
                    (unsigned long long)r.id,(unsigned long long)r.depth[0],(unsigned long long)r.depth[1],
                    (unsigned long long)r.depth[2],(unsigned long long)r.depth[3],(unsigned long long)r.depth[4],
                    (unsigned long long)r.depth[5],(unsigned long long)r.depth[6],(unsigned long long)r.depth[7]);
                DiagnosticLine(line);
            }
        }
        return valid;
    }
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
            CollectLoopDepth(s);
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
            CollectLoopGpu(s);
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
        if(loopTiming)LoopDetails(frame,sample,metrics,stallCluster,graphics.GetRenderingState(),views);
        const bool publication=stallCluster.publications!=lastStallPublications;
        const bool currentRender=stallCluster.traceRenderFrame==frame;
        const bool slow=gapMs>=60.||sample.ms[DlssDiagnosticCapture::Render]>=40.;
        if(slow||publication||(currentRender&&(stallCluster.traceHudTick||stallCluster.traceForestRequestMs>0.f||
            stallCluster.traceForestRayCompared||stallCluster.traceForestRayDirty||stallCluster.traceForestShadowDirty))){
            const double delta=slow&&lastStallQpc?1000.*QpcSeconds(eventQpc-lastStallQpc):-1.;
            char event[1024];
            std::snprintf(event,sizeof(event),
                "cadence t_ms=%.3f image=%llu pause=%u intervalle_ms=%.3f depuis_pause_ms=%.3f "
                "xr_wait_ms=%.3f xr_begin_ms=%.3f prep_ms=%.3f acquire_ms=%.3f render_ms=%.3f end_ms=%.3f options_ms=%.3f "
                "resident_cpu_ms=%.3f mg2_cpu_ms=%.3f ray_cpu_ms=%.3f graph_cpu_ms=%.3f "
                "mg2_publish_cpu_ms=%.3f mg2_retire_cpu_ms=%.3f mg2_request_cpu_ms=%.3f mg2_ray_compared=%u mg2_ray_dirty=%u mg2_shadow_dirty=%u hud_1hz=%u publication=%u publications=%u clas_construits=%u clas_reutilises=%u soumis=%u\r\n",
                1000.*QpcSeconds(eventQpc-stallOriginQpc),static_cast<unsigned long long>(frame),unsigned(slow),gapMs,delta,
                sample.ms[DlssDiagnosticCapture::Wait],sample.ms[DlssDiagnosticCapture::Begin],
                sample.ms[DlssDiagnosticCapture::Prepare],sample.ms[DlssDiagnosticCapture::Acquire],
                sample.ms[DlssDiagnosticCapture::Render],sample.ms[DlssDiagnosticCapture::End],sample.ms[DlssDiagnosticCapture::Update],
                currentRender?stallCluster.traceResidentMs:-1.f,currentRender?stallCluster.traceForestMs:-1.f,
                currentRender?stallCluster.traceRayMs:-1.f,currentRender?stallCluster.traceGraphMs:-1.f,
                currentRender?stallCluster.traceForestPublishMs:-1.f,currentRender?stallCluster.traceForestRetireMs:-1.f,
                currentRender?stallCluster.traceForestRequestMs:-1.f,
                currentRender?stallCluster.traceForestRayCompared:0u,currentRender?stallCluster.traceForestRayDirty:0u,
                currentRender?stallCluster.traceForestShadowDirty:0u,
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
        char line[1280];
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
            "variantes=%u meshlets_raster_residents=%u instances_routees=%u clas_reserve_octets=%llu blas_reserve_octets=%llu "
            "mg2_select_worker_ms=%.3f mg2_pack_worker_ms=%.3f "
            "mg2_build_worker_ms=%.3f mg2_allocate_worker_ms=%.3f mg2_upload_worker_ms=%.3f "
            "mg2_submit_worker_ms=%.3f mg2_clas_gpu_ms=%.3f mg2_blas_gpu_ms=%.3f "
            "mg2_wait_worker_ms=%.3f mg2_validate_worker_ms=%.3f mg2_cleanup_worker_ms=%.3f "
            "mg2_candidate_worker_ms=%.3f mg2_retire_worker_ms=%.3f mg2_retired=%u mg2_retire_backlog=%u "
            "mg2_unchanged=%u mg2_discarded=%u mg2_generation_age_ms=%.3f dernier_echec=%s code=%d\r\n",
            static_cast<unsigned long long>(frame),Mg2BackendName(cluster.lastPublished),
            cluster.publications,cluster.routeOnlyPublications,cluster.clasBuilt,cluster.clasReused,
            cluster.clasDuplicate,cluster.generationReusePublications,cluster.variants,cluster.rasterMeshlets,cluster.routedInstances,
            static_cast<unsigned long long>(cluster.clasBytes),static_cast<unsigned long long>(cluster.blasBytes),
            cluster.selectWorkerMs,cluster.packWorkerMs,cluster.buildWorkerMs,cluster.allocateWorkerMs,
            cluster.uploadWorkerMs,cluster.submitWorkerMs,cluster.clasMs,cluster.blasMs,
            cluster.waitWorkerMs,cluster.validateWorkerMs,cluster.cleanupWorkerMs,
            cluster.candidateWorkerMs,cluster.retireWorkerMs,cluster.retiredGenerations,cluster.retireBacklog,
            cluster.unchangedSelections,cluster.discardedSelections,cluster.generationAgeMs,cluster.lastFailure,cluster.buildCode);
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_gpu_selection image=%llu dispatches=%u worker_gpu_roundtrip_ms=%.3f readback_bytes=%llu compact_bytes=%llu unique_cuts=%llu visited_nodes=%llu pruned_subtrees=%llu\r\n",
            static_cast<unsigned long long>(frame),cluster.gpuSelectDispatches,cluster.gpuSelectWorkerMs,
            static_cast<unsigned long long>(cluster.gpuSelectReadbackBytes),
            static_cast<unsigned long long>(cluster.gpuSelectCompactBytes),
            static_cast<unsigned long long>(cluster.gpuSelectUniqueCuts),
            static_cast<unsigned long long>(cluster.gpuSelectVisitedNodes),
            static_cast<unsigned long long>(cluster.gpuSelectPrunedNodes));
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_schedule image=%llu requests=%llu idle_skips=%llu retries=%llu failure_skips=%llu "
            "interval_ms=%.3f min_interval_ms=%.3f lead_ms=%.3f prediction_ms=%.3f\r\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(cluster.scheduleRequests),
            static_cast<unsigned long long>(cluster.scheduleIdleSkips),
            static_cast<unsigned long long>(cluster.scheduleRetries),
            static_cast<unsigned long long>(cluster.scheduleFailureSkips),
            cluster.scheduleIntervalMs,cluster.scheduleMinIntervalMs,
            cluster.scheduleLeadMs,cluster.schedulePredictionMs);
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_input_registry image=%llu full_rebuild=%u changed_tiles=%llu scanned_objects=%llu scanned_statics=%llu\r\n",
            static_cast<unsigned long long>(frame),unsigned(cluster.inputFullRebuild),
            static_cast<unsigned long long>(cluster.inputTilesChanged),
            static_cast<unsigned long long>(cluster.inputObjectsScanned),
            static_cast<unsigned long long>(cluster.inputStaticsScanned));
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_gpu_registry image=%llu instances=%llu resident_bytes=%llu upload_bytes=%llu\r\n",
            static_cast<unsigned long long>(frame),static_cast<unsigned long long>(cluster.registryInstances),
            static_cast<unsigned long long>(cluster.registryGpuBytes),static_cast<unsigned long long>(cluster.registryUploadBytes));
        DiagnosticLine(line);
        // Reuse the existing periodic CPU-only journal; route metrics describe
        // the last completed selection, which may not yet be published.
        std::snprintf(line,sizeof(line),
            "mg2_coverage image=%llu candidats=%u eligibles=%u routes_selection=%u routes_publiees=%u\r\n",
            static_cast<unsigned long long>(frame),cluster.selectionCandidates,cluster.selectionEligible,
            cluster.selectionRouted,cluster.routedInstances);
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_memory image=%llu allocation_requise_octets=%llu disponible_octets=%llu politique=%s\r\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(cluster.buildRequiredBytes),
            static_cast<unsigned long long>(cluster.buildAvailableBytes),
            "erreur_projetee_budget_dxgi");
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_cpu_cache image=%llu source_octets=%llu raster_octets=%llu variantes=%llu hits_cumul=%llu misses_cumul=%llu evictions_cumul=%llu\r\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(cluster.sourceGeometryBytes),
            static_cast<unsigned long long>(cluster.rasterCacheBytes),
            static_cast<unsigned long long>(cluster.rasterCacheEntries),
            static_cast<unsigned long long>(cluster.rasterCacheHits),
            static_cast<unsigned long long>(cluster.rasterCacheMisses),
            static_cast<unsigned long long>(cluster.rasterCacheEvictions));
        DiagnosticLine(line);
        std::snprintf(line,sizeof(line),
            "mg2_gpu_cache image=%llu clas_entrees=%llu blas_entrees=%llu octets=%llu evictions_cumul=%llu blas_construits_cumul=%llu blas_reutilises_cumul=%llu clas_reference_evitees_cumul=%llu\r\n",
            static_cast<unsigned long long>(frame),
            static_cast<unsigned long long>(cluster.accelerationCacheClas),
            static_cast<unsigned long long>(cluster.accelerationCacheBlas),
            static_cast<unsigned long long>(cluster.accelerationCacheBytes),
            static_cast<unsigned long long>(cluster.accelerationCacheEvictions),
            static_cast<unsigned long long>(cluster.blasBuilt),
            static_cast<unsigned long long>(cluster.blasReused),
            static_cast<unsigned long long>(cluster.clasSkippedByBlasReuse));
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
        atmosphere.density=0.00652333334f;
        atmosphere.emissionReferencePower=16966.7263f;
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
        const float length=frame.input.solarTime<0.&&owner<SpectacleSphereCount?
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
                const float units=OasisSceneUnitsPerMeter;
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

// PRISM: readbacks are joined by the producing frame, never arrival block.
struct PrismBenchmark {
    // Legacy v2 profiles still implemented: 2=reflection_dxr_off,
    // 3=local_shading_off, 4=material_detail_off; retain for future audits.
    static constexpr std::array<uint32_t,4> Profiles{0u,1u,5u,6u};
    static constexpr std::array<const char*,4> Names{"full","solar_off","minimal_pixel","depth_only"};
    // Tree=1, logo=2, land/remainder=4. Camera-only ablation.
    static constexpr std::array<uint32_t,6> Masks{6u,5u,3u,1u,2u,4u};
    static constexpr std::array<const char*,6> MaskNames{"notree","nologo","noland","onlytree","onlylogo","onlyland"};
    static_assert(PrismBlocks==Profiles.size()*Masks.size());
    RenderingOptions baseline{};
    std::array<uint64_t,PrismBlocks> submitted{};
    int appliedBlock{-2},lastSubmitBlock{-2};
    int64_t lastSubmit{};
    void Start(Graphics& graphics){
        baseline=graphics.GetRenderingOptions();
        baseline.antialiasing=SceneAntialiasing::Native;
        baseline.peripheral=PeripheralShading::Auto;
        baseline.reflections=SceneReflections::Full;
        baseline.solarVisibility=SceneSolarVisibility::Adaptive;
        graphics.SetDlssScale(0.f);
        graphics.EnableDepthDiagnostics();
        DiagnosticLine("prism_version=3 walk_center_offset_m=16 warmup_s=6 blocks=24 block_s=3 walk_kmh=5 yaw_deg=0 pitch_deg=0 sun_time=8 dlss=off msaa=1 journal=ram gpu_join=frame_id scene_masks=camera_only ray_scene=unchanged land=non_tree_non_logo depth_only=scene_color_draws_off resident_sample_period=72\r\n");
        Apply(graphics,-1);
    }
    void Apply(Graphics& graphics,int block){
        if(block==appliedBlock||block>=int(PrismBlocks))return;
        appliedBlock=block;
        const unsigned b=unsigned(std::max(0,block)),profileIndex=b%Profiles.size(),maskIndex=b/Profiles.size();
        const auto profile=Profiles[profileIndex];
        auto options=baseline;
        if(profile==1u)options.solarVisibility=SceneSolarVisibility::NoShadows;
        // v2: if(profile==2u)options.reflections=SceneReflections::Ambient;
        graphics.SetPrismProfile(profile);
        graphics.SetPrismSceneMask(block<0?0u:Masks[maskIndex]);
        if(!graphics.SetRenderingOptions(options))Fail("PRISM rendering options");
        char line[256];
        std::snprintf(line,sizeof(line),"prism_block=%d profile=%s speed_kmh=5 target=forward scene=%s mask=%u\r\n",
            block+1,Names[profileIndex],block<0?"all":MaskNames[maskIndex],block<0?0u:Masks[maskIndex]);
        DiagnosticLine(line);
    }
    void Collect(Graphics& graphics){
        std::array<SphereGpuTimings,12> gpu{};
        const auto ng=graphics.ReadSphereGpuTimings(gpu);
        char line[1280];
        for(size_t i=0;i<ng;++i){
            const auto& s=gpu[i];
            std::snprintf(line,sizeof(line),"prism_gpu=%llu direct_ms=%.4f compute_ms=%.4f direct_span_ms=%.4f compute_span_ms=%.4f opaque_ms=%.4f resident_ms=%.4f tail_ms=%.4f transparent_ms=%.4f depth_ms=%.4f seed_ms=%.4f remaining_ms=%.4f tlas_build_ms=%.4f tlas_update_ms=%.4f reflection_post_ms=%.4f present_ms=%.4f solar_visibility_ms=%.4f raster_cull_ms=%.4f raster_direct_total_ms=%.4f gpu_frame_span_ms=%.4f gpu_image_span_ms=%.4f direct_compute_overlap_ms=%.4f dynamic_blas_ms=%.4f\r\n",
                static_cast<unsigned long long>(s.frame),s.directMs,s.computeMs,s.directSpanMs,s.computeSpanMs,
                s.PassMs(DirectGpuPass::Opaque),s.PassMs(DirectGpuPass::OpaqueResident),s.PassMs(DirectGpuPass::OpaqueTail),
                s.PassMs(DirectGpuPass::Transparent),s.PassMs(DirectGpuPass::CpuDepth),s.PassMs(DirectGpuPass::DepthOccluders),
                s.PassMs(DirectGpuPass::DepthRemaining),s.PassMs(DirectGpuPass::TlasBuild),s.PassMs(DirectGpuPass::TlasUpdate),
                s.PassMs(DirectGpuPass::ReflectionPost),s.PassMs(DirectGpuPass::Present),s.PassMs(DirectGpuPass::SolarVisibility),
                s.PassMs(DirectGpuPass::RasterCull),s.PassMs(DirectGpuPass::RasterTotal),
                s.gpuFrameSpanMs,s.gpuImageSpanMs,s.directComputeOverlapMs,s.PassMs(DirectGpuPass::DynamicBlas));
            DiagnosticLine(line);
        }
        std::array<OpticalDiagnosticSample,6> optical{};
        const auto no=graphics.ReadOpticalDiagnostics(optical);
        for(size_t i=0;i<no;++i){
            const auto& s=optical[i];
            std::snprintf(line,sizeof(line),"prism_optical=%llu sample_1_per_256=1 solar_requests=%llu solar_rays=%llu reflection_requests=%llu reflection_rays=%llu solar_shared=%llu solar_bypass=%llu solar_nonopaque_candidates=%llu reflection_nonopaque_candidates=%llu solar_prepass_rays=%llu solar_screen_shared=%llu solar_screen_rejected=%llu solar_stereo_reused=%llu solar_resolved_reads=%llu solar_resolve_rays=%llu solar_pixel_fallback=%llu solar_temporal_attempts=%llu solar_temporal_hits=%llu solar_temporal_misses=%llu solar_receivers_admitted=%llu solar_material_rejected=%llu solar_light_rejected=%llu\r\n",
                static_cast<unsigned long long>(s.frame),static_cast<unsigned long long>(s.counts[0]),
                static_cast<unsigned long long>(s.counts[2]),static_cast<unsigned long long>(s.counts[4]),
                static_cast<unsigned long long>(s.counts[5]),static_cast<unsigned long long>(s.counts[18]),
                static_cast<unsigned long long>(s.counts[20]),static_cast<unsigned long long>(s.counts[23]),
                static_cast<unsigned long long>(s.counts[24]),static_cast<unsigned long long>(s.counts[25]),
                static_cast<unsigned long long>(s.counts[26]),static_cast<unsigned long long>(s.counts[27]),
                static_cast<unsigned long long>(s.counts[28]),
                static_cast<unsigned long long>(s.counts[29]),static_cast<unsigned long long>(s.counts[30]),
                static_cast<unsigned long long>(s.counts[31]),static_cast<unsigned long long>(s.counts[32]),
                static_cast<unsigned long long>(s.counts[33]),static_cast<unsigned long long>(s.counts[34]),
                static_cast<unsigned long long>(s.counts[35]),static_cast<unsigned long long>(s.counts[36]),
                static_cast<unsigned long long>(s.counts[37]));
            DiagnosticLine(line);
        }
        std::array<DepthDiagnosticSample,6> depth{};
        const auto nd=graphics.ReadDepthDiagnostics(depth);
        for(size_t i=0;i<nd;++i){
            const auto& s=depth[i];
            std::snprintf(line,sizeof(line),"prism_depth=%llu candidates=%llu submitted=%llu native_meshlets=%llu mg2_meshlets=%llu rejected=%llu occluders=%llu occluded=%llu color_only=%llu raster_candidates=%llu raster_triangles=%llu raster_rejected_instances=%llu raster_rejected_meshlets=%llu depth_building_meshlets=%llu depth_forest_meshlets=%llu\r\n",
                static_cast<unsigned long long>(s.frame),static_cast<unsigned long long>(s.counts[0]),
                static_cast<unsigned long long>(s.counts[1]),static_cast<unsigned long long>(s.counts[2]),
                static_cast<unsigned long long>(s.counts[3]),static_cast<unsigned long long>(s.counts[4]),
                static_cast<unsigned long long>(s.counts[5]),static_cast<unsigned long long>(s.counts[6]),
                static_cast<unsigned long long>(s.counts[7]),static_cast<unsigned long long>(s.counts[8]),
                static_cast<unsigned long long>(s.counts[9]),static_cast<unsigned long long>(s.counts[10]),
                static_cast<unsigned long long>(s.counts[11]),static_cast<unsigned long long>(s.counts[12]),
                static_cast<unsigned long long>(s.counts[13]));
            DiagnosticLine(line);
        }
    }
    void Record(Graphics& graphics,const XRFrame& frame,const DlssDiagnosticCapture::Sample& timing,double elapsed){
        const int block=PrismBlock(elapsed);
        const auto now=QpcCounter();
        Collect(graphics);
        if(block<0||block>=int(PrismBlocks))return;
        double interval=-1.;
        if(frame.submitted){
            if(lastSubmit&&lastSubmitBlock==block)interval=1000.*QpcSeconds(now-lastSubmit);
            lastSubmit=now;lastSubmitBlock=block;++submitted[size_t(block)];
        }
        const auto metrics=graphics.GetMetrics();
        const auto mg2=graphics.GetMg2ClusterStatus();
        const auto state=graphics.GetRenderingState();
        char line[1536];
        std::snprintf(line,sizeof(line),"prism_image=%llu block=%d elapsed_s=%.6f submitted=%u interval_ms=%.4f prep_ms=%.3f render_ms=%.3f head_x=%.6f head_y=%.6f head_z=%.6f width=%u height=%u views=%u dlss=%u solar=%u reflections=%u camera_tiles=%u camera_instances=%u mg2_pages=%u mg2_clusters=%u mg2_triangles=%u mg2_publications=%u mg2_age_ms=%.3f mg2_pending=%u mg2_failure=%u vram_mio=%llu budget_mio=%llu cpu_source_bytes=%llu cpu_raster_bytes=%llu gpu_accel_bytes=%llu omm_materials=%u omm_native_triangles=%u omm_clas_clusters=%u omm_clas_triangles=%u omm_array_bytes=%llu ptlas_active=%u ptlas_slots=%u ptlas_capacity=%u ptlas_writes=%u ptlas_address_updates=%u solar_prepass_active=%u solar_history_reject_mask=%u solar_display_gap=%u canopy_immutable_bytes=%llu canopy_geometry_upload_bytes=%llu canopy_geometry_uploads=%llu\r\n",
            static_cast<unsigned long long>(frame.input.frameId),block+1,elapsed,unsigned(frame.submitted),interval,
            timing.ms[DlssDiagnosticCapture::Prepare],timing.ms[DlssDiagnosticCapture::Render],
            frame.input.head.p.x,frame.input.head.p.y,frame.input.head.p.z,state.width,state.height,frame.viewCount,
            unsigned(state.dlss),unsigned(state.applied.solarVisibility),unsigned(state.applied.reflections),
            frame.cameraTiles,frame.cameraInstances,frame.forestPages,frame.forestClusters,frame.forestTriangles,
            mg2.publications,mg2.generationAgeMs,unsigned(state.dxrForestBuildPending),unsigned(frame.forestRequestFailed),
            static_cast<unsigned long long>(metrics.vramUsed>>20),static_cast<unsigned long long>(metrics.vramBudget>>20),
            static_cast<unsigned long long>(mg2.sourceGeometryBytes),static_cast<unsigned long long>(mg2.rasterCacheBytes),
            static_cast<unsigned long long>(mg2.accelerationCacheBytes),mg2.ommMaterials,mg2.ommNativeTriangles,
            mg2.ommClasClusters,mg2.ommClasTriangles,static_cast<unsigned long long>(mg2.ommArrayBytes),
            unsigned(state.frame==frame.input.frameId&&state.ptlasActive),
            state.frame==frame.input.frameId?state.dxrRaySlots:0u,
            state.frame==frame.input.frameId?state.ptlasCapacity:0u,
            state.frame==frame.input.frameId?state.ptlasWrites:0u,
            state.frame==frame.input.frameId?state.ptlasAddressUpdates:0u,
            unsigned(state.frame==frame.input.frameId&&state.solarPrepassActive),
            state.frame==frame.input.frameId?state.solarHistoryRejectMask:0xffffffffu,
            unsigned(state.frame==frame.input.frameId&&state.solarDisplayGap),
            static_cast<unsigned long long>(mg2.immutableGeometryBytes),
            static_cast<unsigned long long>(mg2.geometryUploadBytes),
            static_cast<unsigned long long>(mg2.immutableGeometryUploads));
        DiagnosticLine(line);
        if(frame.submitted&&mg2.traceRenderFrame==frame.input.frameId){
            std::snprintf(line,sizeof(line),"prism_work=%llu resident_runs=%u resident_run_capacity=%u resident_instances=%u transparent_draws=%u resident_cpu_ms=%.4f mg2_cpu_ms=%.4f ray_cpu_ms=%.4f graph_cpu_ms=%.4f mg2_select_ms=%.4f mg2_pack_ms=%.4f mg2_routed=%u mg2_eligible=%u mg2_raster_meshlets=%u mg2_select_candidates=%u mg2_ray_dirty=%u mg2_ray_compared=%u ray_slots=%u probe_dispatches=%u output_width=%u output_height=%u canopy_ptlas_partitions_dirty=%u canopy_ptlas_lease_relocations=%u\r\n",
                static_cast<unsigned long long>(frame.input.frameId),frame.prismResidentRuns,frame.prismResidentRunCapacity,
                frame.prismResidentInstances,frame.prismTransparentDraws,mg2.traceResidentMs,mg2.traceForestMs,
                mg2.traceRayMs,mg2.traceGraphMs,mg2.selectWorkerMs,mg2.packWorkerMs,mg2.selectionRouted,mg2.selectionEligible,
                mg2.rasterMeshlets,mg2.selectionCandidates,mg2.traceForestRayDirty,mg2.traceForestRayCompared,
                state.dxrRaySlots,state.dxrProbeDispatches,state.outputWidth,state.outputHeight,
                mg2.tracePtlasDirtyPartitions,mg2.tracePtlasLeaseRelocations);
            DiagnosticLine(line);
            // CPU-only accounting; existing asynchronous diagnostic writer.
            std::snprintf(line,sizeof(line),"prism_submission=%llu resident_buckets=%u ordered_buckets=%u resident_pso_sets=%u resident_indirect_calls=%u resident_compaction_dispatches=%u resident_cached_lists=%u resident_draw_groups=%u resident_order_boundaries=%u resident_cpu_rejected=%u resident_cache_hits=%u resident_cache_misses=%u resident_record_ms=%.4f\r\n",
                static_cast<unsigned long long>(frame.input.frameId),frame.prismResidentRuns,
                frame.prismResidentOrderedBuckets,frame.prismResidentPsoSets,
                frame.prismResidentIndirectCalls,frame.prismResidentCompactionDispatches,
                frame.prismResidentCacheHits,frame.prismResidentIndirectCalls,
                frame.prismResidentOrderBoundaries,frame.prismResidentCpuRejected,
                frame.prismResidentCacheHits,frame.prismResidentCacheMisses,
                frame.prismResidentRecordMs);
            DiagnosticLine(line);
        }
        if(frame.forestDrawSampleFrame){
            const auto& c=frame.prismResidentSample;
            std::snprintf(line,sizeof(line),"prism_resident=%llu tree_meshlets=%u logo_meshlets=%u land_meshlets=%u tree_dispatches=%u logo_dispatches=%u land_dispatches=%u legacy_mg2_meshlets=%u\r\n",
                static_cast<unsigned long long>(frame.forestDrawSampleFrame),c[0],c[1],c[2],c[3],c[4],c[5],frame.forestDrawClusters);
            DiagnosticLine(line);
        }
    }
    bool Finish(Graphics& graphics,bool durationReached){
        Collect(graphics);
        bool valid=durationReached;
        for(unsigned b=0;b<PrismBlocks;++b){
            char line[128];std::snprintf(line,sizeof(line),"prism_block_end=%u submitted=%llu\r\n",b+1,
                static_cast<unsigned long long>(submitted[b]));
            DiagnosticLine(line);valid=valid&&submitted[b]>0;
        }
        DiagnosticLine(valid?"prism_result=complete\r\n":"prism_result=incomplete\r\n");
        graphics.SetPrismProfile(0u);
        graphics.SetPrismSceneMask(0u);
        graphics.SetRenderingOptions(baseline);
        return valid;
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
    const bool targetMode=LaunchSessionMode()==SessionMode::Target;
    if(targetMode&&(!scene.solarSystem||!building||!shuttleAssets||automated))Fail("Target requires OASIS campus and shuttle");
    const bool cosmosRooftop=LaunchSessionMode()==SessionMode::Cosmos&&scene.solarSystem&&building&&!automated;
    if(targetMode||cosmosRooftop)shuttle.SetParkingPosition(TargetCalibration::Parking(building->Pilot()));
    const bool sunTest=LaunchSessionMode()==SessionMode::Sun;
    if(sunTest&&(!scene.solarSystem||!building||automated))Fail("Fixed benchmark requires live OASIS campus");
    SunBenchmark sunBenchmark;
    const bool prismTest=LaunchSessionMode()==SessionMode::Prism;
    if(prismTest&&(!scene.solarSystem||!building||automated||diagnosticLogPath.empty()))Fail("PRISM requires live VR campus and diagnostic log");
    PrismBenchmark prismBenchmark;
    const uint32_t sphereCount=scene.sphereObjectOffsets.empty()?1u:uint32_t(scene.sphereObjectOffsets.size()-1u);
    const bool paired=sphereCount>1u;
    PostMessageW(window,OutputWindowMessage,0,0);
    auto atmosphere=DefaultAtmosphere(logo);
    if(scene.solarSystem){atmosphere.radius=0.f;atmosphere.density=0.f;atmosphere.membrane=false;atmosphere.forestAtmosphere=building!=nullptr;}
    const auto city=CityAtmosphere();
    OrbitTool orbit(profile,logo,atmosphere.density);
    if(building)orbit.SetBuilding(&building->Pilot());
    mappings.SetBuilding(buildingLoop?nullptr:building);
    std::optional<BuildingTour> buildingTour;
    if(buildingLoop)buildingTour.emplace(std::filesystem::path(diagnostic.path).parent_path());
    mappings.SetNavigationOptions((!automated||passageCalibration)&&!buildingLoop&&!sunTest&&!prismTest,!automated&&!buildingLoop&&!sunTest&&!prismTest);
    mappings.SetSpectaclePassage(logo&&!automated&&!scene.solarSystem,sphereCount);
    if(!automated&&!buildingLoop&&!sunTest&&!prismTest)mappings.SetOrbitTool(&orbit);
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
    const bool loopRequested=IsSessionForwardTour(LaunchSessionMode());
    if(sphereDiagnostic||diagnostic.mg2||sunTest||loopRequested||prismTest)
        graphics.EnableSphereBaselineTiming(diagnostic.mg2||sunTest||loopRequested||prismTest);
    if(diagnostic.mg2)graphics.EnableMg2Diagnostic();
    if(paired)graphics.EnableSpherePair(city,sphereCount);
    graphics.SetMaterialTuning(materialProfiles.Values());
    if(building)graphics.SetBuildingRevision(building->Revision(),building->Lights());
    MarkDiagnosticStartupProgress();
    DiagnosticLine("etape=chargement\r\n");FlushDiagnostic();
    graphics.Configure(xr,scene,jobs,contributions);
    DiagnosticLine(graphics.StartupCacheReport());
    StartupCheckpoint("graphics_configured");
    if(diagnostic.mg2){
        const auto cluster=graphics.GetMg2ClusterStatus();
        char line[256];
        std::snprintf(line,sizeof(line),
            "mg2_cluster_gpu demande=%s api=%s raison=%s code=%d pilote=%u adaptateur=%04x:%04x parties=%u pages_source=%u\r\n",
            cluster.request,Mg2BackendName(cluster.configured),cluster.capability,cluster.code,cluster.driver,
            cluster.adapterVendor,cluster.adapterDevice,cluster.meshes,cluster.sourcePages);
        DiagnosticLine(line);
    }
    graphics.SetDeveloperMode(!targetMode);
    orbit.Restore(graphics,atmosphere);
    if(sunTest)sunBenchmark.Start(graphics);
    if(prismTest)prismBenchmark.Start(graphics);
    DlssLoop dlssLoop;
    const bool sessionTraceEnabled=!automated&&!diagnosticLogPath.empty();
    SessionTrace sessionTrace;
    Mg2Trace mg2Trace;
    CutSessionTrace cutTrace;
    graphics.SetCutDiagnostic(sessionTraceEnabled&&!prismTest);
    graphics.SetOpticalDiagnostic(prismTest);
    DlssDiagnosticCapture dlssTrace{!sphereDiagnostic&&!diagnosticLogPath.empty()&&!prismTest};
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
    const bool sessionTour=IsSessionForwardTour(sessionMode)&&scene.solarSystem&&!automated&&building;
    const double sessionTourDuration=SessionForwardDuration(sessionMode);
    if(loopRequested&&(!sessionTour||!sessionTraceEnabled))Fail("Loop requires VR campus and diagnostic log");
    if(sessionTour){
        mappings.EnableSessionTour(sessionTourDuration);
        sessionTrace.BeginLoop(sessionMode);
        if(sessionMode==SessionMode::Walk){
            char line[320];
            std::snprintf(line,sizeof(line),
                "loop_version=4 variante=walk duree_trajet_s=%.9f vitesse_kmh=%.3f distance_m=%.6f camera=avant vitesse=uniforme captures=0 journal=ram cadence=qpc_apres_xrEndFrame gpu=asynchrone\r\n",
                sessionTourDuration,SessionWalkKmh,SessionForwardDistanceMeters);
            DiagnosticLine(line);
        }else DiagnosticLine("loop_version=4 duree_trajet_s=24 camera=avant vitesse=uniforme captures=0 journal=ram cadence=qpc_apres_xrEndFrame gpu=asynchrone\r\n");
        DiagnosticLine("loop_telemetry_version=2 phases=historical_route_segments intervals=destination_image gpu_join=frame_id gpu_active=sum_batches gpu_span=first_begin_last_end_same_queue missing=-1 format=after_route\r\n");
        DiagnosticLine("world_version=V1 frame=campus_fixed membrane=absent planets=0 scale=metric_1m sun=orbiting starfield=on far_m=400\r\n");
    }
    // Standard loop only.
    if(sessionTour&&sessionTraceEnabled&&!diagnostic.mg2&&!sunTest&&!prismTest){
        sessionTrace.depthEnabled=true;
        graphics.EnableDepthDiagnostics();
        DiagnosticLine("depth_telemetry=v3 scope=CpuScene.Depth count_every_frames=8 counts=mean_per_sample rejected=instance_frustum_or_occluded_meshlets_before_dispatch timings=mean_per_frame\r\n");
    }
    if(sunTest)mappings.EnableFixedSessionTour(8.);
    if(prismTest)mappings.EnablePrismTour();
    if(scene.solarSystem&&!automated){
        if(!navigation.valid||targetMode){
            const auto worktop=building&&diagnostic.mg2?building->Pilot().WorktopSpawn():std::nullopt;
            if(diagnostic.mg2)DiagnosticLine(worktop?"mg2_depart=bureau\r\n":"mg2_depart=parvis\r\n");
            const auto spawn=building?worktop.value_or(building->Pilot().Spawn()):SolarOasisApproach(0.);
            auto sessionSpawn=spawn;
            const auto body=SolarBodyToCosmos(0u,SolarBody(0u,0.));
            if(targetMode)sessionSpawn=TargetCalibration::Spawn(building->Pilot());
            else if(prismTest)sessionSpawn=PrismPose(0.);
            else if(sessionTour)sessionSpawn=SessionForwardTourPose(0.,0.);
            else if(sessionMode==SessionMode::Cosmos){
                if(building){
                    // Reuse the authored rooftop and its actual helicopter pad.
                    const auto roofEye=TargetCalibration::Spawn(building->Pilot()).p;
                    sessionSpawn=SolarLookAt(roofEye,shuttle.ParkingPosition());
                    DiagnosticLine("cosmos_depart=helipad_oasis\r\n");
                }else sessionSpawn=SessionTourPose(0.,0.);
            }
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
    DiagnosticLine("etape=attente_image\r\n");FlushDiagnostic();
    int64_t menuClick=0;bool finishCapture=false,exitInitiated=false;
    ULONGLONG exitRequestedAt=0;
    bool pageUpHeld=false,pageDownHeld=false,wasAboard=false,hadAvionics=false;
    BasaltStroke laser;
    LiveCapture liveCapture(config,diagnostic.path,sessionTour||prismTest);
    size_t tourCheckpoint=0;
    std::array<MembraneHands,SpectacleSphereCount> membraneHands;
    MembranePassages passages;
    uint64_t materialRevision=materialProfiles.Revision();
    double previousBuildingTime=0.;
    auto stamp=[&]{return sphereDiagnostic||sessionTraceEnabled?QpcCounter():dlssTrace.Stamp();};
    unsigned startupXrAttempts=0;
    float lastOpticalFar=0.f;
    continentKeyPresses.exchange(0u,std::memory_order_relaxed);
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
        const bool startupXrTrace=sessionTraceEnabled&&startupXrAttempts++<128u;
        if(startupXrTrace)DiagnosticLine("xr_startup_begin_enter\r\n");
        if(!xr.BeginFrame(frame,(dlssTrace.enabled||sessionTraceEnabled||sphereDiagnostic)?&timing.ms[DlssDiagnosticCapture::Wait]:nullptr,
                          !fixedDiagnosticFraming))continue;
        if(startupXrTrace){
            char line[256];
            std::snprintf(line,sizeof(line),"xr_startup_begin_return xr=%llu wants_render=%u focused=%u\r\n",
                static_cast<unsigned long long>(frame.input.frameId),unsigned(frame.render),unsigned(frame.input.focused));
            DiagnosticLine(line);
        }
        timing.ms[DlssDiagnosticCapture::Begin]=dlssTrace.Elapsed(started);
        if(sessionTraceEnabled)MarkSession(SessionStage::Input);
        timing.wanted=frame.render;
        int scaleStep=0;
        const auto& actions=StandardVrActions();
        const bool aboard=shuttle.Active();
        if(fixedDiagnosticFraming&&!(shuttleCalibration&&aboard))
            ApplyOasisDiagnosticControllers(frame.input,actions,sphereDiagnostic);
        if(sunTest||prismTest)ApplyOasisDiagnosticControllers(frame.input,actions,true);
        const double prismElapsed=mappings.PrismElapsed();
        if(prismTest&&!exitInitiated)prismBenchmark.Apply(graphics,PrismBlock(prismElapsed));
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
            // The campus, headset and aim already use the same fixed world frame.
            building->Projectiles(graphics.DrainBuildingImpacts());
            // Residency, view reach and building geometry all use metres.
            const float campusFarMeters=local.farClipMeters;
            building->Tick(world,local.head,float(std::clamp(dt,0.,.075)),
                campusFarMeters);
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
        const bool liveInput=!automated&&!buildingTour&&!sessionTour&&!sunTest&&!prismTest&&!exitInitiated&&frame.input.focused;
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
        if(!automated&&!exitInitiated&&!sessionTour&&!prismTest){
            const bool pageUp=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0;
            const bool pageDown=(GetAsyncKeyState(VK_NEXT)&0x8000)!=0;
            if(building&&scene.solarSystem){
                // Raw input queues physical presses even between XR frames.
                // Poll only if registration failed, to avoid double counting.
                const unsigned presses=continentRawKeyboard?
                    continentKeyPresses.exchange(0u,std::memory_order_relaxed):unsigned(pageUp&&!pageUpHeld);
                for(unsigned press=0;press<presses;++press){
                    const bool grown=building->GrowContinent();
                    char line[192];std::snprintf(line,sizeof(line),"sf8_pageup xr=%llu focused=%u accepted=%u steps=%u\r\n",
                        static_cast<unsigned long long>(frame.input.frameId),unsigned(frame.input.focused),unsigned(grown),building->ContinentSteps());DiagnosticLine(line);
                }
                if(frame.input.focused)scaleStep=-int(pageDown&&!pageDownHeld);
            }else if(frame.input.focused)scaleStep=int(pageUp&&!pageUpHeld)-int(pageDown&&!pageDownHeld);
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
            if(!scene.solarSystem){
                const auto owner=NearestSpectacle(frame.input.head.p,sphereCount,frame.input.solarTime);
                auto local=owner==0u?atmosphere:city;
                local.center=SolarSceneCenter(owner,frame.input.solarTime);
                local.radius=SolarSceneRadius(owner,frame.input.solarTime);
                frame.membraneHandSphere=owner;
                membraneHands[owner].Update(frame,local,*feedback,xr,liveInput&&!aboard);
            }
        }
        if(!sessionTour&&!prismTest)if(const auto saved=liveCapture.Poll()){
            if(*saved&&feedback&&!exitInitiated)
                feedback->Emit(xr,frame.input.head,{FeedbackCue::CaptureSaved,0u,frame.input.time,frame.input.frameId});
            else if(!*saved)DiagnosticLine("capture_live=echec_ecriture\r\n");
        }
        const double tourTime=mappings.SessionTourElapsed();
        if(sessionTour&&!exitInitiated&&tourCheckpoint<SessionTourCaptureTimes.size()&&tourTime>=SessionTourCaptureTimes[tourCheckpoint]*(sessionTourDuration/SessionTourSeconds)){
            char line[256];
            std::snprintf(line,sizeof(line),"tour_seconde=%.3f image=%llu sphere=%u xyz=%.5f,%.5f,%.5f\r\n",
                tourTime,static_cast<unsigned long long>(frame.input.frameId),frame.input.insideSphere,
                frame.input.head.p.x,frame.input.head.p.y,frame.input.head.p.z);
            DiagnosticLine(line);
            ++tourCheckpoint;
        }
        const bool captureLoopFrame=automatedDlss&&dlssLoop.CaptureReady()&&!diagnostic.attempted&&!exitInitiated;
        const auto* captureView=fixedDiagnosticFraming&&captureLoopFrame?&OasisDiagnosticView:nullptr;
        if(captureView){
            frame.input.head=OasisDiagnosticHead();
            ++frame.input.trackingRevision;
        }
        if(sessionTraceEnabled)MarkSession(SessionStage::Prepare);
        if(building&&scene.solarSystem)building->AtmosphereContext(frame.input);
        if(building&&scene.solarSystem&&!automated&&!buildingTour&&!sessionTour&&!sunTest&&!prismTest&&!targetMode){
            // Tick has already selected the local territory with the original
            // streaming reach. Only the camera and compositor use this extent.
            const float residencyReach=frame.input.farClipMeters;
            frame.input.farClipMeters=building->OpticalFarMeters(frame.input.head.p,residencyReach);
            if(frame.input.farClipMeters!=lastOpticalFar){
                char line[192];std::snprintf(line,sizeof(line),"sf8_optical far_m=%.0f residency_input_m=%.0f steps=%u\r\n",
                    frame.input.farClipMeters,residencyReach,building->ContinentSteps());DiagnosticLine(line);
                lastOpticalFar=frame.input.farClipMeters;
            }
        }
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
            frame.headLockedDiagnostic=sphereDiagnostic||buildingTour.has_value()||sunTest||sessionTour||prismTest;
            xr.LocateLate(frame,captureView);
            if(targetMode)frame.targetOrigin=TargetCalibration::Ruler(building->Pilot());
            if(calibration)calibration->ApplyView(frame);
            const bool acquired=frame.tracked&&xr.AcquireSwapchains(frame);
            timing.ms[DlssDiagnosticCapture::Acquire]=dlssTrace.Elapsed(started);
            if(acquired){
                if(!sunTest&&!prismTest&&!sessionTour&&!buildingTour&&(!automated||captureLoopFrame||
                    (calibration&&calibration->CaptureReady()))&&!exitInitiated)
                    diagnostic.BeforeRender(graphics,automated,diagnostic.mg2&&building&&
                        frame.input.insideSphere==0u&&building->ResidentWorktops()>0u);
                if(feedback&&!sessionTour&&!prismTest&&!automated&&!exitInitiated)liveCapture.Prepare(graphics);
                started=stamp();
                if(sessionTraceEnabled)MarkSession(SessionStage::Render);
                if(sessionTraceEnabled&&!sessionTour&&!prismTest)cutTrace.Collect(graphics);
                graphics.Render(frame);
                if(sessionTraceEnabled&&!sessionTour&&!prismTest)sessionTrace.LogDepthMode(graphics);
                if(feedback&&!sessionTour&&!prismTest&&!automated)liveCapture.Submitted(graphics);
                timing.ms[DlssDiagnosticCapture::Render]=dlssTrace.Elapsed(started);
            }
        }
        started=stamp();
        if(sessionTraceEnabled)MarkSession(SessionStage::Submit);
        xr.EndFrame(frame);
        if(startupXrTrace){
            char line[192];
            std::snprintf(line,sizeof(line),"xr_startup_end xr=%llu prepared=%u submitted=%u\r\n",
                static_cast<unsigned long long>(frame.input.frameId),unsigned(timing.prepared),unsigned(frame.submitted));
            DiagnosticLine(line);
        }
        if(sessionTour&&!exitInitiated){
            sessionTrace.LoopSubmitted(frame,QpcCounter(),tourTime);
            if(frame.ready&&frame.tracked&&frame.submitted&&!frame.captureOnly)
                frameSeen.store(true,std::memory_order_release);
        }
        laser.Submitted(frame);
        if(sessionTour&&sessionTraceEnabled)sessionTrace.LogDepthMode(graphics);
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
            if(!sessionTour&&!sunTest&&!prismTest&&!sphereDiagnostic&&(!automatedDlss||dlssLoop.phase!=DlssLoop::Phase::Native))
                graphics.UpdateDlss(scaleStep);
            if(!sessionTour&&!sunTest&&!prismTest&&!sphereDiagnostic&&!exitInitiated&&!orbit.Apply(graphics,atmosphere))DiagnosticLine("orbit_profil=echec_ecriture\r\n");
            if(dlssTrace.enabled||sessionTraceEnabled){
                timing.ms[DlssDiagnosticCapture::Update]=dlssTrace.Elapsed(started);
                const auto state=graphics.GetDlssDiagnostic();
                if(dlssTrace.enabled)dlssTrace.Record(timing,before,state,frame.input.frameId);
                if(sessionTraceEnabled&&!sunTest&&!prismTest){
            
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
            navigation={frame.input.head,true,frame.input.insideSphere!=Invalid};
        if(sessionTraceEnabled)MarkSession(SessionStage::Capture);
        if(prismTest){
            sessionFrame.store(frame.input.frameId,std::memory_order_relaxed);
            if(frame.submitted){frameSeen.store(true,std::memory_order_release);mappings.StartPrismTour();}
            if(!exitInitiated){
                prismBenchmark.Record(graphics,frame,timing,prismElapsed);
                if(prismElapsed>=PrismDuration){diagnostic.prismCompleted=true;finishCapture=true;}
            }
        }else if(sunTest){
            sessionFrame.store(frame.input.frameId,std::memory_order_relaxed);
            if(frame.submitted)frameSeen.store(true,std::memory_order_release);
            if(!exitInitiated&&sunBenchmark.Update(graphics,frame)){
                diagnostic.sunCompleted=true;finishCapture=true;
            }
        }else if(buildingTour){
            if(frame.submitted)frameSeen.store(true,std::memory_order_release);
            if(!exitInitiated&&!buildingTour->Finished())buildingTour->After(graphics,frame);
            if(buildingTour->Finished()){
                diagnostic.captured=buildingTour->Complete();
                diagnosticCaptured.store(diagnostic.captured,std::memory_order_release);
                finishCapture=true;
            }
        }else if(!sessionTour&&diagnostic.AfterRender(graphics,frame.submitted,false,frameSeen,
            frame.captureOnly||passageCalibration||shuttleCalibration?&frame:nullptr))
            finishCapture=true;
        if(sessionTour&&!exitInitiated&&sessionTrace.loopRouteFinished){
            diagnostic.attempted=true;
            finishCapture=true;
        }
    }
    if(dlssTrace.enabled)dlssTrace.Finish(graphics.GetDlssDiagnostic(),automatedDlss);
    if(sessionTraceEnabled)MarkSession(SessionStage::Shutdown);
    graphics.WaitIdle();
    if(calibration){calibration->Collect(graphics);diagnostic.sphereBaselineCompleted=calibration->Write();}
    if(sessionTraceEnabled){cutTrace.Collect(graphics,true);if(!sunTest&&!prismTest){sessionTrace.CollectOptical(graphics,true);sessionTrace.CollectDepth(graphics,true);}}
    if(sessionTour)diagnostic.loopCompleted=sessionTrace.FinishLoop();
    if(prismTest)diagnostic.prismCompleted=prismBenchmark.Finish(graphics,diagnostic.prismCompleted);
    graphics.UnloadScene();
    if(!automated&&!sunTest&&!prismTest&&!shuttle.SaveSettings())DiagnosticLine("navette_profil=echec_ecriture\r\n");
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
        DiagnosticLine(graphics.StartupCacheReport());
        auto atmosphere=DefaultAtmosphere(logo);
        if(scene.solarSystem){atmosphere.radius=0.f;atmosphere.density=0.f;atmosphere.membrane=false;atmosphere.forestAtmosphere=building!=nullptr;}
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
    controller.SetSpectaclePassage(logo&&!scene.solarSystem);
    if(navigation.valid)controller.SetPose(navigation.pose,navigation.inside);
    MembranePassages passages;
    MarkDiagnosticStartupProgress();
    uint64_t frameId=0;bool switchOutput=false,pageUpHeld=false;int64_t previousFrameQpc=0;
    while(!stop.load(std::memory_order_relaxed)){
        if(!GetClientRect(window,&rectangle))break;
        const LONG w=rectangle.right-rectangle.left,h=rectangle.bottom-rectangle.top;
        if(w<=0||h<=0||IsIconic(window)){Sleep(10);continue;}
        width=uint32_t(w);height=uint32_t(h);
        if(width!=desktop.Width()||height!=desktop.Height()){graphics.ReleaseOutput();desktop.Resize(width,height);configure();}
        const auto controls=controller.Poll(++frameId,width,height,input.ActionCount(),GetForegroundWindow()==window);
        navigation={controls.input.head,true,controls.input.insideSphere!=Invalid};
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
        const bool pageUp=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0;
        if(building&&scene.solarSystem&&controls.input.focused&&pageUp&&!pageUpHeld)building->GrowContinent();
        pageUpHeld=pageUp;
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
        if(building&&scene.solarSystem)building->AtmosphereContext(frame.input);
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
    if(sphereDiagnostic)Fail("V1: orbital and membrane calibration removed");
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
    // ARB-3 ships the four botanical life-stage morphologies by default.
    // An explicit single-variant setting remains available for isolation.
    char canopyVariantsSetting[8]{};
    const auto canopyLength=GetEnvironmentVariableA("OASIS_CANOPY_VARIANTS",canopyVariantsSetting,DWORD(sizeof(canopyVariantsSetting)));
    const std::string_view canopySetting(canopyVariantsSetting,canopyLength<sizeof(canopyVariantsSetting)?canopyLength:0u);
    const unsigned canopyVariants=canopySetting=="1"?1u:4u;
    std::filesystem::path campusKey;
    const auto campusStart=GetTickCount64();
    if(mg2||(useCampus&&!sphereDiagnostic&&!buildingLoop&&std::filesystem::exists(pack/"pack.json"))){
        try{
            // Campus assembly resolves its cache after forest placement decides
            // whether the original low vegetation remains in the source.
            mg2Catalog.emplace(pack,useCampus&&!buildingLoop);
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){
            const auto line=std::string("asset_unavailable source=mg2_catalog reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());
        }
    }
    if(!local.empty())materialProfiles.Install(scene);
    WorldRegistry world;
    std::optional<BuildingCampus> building;
    if(useCampus){
        const auto save=buildingLoop?std::filesystem::path{}:local.parent_path()/"buildings"/"oasis-metric";
        try{building.emplace(AssetPath(L"buildings/oasis"),save);}
        catch(const std::bad_alloc&){throw;}
        catch(const std::exception& e){
            const auto line=std::string("asset_unavailable source=building_recipe reason=")+e.what()+"\r\n";DiagnosticLine(line.c_str());
        }
    }
    bool replacesLowVegetation=false;
    ForestFloor forestFloor;
    if(building){
        std::vector<BuildingFootprint> sites;
        for(const auto& letter:building->Letters()){
            const auto& d=letter->Definition();
            for(const auto band:Buildings::GlyphBands(d,0)){
                if(band.left>=band.right)continue;
                const float units=OasisSceneUnitsPerMeter;
                sites.push_back({{d.origin.x+band.left*d.xScale*units,d.origin.y,d.origin.z},
                    (band.right-band.left)*d.xScale*units,d.depth*units});
            }
        }
        building->SetExpansionSource(scene,multiplier);
        auto terrain=PrepareBuildingSite(scene,sites);
        if(mg2Catalog){
            try{
                if(mg2Catalog->PrepareForest(terrain,sites,multiplier,canopyVariants)){
                    replacesLowVegetation=true;
                    const auto forestRoots=mg2Catalog->ForestRoots();
                    forestFloor.Prepare(terrain,sites,forestRoots);
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
    if(mg2Catalog&&useCampus&&!buildingLoop){
        const auto keyStart=GetTickCount64();
        campusKey=CampusBake::Key(pack,local.parent_path(),desktop,replacesLowVegetation);
        char line[128];std::snprintf(line,sizeof(line),"arb0_campus_key_ms=%llu\r\n",
            static_cast<unsigned long long>(GetTickCount64()-keyStart));
        DiagnosticLine(line);
    }
    if(mg2Catalog)mg2Catalog->WriteSummary();
    bool campusKeyDesktop=desktop;
    std::optional<SceneData> pair;
    std::optional<ShuttleAssets> shuttleAssets;
    if(logo&&!automatedDlss&&!buildingLoop){
        pair.emplace(scene);
        pair->solarSystem=true; // fixed campus with analytic orbiting sun
        pair->sphereObjectOffsets.clear();
        pair->lights.clear();
        PrepareSpectacleGeometry(*pair);
        if(pair->solarSystem&&(!sphereDiagnostic||shuttleCalibration))try{shuttleAssets.emplace(*pair);}
        catch(const AssetImportError& error){pair->assetDegraded=true;const auto line=std::string("asset_unavailable source=navette_organes.glb reason=")+error.what()+"\r\n";DiagnosticLine(line.c_str());}
        if(!local.empty())materialProfiles.Install(*pair);
    }
    if(logo){
        if(useCampus){scene.solarSystem=true;scene.sphereObjectOffsets.clear();scene.lights.clear();}
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
            building->GrowthFlora()=ExpansionFlora{};
            if(mg2Catalog){
                if(campusKeyDesktop!=desktop){
                    campusKey=CampusBake::Key(pack,local.parent_path(),desktop,replacesLowVegetation);
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
                mg2Catalog->InstallForest(*campusScene,world,canopyVariants);
                forestFloor.Install(*campusScene,world,&building->GrowthFlora());
                DiagnosticLine("mg2_phase=forest_install_ready\r\n");
            }else campusScene.emplace(source);
            MarkDiagnosticStartupProgress();
            DiagnosticLine("mg2_phase=building_install_begin\r\n");
            building->Install(*campusScene,world,worktop?&*worktop:nullptr,pbr?&*pbr:nullptr);
            MarkDiagnosticStartupProgress();
            DiagnosticLine("mg2_phase=building_install_ready\r\n");
            DiagnosticLine("mg2_phase=static_meshlets_ready\r\n");
        }
        if(LaunchSessionMode()==SessionMode::Target){
            if(!campusScene||!building)Fail("Target rooftop is unavailable");
            TargetCalibration::Install(*campusScene,world,building->Pilot());
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
        {
            const auto& stats=SceneBakeChunks::stats;
            PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
            const bool measured=K32GetProcessMemoryInfo(GetCurrentProcess(),
                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory))!=FALSE;
            char line[640];
            std::snprintf(line,sizeof(line),
                "arb0_scene_cache payload_read_bytes=%llu payload_written_bytes=%llu payload_reused_bytes=%llu "
                "manifest_read_bytes=%llu manifest_written_bytes=%llu read_ms=%llu write_ms=%llu "
                "process_working_set_peak_bytes=%llu process_private_bytes=%llu process_commit_peak_bytes=%llu memory_measured=%u\r\n",
                static_cast<unsigned long long>(stats.payloadRead.load()),static_cast<unsigned long long>(stats.payloadWritten.load()),
                static_cast<unsigned long long>(stats.payloadReused.load()),static_cast<unsigned long long>(stats.manifestRead.load()),
                static_cast<unsigned long long>(stats.manifestWritten.load()),static_cast<unsigned long long>(stats.readMs.load()),
                static_cast<unsigned long long>(stats.writeMs.load()),static_cast<unsigned long long>(memory.PeakWorkingSetSize),
                static_cast<unsigned long long>(memory.PrivateUsage),static_cast<unsigned long long>(memory.PeakPagefileUsage),unsigned(measured));
            DiagnosticLine(line);
            const auto& memo=DiskBakeKeyMemo::stats;
            std::snprintf(line,sizeof(line),"arb0_key_memo hits=%llu misses=%llu unavailable=%llu records_written=%llu\r\n",
                static_cast<unsigned long long>(memo.hits.load()),static_cast<unsigned long long>(memo.misses.load()),
                static_cast<unsigned long long>(memo.unavailable.load()),static_cast<unsigned long long>(memo.recordsWritten.load()));
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
    if(IsSessionForwardTour(LaunchSessionMode())){
        DiagnosticLine(diagnostic.loopCompleted?"resultat=termine\r\n":"resultat=loop_invalide\r\n");
        return diagnostic.loopCompleted?0:1;
    }
    if(LaunchSessionMode()==SessionMode::Prism){
        DiagnosticLine(diagnostic.prismCompleted?"resultat=termine\r\n":"resultat=prism_incomplet\r\n");
        return diagnostic.prismCompleted?0:1;
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
    if(message==WM_INPUT&&continentRawKeyboard){
        RAWINPUT raw{};UINT bytes=sizeof(raw);
        if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),RID_INPUT,&raw,&bytes,sizeof(RAWINPUTHEADER))!=UINT(-1)&&
           raw.header.dwType==RIM_TYPEKEYBOARD&&raw.data.keyboard.VKey==VK_PRIOR){
            const bool down=(raw.data.keyboard.Flags&RI_KEY_BREAK)==0;
            if(down&&!continentKeyDown)continentKeyPresses.fetch_add(1u,std::memory_order_relaxed);
            continentKeyDown=down;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
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
    if(!window)Fail("Desktop window");
    RAWINPUTDEVICE keyboard{0x01,0x06,RIDEV_INPUTSINK,window};
    continentRawKeyboard=RegisterRawInputDevices(&keyboard,1,sizeof(keyboard))!=FALSE;
    DiagnosticLine(continentRawKeyboard?"sf8_keyboard=raw_input_sink\r\n":"sf8_keyboard=polling_fallback\r\n");
    return window;
}
}
