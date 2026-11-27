#pragma once
#include "Core.h"

namespace OASIS {
inline constexpr uint32_t SpectaclePairCount=2u;
inline constexpr uint32_t SpectacleSphereCount=100u;
inline constexpr float SpectacleRadius=110.679612f;
inline const XMFLOAT3 SpectacleCenter{0.f,29.1262136f,160.194175f};
inline const auto SpectacleCenters=[]{
    std::array<XMFLOAT3,SpectacleSphereCount> centers{};
    centers[0]=SpectacleCenter;
    centers[1]={349.514563f,29.1262136f,160.194175f};
    uint32_t seed=0x4f415349u;
    auto jitter=[&]{
        seed=1664525u*seed+1013904223u;
        return (float(seed>>8u)/16777216.f-.5f)*203.883495f;
    };
    // Each cell gets one sphere. The two landmarks take their own cells.
    constexpr float columns[]{-1805.82524f,-873.786407f,0.0f,466.019417f,1689.32039f};
    constexpr float floors[]{-1572.81553f,-728.155339f,0.0f,932.038834f};
    constexpr float rows[]{-1922.33009f,-844.660193f,0.0f,873.786407f,1980.58252f};
    uint32_t next=SpectaclePairCount;
    for(float z:rows)for(float y:floors)for(float x:columns){
        if(z==0.f&&y==0.f&&(x==0.f||x==466.019417f))continue;
        centers[next++]=Add(SpectacleCenter,{x+jitter(),y+jitter(),z+jitter()});
    }
    return centers;
}();

inline SceneLight SpectacleLight(uint32_t owner,XMFLOAT3 eye,double time){
    SceneLight light{};
    light.position=owner==0u?XMFLOAT3{0.f,3.49514563f,160.194175f}:
        Add(SpectacleCenters[owner],{-29.1262136f,53.8834951f,-43.6893203f});
    light.radius=owner==0u?262.135922f:203.883495f;
    light.color=owner==0u?XMFLOAT3{1.f,.52f,.19f}:XMFLOAT3{1.f,.78f,.51f};
    light.power=owner==0u?
        16966.7263f*(1.f+.07f*std::sin(float(time)*.55f)+.025f*std::sin(float(time)*1.13f)):22056.7442f;
    light.direction=Unit(Sub(eye,light.position));
    light.outerCos=owner==0u?-.65f:-1.f;
    light.innerCos=.25f;light.id=0x70000001u+owner;light.flags=1;
    return light;
}

// Native render state only. Meshlet LOD is provided exclusively by MG2.
enum class SphereRepresentation : uint32_t { Geometry=0u, Hidden=5u };

// Fractions shrink the asymmetric frustum around the eye's optical axis.
inline bool SphereIntersectsView(FXMMATRIX vp,XMFLOAT3 center,float radius,bool farClip,
                                 float fraction=1.f){
    const auto m=XMMatrixTranspose(vp);
    const float scale=fraction<1.f?(1.f-fraction)/XMVectorGetX(XMVector3LengthSq(m.r[3])):0.f;
    const float x=XMVectorGetX(XMVector3Dot(m.r[3],m.r[0]))*scale;
    const float y=XMVectorGetX(XMVector3Dot(m.r[3],m.r[1]))*scale;
    const XMVECTOR planes[]{m.r[0]-(x-fraction)*m.r[3],(x+fraction)*m.r[3]-m.r[0],
        m.r[1]-(y-fraction)*m.r[3],(y+fraction)*m.r[3]-m.r[1],m.r[3]-m.r[2],m.r[2]};
    const auto p=XMVectorSet(center.x,center.y,center.z,1.f);
    for(unsigned i=0;i<(farClip?6u:5u);++i){
        if(XMVectorGetX(XMVector4Dot(planes[i],p))<
           -radius*XMVectorGetX(XMVector3Length(planes[i])))return false;
    }
    return true;
}

} // namespace OASIS
