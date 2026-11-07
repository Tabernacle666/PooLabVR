#pragma once
#include "Core.h"
namespace lab {
enum class ReachShape:uint32_t {Point,Segment,Box};
struct GripZone {Pose anchor;XMFLOAT3 halfSize{};float reach{.145f};ReachShape shape{ReachShape::Point};bool align{};};
enum class GripPolicy:uint32_t {General,PrecisionTool};
struct GripImpedance {
    float linearHz{22},angularHz{26},dampingRatio{1.05f};
    float maximumForce{75},maximumTorque{7};
    float maximumLinearAcceleration{55},maximumAngularAcceleration{1000};
};
struct HandImpedance {
    float mass{.45f},linearHz{20},angularHz{22},maximumForce{65},maximumTorque{6};
    float maximumSpeed{2.5f},maximumAngularSpeed{9},maximumAcceleration{18};
    float discontinuityDistance{.30f},discontinuityAngle{1.0f};
};
struct GripComponent {GripPolicy policy{GripPolicy::General};GripImpedance impedance;std::array<GripZone,4> zones;uint32_t count{1};float settleSeconds{.12f};bool bimanual{true};};

}
