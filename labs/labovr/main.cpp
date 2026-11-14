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

static void PublishInformation(Graphics& graphics,const Pose& panelPose,
                               double seconds,double submittedFps,double cpuMs,
                               double gpuP90,double targetHz,double vramMiB,
                               const char* captureState) {
    std::array<LabTerminalElementV2,9> elements{};
    uint32_t count=0;
    auto add=[&](float y,float halfWidth,const char* content,uint32_t kind=LAB_TERMINAL_LABEL_V2,float alpha=.04f) {
        auto& e=elements[count];e.id=++count;e.kind=kind;e.x=0;e.y=y;
        e.halfWidth=halfWidth;e.halfHeight=.043f;e.pixelMeters=.0056f;
        e.backgroundAlpha=alpha;
        std::snprintf(e.text,sizeof(e.text),"%s",content);
    };
    char b[96];
    add(.29f,.82f,"POOLABVR / EMPTY VR",LAB_TERMINAL_PANEL_V2,.20f);
    add(.21f,.82f,"OPENXR + D3D12 / NO WORLD");
    std::snprintf(b,sizeof(b),"APP %.1f FPS / XR %.1f HZ",submittedFps,targetHz);add(.13f,.82f,b);
    std::snprintf(b,sizeof(b),"CPU %.2f MS / GPU P90 %.2f MS",cpuMs,gpuP90);add(.05f,.82f,b);
    std::snprintf(b,sizeof(b),"VRAM %.0f MIB",vramMiB);add(-.03f,.82f,b);
    add(-.11f,.82f,"OBJECTS 0 / LIGHTS 0 / PHYSICS 0");
    add(-.19f,.82f,"NO AUDIO / WEATHER / POPULATION");
    std::snprintf(b,sizeof(b),"DIAGNOSTIC %s",captureState);add(-.27f,.82f,b);
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
        if(frame.tracked&&!panelAnchored) {
            panelPose.p=Transform({0,0,1.55f},frame.input.head);
            panelPose.q=frame.input.head.q;
            panelAnchored=true;
        }
        if(frame.displayPeriod>0)targetHz=1'000'000'000.0/double(frame.displayPeriod);

        if(options.diagnosticSeconds&&elapsed>=1.0&&!gpuProfiling&&!gpuMeasured) {
            graphics.PreparePerformanceCapture();
            graphics.BeginPerformanceCapture();
            xr.SetPerformanceCaptureActive(true);
            gpuProfiling=true;
        }
        if(gpuProfiling&&elapsed>=double(options.diagnosticSeconds)-4.0) {
            graphics.EndPerformanceCapture();
            xr.SetPerformanceCaptureActive(false);
            gpuProfiling=false;gpuMeasured=true;
            std::vector<GpuPerformancePassSample> passes;
            std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
            graphics.CollectPerformanceCapture(passes,queues,gaps);
            std::map<uint64_t,double> frameGpu;
            for(const auto& q:queues)frameGpu[q.frameId]=std::max(frameGpu[q.frameId],q.spanMs);
            std::vector<double> values;
            for(const auto& [id,ms]:frameGpu) { (void)id;if(std::isfinite(ms)&&ms>=0)values.push_back(ms); }
            if(!values.empty()) {
                std::sort(values.begin(),values.end());
                gpuP90=values[std::min(values.size()-1,size_t(std::ceil(values.size()*.90)-1))];
            }
            vramMiB=double(graphics.MemoryStats().usage)/1048576.;
            LogEvent(frame.input.frameId,"shell.gpu","p90_ms=%.3f gpu_frames=%u vram_mib=%.1f",
                     gpuP90,uint32_t(values.size()),vramMiB);
        }

        if(panelAnchored)
            PublishInformation(graphics,panelPose,elapsed,fps,cpuAverage,gpuP90,targetHz,
                               vramMiB,captureState);
        bool prepared=frame.render&&graphics.Prepare(frame.input);
        if(prepared) {
            xr.LocateLate(frame);
            if(frame.tracked&&xr.AcquireSwapchains(frame)) {
                if(options.diagnosticSeconds&&gpuMeasured&&!captureRequested&&panelAnchored) {
                    LabCaptureRequestV1 request{sizeof(LabCaptureRequestV1),0,1,0};
                    if(graphics.RequestCapture(CaptureOwner,request,captureTicket)) {
                        captureRequested=true;captureState="SAVING ONE PNG";
                    } else {captureFailed=true;captureState="FAILED";}
                }
                graphics.Render(frame);
            }
        }
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
    LogEvent(0,"shell.summary","frames=%llu submitted=%llu fps_last=%.3f target_hz=%.3f cpu_ms=%.3f gpu_p90_ms=%.3f screenshot=%u",
             static_cast<unsigned long long>(frames),static_cast<unsigned long long>(submitted),
             fps,targetHz,cpuAverage,gpuP90,unsigned(captureSaved));
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
