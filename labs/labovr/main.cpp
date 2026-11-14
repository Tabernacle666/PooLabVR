#include "Core.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "LabInputMapping.h"
#include "ClientAssets.h"
#include "SessionLog.h"
#include "BuildConfig.h"
#include "BuildVersion.h"
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lab {
const CoreIdentity& LaboVRIdentity() noexcept {
    static const CoreIdentity identity{
        .applicationName="PooLabVR", .engineName="LaboVR",
        .version=LAB_ENGINE_VERSION, .versionW=LAB_ENGINE_VERSION_W,
        .cacheNamespaceW=L"PooLabVR", .diagnosticLabel="PooLabVR",
        .diagnosticLabelW=L"PooLabVR", .pipelineCacheIdentity=LAB_PIPELINE_ABI
    };
    return identity;
}
struct ComScope {
    ComScope(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}
    ~ComScope(){CoUninitialize();}
};
struct Options {int diagnosticSeconds{};bool gpuValidation{};};
constexpr uint64_t PanelOwner=0x504f4f4c41425652ull;
constexpr uint64_t CaptureOwner=0x504f4f4c44494147ull;

// Experimental planning estimates, not measured uncapped display rate.
// Use the complete GPU frame timeline across copy/compute/direct queues.
struct FrameCost {uint32_t submittedObjects{};double activeCpuMs{};};
struct ProfileLevel {double cpuP90{},gpuP90{};uint32_t frames{};};
static double P90(std::vector<double> values) {
    if(values.empty())return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(),values.end());
    return values[std::min(values.size()-1,size_t(std::ceil(values.size()*.9)-1))];
}
static double EstimateObjectCeiling72(const std::map<uint32_t,ProfileLevel>& levels) {
    constexpr double BudgetMs=1000./72.; // Theoretical 72 Hz frame budget.
    if(levels.size()<2)return std::numeric_limits<double>::quiet_NaN();
    auto first=levels.begin();auto last=std::prev(levels.end());
    if(first->second.frames<8||last->second.frames<8||
       last->first<=first->first||last->first-first->first<32)
        return std::numeric_limits<double>::quiet_NaN();
    const double delta=double(last->first-first->first);
    double maximum=std::numeric_limits<double>::infinity();
    bool sawCost=false;
    for(const auto& cost:std::array<std::pair<double,double>,2>{{
        {first->second.cpuP90,last->second.cpuP90},
        {first->second.gpuP90,last->second.gpuP90}}}) {
        const double slope=(cost.second-cost.first)/delta;
        if(!std::isfinite(slope)||slope<=0.00001)continue;
        sawCost=true;
        const double baseline=std::max(0.,cost.first-slope*first->first);
        maximum=std::min(maximum,(BudgetMs-baseline)/slope);
    }
    // An extrapolation that does not reach the observed load is not a useful ceiling.
    return sawCost&&std::isfinite(maximum)&&maximum>=double(last->first)?
           maximum:std::numeric_limits<double>::quiet_NaN();
}

static void PublishInformation(Graphics& graphics,const Pose& panelPose,
                               double seconds,double submittedFps,double cpuMs,
                               double gpuP90,double targetHz,double vramMiB,
                               uint32_t objectCount,double theoreticalFps,
                               double objectCeiling72,const char* captureState) {
    std::array<LabTerminalElementV2,10> elements{};
    uint32_t count=0;
    auto add=[&](float y,float halfWidth,const char* content,uint32_t kind=LAB_TERMINAL_LABEL_V2,float alpha=.04f) {
        auto& e=elements[count];e.id=++count;e.kind=kind;e.x=0;e.y=y;
        e.halfWidth=halfWidth;e.halfHeight=.043f;e.pixelMeters=.0056f;
        e.flags=LAB_TERMINAL_BOTH_FACES_V2;
        e.backgroundAlpha=alpha;
        std::snprintf(e.text,sizeof(e.text),"%s",content);
    };
    char b[96];
    add(.37f,.82f,"POOLABVR / VR CAPACITY GUIDE",LAB_TERMINAL_PANEL_V2,.20f);
    add(.285f,.82f,"OPENXR + D3D12");
    std::snprintf(b,sizeof(b),"APP %.1f FPS / XR %.1f HZ",submittedFps,targetHz);add(.20f,.82f,b);
    std::snprintf(b,sizeof(b),"CPU %.2f MS / GPU P90 %.2f MS",cpuMs,gpuP90);add(.115f,.82f,b);
    std::snprintf(b,sizeof(b),"OBJETS SOUMIS %u (PAS GPU-VISIBLES)",objectCount);add(.03f,.82f,b);
    if(std::isfinite(theoreticalFps))
        std::snprintf(b,sizeof(b),"FPS LIBRES THEO ~%.1f",theoreticalFps);
    else std::snprintf(b,sizeof(b),"FPS LIBRES THEO N/D");
    add(-.055f,.82f,b);
    if(std::isfinite(objectCeiling72))
        std::snprintf(b,sizeof(b),"PLAFOND OBJETS @72 ~%.0f",objectCeiling72);
    else std::snprintf(b,sizeof(b),"PLAFOND OBJETS @72 N/D");
    add(-.14f,.82f,b);
    std::snprintf(b,sizeof(b),"VRAM %.0f MIB",vramMiB);add(-.225f,.82f,b);
    add(-.31f,.82f,"72HZ: 2 CHARGES REQUISES");
    std::snprintf(b,sizeof(b),"DIAGNOSTIC %s",captureState);add(-.395f,.82f,b);
    LabTerminalFrameV2 panel{};
    panel.structSize=sizeof(panel);panel.elementCount=count;
    panel.elements=elements.data();panel.theme=LAB_TERMINAL_THEME_GREEN_V2;
    panel.pose.position={panelPose.p.x,panelPose.p.y,panelPose.p.z};
    panel.pose.orientation={panelPose.q.x,panelPose.q.y,panelPose.q.z,panelPose.q.w};
    panel.timeSeconds=float(seconds);
    if(!graphics.PublishTerminal(PanelOwner,panel))Fail("VR information panel rejected");
}

