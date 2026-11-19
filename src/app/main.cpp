#include "Core.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "VrInputMapping.h"
#include "OrbitTool.h"
#include "WorldRegistry.h"
#include "DesktopInput.h"
#include "DesktopOutput.h"
#include "SpectacleSpace.h"
#include "SphereBaseline.h"
#include "SphereCalibration.h"
#include "Feedback.h"
#include "SurfaceContact.h"
#include <chrono>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace OASIS {
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
    if(!CreateDirectoryW(directory.c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
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
    writePose("oeil_gauche",OasisDiagnosticView.eye);
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
    void ResetPending(){if(!attempted){firstFrame=0;requested=false;}}
    void BeforeRender(Graphics& graphics,bool captureNow=false){
        if(path.empty()||attempted||requested||!firstFrame||graphics.CapturePending()||
           (!captureNow&&QpcSeconds(QpcCounter()-firstFrame)<10.0))return;
        DiagnosticLine("etape=demande_capture\r\n");graphics.RequestCapture(path);requested=true;
    }
    bool AfterRender(Graphics& graphics,bool submitted,bool desktop,std::atomic<bool>& frameSeen,
                     const XRFrame* captureFrame=nullptr){
        if(submitted)frameSeen.store(true,std::memory_order_release);
        if(path.empty()||attempted)return false;
        if(submitted&&!firstFrame){firstFrame=QpcCounter();DiagnosticLine(desktop?"etape=image_ecran\r\n":"etape=image_vr\r\n");}
        if(!requested||!graphics.CaptureDone())return false;
        DiagnosticLine("etape=capture\r\n");
        if(!graphics.CaptureFailed())graphics.WaitIdle();
        captured=graphics.SaveCapture();attempted=true;
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
                const GraphicsMetrics& metrics,uint64_t frame){
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
            "xr_attente_pic_ms=%.2f xr_debut_pic_ms=%.2f preparation_pic_ms=%.2f acquisition_pic_ms=%.2f "
            "rendu_cpu_pic_ms=%.2f soumission_pic_ms=%.2f options_pic_ms=%.2f "
            "gpu_image=%llu gpu_direct_ms=%.2f gpu_compute_ms=%.2f gpu_direct_pic_ms=%.2f "
            "gpu_direct_lot=\"%s\" gpu_compute_pic_ms=%.2f gpu_compute_lot=\"%s\" "
            "ngx_gpu_ms=%.2f dlss=%.3f vram_mio=%llu budget_mio=%llu\r\n",
            now-started,static_cast<unsigned long long>(frame),now-window,frames,wanted,prepared,submitted,
            peaks[0],peaks[1],peaks[2],peaks[3],peaks[4],peaks[5],peaks[6],
            static_cast<unsigned long long>(gpu.gpuFrame),gpu.directMs,gpu.computeMs,gpu.directPeakMs,
            gpu.directPeakBatch.data(),gpu.computePeakMs,gpu.computePeakBatch.data(),
            gpu.dlssGpuMs,gpu.requestedScale,static_cast<unsigned long long>(metrics.vramUsed>>20),
            static_cast<unsigned long long>(metrics.vramBudget>>20));
        DiagnosticLine(line);
        window=now;frames=wanted=prepared=submitted=0;peaks={};
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
        frame.basaltRay=LaserRay{exit,Add(exit,Scale(forward,LaserReach*units))};
        frame.laserHeld=!switching&&frame.input.Down(actions.secondary,right);
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

    void Prepare(XRFrame& frame){
        if(frame.input.trackingRevision!=trackingRevision){
            previous.reset();
            next=count=0;
            trackingRevision=frame.input.trackingRevision;
        }
        for(uint32_t i=0;i<count;++i){
            const auto& stamp=stamps[i];
            const auto age=frame.input.time-stamp.time;
            if(age<0.0||age>=AfterimageSeconds)continue;
            const float fade=1.f-float(age/AfterimageSeconds);
            frame.laserAfterimages[frame.laserAfterimageCount++]={stamp.ray,fade*fade};
        }
        if(!frame.laserHeld||!frame.basaltRay){
            previous.reset();
            return;
        }
        if(frame.displayTime-lastDisplay>2*frame.displayPeriod)previous.reset();
        frame.previousLaserRay=previous;
        if(!previous){
            frame.laserCut=true;
            return;
        }
        const auto& now=*frame.basaltRay;
        const auto originDelta=Sub(now.origin,previous->origin);
        const auto endDelta=Sub(now.end,previous->end);
        const auto sceneDelta=Add(Scale(originDelta,.67f),Scale(endDelta,.33f));
        // Keep accumulating movement below the kerf scale while the aim holds.
        frame.laserCut=std::max(Length(originDelta),Length(sceneDelta))>=
                       .004f*frame.input.sceneUnitsPerMeter;
    }
    void Submitted(const XRFrame& frame){
        if(!frame.submitted||!frame.laserHeld||!frame.basaltRay)return;
        trackingRevision=frame.input.trackingRevision;
        lastDisplay=frame.displayTime;
        if(frame.laserCut)previous=frame.basaltRay;
        stamps[next]={*frame.basaltRay,frame.input.time};
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
    LiveCapture(const std::wstring& config,const std::wstring& diagnosticPath){
        namespace fs=std::filesystem;
        if(!diagnosticPath.empty()){
            directory_=fs::path(diagnosticPath).parent_path()/"live";
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
            pulses_[next_]={Unit(Sub(crossing.position,SpectacleCenters[crossing.sphere])),input.time,crossing.sphere};
            next_=(next_+1u)%MembranePassagePulseCount;
            count_=std::min(count_+1u,MembranePassagePulseCount);
            if(diagnostic){
                char line[200];
                std::snprintf(line,sizeof(line),
                    "sphere_passage=%s sphere=%u image=%llu position_scene=%.6f,%.6f,%.6f facteur=%.3f\r\n",
                    crossing.entering?"entree":"sortie",crossing.sphere,static_cast<unsigned long long>(input.frameId),
                    crossing.position.x,crossing.position.y,crossing.position.z,
                    double(1.f/(crossing.entering?SpectacleInteriorUnitsPerMeter:1.f)));
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
        const auto scenePoint=[&](XMFLOAT3 local){return Add(atmosphere.center,Scale(local,units));};
        for(uint32_t hand=0;hand<HandCount;++hand){
            auto& state=hands_[hand];
            const auto& glove=frame.glovePoses[hand];
            if(enabled&&glove){
                const auto palm=Transform({0.f,0.f,.045f*units},*glove);
                const auto localPalm=Scale(Sub(palm,atmosphere.center),1.f/units);
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

SphereLodSettings ReadSphereLodSettings(){
    const auto path=AssetPath(L"spheres.ini");
    SphereLodSettings settings;
    auto read=[&](const wchar_t* section,const wchar_t* key,float fallback){
        wchar_t value[64]{};
        if(!GetPrivateProfileStringW(section,key,L"",value,unsigned(std::size(value)),path.c_str()))
            return fallback;
        wchar_t* end=nullptr;
        const float parsed=std::wcstof(value,&end);
        if(end==value||*end||!std::isfinite(parsed)||parsed<0.f){
            DiagnosticLine("sphere_lod=seuil_invalide\r\n");
            return fallback;
        }
        return parsed;
    };
    constexpr std::array sections{L"geometry",L"relief",L"relief_lite"};
    for(size_t i=0;i<settings.bands.size();++i){
        auto& band=settings.bands[i];
        const auto section=sections[i];
        band.enterDistance=read(section,L"enter_max_distance_m",band.enterDistance);
        band.exitDistance=read(section,L"exit_max_distance_m",band.exitDistance);
        band.enterFraction=read(section,L"enter_fov_fraction",band.enterFraction);
        band.exitFraction=read(section,L"exit_fov_fraction",band.exitFraction);
        band.width=read(section,L"ellipse_half_width_m",band.width);
        band.exitWidth=read(section,L"ellipse_exit_half_width_m",band.exitWidth);
        const auto& previous=settings.bands[i?i-1u:0u];
        if(band.enterDistance>band.exitDistance||band.enterFraction<=0.f||
           band.enterFraction>band.exitFraction||band.exitFraction>1.f||band.width<=0.f||band.width>band.exitWidth||
           previous.enterDistance>band.enterDistance||previous.exitDistance>band.exitDistance||
           previous.enterFraction>band.enterFraction||previous.exitFraction>band.exitFraction||
           previous.width>band.width||previous.exitWidth>band.exitWidth){
            DiagnosticLine("sphere_lod=hysterese_invalide_defauts\r\n");
            return {};
        }
    }
    settings.liteRefreshHz=std::clamp(read(L"relief_lite",L"refresh_hz",settings.liteRefreshHz),1.f,12.f);
    settings.pointEnter=read(L"point",L"enter_distance_m",settings.pointEnter);
    settings.pointExit=read(L"point",L"exit_distance_m",settings.pointExit);
    settings.promoteSeconds=read(L"transition",L"promote_ms",70.f)*.001f;
    settings.demoteSeconds=read(L"transition",L"demote_ms",180.f)*.001f;
    settings.warpSeconds=read(L"transition",L"warp_duration_ms",120.f)*.001f;
    settings.warpDegrees=std::clamp(read(L"transition",L"warp_peak_deg",.08f),0.f,.3f);
    if(settings.pointExit>settings.pointEnter){
        DiagnosticLine("sphere_lod=hysterese_invalide_defauts\r\n");
        return {};
    }
    return settings;
}

void BakeSphereViews(XR& xr,Graphics& graphics,const std::atomic<bool>& stop,uint32_t count){
    DiagnosticLine("sphere_lod=preparation_vues\r\n");
    const float distance=12.f*SpectacleRadius;
    const float halfFov=std::atan(1.06f*SpectacleRadius/std::sqrt(
        distance*distance-SpectacleRadius*SpectacleRadius));
    for(uint32_t index=0;index<SphereBankAngles*count&&!stop.load(std::memory_order_relaxed);){
        xr.PollEvents();
        if(xr.ExitRequested())return;
        if(!xr.Running()){Sleep(5);continue;}
        XRFrame frame;
        if(!xr.BeginFrame(frame,nullptr,false))continue;
        const auto owner=index/SphereBankAngles;
        const auto center=SpectacleCenters[owner];
        const auto direction=SphereViewDirection(index%SphereBankAngles);
        const auto camera=Add(center,Scale(direction,distance));
        const auto up=std::abs(direction.y)>.99f?XMVectorSet(0.f,0.f,1.f,0.f):
            XMVectorSet(0.f,1.f,0.f,0.f);
        const auto view=XMMatrixLookAtLH(XMLoadFloat3(&camera),XMLoadFloat3(&center),up);
        const XRCaptureView capture{FromMatrix(XMMatrixInverse(nullptr,view)),
                                    {-halfFov,halfFov,halfFov,-halfFov}};
        frame.input.head=capture.eye;
        frame.input.sceneUnitsPerMeter=1.f;
        frame.input.farClipMeters=FarZ;
        frame.input.trackingRevision=frame.input.frameId;
        frame.sphereBakeView=index%SphereBankAngles;frame.sphereBakeOwner=owner;
        if(frame.render&&graphics.Prepare(frame.input)){
            xr.LocateLate(frame,&capture);
            if(frame.tracked&&xr.AcquireSwapchains(frame))graphics.Render(frame);
        }
        xr.EndFrame(frame);
        if(frame.submitted)++index;
    }
    if(!stop.load(std::memory_order_relaxed))DiagnosticLine("sphere_lod=vues_preparees\r\n");
}

LoopResult RunVr(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                 const std::wstring& config,const SceneData& scene,WorldRegistry& world,JobSystem& jobs,
                 RenderContributions contributions,bool logo,NavigationContinuity& navigation,
                 DiagnosticCapture& diagnostic,bool automatedDlss,bool sphereBaseline,bool orbitCalibration,
                 bool fieldCalibration,OrbitProfile& profile){
    std::optional<SphereBaseline> baseline;
    if(sphereBaseline)baseline.emplace();
    const auto lodSettings=ReadSphereLodSettings();
    std::optional<SphereCalibration> calibration;
    if(orbitCalibration||fieldCalibration)calibration.emplace(lodSettings,fieldCalibration);
    const bool sphereDiagnostic=sphereBaseline||orbitCalibration||fieldCalibration;
    const bool automated=automatedDlss||sphereDiagnostic;
    const uint32_t sphereCount=scene.sphereObjectOffsets.empty()?1u:uint32_t(scene.sphereObjectOffsets.size()-1u);
    const bool paired=sphereCount>1u;
    PostMessageW(window,OutputWindowMessage,0,0);
    auto atmosphere=DefaultAtmosphere(logo);
    const auto city=CityAtmosphere();
    OrbitTool orbit(profile,logo,atmosphere.density);
    StandardVrInput mappings;mappings.LoadConfiguration(config);
    mappings.SetNavigationOptions(!automated,!automated);
    mappings.SetSpectaclePassage(logo&&!automated,sphereCount);
    if(!automated)mappings.SetOrbitTool(&orbit);
    const bool fixedDiagnosticFraming=automated&&logo;
    if(fixedDiagnosticFraming)mappings.SetInitialPose(OasisDiagnosticHead());
    else if(navigation.valid)mappings.SetInitialPose(navigation.pose);
    XR xr;Graphics graphics;
    DiagnosticLine("etape=openxr\r\n");xr.InitializeInstance({"OASIS","OASIS",1},input,
        calibration?calibration->Mapping():baseline?baseline->Mapping():mappings.Mapping());
    DiagnosticLine("etape=dx12\r\n");graphics.Initialize(xr.GraphicsRequirements());
    DiagnosticLine("etape=session_vr\r\n");xr.InitializeSession(graphics.Device(),graphics.Queue());
    if(sphereDiagnostic)graphics.EnableSphereBaselineTiming();
    const bool sphereLod=logo&&!automatedDlss;
    if(sphereLod)graphics.EnableSphereLod(lodSettings);
    if(paired)graphics.EnableSpherePair(city,sphereCount);
    DiagnosticLine("etape=chargement\r\n");graphics.Configure(xr,scene,jobs,contributions);
    graphics.SetDeveloperMode(true);
    orbit.Restore(graphics,atmosphere);
    DlssLoop dlssLoop;
    const bool sessionTraceEnabled=!automated&&!diagnosticLogPath.empty();
    SessionTrace sessionTrace;
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
    if(sphereLod)BakeSphereViews(xr,graphics,stop,std::min(sphereCount,SpectaclePairCount));
    DiagnosticLine("etape=attente_image\r\n");
    int64_t menuClick=0;bool finishCapture=false,exitInitiated=false;
    ULONGLONG exitRequestedAt=0;
    bool pageUpHeld=false,pageDownHeld=false;
    BasaltStroke laser;
    LiveCapture liveCapture(config,diagnostic.path);
    std::array<MembraneHands,SpectacleSphereCount> membraneHands;
    MembranePassages passages;
    std::optional<Feedback> feedback;
    if(!automated)feedback.emplace(std::filesystem::path(config).parent_path()/"audio",
                                      StandardVrActions().feedback);
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
        if(fixedDiagnosticFraming)ApplyOasisDiagnosticControllers(frame.input,actions,sphereDiagnostic);
        passages.Update(frame,sessionTraceEnabled);
        frame.orbit=orbit.Display();
        frame.glovePoses[0]=VrGlovePose(frame.input.PoseState(actions.handPose,0),0);
        frame.glovePoses[1]=VrGlovePose(frame.input.PoseState(actions.handPose,1),1);
        const bool liveInput=!automated&&!exitInitiated&&frame.input.focused;
        frame.rightTool=fixedDiagnosticFraming?RightHandTool::Basalt:orbit.EquippedTool();
        TrackRightTool(frame,actions,liveInput);
        laser.Prepare(frame);
        if(!automated&&!exitInitiated){
            const bool pageUp=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0;
            const bool pageDown=(GetAsyncKeyState(VK_NEXT)&0x8000)!=0;
            if(frame.input.focused)scaleStep=int(pageUp&&!pageUpHeld)-int(pageDown&&!pageDownHeld);
            pageUpHeld=pageUp;
            pageDownHeld=pageDown;
            if(liveInput&&(frame.input.Pressed(actions.capture,1)||frame.input.Pressed(actions.next,1)))
                liveCapture.Queue();
            if(frame.input.Pressed(actions.quit,0)){
                const int64_t now=QpcCounter();
                if(menuClick&&now-menuClick<=2*QpcFrequency()){stop.store(true,std::memory_order_relaxed);menuClick=0;}
                else menuClick=now;
            }
        }
        if(feedback){
            const auto owner=NearestSpectacle(frame.input.head.p,sphereCount);
            auto local=owner==0u?atmosphere:city;
            if(owner!=0u)local.center=SpectacleCenters[owner];
            frame.membraneHandSphere=owner;
            membraneHands[owner].Update(frame,local,*feedback,xr,liveInput);
            if(const auto saved=liveCapture.Poll()){
                if(*saved&&!exitInitiated)
                    feedback->Emit(xr,frame.input.head,{FeedbackCue::CaptureSaved,1u,frame.input.time,frame.input.frameId});
                else if(!*saved)DiagnosticLine("capture_live=echec_ecriture\r\n");
            }
        }
        const bool captureLoopFrame=automatedDlss&&dlssLoop.CaptureReady()&&!diagnostic.attempted&&!exitInitiated;
        const auto* captureView=fixedDiagnosticFraming&&captureLoopFrame?&OasisDiagnosticView:nullptr;
        if(captureView){
            frame.input.head=OasisDiagnosticHead();
            ++frame.input.trackingRevision;
        }
        if(sessionTraceEnabled)MarkSession(SessionStage::Prepare);
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        started=stamp();
        timing.prepared=frame.render&&graphics.Prepare(frame.input);
        timing.ms[DlssDiagnosticCapture::Prepare]=dlssTrace.Elapsed(started);
        if(timing.prepared){
            started=stamp();
            if(sessionTraceEnabled)MarkSession(SessionStage::Acquire);
            frame.headLockedDiagnostic=sphereDiagnostic;
            xr.LocateLate(frame,captureView);
            if(baseline)baseline->ApplyView(frame);
            if(calibration)calibration->ApplyView(frame);
            const bool acquired=frame.tracked&&xr.AcquireSwapchains(frame);
            timing.ms[DlssDiagnosticCapture::Acquire]=dlssTrace.Elapsed(started);
            if(acquired){
                if((!automated||captureLoopFrame||(baseline&&baseline->CaptureReady())||
                    (calibration&&calibration->CaptureReady()))&&!exitInitiated)
                    diagnostic.BeforeRender(graphics,automated);
                if(feedback&&!exitInitiated)liveCapture.Prepare(graphics);
                started=stamp();
                if(sessionTraceEnabled)MarkSession(SessionStage::Render);
                if(sessionTraceEnabled)cutTrace.Collect(graphics);
                graphics.Render(frame);
                if(feedback)liveCapture.Submitted(graphics);
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
                if(sessionTraceEnabled)sessionTrace.Record(timing,state,graphics.GetMetrics(),frame.input.frameId);
                if(!dlssTrace.enabled&&!sessionTraceEnabled)graphics.SetDlssDiagnostic(false);
            }
        }
        if(baseline&&!exitInitiated){
            using Stages=DlssDiagnosticCapture;
            baseline->Record(frame,timing.prepared,timing.ms[Stages::Prepare]+timing.ms[Stages::Render],
                             timing.ms[Stages::Acquire],graphics.GetMetrics());
            baseline->Collect(graphics);
        }
        if(calibration&&!exitInitiated){
            calibration->Record(frame,timing.ms[DlssDiagnosticCapture::Prepare]+timing.ms[DlssDiagnosticCapture::Render]);
            calibration->Collect(graphics);
        }
        if(frame.submitted)
            navigation={frame.input.head,true,frame.input.sceneUnitsPerMeter!=1.f};
        if(sessionTraceEnabled)MarkSession(SessionStage::Capture);
        if(diagnostic.AfterRender(graphics,frame.submitted,false,frameSeen,frame.captureOnly?&frame:nullptr))
            finishCapture=true;
    }
    if(dlssTrace.enabled)dlssTrace.Finish(graphics.GetDlssDiagnostic(),automatedDlss);
    if(sessionTraceEnabled)MarkSession(SessionStage::Shutdown);
    graphics.WaitIdle();
    if(baseline){
        baseline->Collect(graphics);
        diagnostic.sphereBaselineCompleted=baseline->Write();
    }
    if(calibration){calibration->Collect(graphics);diagnostic.sphereBaselineCompleted=calibration->Write();}
    if(sessionTraceEnabled)cutTrace.Collect(graphics,true);
    graphics.UnloadScene();
    return LoopResult::Exit;
}

LoopResult RunDesktop(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                      const SceneData& scene,WorldRegistry& world,JobSystem& jobs,RenderContributions contributions,
                      bool logo,bool& developer,NavigationContinuity& navigation,DiagnosticCapture& diagnostic,
                      const OrbitProfile& profile){
    PostMessageW(window,OutputWindowMessage,1,0);
    Graphics graphics;DiagnosticLine("etape=dx12_ecran\r\n");graphics.InitializeDesktop();
    RECT rectangle{};if(!GetClientRect(window,&rectangle))Fail("Desktop client area");
    uint32_t width=uint32_t(std::max<LONG>(1,rectangle.right-rectangle.left));
    uint32_t height=uint32_t(std::max<LONG>(1,rectangle.bottom-rectangle.top));
    DesktopOutput desktop;desktop.Create(window,graphics.Device(),graphics.Queue(),width,height);
    auto configure=[&]{
        OutputSurface output{};output.width=desktop.Width();output.height=desktop.Height();output.viewCount=1;
        output.format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;output.desktop=true;
        for(uint32_t i=0;i<DesktopOutput::BufferCount;++i)output.colors.push_back(desktop.Color(i));
        output.depths.push_back(desktop.Depth());graphics.ConfigureOutput(output,scene,jobs,contributions);
        auto atmosphere=DefaultAtmosphere(logo);
        profile.RestoreAtmosphere(atmosphere);
        graphics.SetAtmosphere(atmosphere);graphics.SetDeveloperMode(developer);
    };
    DiagnosticLine("etape=chargement_ecran\r\n");configure();
    const bool traceEnabled=!diagnosticLogPath.empty();
    graphics.SetDlssDiagnostic(traceEnabled);
    graphics.SetCutDiagnostic(traceEnabled);
    SessionTrace sessionTrace;
    CutSessionTrace cutTrace;
    DesktopInput controller;
    controller.SetSpectaclePassage(logo);
    if(navigation.valid)controller.SetPose(navigation.pose,navigation.inside);
    MembranePassages passages;
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

int Run(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,std::atomic<ULONGLONG>& modeStarted,HWND window,const std::wstring& worldPath,
        const std::wstring& capturePath,bool interactive,bool desktop,bool automatedDlss,bool sphereBaseline,
        bool orbitCalibration,bool fieldCalibration){
    ComScope com;JobSystem jobs;jobs.Start();
    InputSystem input;DeclareStandardVrInputActions(input);
    const auto config=AssetPath(L"input.ini");input.LoadConfiguration(config);
    const bool sphereDiagnostic=sphereBaseline||orbitCalibration||fieldCalibration;
    OrbitProfile orbitProfile(automatedDlss||sphereDiagnostic?std::wstring{}:config);
    const bool logo=worldPath.empty();
    SceneData scene=LoadSceneGLTF(logo?AssetPath(L"oasis_logo.glb"):worldPath);
    std::optional<SceneData> pair;
    if(logo&&!automatedDlss&&!sphereBaseline){
        pair.emplace(scene);
        AppendSpectacleCity(*pair);
        PrepareSpectacleGeometry(*pair);PrepareStaticMeshletRegions(*pair);
        InstantiateSpectacleCities(*pair,orbitCalibration?SpectaclePairCount:SpectacleSphereCount);
    }
    if(logo){PrepareSpectacleGeometry(scene);PrepareStaticMeshletRegions(scene);}
    WorldRegistry world;
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
    while(!stop.load(std::memory_order_relaxed)){
        modeStarted.store(GetTickCount64(),std::memory_order_release);
        frameSeen.store(false,std::memory_order_release);
        const auto result=desktop?RunDesktop(stop,frameSeen,window,input,scene,world,jobs,contributions,logo,developer,navigation,diagnostic,orbitProfile):
            RunVr(stop,frameSeen,window,input,config,pair?*pair:scene,world,jobs,
                  pair?WorldContributions(world,orbitCalibration,true):contributions,logo,navigation,diagnostic,
                  automatedDlss,sphereBaseline,orbitCalibration,fieldCalibration,orbitProfile);
        if(result!=LoopResult::SwitchOutput)break;
        desktop=!desktop;diagnostic.ResetPending();
    }
    jobs.Stop();
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
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int) {
    std::wstring worldPath,diagnosticDirectory;
    bool interactive=false,desktop=false,automatedDlss=false,sphereBaseline=false,orbitCalibration=false,fieldCalibration=false;
    int count=0;
    auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)OASIS::Fail("Command line");
    for(int i=1;i<count;++i){
        if(wcscmp(args[i],L"--world")==0&&i+1<count)
            worldPath=args[++i];
        else if(wcscmp(args[i],L"--diagnostic")==0&&i+1<count)
            diagnosticDirectory=args[++i];
        else if(wcscmp(args[i],L"--interactive")==0)interactive=true;
        else if(wcscmp(args[i],L"--flat")==0)desktop=true;
        else if(wcscmp(args[i],L"--dlss-loop")==0)automatedDlss=true;
        else if(wcscmp(args[i],L"--sphere-baseline")==0)sphereBaseline=true;
        else if(wcscmp(args[i],L"--sphere-phase2")==0)orbitCalibration=true;
        else if(wcscmp(args[i],L"--sphere-phase3")==0)fieldCalibration=true;
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    const bool sphereDiagnostic=sphereBaseline||orbitCalibration||fieldCalibration;
    if((sphereDiagnostic&&(automatedDlss||!worldPath.empty()))||
       (unsigned(sphereBaseline)+unsigned(orbitCalibration)+unsigned(fieldCalibration)>1u))
        OASIS::Fail("Sphere diagnostics use one native VR scenario");
    if(automatedDlss||sphereDiagnostic){
        desktop=false;
        interactive=false;
        if(diagnosticDirectory.empty())diagnosticDirectory=L"diagnostics";
    }
    if(!diagnosticDirectory.empty()&&!OASIS::StartDiagnostic(diagnosticDirectory)){
        if(!automatedDlss&&!sphereDiagnostic)MessageBoxW(nullptr,L"Impossible de creer le journal OASIS.",L"OASIS",MB_OK|MB_ICONERROR);
        return 1;
    }
    if(!diagnosticDirectory.empty())
        OASIS::DiagnosticLine(fieldCalibration?"diagnostic=sphere_phase3\r\n":
            orbitCalibration?"diagnostic=sphere_phase2\r\n":sphereBaseline?"diagnostic=sphere_avant\r\n":
            automatedDlss?"diagnostic=minimal\r\n":"diagnostic=complet\r\n");
    const std::wstring capturePath=diagnosticDirectory.empty()?std::wstring{}:diagnosticDirectory+L"\\OASIS.png";
    HWND window=OASIS::CreateOutputWindow(instance);
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::atomic<bool> frameSeen{};
    std::atomic<ULONGLONG> modeStarted{GetTickCount64()};
    std::thread render([&]{result=OASIS::Run(stop,frameSeen,modeStarted,window,worldPath,capturePath,interactive,desktop,
        automatedDlss,sphereBaseline,orbitCalibration,fieldCalibration);finished.store(true,std::memory_order_release);});
    ULONGLONG exitDeadline=0,lastStallReport=0;
    bool stalled=false;
    const ULONGLONG diagnosticDeadline=capturePath.empty()?0:GetTickCount64()+(sphereDiagnostic?180000:60000);
    while(!finished.load(std::memory_order_acquire)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT)stop.store(true,std::memory_order_relaxed);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)stop.store(true,std::memory_order_relaxed);
        if(interactive&&!OASIS::diagnosticLogPath.empty()){
            const auto now=GetTickCount64();
            const auto progress=OASIS::sessionProgress.load(std::memory_order_acquire);
            const auto delay=progress&&now>=progress?now-progress:0;
            if(delay>=2000&&(!stalled||now-lastStallReport>=5000)){
                char line[192];
                std::snprintf(line,sizeof(line),"blocage_ms=%llu etape=%s derniere_image=%llu\r\n",delay,
                    OASIS::SessionStageName(OASIS::sessionStage.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(OASIS::sessionFrame.load(std::memory_order_relaxed)));
                OASIS::DiagnosticLine(line);
                stalled=true;lastStallReport=now;
            }else if(stalled&&delay<2000){
                OASIS::DiagnosticLine("reprise=progression\r\n");
                stalled=false;
            }
        }
        const bool startupExpired=interactive&&!frameSeen.load(std::memory_order_acquire)&&
            GetTickCount64()-modeStarted.load(std::memory_order_acquire)>=60000;
        if(diagnosticDeadline&&(startupExpired||(!interactive&&GetTickCount64()>=diagnosticDeadline))&&!stop.exchange(true,std::memory_order_relaxed)){
            OASIS::DiagnosticLine(sphereDiagnostic?"erreur=delai_de_180_secondes\r\n":
                "erreur=delai_de_60_secondes\r\n");
            if(!OASIS::diagnosticCaptured.load(std::memory_order_acquire))OASIS::DiagnosticLine("capture=aucune\r\n");
        }
        if(stop.load(std::memory_order_relaxed)){
            if(!exitDeadline)exitDeadline=GetTickCount64()+3000;
            else if(GetTickCount64()>=exitDeadline&&!finished.load(std::memory_order_acquire)){
                if(!OASIS::diagnosticCaptured.load(std::memory_order_acquire))OASIS::DiagnosticLine("capture=aucune\r\n");
                OASIS::DiagnosticLine("resultat=fermeture_forcee\r\n");
                TerminateProcess(GetCurrentProcess(),1);
            }
        }
        Sleep(5);
    }
    render.join();
    if(IsWindow(window))DestroyWindow(window);
    return result;
}
