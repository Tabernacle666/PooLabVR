#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::DrawPresentComposite(ID3D12GraphicsCommandList7* l,Frame& f,const XRFrame& xr,
                                              const uint32_t (&params)[4],const PresentComposite& options){
        Viewport(l,width,height);
        const auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);
        l->OMSetRenderTargets(1,&target,FALSE,&ds);
        Bind(l,f,true);
        l->SetGraphicsRoot32BitConstants(1,4,params,0);
        l->SetGraphicsRoot32BitConstant(1,options.reflections?1u:0u,3);
        const bool showPerception=xr.spherePerception&&!options.useDlss&&spherePerceptionPSO;
        l->SetGraphicsRoot32BitConstant(1,options.dlssStatus,4);
        l->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(options.proxyOnly){
            const bool inspect=options.dense||xr.spherePerception||options.runOverlay;
            l->SetGraphicsRoot32BitConstant(1,uint32_t(xr.sphereRepresentation),2);
            l->SetGraphicsRoot32BitConstant(1,xr.spherePerception?1u:0u,3);
            if(inspect){
                l->SetPipelineState(sphereInspectPSO.Get());
                l->DrawInstanced(3,1,0,0);
            }else{
                // The same curve as the native shader's background.
                const float background[]{0.002765409f,0.005498287f,0.011318204f,1.f};
                l->ClearRenderTargetView(target,background,0,nullptr);
                l->ClearDepthStencilView(ds,D3D12_CLEAR_FLAG_DEPTH,0.f,0,0,nullptr);
                if(options.visibleSphere){
                    l->SetPipelineState(sphereProxyPSO.Get());
                    l->DrawInstanced(6,1,0,0);
                }
            }
        }else{
            l->SetPipelineState(showPerception?spherePerceptionPSO.Get():
                (options.useDlss?dlssPresentPSO.Get():presentPSO.Get()));
            l->DrawInstanced(3,1,0,0);
        }
    }
}
