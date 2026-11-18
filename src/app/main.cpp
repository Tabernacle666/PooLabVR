#include "Core.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "VrInputMapping.h"
#include "WorldRegistry.h"
#include "DesktopInput.h"
#include "DesktopOutput.h"
#include "SpectacleSpace.h"
#include <shellapi.h>
#include <algorithm>
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
struct NavigationContinuity {Pose pose{};bool valid{};};

struct DiagnosticCapture {
    std::wstring path;
    bool interactive{},requested{},attempted{},captured{};
    int64_t firstFrame{};
    void ResetPending(){if(!attempted){firstFrame=0;requested=false;}}
    void BeforeRender(Graphics& graphics){
        if(path.empty()||attempted||requested||!firstFrame||QpcSeconds(QpcCounter()-firstFrame)<10.0)return;
        DiagnosticLine("etape=demande_capture\r\n");graphics.RequestCapture(path);requested=true;
    }
    bool AfterRender(Graphics& graphics,bool submitted,bool desktop,std::atomic<bool>& frameSeen){
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
        DiagnosticLine(line);return !interactive;
    }
};

void ConfigureAtmosphere(Graphics& graphics,bool logo){
    AtmosphereSettings atmosphere{};
    if(logo){atmosphere.center=SpectacleCenter;atmosphere.radius=SpectacleRadius;atmosphere.color={.32f,.55f,.25f};atmosphere.density=.19f;atmosphere.emissionReferencePower=20.f;}
    graphics.SetAtmosphere(atmosphere);
}

RenderContributions WorldContributions(WorldRegistry& world,bool logo){
    auto result=world.Contributions();
    if(logo)result.appendContext=[](void*,RenderScene& render,const FrameInput& input,double time){
        SceneLight light{};light.position={0.f,.12f,5.5f};light.radius=9.f;
        light.color={1.f,.52f,.19f};light.power=20.f*(1.f+.07f*std::sin(float(time)*.55f)+.025f*std::sin(float(time)*1.13f));
        light.direction=Unit(Sub(input.head.p,light.position));light.outerCos=-.65f;light.innerCos=.25f;
        light.id=0x70000001u;light.flags=1;render.lights.push_back(light);render.lightCount=uint32_t(render.lights.size());
    };
    return result;
}

LoopResult RunVr(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                 const std::wstring& config,const SceneData& scene,WorldRegistry& world,JobSystem& jobs,
                 RenderContributions contributions,bool logo,bool& developer,NavigationContinuity& navigation,DiagnosticCapture& diagnostic){
    PostMessageW(window,OutputWindowMessage,0,0);
    StandardVrInput mappings;mappings.LoadConfiguration(config);mappings.SetNavigationOptions(true,true);mappings.SetSpectacleBoundary(logo);
    if(navigation.valid)mappings.SetInitialPose(navigation.pose);
    XR xr;Graphics graphics;
    DiagnosticLine("etape=openxr\r\n");xr.InitializeInstance({"OASIS","OASIS",1},input,mappings.Mapping());
    DiagnosticLine("etape=dx12\r\n");graphics.Initialize(xr.GraphicsRequirements());
    DiagnosticLine("etape=session_vr\r\n");xr.InitializeSession(graphics.Device(),graphics.Queue());
    DiagnosticLine("etape=chargement\r\n");graphics.Configure(xr,scene,jobs,contributions);
    ConfigureAtmosphere(graphics,logo);graphics.SetDeveloperMode(developer);
    DiagnosticLine("etape=attente_image\r\n");
    int64_t menuClick=0;bool switchOutput=false,finishCapture=false,exitInitiated=false;
    ULONGLONG exitRequestedAt=0;
    bool pageUpHeld=false,pageDownHeld=false;
    while(!xr.ExitRequested()){
        xr.PollEvents();if(xr.ExitRequested())break;
        if(stop.load(std::memory_order_relaxed)||switchOutput||finishCapture){
            if(!xr.Running())break;
            if(!exitInitiated){xr.RequestExit();exitInitiated=true;exitRequestedAt=GetTickCount64();}
            if(GetTickCount64()-exitRequestedAt>=2500)break;
        }
        if(!xr.Running()){Sleep(5);continue;}
        XRFrame frame;if(!xr.BeginFrame(frame))continue;
        const auto& actions=StandardVrActions();
        if(!exitInitiated&&frame.input.Pressed(actions.mode,1)){developer=!developer;graphics.SetDeveloperMode(developer);}
        const bool pageUp=(GetAsyncKeyState(VK_PRIOR)&0x8000)!=0;
        const bool pageDown=(GetAsyncKeyState(VK_NEXT)&0x8000)!=0;
        const int scaleStep=!exitInitiated&&frame.input.focused?
            int(pageUp&&!pageUpHeld)-int(pageDown&&!pageDownHeld):0;
        pageUpHeld=pageUp;
        pageDownHeld=pageDown;
        if(!exitInitiated&&frame.input.Pressed(actions.next,1))switchOutput=true;
        if(!exitInitiated&&frame.input.Pressed(actions.quit,0)){
            const int64_t now=QpcCounter();
            if(menuClick&&now-menuClick<=2*QpcFrequency()){stop.store(true,std::memory_order_relaxed);menuClick=0;}
            else menuClick=now;
        }
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        if(frame.render&&graphics.Prepare(frame.input)){
            xr.LocateLate(frame);
            if(frame.tracked&&xr.AcquireSwapchains(frame)){diagnostic.BeforeRender(graphics);graphics.Render(frame);}
        }
        xr.EndFrame(frame);
        if(!switchOutput)graphics.UpdateDlss(scaleStep);
        if(frame.submitted){navigation.pose=frame.input.head;navigation.valid=true;}
        if(diagnostic.AfterRender(graphics,frame.submitted,false,frameSeen))finishCapture=true;
    }
    graphics.WaitIdle();graphics.UnloadScene();
    return switchOutput&&!stop.load(std::memory_order_relaxed)?LoopResult::SwitchOutput:LoopResult::Exit;
}

