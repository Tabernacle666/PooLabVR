#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>

namespace lab {
struct GpuValidationState {
    bool enabled{},deviceRemoved{};
    uint64_t errors{},warnings{};
};
// Generic diagnostics: no domain identities, scheduling or resource ownership.
class GpuValidation {
    Microsoft::WRL::ComPtr<ID3D12Debug1> debug_;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue1> messages_;
    std::atomic<uint64_t> errors_{},warnings_{};
    DWORD cookie_{};bool registered_{},enabled_{};
    static void __stdcall Message(D3D12_MESSAGE_CATEGORY,D3D12_MESSAGE_SEVERITY,
        D3D12_MESSAGE_ID,LPCSTR,void*) noexcept;
public:
    ~GpuValidation();
    void Enable(); // Before any D3D12 device exists.
    void Attach(ID3D12Device*);
    bool Enabled()const noexcept{return enabled_;}
    GpuValidationState State(ID3D12Device*)const;
};
}
