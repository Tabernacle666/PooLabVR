#include "GraphicsInternal.h"

namespace OASIS {
    void Graphics::Impl::DrawPresentComposite(ID3D12GraphicsCommandList7* l,Frame& f,const XRFrame& xr,
                                              const uint32_t (&params)[4],const PresentComposite& options){
        Viewport(l,width,height);
        const auto target=Rtv(xrRTV[xr.color]),ds=Dsv(xrDSV[xr.depth]);
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
