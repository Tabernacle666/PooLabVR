#include "Core.h"
#include "XR.h"
#include "Graphics.h"
#include "Jobs.h"
#include "Assets.h"
#include "InputSystem.h"
#include "LabInputMapping.h"
#include "WorldRegistry.h"
#include "BuildConfig.h"
#include "BuildVersion.h"
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lab {
const CoreIdentity& LaboVRIdentity() noexcept {
    static const CoreIdentity identity{
        .applicationName="PooLabVR", .engineName="LaboVR",
        .version=LAB_ENGINE_VERSION, .versionW=LAB_ENGINE_VERSION_W,
        .cacheNamespaceW=L"PooLabVR", .pipelineCacheIdentity=LAB_PIPELINE_ABI
    };
    return identity;
}
struct ComScope {
    ComScope(){Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");}
    ~ComScope(){CoUninitialize();}
};
int Run(Channels& io,const std::wstring& worldPath) {
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
    xr.InitializeInstance({GetCoreIdentity().applicationName,
                           GetCoreIdentity().engineName,140},
                          input,mappings.Mapping());
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
    while(!io.stop.load(std::memory_order_relaxed)&&!xr.ExitRequested()) {
        xr.PollEvents();
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
    lab::ConfigureCore(lab::LaboVRIdentity());
    std::wstring worldPath;
    int count=0;
    auto* args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args)lab::Fail("Command line");
    for(int i=1;i<count;++i){
        if(wcscmp(args[i],L"--world")==0&&i+1<count)
            worldPath=args[++i];
        else lab::Fail("Unknown option");
    }
    LocalFree(args);
    int result=0;
    auto io=std::make_unique<lab::Channels>();
    std::thread render([&]{result=lab::Run(*io,worldPath);});
    while(!io->stop.load(std::memory_order_relaxed)) {
        MSG message;
        while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT)io->stop=true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if(GetAsyncKeyState(VK_ESCAPE)&0x8000)io->stop=true;
        Sleep(5);
    }
    render.join();
    return result;
}
