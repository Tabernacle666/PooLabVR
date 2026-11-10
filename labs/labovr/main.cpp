#include "Core.h"
#include "ContentCatalog.h"
#include "PluginHost.h"
#include "PluginLoader.h"
#include "PluginWorldAdapter.h"
#include "ClientAssets.h"
#include "ClientLayers.h"
#include "HomeWorld.h"
#include "LabInputMapping.h"
#include "LabAudio.h"
#include "PerformanceCapture.h"
#include "DiagnosticStatistics.h"
#include "XR.h"
#include "Graphics.h"
#include "PhysicsRuntime.h"
#include "Audio.h"
#include "Jobs.h"
#include "SessionLog.h"
#include "BuildConfig.h"
#include "BuildVersion.h"
#include "World.h"
#include <shellapi.h>
#include <memory>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <map>
#include <algorithm>
#include <cmath>
namespace lab {
const CoreIdentity& LaboVRIdentity() noexcept {
    static const CoreIdentity identity{
        .applicationName="LaboVR",
        .engineName="LaboVR",
        .version=LAB_ENGINE_VERSION,
        .versionW=LAB_ENGINE_VERSION_W,
        .cacheNamespaceW=L"LaboVR",
        .diagnosticLabel="LaboVR",
        .diagnosticLabelW=L"LaboVR",
        .pipelineCacheIdentity=LAB_PIPELINE_ABI,
        .telemetryProvider=nullptr
    };
    return identity;
}
struct ComScope {ComScope(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}~ComScope(){CoUninitialize();}};
struct RunOptions {
    std::wstring scene;
    bool gpuValidation{};
    int diagnosticSeconds{};
    std::wstring diagnosticCamerasFile;
};
struct DiagnosticView {std::string name;XMFLOAT3 position{},target{};};
std::vector<DiagnosticView> LoadDiagnosticViews(const std::wstring& fileName){
    std::ifstream input{std::filesystem::path(fileName)};
    if(!input)Fail("Diagnostic camera CSV is missing");
    std::string line;
    if(!std::getline(input,line)||line.find("name,x,y,z,target_x,target_y,target_z")!=0)
        Fail("Diagnostic camera CSV header invalid");
    std::vector<DiagnosticView> views;
    while(std::getline(input,line)){
        if(line.empty())continue;
        std::istringstream stream(line);
        std::array<std::string,7> values{};
        for(auto& value:values)if(!std::getline(stream,value,','))Fail("Diagnostic camera CSV row incomplete");
        std::string extra;if(std::getline(stream,extra,','))Fail("Diagnostic camera CSV has too many columns");
        DiagnosticView view;view.name=values[0];
        for(char c:view.name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'))
            Fail("Diagnostic camera name must be alphanumeric");
        if(view.name.empty())Fail("Diagnostic camera name empty");
        float coord[6]{};
        for(int i=0;i<6;i++){
            char* end=nullptr;coord[i]=std::strtof(values[i+1].c_str(),&end);
            if(end==values[i+1].c_str()||*end||!std::isfinite(coord[i]))Fail("Diagnostic camera coordinate invalid");
        }
        view.position={coord[0],coord[1],coord[2]};
        view.target={coord[3],coord[4],coord[5]};
        if(Length(Sub(view.target,view.position))<.1f)Fail("Diagnostic camera target too close");
        views.push_back(view);
    }
    if(views.size()<6)Fail("Diagnostic CSV requires at least six viewpoints");
    return views;
}
class HostGpuStream final:public GpuStream {
    PluginHost& host_;
public:
    explicit HostGpuStream(PluginHost& host):host_(host){}
    GpuWorldDomain Domain()const noexcept override{return LABOVR_GPU_DOMAIN_MESH_SOLID_V1;}
    GpuPublication ReserveForFrame()override{return host_.ReserveGpuPublication();}
    GpuPublication Acquire()override{return host_.AcquireGpuPublication();}
    void Release(const GpuPublication& publication)override{host_.ReleaseGpuPublication(publication);}
};
int RenderThread(Channels& io,const RunOptions& options) {
    wchar_t threadName[96];swprintf_s(threadName,L"%ls render",GetCoreIdentity().diagnosticLabelW);SetThreadDescription(GetCurrentThread(),threadName);ComScope com;JobSystem jobs;jobs.Start();
    const auto inputConfiguration=AssetPath(L"input.ini");
    InputSystem inputSystem;DeclareStandardVrInputActions(inputSystem);inputSystem.LoadConfiguration(inputConfiguration);StandardVrInput clientInput;clientInput.LoadConfiguration(inputConfiguration);clientInput.SetNavigationOptions(true,true);Graphics graphics;XR xr;ClientAudio audioSignals;Audio audio;PhysicsRuntime physics;PluginHost homeHost;PluginHost pluginHost;pluginHost.BindChannels(io);pluginHost.BindInputSystem(inputSystem);
    WorldLifecycle worlds;worlds.Services().Register(graphics);worlds.Services().Register(audio);worlds.Services().Register(xr);worlds.Services().Register(jobs);worlds.Services().Register(io);worlds.Services().Register(inputSystem);worlds.Services().Register(clientInput);worlds.Services().Register(pluginHost);HomeWorld home;ClientLayers presentation;PluginLoader pluginLoader;std::unique_ptr<LoadedPlugin> activeModule;std::unique_ptr<PluginWorldAdapter> activePlugin;auto worldCatalog=DiscoverWorldCatalog();std::vector<std::string> worldNames;for(const auto& entry:worldCatalog)worldNames.emplace_back(entry.name);home.SetWorldNames(std::move(worldNames));
    xr.InitializeInstance({GetCoreIdentity().applicationName,GetCoreIdentity().engineName,140},inputSystem,clientInput.Mapping());graphics.Initialize(xr.GraphicsRequirements(),options.gpuValidation);pluginHost.BindGraphics(graphics);physics.Initialize(graphics.Device());pluginHost.BindPhysics(physics);
    audio.InitializeService(io,nullptr,audioSignals.Provider());homeHost.BindAudio(audio);pluginHost.BindAudio(audio);
    homeHost.BindChannels(io);homeHost.BindInputSystem(inputSystem);homeHost.BindGraphics(graphics);homeHost.BindPhysics(physics);
    const std::array<std::string,6> homeServices{"physics_v1","input_v1","audio_v1","gpu_v1","gpu_interop_v1","storage_v1"};homeHost.ConfigureWorldServices("home",{},homeServices);
    LabHostApiV1 homeApi=homeHost.Api();worlds.Services().Register(homeApi);home.LoadConfiguration(inputConfiguration);worlds.LoadPersistent(home);if(options.diagnosticSeconds)home.ApplyDiagnosticDefaults();HostGpuStream homeStream(homeHost);
    xr.InitializeSession(graphics.Device(),graphics.Queue());presentation.Set(home.Contributions());graphics.Configure(xr,home.Scene(),jobs,presentation.Contributions());audio.LoadScene(home.Scene());audio.Start();graphics.AttachStream(homeStream);
    PerformanceCapture performance;
    const auto diagnosticViews=options.diagnosticSeconds?LoadDiagnosticViews(options.diagnosticCamerasFile):std::vector<DiagnosticView>{};
    constexpr LabU64 diagnosticOwner=0x444941474e4f5354ull;
    constexpr LabU64 manualOwner=0x4d414e55414c5852ull;
    LabU64 manualTicket=0;
    Pose manualHeadPose{};
    uint64_t manualFrame=0;
    auto saveManualCamera=[&](const char* file){
        const std::filesystem::path screenshot(file);
        const auto csv=screenshot.parent_path()/"manual_camera_poses.csv";
        const bool headerMissing=!std::filesystem::exists(csv);
        std::ofstream stream(csv,std::ios::app);
        if(!stream) {
            LogEvent(manualFrame,"manual.capture.error","reason=pose_file_not_writable");
            return;
        }
        if(headerMissing)stream<<"image,frame,x,y,z,yaw_deg,pitch_deg,roll_deg,qx,qy,qz,qw\n";
        const auto forward=Sub(Transform({0,0,1},manualHeadPose),manualHeadPose.p);
        const auto right=Sub(Transform({1,0,0},manualHeadPose),manualHeadPose.p);
        const float yaw=std::atan2(forward.x,forward.z)*180.f/XM_PI;
        const float pitch=std::atan2(-forward.y,
            std::sqrt(forward.x*forward.x+forward.z*forward.z))*180.f/XM_PI;
        const float roll=std::atan2(right.y,
            std::sqrt(right.x*right.x+right.z*right.z))*180.f/XM_PI;
        stream<<std::fixed<<std::setprecision(6)<<screenshot.filename().string()
              <<","<<manualFrame<<","<<manualHeadPose.p.x
              <<","<<manualHeadPose.p.y<<","<<manualHeadPose.p.z
              <<","<<yaw<<","<<pitch<<","<<roll
              <<","<<manualHeadPose.q.x<<","<<manualHeadPose.q.y
              <<","<<manualHeadPose.q.z<<","<<manualHeadPose.q.w<<"\n";
        stream.flush();
        // Replay-ready coordinate pair for the existing diagnostic sweep,
        // in addition to the exact unit quaternion in the pose CSV.
        const auto replay=screenshot.parent_path()/"manual_replay_cameras.csv";
        const bool replayHeader=!std::filesystem::exists(replay);
        std::ofstream view(replay,std::ios::app);
        if(view) {
            if(replayHeader)view<<"name,x,y,z,target_x,target_y,target_z\n";
            view<<std::fixed<<std::setprecision(6)
                <<"manual_"<<manualFrame
                <<","<<manualHeadPose.p.x<<","<<manualHeadPose.p.y
                <<","<<manualHeadPose.p.z
                <<","<<manualHeadPose.p.x+forward.x*10.f
                <<","<<manualHeadPose.p.y+forward.y*10.f
                <<","<<manualHeadPose.p.z+forward.z*10.f<<"\n";
        }
        LogEvent(manualFrame,"manual.capture.saved",
            "file=%s pose_csv=%s xyz=%.6f,%.6f,%.6f ypr_deg=%.3f,%.3f,%.3f quat=%.6f,%.6f,%.6f,%.6f",
            file,csv.string().c_str(),manualHeadPose.p.x,manualHeadPose.p.y,manualHeadPose.p.z,
            yaw,pitch,roll,manualHeadPose.q.x,manualHeadPose.q.y,
            manualHeadPose.q.z,manualHeadPose.q.w);
    };
    size_t diagnosticSlot=0;LabU64 diagnosticRequest=0;
    double diagnosticSince=0,diagnosticStarted=QpcNowSeconds();
    bool diagnosticFailed=false;
    uint64_t diagnosticFrames=0,diagnosticTracked=0,diagnosticSubmitted=0;
    double diagnosticCpuTotal=0,diagnosticCpuPeak=0,diagnosticPeriodMs=0;
    uint64_t diagnosticOverBudget=0,diagnosticRenderRequested=0;
    struct StageCpu {
        double totalMs{};uint64_t calls{};
        double lastMs{};
        void Add(double elapsedMs){lastMs=elapsedMs;totalMs+=elapsedMs;++calls;}
        double Average()const{return calls?totalMs/double(calls):0.0;}
    };
    StageCpu homeCpu,physicsCpu,prepareCpu,acquireCpu,submitCpu;
    // Per-view synchronized profiling precedes each screenshot.
    // A fixed-camera GPU capture follows all screenshots.
    constexpr uint64_t DiagnosticGpuBurstFrames=90;
    constexpr uint64_t DiagnosticGpuTotalFrames=DiagnosticGpuBurstFrames;
    bool diagnosticGpuActive=false,diagnosticGpuFinished=false;
    uint64_t diagnosticGpuFrames=0;
    std::vector<GpuPerformancePassSample> diagnosticGpuPasses;
    std::vector<GpuQueueFrameAnalysis> diagnosticGpuQueues;
    std::vector<GpuGapSample> diagnosticGpuGaps;
    std::vector<uint64_t> diagnosticProfileFrames;diagnosticProfileFrames.reserve(DiagnosticGpuTotalFrames);
    constexpr double DiagnosticViewSeconds=2.0;
    bool viewSampling=false,viewSampleFinished=false;
    struct ViewSample {uint64_t id;double wall,active,physics,prepare,submit,xrWait;bool submitted;};
    std::vector<ViewSample> viewSamples;viewSamples.reserve(256);
    double viewFrameStarted=0.,viewWindowStarted=0.;
    StageCpu xrWaitCpu,xrBeginCpu,xrSyncCpu,xrLocateCpu,xrAcquireColorCpu,
        xrWaitColorCpu,xrAcquireDepthCpu,xrWaitDepthCpu,xrEndCpu;
    if(options.diagnosticSeconds){
        const auto& fog=home.DiagnosticFog();
        LogEvent(0,"sweep.start","views=%u view_sample_s=2.0 transition=teleport max_s=%d weather=winter_street theme=%u fog=%u density=%.4f flies=%u",
            uint32_t(diagnosticViews.size()),options.diagnosticSeconds,home.DiagnosticTheme(),fog.enabled,fog.density,home.DiagnosticPopulation());
    }
    std::string activeWorldId="home",activeWorldName="Home";
    bool menuVisible=false,activeWorldRetainsHomeAmbient=true;
    std::error_code inputConfigurationError;
    auto inputConfigurationWrite=std::filesystem::last_write_time(inputConfiguration,inputConfigurationError);double inputConfigurationCheckAt{};
    auto composedScene=[&](){SceneData scene=home.Scene();if(!activeWorldRetainsHomeAmbient){scene.objects.clear();scene.lights.clear();}if(activePlugin&&activePlugin->Loaded())AppendClientScene(scene,activePlugin->Scene());return scene;};
    while(!io.stop.load(std::memory_order_relaxed)&&!xr.ExitRequested()) {
        graphics.PumpCaptures();
        if(manualTicket) {
            LabCaptureStatusV1 status{};status.structSize=sizeof(status);
            if(graphics.CaptureStatus(manualOwner,status)&&status.requestId==manualTicket){
                if(status.state==LAB_CAPTURE_SAVED_V1) {
                    saveManualCamera(status.fileUtf8);
                    manualTicket=0;
                } else if(status.state==LAB_CAPTURE_FAILED_V1){
                    LogEvent(manualFrame,"manual.capture.failed","reason=%s",status.message);
                    manualTicket=0;
                }
            }
        }
        if(options.diagnosticSeconds){
            if(diagnosticRequest){
                LabCaptureStatusV1 status{};status.structSize=sizeof(status);
                if(graphics.CaptureStatus(diagnosticOwner,status)&&status.requestId==diagnosticRequest){
                    if(status.state==LAB_CAPTURE_SAVED_V1){
                        const auto& view=diagnosticViews[diagnosticSlot];
                        LogEvent(status.frameId,"sweep.capture.saved","slot=%u name=%s file=%s",
                            uint32_t(diagnosticSlot),view.name.c_str(),status.fileUtf8);
                        ++diagnosticSlot;diagnosticRequest=0;diagnosticSince=0;viewSampleFinished=false;
                        if(diagnosticSlot==diagnosticViews.size()){
                            graphics.PreparePerformanceCapture();
                            graphics.BeginPerformanceCapture();
                            xr.SetPerformanceCaptureActive(true);
                            diagnosticGpuActive=true;
                            LogEvent(0,"sweep.gpu_profile_start","frames=%llu",
                                static_cast<unsigned long long>(DiagnosticGpuBurstFrames));
                        }
                    }else if(status.state==LAB_CAPTURE_FAILED_V1){
                        LogEvent(status.frameId,"sweep.capture.failed","slot=%u message=%s",
                            uint32_t(diagnosticSlot),status.message);
                        diagnosticFailed=true;io.stop=true;
                    }
                }
            }
            if(QpcNowSeconds()-diagnosticStarted>options.diagnosticSeconds){
                LogEvent(0,"sweep.timeout","slot=%u pending=%llu elapsed=%.2f",
                    uint32_t(diagnosticSlot),static_cast<unsigned long long>(diagnosticRequest),QpcNowSeconds()-diagnosticStarted);
                diagnosticFailed=true;io.stop=true;
            }
            if(io.stop.load(std::memory_order_relaxed))break;
        }
        xr.PollEvents();if(!xr.Running()){Sleep(5);continue;}
        if(options.diagnosticSeconds&&diagnosticSlot<diagnosticViews.size()&&
           diagnosticSince&&!viewSampling&&!viewSampleFinished){
            graphics.PreparePerformanceCapture();graphics.BeginPerformanceCapture();
            xr.SetPerformanceCaptureActive(true);viewSampling=true;viewSamples.clear();
            viewWindowStarted=QpcNowSeconds();
        }
        viewFrameStarted=QpcNowSeconds();
        XRFrame frame;if(!xr.BeginFrame(frame))continue;
        const bool sampleStageCpu=options.diagnosticSeconds&&!diagnosticGpuActive&&!diagnosticGpuFinished;
        if(diagnosticGpuActive){xrWaitCpu.Add(frame.performanceTiming.waitFrameMs);xrBeginCpu.Add(frame.performanceTiming.beginFrameMs);xrSyncCpu.Add(frame.performanceTiming.syncActionsMs);}
        homeCpu.lastMs=physicsCpu.lastMs=prepareCpu.lastMs=acquireCpu.lastMs=submitCpu.lastMs=0.;
        const double activeFrameBegin=QpcNowSeconds(); // also drives live input.ini hot reload
        if(activeFrameBegin>=inputConfigurationCheckAt){
            inputConfigurationCheckAt=activeFrameBegin+1.;auto modified=std::filesystem::last_write_time(inputConfiguration,inputConfigurationError);
            if(!inputConfigurationError&&modified!=inputConfigurationWrite){inputSystem.LoadConfiguration(inputConfiguration);clientInput.LoadConfiguration(inputConfiguration);home.LoadConfiguration(inputConfiguration);inputConfigurationWrite=modified;}
        }
        performance.BeginFrame(frame,activeWorldId,activeWorldName,options.scene,graphics,io);
        io.input.write()=frame.input;io.input.publish();double now=frame.input.targetDisplayQpc?QpcSeconds(frame.input.targetDisplayQpc):frame.input.time;if(frame.input.Pressed(StandardVrActions().menu,0))menuVisible=!menuVisible;home.SetMenuVisible(menuVisible);pluginHost.SetInputCaptured(menuVisible&&activePlugin!=nullptr);if(activePlugin&&activePlugin->Active()){
            auto scope=performance.Measure(CpuStage::WorldFrame);
            activePlugin->Frame(frame.input.frameId,now);
        }
        std::span<const SceneLight> attractive;if(activePlugin&&activePlugin->Active())attractive=activePlugin->AttractiveLights();
        {
            auto scope=performance.Measure(CpuStage::HomeUpdate);
            const double begin=sampleStageCpu?QpcNowSeconds():0.;
            home.Update(frame.input,now,attractive);
            if(sampleStageCpu)homeCpu.Add((QpcNowSeconds()-begin)*1000.);
        }
        HomeCommand command=home.ConsumeCommand();
        if(command.kind==HomeCommandKind::NewSession){activeWorldId="home";activeWorldName="Home";menuVisible=true;activeWorldRetainsHomeAmbient=true;home.SetMenuVisible(true);pluginHost.SetInputCaptured(false);worlds.UnloadActive();activePlugin.reset();activeModule.reset();pluginHost.ClearWorldServices();clientInput.SetNavigationOptions(true,true);graphics.WaitIdle();worlds.ResetPersistent(home);home.SetAmbientVisible(true);presentation.Set(home.Contributions());SceneData scene=home.Scene();graphics.WaitIdle();graphics.ReplaceScene(scene,presentation.Contributions());audio.LoadScene(scene);}
        else if(command.kind==HomeCommandKind::LoadWorld&&command.world<worldCatalog.size()){
            const auto& selected=worldCatalog[command.world];menuVisible=false;activeWorldRetainsHomeAmbient=selected.navigation.retainHomeAmbient;home.SetMenuVisible(false);home.SetAmbientVisible(activeWorldRetainsHomeAmbient);pluginHost.SetInputCaptured(false);LogEvent(frame.input.frameId,"plugin.transition","stage=begin world=%u",command.world);
            worlds.UnloadActive();activePlugin.reset();activeModule.reset();pluginHost.ClearWorldServices();
            clientInput.SetNavigationOptions(selected.navigation.smoothLocomotion,selected.navigation.heightAdjust);
            pluginHost.ConfigureWorldServices(selected.id,selected.packageRoot,selected.requiredServices);
            activeModule=pluginLoader.Load(selected);
            LogEvent(frame.input.frameId,"plugin.transition","stage=dll_loaded");
            activePlugin=std::make_unique<PluginWorldAdapter>(activeModule->Api(),pluginHost.Api(),selected.packageRoot.wstring(),options.scene,selected.requiredInputActions);
            LogEvent(frame.input.frameId,"plugin.transition","stage=adapter_created");
            worlds.ReplaceActive(*activePlugin);
            LogEvent(frame.input.frameId,"plugin.transition","stage=loaded scene_objects=%u",uint32_t(activePlugin->Scene().objects.size()));
            activePlugin->Activate(uint32_t(home.Scene().meshes.ranges.size()),uint32_t(home.Scene().materials.size()),activeWorldRetainsHomeAmbient?uint32_t(home.Scene().lights.size()):0u);
            LogEvent(frame.input.frameId,"plugin.transition","stage=activated");
            presentation.Set(home.Contributions(),activePlugin->Contributions());SceneData scene=composedScene();
            LogEvent(frame.input.frameId,"plugin.transition","stage=scene_composed objects=%u",uint32_t(scene.objects.size()));
            graphics.WaitIdle();
            LogEvent(frame.input.frameId,"plugin.transition","stage=gpu_idle");
            graphics.ReplaceScene(scene,presentation.Contributions());
            LogEvent(frame.input.frameId,"plugin.transition","stage=scene_replaced");
            audio.LoadScene(scene);
            activeWorldId=selected.id;activeWorldName=selected.name;
            LogEvent(frame.input.frameId,"plugin.transition","stage=complete");
        }
        else if(command.kind==HomeCommandKind::Quit)io.stop=true;
        // Reserve the current-frame GPU publications before the worker starts.
        // The renderer can submit independent work while simulation runs;
        // dependent batches wait until the worker has submitted its signal.
        if(frame.render)graphics.ReserveStreams();
        struct PhysicsFrameWork {
            PhysicsRuntime* runtime;
            double now;
            uint64_t frame;
            double cpuMs{};
        };
        PhysicsFrameWork physicsWork{&physics,now,frame.input.frameId};
        JobSystem::Group physicsGroup;
        auto submissionProgress=std::make_shared<GpuSubmissionProgress>();
        physicsGroup.progress=submissionProgress;
        const double physicsSubmittedAt=QpcNowSeconds();
        jobs.Submit([](void* context,uint32_t,uint32_t){
            auto& work=*static_cast<PhysicsFrameWork*>(context);
            const double started=QpcNowSeconds();
            // Core is intentionally compiled without C++ exceptions.
            // Fatal PhysX failures use Fail(), which logs and terminates.
            work.runtime->Advance(work.now,work.frame);
            work.cpuMs=(QpcNowSeconds()-started)*1000.;
        },&physicsWork,0,1,&physicsGroup);
        bool physicsJoined=false;
        auto finishPhysics=[&]{
            if(physicsJoined)return;
            const double waitStarted=QpcNowSeconds();
            jobs.Wait(physicsGroup);
            const double waitedMs=(QpcNowSeconds()-waitStarted)*1000.;
            physicsJoined=true;
            // Report actual worker execution, never the duration of overlapping
            // main-thread work as a fictitious physics CPU stage.
            performance.RecordCpuStage(CpuStage::PhysicsAdvance,physicsWork.cpuMs);
            if(sampleStageCpu)physicsCpu.Add(physicsWork.cpuMs);
            if(SessionLogCompact())LogEvent(frame.input.frameId,"physics.overlap",
                "worker_cpu_ms=%.4f main_wait_ms=%.4f elapsed_since_submit_ms=%.4f",
                physicsWork.cpuMs,waitedMs,(QpcNowSeconds()-physicsSubmittedAt)*1000.);
        };
        home.PublishMenu();
        {
            auto scope=performance.Measure(CpuStage::AudioAndEvents);
            auto& audioFrame=io.audio.write();home.FillAudio(audioFrame,frame.input,now);homeHost.AppendAudio(audioFrame);pluginHost.AppendAudio(audioFrame);io.audio.publish();
            HapticEvent h;while(io.haptics.pop(h))xr.Haptic(h);PhysicsEvent event;while(io.physicsEvents.pop(event)){LogEvent(event.inputFrameId,"physics.event","kind=%u a=%llu b=%llu hand=%u strength=%.4f xyz=%.4f,%.4f,%.4f",uint32_t(event.kind),static_cast<unsigned long long>(event.a.value),static_cast<unsigned long long>(event.b.value),event.hand,event.strength,event.position.x,event.position.y,event.position.z);}
        }
        if(frame.render){
            const bool captureHands=options.diagnosticSeconds&&(diagnosticSlot<diagnosticViews.size()||diagnosticGpuActive);
            DiagnosticView diagnosticCamera{};
            if(captureHands){
                diagnosticCamera=diagnosticViews[std::min(diagnosticSlot,diagnosticViews.size()-1)];
                home.SetDiagnosticHands(true,diagnosticCamera.position,diagnosticCamera.target);
            }
            else home.SetDiagnosticHands(false);
            bool prepared;
            {
                auto scope=performance.Measure(CpuStage::GraphicsPrepare);
                const double begin=sampleStageCpu?QpcNowSeconds():0.;
                prepared=graphics.Prepare(frame.input);
                if(sampleStageCpu)prepareCpu.Add((QpcNowSeconds()-begin)*1000.);
            }
            if(prepared){
                bool acquired;
                {
                    auto scope=performance.Measure(CpuStage::LateLocateAcquire);
                    const double begin=sampleStageCpu?QpcNowSeconds():0.;
                    xr.LocateLate(frame);
                    acquired=frame.tracked&&xr.AcquireSwapchains(frame);
                    if(sampleStageCpu)acquireCpu.Add((QpcNowSeconds()-begin)*1000.);
                    if(diagnosticGpuActive){
                        xrLocateCpu.Add(frame.performanceTiming.locateViewsMs);
                        xrAcquireColorCpu.Add(frame.performanceTiming.acquireColorMs);
                        xrWaitColorCpu.Add(frame.performanceTiming.waitColorMs);
                        xrAcquireDepthCpu.Add(frame.performanceTiming.acquireDepthMs);
                        xrWaitDepthCpu.Add(frame.performanceTiming.waitDepthMs);
                    }
                }
                if(acquired){
                    // Quest left X (interaction.mode): independent one-shot
                    // capture of the real view, not a diagnostic flying camera.
                    if(!options.diagnosticSeconds&&!manualTicket&&
                        frame.input.Pressed(StandardVrActions().mode,0)) {
                        LabCaptureRequestV1 req{sizeof(LabCaptureRequestV1),0,1,0};
                        LabU64 requested=0;
                        if(graphics.RequestCapture(manualOwner,req,requested)) {
                            manualTicket=requested;
                            manualFrame=frame.input.frameId;
                            manualHeadPose=frame.input.head;
                            LogEvent(manualFrame,"manual.capture.request",
                                "ticket=%llu xyz=%.6f,%.6f,%.6f",
                                static_cast<unsigned long long>(manualTicket),
                                manualHeadPose.p.x,manualHeadPose.p.y,manualHeadPose.p.z);
                        } else LogEvent(frame.input.frameId,"manual.capture.failed",
                                        "reason=request_rejected");
                    }
                    if(options.diagnosticSeconds && diagnosticSlot<diagnosticViews.size()){
                        const auto& view=diagnosticViews[diagnosticSlot];
                        xr.SetDiagnosticCamera(frame,diagnosticCamera.position,diagnosticCamera.target);
                        if(!diagnosticSince){
                            diagnosticSince=QpcNowSeconds();
                            const auto cameraPose=FromMatrix(XMMatrixInverse(nullptr,
                                XMMatrixLookAtLH(XMLoadFloat3(&view.position),
                                                 XMLoadFloat3(&view.target),XMVectorSet(0,1,0,0))));
                            const auto direction=Sub(view.target,view.position);
                            const float yaw=std::atan2(direction.x,direction.z)*180.f/XM_PI;
                            const float pitch=std::atan2(-direction.y,
                                std::sqrt(direction.x*direction.x+direction.z*direction.z))*180.f/XM_PI;
                            LogEvent(frame.input.frameId,"sweep.spawn","slot=%u name=%s",uint32_t(diagnosticSlot),view.name.c_str());
                            LogEvent(frame.input.frameId,"sweep.view",
                                "slot=%u name=%s eye=%.3f,%.3f,%.3f target=%.3f,%.3f,%.3f yaw_deg=%.3f pitch_deg=%.3f quat=%.6f,%.6f,%.6f,%.6f",
                                uint32_t(diagnosticSlot),view.name.c_str(),
                                view.position.x,view.position.y,view.position.z,
                                view.target.x,view.target.y,view.target.z,yaw,pitch,
                                cameraPose.q.x,cameraPose.q.y,cameraPose.q.z,cameraPose.q.w);
                        }
                        if(!diagnosticRequest&&viewSampleFinished){
                            LabCaptureRequestV1 request{sizeof(LabCaptureRequestV1),0,1,0};LabU64 ticket=0;
                            if(!graphics.RequestCapture(diagnosticOwner,request,ticket)){
                                LogEvent(frame.input.frameId,"sweep.capture.failed","slot=%u message=request_refused",uint32_t(diagnosticSlot));
                                diagnosticFailed=true;io.stop=true;
                            }else{
                                diagnosticRequest=ticket;
                                LogEvent(frame.input.frameId,"sweep.capture.request","slot=%u name=%s request=%llu",
                                    uint32_t(diagnosticSlot),view.name.c_str(),static_cast<unsigned long long>(ticket));
                            }
                        }
                    }else if(options.diagnosticSeconds&&(diagnosticGpuActive)&&!diagnosticViews.empty()){
                        // Hold a fixed three-quarter viewpoint during the GPU burst.
                        const auto& view=diagnosticViews.back();
                        xr.SetDiagnosticCamera(frame,view.position,view.target);
                    }
                    auto scope=performance.Measure(CpuStage::GraphicsRender);
                    const double begin=sampleStageCpu?QpcNowSeconds():0.;
                    graphics.Render(frame,{
                        submissionProgress,
                        [&]{return physicsGroup.Complete();},
                        finishPhysics
                    });
                    if(sampleStageCpu)submitCpu.Add((QpcNowSeconds()-begin)*1000.);
                }
            }
        }
        finishPhysics(); // Also complete physics when XR rendering was skipped.
        graphics.CancelReservedStreams();
        {
            auto scope=performance.Measure(CpuStage::XrEndFrame);
            const double xrEndStart=diagnosticGpuActive?QpcNowSeconds():0.;
            xr.EndFrame(frame);
            if(diagnosticGpuActive)xrEndCpu.Add((QpcNowSeconds()-xrEndStart)*1000.);
        }
        double activeCpuMs=options.diagnosticSeconds?(QpcNowSeconds()-activeFrameBegin)*1000.:0.;
        double displayBudgetMs=double(frame.displayPeriod)/1'000'000.0;
        // Unconditional frame tracing is disabled in ordinary VR gameplay.
        // Gesture-triggered profiling uses the separate PerformanceCapture.
        if(sampleStageCpu){
            ++diagnosticFrames;diagnosticTracked+=frame.tracked?1:0;diagnosticSubmitted+=frame.submitted?1:0;
            diagnosticRenderRequested+=frame.render?1:0;
            diagnosticCpuTotal+=activeCpuMs;diagnosticCpuPeak=(std::max)(diagnosticCpuPeak,activeCpuMs);
            diagnosticPeriodMs+=displayBudgetMs;
            diagnosticOverBudget+=(displayBudgetMs>0&&activeCpuMs>displayBudgetMs)?1u:0u;
        }
        performance.EndFrame(frame,graphics,xr,io);
        if(viewSampling){
            viewSamples.push_back({frame.input.frameId,(QpcNowSeconds()-viewFrameStarted)*1000.,
                activeCpuMs,physicsCpu.lastMs,prepareCpu.lastMs,submitCpu.lastMs,
                frame.performanceTiming.waitFrameMs,frame.submitted});
            if(QpcNowSeconds()-viewWindowStarted>=DiagnosticViewSeconds){
                const double measuredSeconds=QpcNowSeconds()-viewWindowStarted;
                graphics.EndPerformanceCapture();xr.SetPerformanceCaptureActive(false);
                viewSampling=false;viewSampleFinished=true;
                std::vector<GpuPerformancePassSample> passes;
                std::vector<GpuQueueFrameAnalysis> queues;std::vector<GpuGapSample> gaps;
                // Readback waits occur AFTER the sample window, never in its timings.
                graphics.CollectPerformanceCapture(passes,queues,gaps);
                std::map<uint64_t,double> gpu;
                for(const auto& q:queues)gpu[q.frameId]=std::max(gpu[q.frameId],q.spanMs);
                std::vector<double> wall,active,phys,prep,submit,wait,gpuMs;
                uint32_t submitted=0;
                for(const auto& row:viewSamples){
                    wall.push_back(row.wall);active.push_back(row.active);phys.push_back(row.physics);
                    prep.push_back(row.prepare);submit.push_back(row.submit);wait.push_back(row.xrWait);
                    submitted+=row.submitted?1u:0u;
                    auto g=gpu.find(row.id);if(g!=gpu.end())gpuMs.push_back(g->second);
                    LogEvent(row.id,"sweep.sample",
                        "slot=%u wall_ms=%.4f active_ms=%.4f physics_ms=%.4f prepare_ms=%.4f submit_ms=%.4f xr_wait_ms=%.4f gpu_queue_span_ms=%.4f submitted=%u",
                        uint32_t(diagnosticSlot),row.wall,row.active,row.physics,row.prepare,row.submit,row.xrWait,
                        g!=gpu.end()?g->second:-1.,unsigned(row.submitted));
                }
                const auto emit=[&](const char* metric,const std::vector<double>& values){
                    const auto stats=DiagnosticStatistics(values);
                    LogEvent(0,"sweep.view_stats",
                        "slot=%u name=%s metric=%s n=%u median_ms=%.4f p90_ms=%.4f trimmed_mean_ms=%.4f spikes=%u spike_threshold_ms=%.4f",
                        uint32_t(diagnosticSlot),diagnosticViews[diagnosticSlot].name.c_str(),metric,
                        uint32_t(values.size()),stats.median,stats.p90,stats.trimmedMean,stats.spikes,stats.spikeThreshold);
                };
                emit("wall",wall);emit("active",active);emit("physics",phys);emit("prepare",prep);
                emit("submit",submit);emit("xr_wait",wait);emit("gpu_queue_span",gpuMs);
                LogEvent(0,"sweep.view_rate",
                    "slot=%u name=%s submitted=%u measured_seconds=%.4f app_submitted_fps=%.3f typical_loop_hz=%.3f target_hz=%.3f gpu_frames=%u dropped_passes=%llu",
                    uint32_t(diagnosticSlot),diagnosticViews[diagnosticSlot].name.c_str(),submitted,measuredSeconds,
                    measuredSeconds>0?submitted/measuredSeconds:0.,DiagnosticStatistics(wall).median>0?1000./DiagnosticStatistics(wall).median:0.,
                    displayBudgetMs>0?1000./displayBudgetMs:0.,uint32_t(gpuMs.size()),
                    static_cast<unsigned long long>(graphics.PerformanceInfo().droppedGpuPasses));
                std::map<std::string,std::vector<double>> passTimes;
                for(const auto& pass:passes)passTimes[pass.name].push_back(pass.milliseconds);
                for(const auto& [name,times]:passTimes){
                    const auto stats=DiagnosticStatistics(times);
                    LogEvent(0,"sweep.view_pass","slot=%u pass=%s calls=%u median_ms=%.4f p90_ms=%.4f trimmed_mean_ms=%.4f",
                        uint32_t(diagnosticSlot),name.c_str(),uint32_t(times.size()),stats.median,stats.p90,stats.trimmedMean);
                }
            }
        }

        if(diagnosticGpuActive){
            diagnosticProfileFrames.push_back(frame.input.frameId);
            ++diagnosticGpuFrames;
            if(diagnosticGpuFrames>=DiagnosticGpuTotalFrames){
                graphics.EndPerformanceCapture();
                xr.SetPerformanceCaptureActive(false);
                diagnosticGpuActive=false;
                diagnosticGpuFinished=true;
                LogEvent(0,"sweep.gpu_profile_end","frames=%llu",
                    static_cast<unsigned long long>(diagnosticGpuFrames));
                // Preserve GPU samples before reusing the profiler.
                graphics.CollectPerformanceCapture(diagnosticGpuPasses,diagnosticGpuQueues,diagnosticGpuGaps);
                // Existing per-view and GPU timings make a second calibration redundant.
                io.stop=true;
            }
        }

    }
    io.stop=true;
    if(diagnosticGpuActive){
        graphics.EndPerformanceCapture();
        xr.SetPerformanceCaptureActive(false);
        diagnosticGpuActive=false;
    }
    if(diagnosticGpuFrames&&diagnosticGpuPasses.empty())
        graphics.CollectPerformanceCapture(diagnosticGpuPasses,diagnosticGpuQueues,diagnosticGpuGaps);
    // Sample DXGI memory usage once, after the frame loop; never poll it per frame.
    const auto diagVram=options.diagnosticSeconds?graphics.MemoryStats():GpuMemoryStats{};
    const auto diagGpu=options.diagnosticSeconds?graphics.PerformanceInfo():GraphicsPerformanceInfo{};
    graphics.WaitIdle();graphics.PumpCaptures();graphics.DetachStream(homeStream);worlds.UnloadAll();activePlugin.reset();activeModule.reset();pluginHost.ClearWorldServices();homeHost.ClearWorldServices();physics.Shutdown();audio.Stop();graphics.WaitIdle();audio.UnloadScene();graphics.UnloadScene();jobs.Stop();
    const auto validation=graphics.ValidationState();bool clean=!validation.deviceRemoved&&!validation.errors&&!validation.warnings;
    if(options.diagnosticSeconds){
        LogEvent(0,"sweep.summary","views_saved=%u expected=%u frames=%llu tracked=%llu submitted=%llu avg_cpu_ms=%.3f peak_cpu_ms=%.3f elapsed_s=%.2f gpu_errors=%u gpu_warnings=%u",
            uint32_t(diagnosticSlot),uint32_t(diagnosticViews.size()),static_cast<unsigned long long>(diagnosticFrames),
            static_cast<unsigned long long>(diagnosticTracked),static_cast<unsigned long long>(diagnosticSubmitted),
            diagnosticFrames?diagnosticCpuTotal/double(diagnosticFrames):0,diagnosticCpuPeak,
            QpcNowSeconds()-diagnosticStarted,unsigned(validation.errors),unsigned(validation.warnings));
        LogEvent(0,"sweep.cpu","home_avg_ms=%.3f physics_avg_ms=%.3f prepare_avg_ms=%.3f acquire_avg_ms=%.3f submit_avg_ms=%.3f over_budget=%llu display_period_ms=%.3f requested=%llu",
            homeCpu.Average(),physicsCpu.Average(),prepareCpu.Average(),acquireCpu.Average(),submitCpu.Average(),
            static_cast<unsigned long long>(diagnosticOverBudget),
            diagnosticFrames?diagnosticPeriodMs/double(diagnosticFrames):0.,
            static_cast<unsigned long long>(diagnosticRenderRequested));
        LogEvent(0,"sweep.gpu","adapter=%s width=%u height=%u views=%u vram_usage_mb=%.1f vram_budget_mb=%.1f vram_physical_mb=%.1f gpu_validation=%u",
            diagGpu.adapter.c_str(),diagGpu.renderWidth,diagGpu.renderHeight,diagGpu.viewCount,
            double(diagVram.usage)/1048576.,double(diagVram.budget)/1048576.,
            double(diagGpu.dedicatedVramBytes)/1048576.,unsigned(options.gpuValidation));
        if(diagnosticGpuFinished){
            // Clocks from different GPU queues may not share an epoch. Never
            // subtract a Copy clock timestamp from a Direct clock timestamp.
            // Compute the longest independently measured queue span per frame.
            std::array<std::map<uint64_t,double>,1> bottlenecks;
            std::array<std::map<std::string,std::pair<double,uint64_t>>,1> passTotals;
            std::array<std::array<std::pair<double,uint64_t>,3>,1> queueSpans{};
            std::array<std::array<std::pair<double,uint64_t>,3>,1> queueWork{};
            const auto phaseOf=[&](uint64_t id)->uint32_t{
                (void)id;return 0u;
            };
            const auto stableProfileFrame=[&](uint64_t id){
                auto it=std::find(diagnosticProfileFrames.begin(),diagnosticProfileFrames.end(),id);
                return it!=diagnosticProfileFrames.end()&&size_t(it-diagnosticProfileFrames.begin())%DiagnosticGpuBurstFrames>=15;
            };
            for(const auto& sample:diagnosticGpuQueues){
                if(!stableProfileFrame(sample.frameId))continue;
                if(sample.spanMs<0||sample.spanMs>1000.)continue;
                uint32_t phase=phaseOf(sample.frameId),queue=uint32_t(sample.queue);
                if(queue>=3)continue;
                auto& maxSpan=bottlenecks[phase][sample.frameId];
                maxSpan=std::max(maxSpan,sample.spanMs);
                queueSpans[phase][queue].first+=sample.spanMs;++queueSpans[phase][queue].second;
                queueWork[phase][queue].first+=sample.workMs;++queueWork[phase][queue].second;
            }
            for(const auto& pass:diagnosticGpuPasses){
                if(!stableProfileFrame(pass.frameId))continue;
                if(pass.milliseconds<0||pass.milliseconds>1000.)continue;
                auto& total=passTotals[phaseOf(pass.frameId)][pass.name];
                total.first+=pass.milliseconds;++total.second;
            }
            const char* phases[]{"baseline"};
            const char* queueNames[]{"copy","compute","direct"};
            double avgBottleneck[1]{};
            for(uint32_t phase=0;phase<1;++phase){
                std::vector<double> ms;
                for(const auto& [frameId,span]:bottlenecks[phase]){
                    (void)frameId;ms.push_back(span);
                }
                std::sort(ms.begin(),ms.end());
                double total=0.;for(double v:ms)total+=v;
                avgBottleneck[phase]=ms.empty()?0.:total/double(ms.size());
                const auto robust=DiagnosticStatistics(ms);
                LogEvent(0,"sweep.gpu_robust","phase=%s n=%u median_ms=%.4f p90_ms=%.4f trimmed_mean_ms=%.4f spikes=%u",
                    phases[phase],uint32_t(ms.size()),robust.median,robust.p90,robust.trimmedMean,robust.spikes);
                const double p95=ms.empty()?0.:ms[size_t(std::ceil(ms.size()*.95))-1];
                LogEvent(0,"sweep.gpu_timing",
                    "phase=%s sampled_frames=%u avg_busiest_queue_ms=%.3f p95_busiest_queue_ms=%.3f peak_busiest_queue_ms=%.3f dropped_passes=%llu",
                    phases[phase],uint32_t(ms.size()),avgBottleneck[phase],p95,
                    ms.empty()?0.:ms.back(),static_cast<unsigned long long>(diagGpu.droppedGpuPasses));
                std::vector<std::pair<std::string,std::pair<double,uint64_t>>> ranked(
                    passTotals[phase].begin(),passTotals[phase].end());
                std::sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.second.first>b.second.first;});
                for(size_t i=0;i<std::min<size_t>(8,ranked.size());++i){
                    const auto& [name,totals]=ranked[i];
                    LogEvent(0,"sweep.gpu_pass",
                        "phase=%s rank=%u name=%s calls=%llu avg_call_ms=%.3f",
                        phases[phase],uint32_t(i+1),name.c_str(),
                        static_cast<unsigned long long>(totals.second),
                        totals.second?totals.first/double(totals.second):0.);
                }
                for(uint32_t q=0;q<3;++q){
                    const auto span=queueSpans[phase][q],work=queueWork[phase][q];
                    LogEvent(0,"sweep.gpu_queue",
                        "phase=%s queue=%s frames=%llu avg_span_ms=%.3f avg_work_ms=%.3f",
                        phases[phase],queueNames[q],
                        static_cast<unsigned long long>(span.second),
                        span.second?span.first/double(span.second):0.,
                        work.second?work.first/double(work.second):0.);
                }
            }
            LogEvent(0,"sweep.xr",
                "frames=%llu wait_frame_ms=%.3f begin_frame_ms=%.3f sync_actions_ms=%.3f locate_ms=%.3f acquire_color_ms=%.3f wait_color_ms=%.3f acquire_depth_ms=%.3f wait_depth_ms=%.3f end_frame_ms=%.3f",
                static_cast<unsigned long long>(diagnosticGpuFrames),
                xrWaitCpu.Average(),xrBeginCpu.Average(),xrSyncCpu.Average(),xrLocateCpu.Average(),
                xrAcquireColorCpu.Average(),xrWaitColorCpu.Average(),xrAcquireDepthCpu.Average(),
                xrWaitDepthCpu.Average(),xrEndCpu.Average());
        }
        if(diagnosticFailed||diagnosticSlot!=diagnosticViews.size()||!diagnosticGpuFinished)return 3;
    }
    return options.gpuValidation&&!clean?2:0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
#if LABOVR_DIAGNOSTIC_TRACE
    using lab::LogEvent;
#endif
    lab::ConfigureCore(lab::LaboVRIdentity());lab::RunOptions options;int count=0;auto* args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)lab::Fail("Cannot parse command line");
    if(args){for(int i=1;i<count;i++)if(wcscmp(args[i],L"--log")==0&&i+1<count){lab::StartSessionLog(args[i+1]);break;}LogEvent(0,"startup.build","engine=%s pipeline_abi=%s cuda_expected=sm_%u toolkit=%s",LAB_ENGINE_VERSION,LAB_PIPELINE_ABI,LAB_CUDA_ARCHITECTURE,LAB_CUDA_TOOLKIT_VERSION);
    for(int i=1;i<count;i++){if(wcscmp(args[i],L"--log")==0&&i+1<count){++i;}else if(wcscmp(args[i],L"--scene")==0&&i+1<count)options.scene=args[++i];else if(wcscmp(args[i],L"--gpu-validation")==0)options.gpuValidation=true;
        else if(wcscmp(args[i],L"--diagnostic-sweep")==0&&i+1<count){
            wchar_t* end=nullptr;long seconds=std::wcstol(args[++i],&end,10);
            if(!end||*end||seconds<15||seconds>600)lab::Fail("Diagnostic sweep duration must be 15..600 seconds");
            options.diagnosticSeconds=int(seconds);
        }else if(wcscmp(args[i],L"--diagnostic-camera-file")==0&&i+1<count)
            options.diagnosticCamerasFile=args[++i];
        else lab::Fail("Unknown or incomplete option");}LocalFree(args);}
    if(options.diagnosticSeconds&&options.diagnosticCamerasFile.empty())lab::Fail("Diagnostic sweep requires --diagnostic-camera-file");
    lab::SetSessionLogCompact(options.diagnosticSeconds!=0);
    lab::SetSessionLogGameplay(options.diagnosticSeconds==0);
    int result=0;auto io=std::make_unique<lab::Channels>();std::thread render([&]{result=lab::RenderThread(*io,options);});while(!io->stop.load(std::memory_order_relaxed)){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){if(message.message==WM_QUIT)io->stop=true;TranslateMessage(&message);DispatchMessageW(&message);}if(GetAsyncKeyState(VK_ESCAPE)&0x8000)io->stop=true;Sleep(5);}render.join();LogEvent(0,"session.end","clean_shutdown=1 result=%d",result);lab::StopSessionLog();return result;
}
