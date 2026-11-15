#pragma once

#include "Core.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>

namespace OASIS {

class DesktopOutput {
public:
    static constexpr uint32_t BufferCount = 3;

private:
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, BufferCount> colors_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    uint32_t width_{}, height_{};

    void CreateSurfaces(uint32_t width, uint32_t height) {
        for (uint32_t i = 0; i < BufferCount; ++i)
            Hr(swapchain_->GetBuffer(i, IID_PPV_ARGS(&colors_[i])), "Desktop color buffer");

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 0.0f;
        Hr(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth_)),
            "Desktop depth buffer");
        width_ = width;
        height_ = height;
    }

public:
    void Create(HWND window, ID3D12Device* device, ID3D12CommandQueue* queue,
                uint32_t width, uint32_t height) {
        if (!window || !device || !queue || !width || !height || swapchain_)
            Fail("Invalid desktop output creation");

        device_ = device;
        Hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)), "Desktop DXGI factory");

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = width;
        desc.Height = height;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = BufferCount;
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

        Microsoft::WRL::ComPtr<IDXGISwapChain1> created;
        Hr(factory_->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &created),
            "Desktop swapchain");
        Hr(created.As(&swapchain_), "Desktop swapchain interface");
        Hr(factory_->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
            "Desktop window association");
        CreateSurfaces(width, height);
    }

    void Resize(uint32_t width, uint32_t height) {
        if (!swapchain_ || !width || !height) Fail("Invalid desktop output resize");
        if (width == width_ && height == height_) return;
        for (auto& color : colors_) color.Reset();
        depth_.Reset();
        Hr(swapchain_->ResizeBuffers(BufferCount, width, height,
            DXGI_FORMAT_R8G8B8A8_UNORM, 0), "Desktop resize buffers");
        CreateSurfaces(width, height);
    }

    void Present() {
        if (!swapchain_) Fail("Desktop output not created");
        Hr(swapchain_->Present(0, 0), "Desktop present");
    }

    uint32_t CurrentIndex() const {
        if (!swapchain_) Fail("Desktop output not created");
        return swapchain_->GetCurrentBackBufferIndex();
    }

    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }

    ID3D12Resource* Color(uint32_t index) const {
        if (index >= BufferCount || !colors_[index]) Fail("Invalid desktop color index");
        return colors_[index].Get();
    }

    ID3D12Resource* Depth() const {
        if (!depth_) Fail("Desktop depth buffer not created");
        return depth_.Get();
    }
};

}
