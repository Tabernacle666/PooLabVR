#pragma once

#include "SolarPassage.h"
#include "ShuttleWake.h"
#include "shuttle/ProfilNavette.h"

namespace OASIS {
struct VrInputActions;

class ShuttlePilot {
public:
    static constexpr uint32_t PropulsorCount=uint32_t(Vaisseaux::Lunaire::Mounts.size());
    enum class Setting : uint32_t {
        LeftAngle,RightAngle,DeadZone,Color,Calibrate,ReactorLimit,FanRpm,FanRise,Seat,Count
    };

    enum class Mixer : uint32_t {JetMin,JetMax,FanMin,FanMax,DustImpact,Ignition,DustDensity,Count};
    void ReadMixer(const FrameInput&,const VrInputActions&,bool toolOwnsStick);
    float Mix(Mixer channel) const noexcept {return float(mix_[uint32_t(channel)])*.01f;}
    void SetCanopy(std::span<const ShuttleWake::Triangle> canopy) noexcept {canopy_=canopy;}
    const ShuttleWake& Wake() const noexcept {return wake_;}
    bool AvionicsOn() const noexcept {return avionics_;}

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
    XMFLOAT3 HandleAngles(uint32_t hand) const noexcept;
    uint32_t DisplayData(uint32_t page) const noexcept;
    float Brake(uint32_t side) const noexcept {return brake_[side];}
    const auto& Loads() const noexcept {return loads_;}
    const auto& Map() const noexcept {return map_;}
    void LogSample(uint64_t frame) const noexcept;

private:
    enum class Power { Off,Aligning,Running };

    bool occupied_{},exitArmed_{},ignition_{},hasAlignedSince_{},requireUpright_{true};
    Power power_{Power::Off};
    bool avionics_{},mixStickLatched_{};
    Mixer mixer_{Mixer::JetMax};
    static constexpr std::array<int,uint32_t(Mixer::Count)> DefaultMix{10,72,8,60,65,72,65};
    std::array<int,uint32_t(Mixer::Count)> mix_=DefaultMix;
    ShuttleWake wake_;
    std::span<const ShuttleWake::Triangle> canopy_;
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
    void ResetNeutral(bool requireUpright=true);
    void AdjustSettings(const FrameInput&,const VrInputActions&);
    bool Upright(const PoseActionState&,uint32_t hand) const;
    Vaisseaux::HandDemand HandAxes(const PoseActionState&,uint32_t hand) const;
};
}
