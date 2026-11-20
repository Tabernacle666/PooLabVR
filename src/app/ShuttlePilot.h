#pragma once

#include "SolarPassage.h"
#include "shuttle/ProfilNavette.h"

namespace OASIS {
struct VrInputActions;

class ShuttlePilot {
public:
    static constexpr uint32_t PropulsorCount=uint32_t(Vaisseaux::Lunaire::Mounts.size());
    enum class Setting : uint32_t {
        LeftAngle,RightAngle,DeadZone,Color,Calibrate,ReactorLimit,FanRpm,FanRise,Seat,Count
    };

    void ReadInput(const FrameInput&,const Pose*,const VrInputActions&);
    VrTravel Advance(double seconds,SolarPassage& passage,const Pose& previousHead);
    void Crossed(std::span<const MembraneCrossing>);
    void Reanchor(const Pose& previous,const Pose& head);
    void Publish(FXMMATRIX trackingToWorld,const FrameInput&);
    bool Active() const noexcept {return occupied_;}
    bool Ready() const noexcept {return power_==Power::Running;}
    bool ConsumeIgnition() noexcept;
    XMMATRIX ModelToWorld() const noexcept;
    XMMATRIX ToolInTracking() const noexcept;
    XMMATRIX ToolInWorld() const noexcept;
    void ReleaseToolHand() noexcept {demand_.hands[1]={};}
    uint32_t DisplayData(uint32_t page) const noexcept;
    float Brake(uint32_t side) const noexcept {return brake_[side];}
    const auto& Loads() const noexcept {return loads_;}
    const auto& Map() const noexcept {return map_;}
    void LogSample(uint64_t frame) const noexcept;

private:
    enum class Power { Off,Aligning,Running };

    bool occupied_{},exitArmed_{},ignition_{},hasAlignedSince_{};
    Power power_{Power::Off};
    int64_t alignedSince_{},aHeldSince_{},lastSample_{};
    XMFLOAT4X4 modelToTracking_{},modelToWorld_{};
    std::array<Pose,2> neutral_{};
    std::array<int,2> offset_{};
    int deadZone_{10};
    int reactorLimit_{100},fanRpmSteps_{77},fanRiseTenths_{8},seat_{8};
    float seatPosition_{8.f};
    Vaisseaux::PropulsionProfile propulsion_{Vaisseaux::Lunaire::Propulsion};
    uint32_t theme_{};
    Setting setting_{Setting::DeadZone};
    bool stickLatched_{};
    std::array<bool,2> aligned_{};
    std::array<float,2> brake_{};
    float heldFraction_{};
    Vaisseaux::FlightDemand demand_{};
    std::array<Vaisseaux::DeviceState,PropulsorCount> devices_{};
    std::array<Vaisseaux::DeviceLoad,PropulsorCount> loads_{};
    XMFLOAT3 velocity_{};
    XMFLOAT3 localVelocity_{};
    float pitchDegrees_{};
    Vaisseaux::Vec3 angularVelocity_{};
    Vaisseaux::SolarMapCache<SolarBodyCount> map_{};
    uint32_t activeBody_{Invalid};

    void Board(const Pose&);
    void Leave();
    void ResetNeutral();
    void AdjustSettings(const FrameInput&,const VrInputActions&);
    bool Upright(const PoseActionState&,uint32_t hand) const;
    Vaisseaux::HandDemand HandAxes(const PoseActionState&,uint32_t hand) const;
};
}
