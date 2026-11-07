#pragma once
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace lab {
struct WorldNavigationOptions {
    bool smoothLocomotion{};
    bool heightAdjust{};
    bool retainHomeAmbient{true};
};

struct ClientWorldEntry {
    std::string id;
    std::string name;
    std::filesystem::path packageRoot;
    std::filesystem::path manifestPath;
    std::string manifestJson;
    std::filesystem::path dllPath;
    uint32_t pluginAbi{};
    uint32_t gpuAbi{};
    std::vector<std::string> requiredServices;
    std::vector<std::string> requiredInputActions;
    WorldNavigationOptions navigation;
};

class ContentCatalog {
    std::vector<ClientWorldEntry> worlds_;
public:
    void Add(ClientWorldEntry entry){worlds_.push_back(std::move(entry));}
    size_t size()const noexcept{return worlds_.size();}
    bool empty()const noexcept{return worlds_.empty();}
    const ClientWorldEntry& operator[](size_t i)const{return worlds_[i];}
    auto begin()const noexcept{return worlds_.begin();}
    auto end()const noexcept{return worlds_.end();}
};

ContentCatalog DiscoverWorldCatalog();
}
