#pragma once
#include "SolarSystem.h"
#include <span>

namespace OASIS {
inline constexpr uint32_t LunarInfantryCount=10,LunarTankCount=2;
inline constexpr uint32_t LunarCompanySize=LunarInfantryCount+LunarTankCount;
inline constexpr uint32_t LunarUnitCount=LunarCompanySize*2,LunarPartsPerUnit=3;
inline constexpr uint32_t LunarActorPoseCount=LunarUnitCount*LunarPartsPerUnit;
inline constexpr uint32_t LunarTracerCount=16,LunarRippleCount=8;
inline constexpr uint32_t LunarPoseCount=LunarActorPoseCount+LunarTracerCount;
inline constexpr float LunarGroundHeight=.17f,LunarHipHeight=.059f;
inline constexpr XMFLOAT3 LunarTurretPivot{0.f,.087f,-.009f};
inline bool LunarWallFace(XMFLOAT3 point){
    return std::abs(point.z)<.51f&&point.y>=LunarGroundHeight&&point.y<LunarGroundHeight+.211f;
}

struct LunarFormationPose {
    XMFLOAT3 position;
    float yaw;
    bool tank;
};

inline LunarFormationPose LunarFormation(uint32_t unit){
    const float side=unit<LunarCompanySize?-1.f:1.f;
    const uint32_t index=unit%LunarCompanySize;
    if(index>=LunarInfantryCount){
        return {{side*.621f,LunarGroundHeight,index==LunarInfantryCount?-.257f:.286f},-side*XM_PIDIV2,true};
    }
    const float row=float(index/5u);
    const float rank=float(int(index%5u)-2);
    return {{side*(.227f+.154f*row),LunarGroundHeight,.138f*rank+.027f*row},-side*XM_PIDIV2,false};
}

struct LunarDrawPose {
    XMFLOAT4 positionScale{};
    XMFLOAT4 rotation{};
};
static_assert(sizeof(LunarDrawPose)==32u);

struct LunarBattleView {
    std::array<LunarDrawPose,LunarPoseCount> poses{};
    std::array<XMFLOAT4,LunarRippleCount> ripples{};
};

enum class LunarCue : uint32_t { Rifle,Cannon,Surface,Membrane,Count };
struct LunarSoundEvent {
    XMFLOAT3 position;
    LunarCue cue;
    float strength;
};
struct LunarMotor {
    XMFLOAT3 position{};
    float effort{};
};

class LunarBattle {
    struct UnitTiming {
        double nextShot{},lastShot{-10.};
        uint32_t shots{};
    };
    struct Shot {
        XMFLOAT3 origin{},end{};
        double begin{},arrive{};
        bool live{},shell{},dome{};
    };
    struct Ripple {
        XMFLOAT3 normal{};
        double time{-10.};
    };
    struct UnitPose {
        Pose base,weapon;
        float stepPhase{},stride{},motor{};
    };

    double previous_{-1.},time_{};
    bool paused_{};
    uint32_t nextRipple_{},soundCount_{};
    std::array<UnitTiming,LunarUnitCount> units_{};
    std::array<Shot,LunarTracerCount> shots_{};
    std::array<Ripple,LunarRippleCount> ripples_{};
    std::array<LunarSoundEvent,32> sounds_{};
    std::array<LunarMotor,LunarTankCount*2u> motors_{};
    LunarBattleView view_{};

    UnitPose Animate(uint32_t index)const;
    void Fire(uint32_t index,const UnitPose& pose);
    void Sound(LunarCue cue,XMFLOAT3 position,float strength);
public:
    LunarBattle();
    void Update(double solarTime);
    void TogglePause()noexcept{paused_=!paused_;}
    bool Paused()const noexcept{return paused_;}
    const LunarBattleView& View()const noexcept{return view_;}
    std::span<const LunarSoundEvent> Sounds()const noexcept{return {sounds_.data(),soundCount_};}
    const auto& Motors()const noexcept{return motors_;}
};
}
