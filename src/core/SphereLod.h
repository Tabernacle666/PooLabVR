#pragma once
#include "Core.h"

namespace OASIS {
enum class SphereRepresentation : uint32_t { Geometry, Depth, Color, Point, Hidden };
inline constexpr uint32_t SphereBankAngles=26, SphereCacheSize=256, SphereColorSize=128;
inline constexpr uint32_t SphereCacheLayers=SphereBankAngles+2u;

struct SphereLodSettings {
    float geometryEnter{42.f},geometryExit{52.f},geometryAngleEnter{12.f},geometryAngleExit{18.f};
    float depthEnter{90.f},depthExit{105.f},depthAngleEnter{55.f},depthAngleExit{65.f};
    float pointEnter{250.f},pointExit{220.f};
    float promoteSeconds{.07f},demoteSeconds{.18f},warpSeconds{.12f},warpDegrees{.08f};
};

inline XMFLOAT3 SphereViewDirection(uint32_t index){
    if(index>=24u)return {0.f,index==24u?1.f:-1.f,0.f};
    const float azimuth=XM_PIDIV4*float(index%8u);
    const float elevation=(int(index/8u)-1)*XM_PI/3.f;
    return {std::sin(azimuth)*std::cos(elevation),std::sin(elevation),
            -std::cos(azimuth)*std::cos(elevation)};
}

// A six-plane test for native depth, or an unbounded frustum for projections.
inline bool SphereIntersectsView(FXMMATRIX vp,XMFLOAT3 center,float radius,bool farClip){
    const auto m=XMMatrixTranspose(vp);
    const XMVECTOR planes[]{m.r[3]+m.r[0],m.r[3]-m.r[0],m.r[3]+m.r[1],
        m.r[3]-m.r[1],m.r[3]-m.r[2],m.r[2]};
    const auto p=XMVectorSet(center.x,center.y,center.z,1.f);
    for(unsigned i=0;i<(farClip?6u:5u);++i){
        if(XMVectorGetX(XMVector4Dot(planes[i],p))<
           -radius*XMVectorGetX(XMVector3Length(planes[i])))return false;
    }
    return true;
}

inline XMFLOAT4 SphereScreenBounds(FXMMATRIX vp,XMFLOAT3 center,float radius){
    XMFLOAT4 bounds{1.f,1.f,0.f,0.f};
    for(unsigned i=0;i<8;++i){
        const auto corner=Add(center,{i&1u?radius:-radius,i&2u?radius:-radius,i&4u?radius:-radius});
        const auto clip=XMVector3Transform(XMLoadFloat3(&corner),vp);
        const float w=XMVectorGetW(clip);
        if(w<=0.f)return {0.f,0.f,1.f,1.f};
        const float x=.5f+.5f*XMVectorGetX(clip)/w,y=.5f-.5f*XMVectorGetY(clip)/w;
        bounds.x=std::min(bounds.x,x);bounds.y=std::min(bounds.y,y);
        bounds.z=std::max(bounds.z,x);bounds.w=std::max(bounds.w,y);
    }
    return bounds;
}
}
