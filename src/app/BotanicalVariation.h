#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace OASIS::BotanicalVariation {
inline float Random(uint32_t n){
    n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;
    return float(n&0x00ffffffu)/16777215.f;
}
// Diagnostic mix: 1% conifers (split equally), 99% broadleaf.
// Both the original campus and territorial expansion use the same policy.
inline constexpr float ConiferShare=.01f;
inline unsigned TreeSpecies(float roll){
    return roll<ConiferShare*.5f?0u:roll<ConiferShare?1u:2u;
}
// Inverse CDF: a dense ordinary population and a thin, unquotaed upper tail.
inline float Tree(float u){
    constexpr std::array<float,7> p{0.f,.10f,.50f,.90f,.99f,.9975f,1.f};
    constexpr std::array<float,7> s{.35f,.65f,1.f,1.40f,2.f,2.80f,4.f};
    u=std::clamp(u,0.f,1.f);
    for(size_t i=1;i<p.size();++i)
        if(u<=p[i])return std::lerp(s[i-1],s[i],(u-p[i-1])/(p[i]-p[i-1]));
    return s.back();
}
inline float Plant(float u,float lo,float hi){
    // Keep median size, and ordinary cover, close to the authored baseline.
    const float t=Tree(u);
    return t<=1.f?std::lerp(lo,1.f,(t-.35f)/.65f):std::lerp(1.f,hi,(t-1.f)/3.f);
}
inline constexpr std::array<std::array<float,2>,5> Floor{{
    {.35f,2.f},{.35f,2.4f},{.40f,2.2f},{.40f,2.5f},{.50f,3.f}
}};
inline constexpr std::array<std::array<float,2>,5> Understory{{
    {.40f,2.2f},{.35f,2.4f},{.40f,1.9f},{.35f,2.f},{.35f,2.8f}
}};
inline unsigned TreeForm(uint32_t cell,unsigned variants,float scale){
    uint32_t seed=cell^0x43414e4fu;
    seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;
    seed*=0x846ca68bu;seed^=seed>>16;
    if(variants!=4u)return seed%variants;
    if(scale>2.f)return seed%2u?2u:0u; // old-growth morphology, not an enlarged sapling
    const unsigned roll=seed%100u;
    return roll<50u?0u:roll<65u?1u:roll<90u?2u:3u;
}
}
