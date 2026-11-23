#pragma once
#include "Core.h"
#include <optional>

namespace OASIS {
enum class OrbitControl : uint32_t { None, Select, Adjust, Color, Set };
enum class OrbitFeature : uint32_t {
    None, FogDensity, DlssScale, PatternSpeed, Opacity, Deformation,
    Glitter, GlitterWidth, WaveStrength, WaveSpeed, WaveDuration,
    SceneReflections, Antialiasing, PeripheralShading, Count
};
inline constexpr uint32_t OrbitFeatureCount=uint32_t(OrbitFeature::Count)-1u;
inline constexpr uint32_t OrbitVisibleRows=8u;
static_assert(uint32_t(OrbitFeature::Count)<=16u);

constexpr uint32_t OrbitFeatureBit(OrbitFeature feature){return 1u<<uint32_t(feature);}

struct OrbitControlZone {
    OrbitControl control;
    XMFLOAT2 center,halfSize;
    float front;
};

// Face coordinates of the movable pieces in assets/phosphor_orbit.glb,
// with a little extra room around each piece for controller aim.
inline constexpr std::array<OrbitControlZone,4> OrbitControlZones{{
    {OrbitControl::Select,{-3.37f,.66f},{.66f,.66f},-.991f},
    {OrbitControl::Adjust,{-3.37f,-.80f},{.66f,.66f},-.991f},
    {OrbitControl::Color,{1.90f,1.63f},{.57f,.38f},-.73f},
    {OrbitControl::Set,{3.49f,.39f},{.53f,.67f},-.92f}
}};

struct OrbitDisplay {
    uint32_t features{},rows{},color{};
    OrbitFeature selected{},lastTouched{};
    OrbitControl hover{},locked{};
    float selectAngle{},adjustAngle{},colorTravel{},setTravel{};
    float liveMark{},factoryMark{},userMark{-1.f},value{};
    bool rangeKnown{};
    std::optional<XMFLOAT4> pointer;
    std::array<XMUINT4,6> buildingText{};
    std::array<XMUINT4,4> graphicsText{};
    std::array<XMFLOAT4,8> jointWire{};
    bool buildingPage{},plasterPage{},jointPreview{};
};

inline std::optional<Pose> VrGlovePose(const PoseActionState& grip,uint32_t hand){
    if(!grip.tracked)return std::nullopt;
    // The GLBs use +Z along the fingers and +Y on the back of the hand.
    // In the engine's OpenXR grip frame, +Z points from pinky to thumb.
    const float side=hand==0u?1.f:-1.f;
    const auto gloveToGrip=XMMatrixSet(0.f,0.f,side,0.f,
                                     -side,0.f,0.f,0.f,
                                     0.f,-1.f,0.f,0.f,
                                     0.f,0.f,0.f,1.f);
    return FromMatrix(gloveToGrip*Matrix(grip.pose));
}

inline XMMATRIX PhosphorOrbitModel(const Pose& leftGlove,float unitsPerMeter=1.f){
    const float scale=.034f*unitsPerMeter;
    // Turn counterclockwise in the face plane, then mount on the glove.
    // Translation follows both rotations so the wrist anchor stays fixed.
    return XMMatrixScaling(scale,scale,scale)*XMMatrixRotationZ(XM_PIDIV2)*
        XMMatrixRotationX(XM_PIDIV2)*XMMatrixTranslation(0.f,.049f*unitsPerMeter,-.072f*unitsPerMeter)*Matrix(leftGlove);
}
}
