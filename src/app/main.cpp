#include "Core.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "VrInputMapping.h"
#include "WorldRegistry.h"
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
int Run(std::atomic<bool>& stop,const std::wstring& worldPath,const std::wstring& capturePath) {
    ComScope com;
    const bool diagnostic=!capturePath.empty();
    JobSystem jobs;
    jobs.Start();
    InputSystem input;
    DeclareStandardVrInputActions(input);
    auto config=AssetPath(L"input.ini");
    input.LoadConfiguration(config);
    StandardVrInput mappings;
    mappings.LoadConfiguration(config);
    mappings.SetNavigationOptions(false,false);
    XR xr;
    Graphics graphics;
    DiagnosticLine("etape=openxr\r\n");
    xr.InitializeInstance({"OASIS","OASIS",1},input,mappings.Mapping());
    DiagnosticLine("etape=dx12\r\n");
    graphics.Initialize(xr.GraphicsRequirements());
    DiagnosticLine("etape=session_vr\r\n");
    xr.InitializeSession(graphics.Device(),graphics.Queue());

    WorldRegistry world;
    SceneData scene=worldPath.empty()?SceneData{}:LoadSceneGLTF(worldPath);
    if(!scene.objects.empty()) {
        std::vector<WorldObject> objects;
        objects.reserve(scene.objects.size());
        for(const auto& source:scene.objects) {
            if(source.mesh>=scene.meshes.ranges.size()||
               source.material>=scene.materials.size())
                Fail("World references unavailable geometry or material");
            WorldObject object{};
            object.current=source.current;
            object.previous=source.previous;
            object.scale=source.scale;
            object.mesh=source.mesh;
            object.material=source.material;
            object.flags=source.flags;
            object.boundRadius=std::max(.001f,scene.meshes.ranges[source.mesh].bounds.w);
            objects.push_back(object);
        }
        world.CreateBatch(objects);
        scene.objects.clear();
    }
    DiagnosticLine("etape=chargement\r\n");
    graphics.Configure(xr,scene,jobs,world.Contributions());
    if(diagnostic)DiagnosticLine("etape=attente_image\r\n");
    int64_t firstFrame=0;
    bool captureRequested=false,captured=false;
    while(!stop.load(std::memory_order_relaxed)&&!xr.ExitRequested()) {
        xr.PollEvents();
        if(xr.ExitRequested())break;
        if(!xr.Running()){Sleep(5);continue;}
        XRFrame frame;
        if(!xr.BeginFrame(frame))continue;
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        if(frame.render&&graphics.Prepare(frame.input)) {
            xr.LocateLate(frame);
            if(frame.tracked&&xr.AcquireSwapchains(frame)){
                if(diagnostic&&firstFrame&&!captureRequested&&QpcSeconds(QpcCounter()-firstFrame)>=10.0){
                    DiagnosticLine("etape=demande_capture\r\n");graphics.RequestCapture(capturePath);captureRequested=true;
                }
                graphics.Render(frame);
            }
        }
        xr.EndFrame(frame);
        if(diagnostic&&frame.submitted&&!firstFrame){firstFrame=QpcCounter();DiagnosticLine("etape=image_vr\r\n");}
        if(diagnostic&&captureRequested&&graphics.CaptureDone()){
            DiagnosticLine("etape=capture\r\n");
            if(!graphics.CaptureFailed())graphics.WaitIdle();
            captured=graphics.SaveCapture();
            diagnosticCaptured.store(captured,std::memory_order_release);
            const auto metrics=graphics.GetMetrics();
            char line[320];
            if(metrics.fps>0)std::snprintf(line,sizeof(line),"fps_soumis=%.2f\r\ntemps_image_ms=%.2f\r\n",metrics.fps,1000.0/double(metrics.fps));
            else std::snprintf(line,sizeof(line),"fps_soumis=indisponible\r\ntemps_image_ms=indisponible\r\n");
            DiagnosticLine(line);
            constexpr double gibibyte=1024.0*1024.0*1024.0;
            if(metrics.vramBudget)std::snprintf(line,sizeof(line),"vram_gio=%.2f / %.2f\r\n",double(metrics.vramUsed)/gibibyte,double(metrics.vramBudget)/gibibyte);
            else std::snprintf(line,sizeof(line),"vram_gio=indisponible\r\n");
            DiagnosticLine(line);
            std::snprintf(line,sizeof(line),"oeil=gauche\r\nresolution_oeil=%u x %u\r\ncapture=%s\r\n",metrics.width,metrics.height,captured?"OASIS.png":"aucune");
            DiagnosticLine(line);
            break;
        }
    }
    graphics.WaitIdle();
    graphics.UnloadScene();
    jobs.Stop();
    if(diagnostic){DiagnosticLine(captured?"resultat=termine\r\n":"capture=aucune\r\nresultat=sans_capture\r\n");return captured?0:1;}
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    std::wstring worldPath,diagnosticDirectory;
    int count=0;
    auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)OASIS::Fail("Command line");
    for(int i=1;i<count;++i){
        if(wcscmp(args[i],L"--world")==0&&i+1<count)
            worldPath=args[++i];
        else if(wcscmp(args[i],L"--diagnostic")==0&&i+1<count)
            diagnosticDirectory=args[++i];
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    if(!diagnosticDirectory.empty()&&!OASIS::StartDiagnostic(diagnosticDirectory)){
        MessageBoxW(nullptr,L"Impossible de creer le journal OASIS.",L"OASIS",MB_OK|MB_ICONERROR);
        return 1;
    }
    const std::wstring capturePath=diagnosticDirectory.empty()?std::wstring{}:diagnosticDirectory+L"\\OASIS.png";
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::thread render([&]{result=OASIS::Run(stop,worldPath,capturePath);finished.store(true,std::memory_order_release);});
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
        if(diagnosticDeadline&&GetTickCount64()>=diagnosticDeadline&&!stop.exchange(true,std::memory_order_relaxed)){
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
    return result;
}
