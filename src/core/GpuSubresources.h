#pragma once
#include <cstdint>
namespace lab {
struct GpuSubresourceShape {uint32_t mips{1},arraySlices{1},planes{1};};
struct GpuTextureSubresources {
    // A zero count means all remaining levels/slices/planes. {} is the whole
    // texture. A Texture3D has ONE array slice; Z slabs are not D3D12 subresources.
    uint32_t firstMip{},mipCount{},firstArraySlice{},arraySliceCount{},firstPlane{},planeCount{};
};
inline bool GpuNormalizeSubresources(GpuSubresourceShape shape,GpuTextureSubresources requested,GpuTextureSubresources& out){
    if(!shape.mips||!shape.arraySlices||!shape.planes||requested.firstMip>=shape.mips||requested.firstArraySlice>=shape.arraySlices||requested.firstPlane>=shape.planes)return false;
    out=requested;
    if(!out.mipCount)out.mipCount=shape.mips-out.firstMip;
    if(!out.arraySliceCount)out.arraySliceCount=shape.arraySlices-out.firstArraySlice;
    if(!out.planeCount)out.planeCount=shape.planes-out.firstPlane;
    return out.mipCount<=shape.mips-out.firstMip&&out.arraySliceCount<=shape.arraySlices-out.firstArraySlice&&out.planeCount<=shape.planes-out.firstPlane;
}
inline bool GpuSubresourcesOverlap(const GpuTextureSubresources& a,const GpuTextureSubresources& b){
    auto overlap=[](uint32_t x,uint32_t nx,uint32_t y,uint32_t ny){return uint64_t(x)<uint64_t(y)+ny&&uint64_t(y)<uint64_t(x)+nx;};
    return overlap(a.firstMip,a.mipCount,b.firstMip,b.mipCount)&&overlap(a.firstArraySlice,a.arraySliceCount,b.firstArraySlice,b.arraySliceCount)&&overlap(a.firstPlane,a.planeCount,b.firstPlane,b.planeCount);
}
template<class F> void GpuForEachSubresource(GpuSubresourceShape shape,const GpuTextureSubresources& range,F&& visit){
    for(uint32_t plane=range.firstPlane;plane<range.firstPlane+range.planeCount;++plane)
        for(uint32_t slice=range.firstArraySlice;slice<range.firstArraySlice+range.arraySliceCount;++slice)
            for(uint32_t mip=range.firstMip;mip<range.firstMip+range.mipCount;++mip)
                visit(mip+slice*shape.mips+plane*shape.mips*shape.arraySlices,mip,slice,plane);
}
}
