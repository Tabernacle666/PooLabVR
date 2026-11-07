#include "Assets.h"
#include <filesystem>
#include <cstdint>
#include <cwchar>
#include <objbase.h>
#pragma comment(lib, "Ole32.lib")
int wmain(int argc,wchar_t** argv){
    lab::Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"asset cooker COM");
    if(argc!=4||wcscmp(argv[1],L"--scene")!=0){CoUninitialize();return 1;}
    std::filesystem::path output=argv[3];
    std::filesystem::create_directories(output);
    auto scene=lab::LoadSceneGLTF(argv[2]);
    for(uint32_t i=0;i<scene.textures.size();i++){
        wchar_t name[64];swprintf_s(name,L"texture_%04u.ktx2",i);
        scene.textures[i]=lab::CookTexture(scene.textures[i],(output/name).wstring());
    }
    lab::SaveCookedScene(scene,(output/L"scene.lvscene").wstring());
    CoUninitialize();return 0;
}