LoopResult RunDesktop(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,HWND window,InputSystem& input,
                      const SceneData& scene,WorldRegistry& world,JobSystem& jobs,RenderContributions contributions,
                      bool logo,bool& developer,NavigationContinuity& navigation,DiagnosticCapture& diagnostic){
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
        ConfigureAtmosphere(graphics,logo);graphics.SetDeveloperMode(developer);
    };
    DiagnosticLine("etape=chargement_ecran\r\n");configure();
    DesktopInput controller;controller.SetSpectacleBoundary(logo);if(navigation.valid)controller.SetPose(navigation.pose);
    uint64_t frameId=0;bool switchOutput=false;int64_t previousFrameQpc=0;
    while(!stop.load(std::memory_order_relaxed)){
        if(!GetClientRect(window,&rectangle))break;
        const LONG w=rectangle.right-rectangle.left,h=rectangle.bottom-rectangle.top;
        if(w<=0||h<=0||IsIconic(window)){Sleep(10);continue;}
        width=uint32_t(w);height=uint32_t(h);
        if(width!=desktop.Width()||height!=desktop.Height()){graphics.ReleaseOutput();desktop.Resize(width,height);configure();}
        const auto controls=controller.Poll(++frameId,width,height,input.ActionCount(),GetForegroundWindow()==window);
        navigation.pose=controls.input.head;navigation.valid=true;
        if(controls.toggleDeveloper){developer=!developer;graphics.SetDeveloperMode(developer);}
        if(controls.quit){stop.store(true,std::memory_order_relaxed);break;}
        if(controls.toggleOutput){switchOutput=true;break;}
        XRFrame frame{};frame.begun=frame.render=frame.ready=frame.tracked=true;frame.viewCount=1;
        frame.color=desktop.CurrentIndex();frame.depth=0;frame.input=controls.input;
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
        if(graphics.Prepare(frame.input)){diagnostic.BeforeRender(graphics);graphics.Render(frame);}
        if(frame.submitted)desktop.Present();else SwitchToThread();
        if(diagnostic.AfterRender(graphics,frame.submitted,true,frameSeen))break;
    }
    graphics.ReleaseOutput();
    return switchOutput&&!stop.load(std::memory_order_relaxed)?LoopResult::SwitchOutput:LoopResult::Exit;
}

int Run(std::atomic<bool>& stop,std::atomic<bool>& frameSeen,std::atomic<ULONGLONG>& modeStarted,HWND window,const std::wstring& worldPath,
        const std::wstring& capturePath,bool interactive,bool desktop){
    ComScope com;JobSystem jobs;jobs.Start();
    InputSystem input;DeclareStandardVrInputActions(input);
    const auto config=AssetPath(L"input.ini");input.LoadConfiguration(config);
    const bool logo=worldPath.empty();
    SceneData scene=LoadSceneGLTF(logo?AssetPath(L"oasis_logo.glb"):worldPath);
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
        const auto result=desktop?RunDesktop(stop,frameSeen,window,input,scene,world,jobs,contributions,logo,developer,navigation,diagnostic):
            RunVr(stop,frameSeen,window,input,config,scene,world,jobs,contributions,logo,developer,navigation,diagnostic);
        if(result!=LoopResult::SwitchOutput)break;
        desktop=!desktop;diagnostic.ResetPending();
    }
    jobs.Stop();
    if(!capturePath.empty()){DiagnosticLine(diagnostic.captured?"resultat=termine\r\n":"capture=aucune\r\nresultat=sans_capture\r\n");return diagnostic.captured?0:1;}
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
    bool interactive=false,desktop=false;
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
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    if(!diagnosticDirectory.empty()&&!OASIS::StartDiagnostic(diagnosticDirectory)){
        MessageBoxW(nullptr,L"Impossible de creer le journal OASIS.",L"OASIS",MB_OK|MB_ICONERROR);
        return 1;
    }
    const std::wstring capturePath=diagnosticDirectory.empty()?std::wstring{}:diagnosticDirectory+L"\\OASIS.png";
    HWND window=OASIS::CreateOutputWindow(instance);
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::atomic<bool> frameSeen{};
    std::atomic<ULONGLONG> modeStarted{GetTickCount64()};
    std::thread render([&]{result=OASIS::Run(stop,frameSeen,modeStarted,window,worldPath,capturePath,interactive,desktop);finished.store(true,std::memory_order_release);});
    ULONGLONG exitDeadline=0;
    const ULONGLONG diagnosticDeadline=capturePath.empty()?0:GetTickCount64()+60000;
    while(!finished.load(std::memory_order_acquire)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT)stop.store(true,std::memory_order_relaxed);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)stop.store(true,std::memory_order_relaxed);
        const bool startupExpired=interactive&&!frameSeen.load(std::memory_order_acquire)&&
            GetTickCount64()-modeStarted.load(std::memory_order_acquire)>=60000;
        if(diagnosticDeadline&&(startupExpired||(!interactive&&GetTickCount64()>=diagnosticDeadline))&&!stop.exchange(true,std::memory_order_relaxed)){
            OASIS::DiagnosticLine("erreur=delai_de_60_secondes\r\n");
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