int RenderThread(Channels& io,const Options& options) {
    ComScope com;
    JobSystem jobs;jobs.Start();
    InputSystem input;
    DeclareStandardVrInputActions(input);
    auto configuration=AssetPath(L"input.ini");
    input.LoadConfiguration(configuration);
    StandardVrInput mappings;mappings.LoadConfiguration(configuration);
    mappings.SetNavigationOptions(false,false);
    XR xr;Graphics graphics;
    xr.InitializeInstance({GetCoreIdentity().applicationName,GetCoreIdentity().engineName,140},
                          input,mappings.Mapping());
    graphics.Initialize(xr.GraphicsRequirements(),options.gpuValidation);
    xr.InitializeSession(graphics.Device(),graphics.Queue());

    // Primitive tables are currently required by the renderer's nonempty
    // upload contract. No mesh, material, light or object is instantiated.
    SceneData scene=CreateClientAssetSet();
    scene.objects.clear();scene.lights.clear();
    graphics.Configure(xr,scene,jobs);
    const auto gpu=graphics.PerformanceInfo();
    LogEvent(0,"shell.start","scene_objects=0 lights=0 physics=0 audio=0 urban=0 backend=d3d12 adapter=%s",
             gpu.adapter.c_str());

    double started=QpcNowSeconds(),windowStart=started,cpuSum=0.,gpuP90=0.;
    uint64_t frames=0,submitted=0,windowSubmitted=0,windowFrames=0;
    double fps=0.,cpuAverage=0.,targetHz=72.,vramMiB=0.;
    double cpuActiveP90=0.,fpsUncapped=std::numeric_limits<double>::quiet_NaN();
    double objectsAt72=std::numeric_limits<double>::quiet_NaN();
    uint32_t objectCount=0;
    std::map<uint64_t,FrameCost> profiledFrames;
    std::map<uint32_t,ProfileLevel> loadLevels;
    double nextSampleAt=2.,sampleEndsAt=0.;
    Pose panelPose{};bool panelAnchored=false;
    bool gpuProfiling=false,gpuMeasured=false;
    bool captureRequested=false,captureSaved=false,captureFailed=false;
    LabU64 captureTicket=0;
    const char* captureState=options.diagnosticSeconds?"PENDING":"IDLE";
    while(!io.stop.load(std::memory_order_relaxed)&&!xr.ExitRequested()) {
        graphics.PumpCaptures();
        if(captureRequested&&!captureSaved&&!captureFailed) {
            LabCaptureStatusV1 status{};status.structSize=sizeof(status);
            if(graphics.CaptureStatus(CaptureOwner,status)&&status.requestId==captureTicket) {
                if(status.state==LAB_CAPTURE_SAVED_V1) {
                    captureSaved=true;captureState="SAVED / ONE PNG";
                    LogEvent(status.frameId,"shell.capture","file=%s",status.fileUtf8);
                } else if(status.state==LAB_CAPTURE_FAILED_V1) {
                    captureFailed=true;captureState="FAILED";
                    LogEvent(status.frameId,"shell.capture.failed","reason=%s",status.message);
                }
            }
        }
        xr.PollEvents();
        if(!xr.Running()){Sleep(5);continue;}
        XRFrame frame;
        if(!xr.BeginFrame(frame))continue;
        const double frameBegin=QpcNowSeconds();
        const double elapsed=frameBegin-started;
        if(frame.input.Pressed(StandardVrActions().menu,0))io.stop=true;
        if(frame.displayPeriod>0)targetHz=1'000'000'000.0/double(frame.displayPeriod);

        // Small periodic GPU timestamp bursts, never a forced uncapped XR mode.
        if(elapsed>=nextSampleAt&&!gpuProfiling&&
           (!options.diagnosticSeconds||!gpuMeasured)) {
            graphics.PreparePerformanceCapture();
            graphics.BeginPerformanceCapture();
            xr.SetPerformanceCaptureActive(true);
            profiledFrames.clear();
            gpuProfiling=true;
            sampleEndsAt=elapsed+4.; // Below the 512-frame timestamp limit at 72Hz.
        }
        if(gpuProfiling&&elapsed>=sampleEndsAt) {
            graphics.EndPerformanceCapture();
            xr.SetPerformanceCaptureActive(false);
            gpuProfiling=false;gpuMeasured=true;
            nextSampleAt=elapsed+30.;
            std::vector<GpuPerformancePassSample> passes;
            std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
            graphics.CollectPerformanceCapture(passes,queues,gaps);
            LARGE_INTEGER frequency{};
            if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)
                Fail("GPU performance QPC frequency");
            struct Bounds {double first{std::numeric_limits<double>::infinity()},last{-std::numeric_limits<double>::infinity()};};
            std::map<uint64_t,Bounds> bounds;
            for(const auto& p:passes)if(std::isfinite(p.startQpc)&&std::isfinite(p.endQpc)&&p.endQpc>=p.startQpc) {
                auto& b=bounds[p.frameId];
                b.first=std::min(b.first,p.startQpc);
                b.last=std::max(b.last,p.endQpc);
            }
            std::map<uint32_t,std::pair<std::vector<double>,std::vector<double>>> groups;
            for(const auto& [frameId,b]:bounds) {
                const auto cpu=profiledFrames.find(frameId);
                const double gpuMs=(b.last-b.first)*1000./double(frequency.QuadPart);
                if(cpu==profiledFrames.end()||!std::isfinite(gpuMs)||gpuMs<=0||gpuMs>1000||
                   cpu->second.activeCpuMs<=0||!std::isfinite(cpu->second.activeCpuMs))continue;
                auto& grouped=groups[cpu->second.submittedObjects];
                grouped.first.push_back(cpu->second.activeCpuMs);
                grouped.second.push_back(gpuMs);
            }
            for(auto& [count,values]:groups)if(values.first.size()>=8)
                loadLevels[count]={P90(std::move(values.first)),P90(std::move(values.second)),
                                  uint32_t(values.second.size())};
            const auto level=loadLevels.find(objectCount);
            if(level!=loadLevels.end()) {
                cpuActiveP90=level->second.cpuP90;
                gpuP90=level->second.gpuP90;
                fpsUncapped=1000./std::max(cpuActiveP90,gpuP90);
            } else fpsUncapped=std::numeric_limits<double>::quiet_NaN();
            objectsAt72=EstimateObjectCeiling72(loadLevels);
            vramMiB=double(graphics.MemoryStats().usage)/1048576.;
            LogEvent(frame.input.frameId,"shell.guide",
                     "objects=%u uncapped_est_fps=%.2f objects72_est=%.0f cpu_active_p90_ms=%.3f gpu_span_p90_ms=%.3f gpu_frames=%u load_levels=%u vram_mib=%.1f",
                     objectCount,fpsUncapped,objectsAt72,cpuActiveP90,gpuP90,
                     uint32_t(bounds.size()),uint32_t(loadLevels.size()),vramMiB);
        }

        double cpuActiveMs=0.;
        double workBegin=QpcNowSeconds();
        if(panelAnchored)
            PublishInformation(graphics,panelPose,elapsed,fps,cpuAverage,gpuP90,targetHz,
                               vramMiB,objectCount,fpsUncapped,objectsAt72,captureState);
        cpuActiveMs+=(QpcNowSeconds()-workBegin)*1000.;
        workBegin=QpcNowSeconds();
        bool prepared=frame.render&&graphics.Prepare(frame.input);
        cpuActiveMs+=(QpcNowSeconds()-workBegin)*1000.;
        if(prepared) {
            objectCount=graphics.PreparedObjectCount();
            // An estimate measured at another object count is not current.
            if(const auto known=loadLevels.find(objectCount);known!=loadLevels.end()) {
                cpuActiveP90=known->second.cpuP90;
                gpuP90=known->second.gpuP90;
                fpsUncapped=1000./std::max(cpuActiveP90,gpuP90);
            }else fpsUncapped=std::numeric_limits<double>::quiet_NaN();
            workBegin=QpcNowSeconds();
            xr.LocateLate(frame);
            if(frame.tracked&&!panelAnchored) {
                panelPose.p=Transform({0,0,1.55f},frame.input.head);
                panelPose.q=frame.input.head.q;
                panelAnchored=true;
            }
            cpuActiveMs+=(QpcNowSeconds()-workBegin)*1000.;
            if(frame.tracked&&xr.AcquireSwapchains(frame)) {
                if(options.diagnosticSeconds&&gpuMeasured&&!captureRequested&&panelAnchored) {
                    LabCaptureRequestV1 request{sizeof(LabCaptureRequestV1),0,1,0};
                    if(graphics.RequestCapture(CaptureOwner,request,captureTicket)) {
                        captureRequested=true;captureState="SAVING ONE PNG";
                    } else {captureFailed=true;captureState="FAILED";}
                }
                workBegin=QpcNowSeconds();
                graphics.Render(frame);
                cpuActiveMs+=(QpcNowSeconds()-workBegin)*1000.;
            }
        }
        // Excludes xrWaitFrame, swapchain acquire and xrEndFrame pacing.
        if(gpuProfiling&&frame.submitted)
            profiledFrames[frame.input.frameId]={objectCount,cpuActiveMs};
        xr.EndFrame(frame);
        ++frames;++windowFrames;
        if(frame.submitted){++submitted;++windowSubmitted;}
        cpuSum+=(QpcNowSeconds()-frameBegin)*1000.0;
        if(QpcNowSeconds()-windowStart>=1.) {
            const double span=QpcNowSeconds()-windowStart;
            fps=double(windowSubmitted)/span;
            cpuAverage=windowFrames?cpuSum/double(windowFrames):0.;
            LogEvent(frame.input.frameId,"shell.rate","app_submitted_fps=%.3f xr_hz=%.3f cpu_ms=%.3f",
                     fps,targetHz,cpuAverage);
            windowStart=QpcNowSeconds();windowSubmitted=windowFrames=0;cpuSum=0.;
        }
        if(options.diagnosticSeconds&&elapsed>=options.diagnosticSeconds&&
           (captureSaved||captureFailed))io.stop=true;
        if(options.diagnosticSeconds&&elapsed>=options.diagnosticSeconds+10) {
            captureFailed=true;io.stop=true;
        }
    }
    if(gpuProfiling){graphics.EndPerformanceCapture();xr.SetPerformanceCaptureActive(false);}
    graphics.PumpCaptures();
    graphics.WaitIdle();
    LogEvent(0,"shell.summary","frames=%llu submitted=%llu fps_last=%.3f target_hz=%.3f cpu_ms=%.3f gpu_span_p90_ms=%.3f uncapped_est_fps=%.2f objects=%u objects72_est=%.0f screenshot=%u",
             static_cast<unsigned long long>(frames),static_cast<unsigned long long>(submitted),
             fps,targetHz,cpuAverage,gpuP90,fpsUncapped,objectCount,objectsAt72,unsigned(captureSaved));
    graphics.ClearTerminal(PanelOwner);
    graphics.UnloadScene();
    jobs.Stop();
    return options.diagnosticSeconds&&!captureSaved?3:0;
}
}

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    lab::ConfigureCore(lab::LaboVRIdentity());
    lab::Options options;
    int count=0;auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)lab::Fail("Command line");
    for(int i=1;i<count;i++) {
        if(wcscmp(args[i],L"--log")==0&&i+1<count) {
            lab::StartSessionLog(args[++i]);
        } else if(wcscmp(args[i],L"--diagnostic-sweep")==0&&i+1<count) {
            wchar_t* end=nullptr;long n=std::wcstol(args[++i],&end,10);
            if(!end||*end||n<15||n>600)lab::Fail("Diagnostic duration must be 15..600 seconds");
            options.diagnosticSeconds=int(n);
        } else if(wcscmp(args[i],L"--gpu-validation")==0) {
            options.gpuValidation=true;
        } else lab::Fail("Unknown option");
    }
    LocalFree(args);
    lab::SetSessionLogCompact(true);
    lab::SetSessionLogGameplay(false);
    int result=0;auto io=std::make_unique<lab::Channels>();
    std::thread render([&]{result=lab::RenderThread(*io,options);});
    while(!io->stop.load(std::memory_order_relaxed)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message==WM_QUIT)io->stop=true;
            TranslateMessage(&message);DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)io->stop=true;
        Sleep(5);
    }
    render.join();lab::StopSessionLog();return result;
}
