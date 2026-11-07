#include "PluginLoader.h"
#include "Core.h"
#include "SessionLog.h"
#include <cstddef>
#include <cstring>

namespace lab {
LoadedPlugin::LoadedPlugin(LoadedPlugin&& other)noexcept:module_(other.module_),api_(other.api_),entry_(std::move(other.entry_)){other.module_=nullptr;other.api_=nullptr;}
LoadedPlugin& LoadedPlugin::operator=(LoadedPlugin&& other)noexcept{if(this!=&other){Reset();module_=other.module_;api_=other.api_;entry_=std::move(other.entry_);other.module_=nullptr;other.api_=nullptr;}return *this;}
LoadedPlugin::~LoadedPlugin(){Reset();}
void LoadedPlugin::Reset()noexcept{api_=nullptr;if(module_){FreeLibrary(module_);module_=nullptr;}entry_={};}

std::unique_ptr<LoadedPlugin> PluginLoader::Load(const ClientWorldEntry& entry)const{
    if(entry.pluginAbi!=LABOVR_PLUGIN_ABI_V1)Fail("World manifest plugin ABI is unsupported");
    constexpr DWORD secureFlags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    HMODULE module=LoadLibraryExW(entry.dllPath.c_str(),nullptr,secureFlags);
    if(!module){
        const DWORD secureError=GetLastError();
        if(secureError==ERROR_MOD_NOT_FOUND||secureError==ERROR_PROC_NOT_FOUND){
            module=LoadLibraryExW(entry.dllPath.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
            if(module)LogEvent(0,"plugin.load_fallback","secure_error=%lu",static_cast<unsigned long>(secureError));
        }
        if(!module){
            const DWORD fallbackError=GetLastError();
            LogEvent(0,"plugin.load_error","secure_error=%lu fallback_error=%lu",static_cast<unsigned long>(secureError),static_cast<unsigned long>(fallbackError));
            Fail("World plugin DLL load failed");
        }
    }
    using GetApiFn=const LabPluginApiV1* (LABOVR_PLUGIN_CALL *)(LabU32);
    auto getApi=reinterpret_cast<GetApiFn>(GetProcAddress(module,"LaboVR_GetPluginApi"));
    if(!getApi){FreeLibrary(module);Fail("World plugin entry point missing");}
    const LabPluginApiV1* api=getApi(entry.pluginAbi);
    constexpr size_t infoEnd=offsetof(LabPluginApiV1,info)+sizeof(LabPluginInfoV1);
    if(!api||api->abiVersion!=entry.pluginAbi||api->structSize<infoEnd||api->info.abiVersion!=entry.pluginAbi||api->info.structSize<sizeof(LabPluginInfoV1)||!api->info.pluginId||entry.id!=api->info.pluginId){FreeLibrary(module);Fail("World plugin API does not match manifest");}
    return std::unique_ptr<LoadedPlugin>(new LoadedPlugin(module,api,entry));
}
}
