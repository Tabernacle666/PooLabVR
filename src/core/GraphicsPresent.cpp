#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::DrawPresentComposite(ID3D12GraphicsCommandList7* l,Frame& f,const XRFrame& xr,
                                              const uint32_t (&params)[4],const PresentComposite& options){
        const auto target=Rtv(forestWarmRender?forestWarmRTV:xrRTV[xr.color]),
            ds=Dsv(forestWarmRender?forestWarmDSV:xrDSV[xr.depth]);
        if(!outputSurface.desktop&&xr.presentRects[0].extent.width>0){
            // The scene buffers stay cropped. Only the acquired swapchain
            // receives a full-surface constant clear and per-eye viewports.
            const float black[]{0.f,0.f,0.f,1.f};
            l->ClearRenderTargetView(target,black,0,nullptr);
            l->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0.f,0,0,nullptr);
            std::array<D3D12_VIEWPORT,MaxViews> viewports{};
            std::array<D3D12_RECT,MaxViews> scissors{};
            for(uint32_t i=0;i<viewCount;++i){
                const auto& r=xr.presentRects[i];
                viewports[i]={float(r.offset.x),float(r.offset.y),float(r.extent.width),float(r.extent.height),0.f,1.f};
                scissors[i]={r.offset.x,r.offset.y,r.offset.x+r.extent.width,r.offset.y+r.extent.height};
            }
            l->RSSetViewports(viewCount,viewports.data());
            l->RSSetScissorRects(viewCount,scissors.data());
        }else{
            Viewport(l,width,height);
        }
        l->OMSetRenderTargets(1,&target,FALSE,&ds);
        Bind(l,f,true);
        l->SetGraphicsRoot32BitConstants(1,4,params,0);
        l->SetGraphicsRoot32BitConstant(1,options.blendReflection?1u:0u,3);
        l->SetGraphicsRoot32BitConstant(1,options.dlssStatus,4);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // The present stage always composites the native 3D frame.
        l->SetPipelineState(options.useDlss?dlssPresentPSO.Get():presentPSO.Get());
        l->DrawInstanced(3,1,0,0);
    }
}
