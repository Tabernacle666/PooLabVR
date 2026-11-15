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
struct ComScope {
    ComScope(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}
    ~ComScope(){CoUninitialize();}
};
int Run(std::atomic<bool>& stop,const std::wstring& worldPath) {
    ComScope com;
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
    xr.InitializeInstance({"OASIS","OASIS",1},input,mappings.Mapping());
    graphics.Initialize(xr.GraphicsRequirements());
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
    graphics.Configure(xr,scene,jobs,world.Contributions());
    while(!stop.load(std::memory_order_relaxed)&&!xr.ExitRequested()) {
        xr.PollEvents();
        if(xr.ExitRequested())break;
        if(!xr.Running()){Sleep(5);continue;}
        XRFrame frame;
        if(!xr.BeginFrame(frame))continue;
        world.SetViewer(frame.input.head.p.x,frame.input.head.p.z);
        if(frame.render&&graphics.Prepare(frame.input)) {
            xr.LocateLate(frame);
            if(frame.tracked&&xr.AcquireSwapchains(frame))
                graphics.Render(frame);
        }
        xr.EndFrame(frame);
    }
    graphics.WaitIdle();
    graphics.UnloadScene();
    jobs.Stop();
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    std::wstring worldPath;
    int count=0;
    auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)OASIS::Fail("Command line");
    for(int i=1;i<count;++i){
        if(wcscmp(args[i],L"--world")==0&&i+1<count)
            worldPath=args[++i];
        else OASIS::Fail("Unknown option");
    }
    LocalFree(args);
    int result=0;
    std::atomic<bool> stop{};
    std::atomic<bool> finished{};
    std::thread render([&]{result=OASIS::Run(stop,worldPath);finished.store(true,std::memory_order_release);});
    ULONGLONG exitDeadline=0;
    while(!finished.load(std::memory_order_acquire)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT)stop.store(true,std::memory_order_relaxed);
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)stop.store(true,std::memory_order_relaxed);
        if(stop.load(std::memory_order_relaxed)){
            if(!exitDeadline)exitDeadline=GetTickCount64()+3000;
            else if(GetTickCount64()>=exitDeadline&&!finished.load(std::memory_order_acquire))
                TerminateProcess(GetCurrentProcess(),1);
        }
        Sleep(5);
    }
    render.join();
    return result;
}
