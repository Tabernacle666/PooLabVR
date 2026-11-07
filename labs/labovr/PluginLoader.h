#pragma once
#include "ContentCatalog.h"
#include "LaboVRPlugin.h"
#include <windows.h>
#include <memory>

namespace lab {
class LoadedPlugin {
    HMODULE module_{};
    const LabPluginApiV1* api_{};
    ClientWorldEntry entry_;
    LoadedPlugin(HMODULE module,const LabPluginApiV1* api,ClientWorldEntry entry):module_(module),api_(api),entry_(std::move(entry)){}
    friend class PluginLoader;
public:
    LoadedPlugin(const LoadedPlugin&)=delete;
    LoadedPlugin& operator=(const LoadedPlugin&)=delete;
    LoadedPlugin(LoadedPlugin&& other)noexcept;
    LoadedPlugin& operator=(LoadedPlugin&& other)noexcept;
    ~LoadedPlugin();
    const LabPluginApiV1& Api()const noexcept{return *api_;}
    const ClientWorldEntry& Entry()const noexcept{return entry_;}
    HMODULE Module()const noexcept{return module_;}
    void Reset()noexcept;
};

class PluginLoader {
public:
    std::unique_ptr<LoadedPlugin> Load(const ClientWorldEntry& entry)const;
};
}
