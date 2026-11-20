#include "PropulsionVaisseau.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace OASIS::Vaisseaux {

Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 v, double scale) noexcept { return {v.x * scale, v.y * scale, v.z * scale}; }

Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

namespace {

constexpr double Turn = 2.0 * std::numbers::pi;

bool Finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool ValidCurve(const PowerCurve& curve) noexcept {
    if (curve.front().rpmFraction != 0.0 || curve.back().rpmFraction != 1.0)
        return false;
    double previous = -1.0;
    for (const auto& point : curve) {
        if (!(point.rpmFraction > previous) || !std::isfinite(point.forceN) ||
            !std::isfinite(point.inputW) || point.forceN < 0.0 || point.inputW < 0.0)
            return false;
        previous = point.rpmFraction;
    }
    return true;
}

bool ValidDrive(double idle, double maximum, double up, double down) noexcept {
    return std::isfinite(idle) && std::isfinite(maximum) && idle >= 0.0 &&
           maximum > idle && std::isfinite(up) && up > 0.0 && std::isfinite(down) && down > 0.0;
}

bool ValidRange(double low, double high, double rate) noexcept {
    return low > -Turn / 4.0 && low < 0.0 && high > 0.0 && high < Turn / 4.0 &&
           std::isfinite(rate) && rate > 0.0;
}

PowerPoint ReadCurve(const PowerCurve& curve, double fraction) noexcept {
    fraction = std::clamp(fraction, 0.0, 1.0);
    for (std::size_t i = 1; i < curve.size(); ++i) {
        const auto& a = curve[i - 1];
        const auto& b = curve[i];
        if (fraction > b.rpmFraction)
            continue;
        const double t = (fraction - a.rpmFraction) / (b.rpmFraction - a.rpmFraction);
        return {fraction, std::lerp(a.forceN, b.forceN, t), std::lerp(a.inputW, b.inputW, t)};
    }
    return curve.back();
}

double Axis(double raw) noexcept {
    return std::isfinite(raw) ? std::clamp(raw, -1.0, 1.0) : 0.0;
}

double AngleDemand(double signedAxis, double low, double high) noexcept {
    return signedAxis < 0.0 ? -signedAxis * low : signedAxis * high;
}

double Approach(double value, double target, double rate, double dt) noexcept {
    return value + std::clamp(target - value, -rate * dt, rate * dt);
}

double Spool(double value, double target, double up, double down, double dt) noexcept {
    // Les temps de montée et de descente correspondent à une réponse de 90 %.
    constexpr double Log10 = 2.302585092994046;
    const double time = target > value ? up : down;
    return std::lerp(value, target, -std::expm1(-Log10 * dt / time));
}

DeviceLoad Reactor(const ReactorProfile& p, const Mount& mount, const HandDemand& input,
                   bool powered, double dt, DeviceState& state) noexcept {
    const double request = powered ? Axis(input.longitudinal) * mount.direction : 0.0;
    const bool requestingThrust = request > 0.001;
    const double throttle = requestingThrust ? request : 0.0;
    if (requestingThrust && !state.requestingThrust)
        state.startRemainingSeconds = p.startLagSeconds;
    state.requestingThrust = requestingThrust;
    if (!requestingThrust)
        state.startRemainingSeconds = 0.0;
    const double delay = std::min(dt, state.startRemainingSeconds);
    state.startRemainingSeconds -= delay;
    const double idle = powered ? p.idleRpm : 0.0;
    state.rpm = Spool(state.rpm, idle, p.spinUpSeconds, p.spinDownSeconds, delay);
    state.rpm = Spool(state.rpm, idle + throttle * (p.maxRpm - p.idleRpm),
                      p.spinUpSeconds, p.spinDownSeconds, dt - delay);

    // Le réacteur avant est monté à 180°. La demande latérale reste dans le repère du pilote.
    const double lateral = powered ? Axis(input.lateral) * mount.direction : 0.0;
    const double target = AngleDemand(lateral, p.gimbalMinRad, p.gimbalMaxRad);
    state.gimbalRad = Approach(state.gimbalRad, target, p.gimbalRateRadS, dt);
    const auto sample = ReadCurve(p.curve, (state.rpm - p.idleRpm) / (p.maxRpm - p.idleRpm));
    const Vec3 direction{std::cos(state.gimbalRad) * mount.direction,
                         std::sin(state.gimbalRad) * mount.direction, 0.0};
    return {mount.positionMetres, direction * sample.forceN, {}, state.rpm,
            state.gimbalRad, 0.0, 0.0, sample.inputW, 0.0};
}

DeviceLoad Fan(const FanProfile& p, const Mount& mount, const HandDemand& input,
               bool powered, double liftShare, double density, double dt, DeviceState& state) noexcept {
    const double collective = powered ? Axis(input.vertical) : 0.0;
    const double targetRpm = powered ? std::lerp(p.idleRpm, p.maxRpm, std::abs(collective)) : 0.0;
    const double oldOmega = state.rpm * Turn / 60.0;
    state.rpm = Spool(state.rpm, targetRpm, p.spinUpSeconds, p.spinDownSeconds, dt);
    const double omega = state.rpm * Turn / 60.0;
    state.rotorPhaseRad = std::remainder(state.rotorPhaseRad + mount.direction *
                                        (oldOmega + omega) * 0.5 * dt, Turn);
    const double collectivePitch = AngleDemand(collective, p.pitchMinRad, p.pitchMaxRad);
    const double targetPitch = std::asin(liftShare * std::sin(collectivePitch));
    state.pitchRad = Approach(state.pitchRad, targetPitch, p.pitchRateRadS, dt);
    const double maxPitch = std::max(-p.pitchMinRad, p.pitchMaxRad);
    const double lift = std::sin(state.pitchRad) / std::sin(maxPitch);
    const double ratio = density / p.referenceDensityKgM3;
    const auto sample = ReadCurve(p.curve, state.rpm / p.maxRpm);

    const double airForce = sample.forceN * lift * ratio;
    const double gasForce = powered ? p.vacuumGasMaxN * lift * (1.0 - std::min(ratio, 1.0)) : 0.0;
    const double motorW = sample.inputW * std::abs(lift) * ratio;
    const double shaftTorque = omega > 1.0 ? motorW / omega : 0.0;
    const double reaction = -mount.direction * (p.rotorInertiaKgM2 * (omega - oldOmega) / dt + shaftTorque);
    return {mount.positionMetres, {0.0, 0.0, airForce + gasForce}, {0.0, 0.0, reaction}, state.rpm,
            0.0, state.pitchRad, state.rotorPhaseRad, motorW, gasForce};
}

} // namespace

bool Valid(const PropulsionProfile& profile) noexcept {
    const auto& r = profile.reactor;
    const auto& f = profile.fan;
    return ValidDrive(r.idleRpm, r.maxRpm, r.spinUpSeconds, r.spinDownSeconds) &&
           ValidRange(r.gimbalMinRad, r.gimbalMaxRad, r.gimbalRateRadS) &&
           std::isfinite(r.startLagSeconds) && r.startLagSeconds >= 0.0 && ValidCurve(r.curve) &&
           r.curve.front().forceN == 0.0 &&
           ValidDrive(f.idleRpm, f.maxRpm, f.spinUpSeconds, f.spinDownSeconds) &&
           ValidRange(f.pitchMinRad, f.pitchMaxRad, f.pitchRateRadS) &&
           std::isfinite(f.rotorInertiaKgM2) && f.rotorInertiaKgM2 >= 0.0 &&
           std::isfinite(f.referenceDensityKgM3) && f.referenceDensityKgM3 > 0.0 &&
           std::isfinite(f.vacuumGasMaxN) && f.vacuumGasMaxN >= 0.0 &&
           std::isfinite(f.fullBiasSpeedMps) && f.fullBiasSpeedMps > 0.0 &&
           ValidCurve(f.curve) && f.curve.front().forceN == 0.0;
}

Wrench StepPropulsion(const PropulsionProfile& profile, std::span<const Mount> mounts,
                      const FlightDemand& demand, double atmosphericDensityKgM3,
                      Vec3 centreOfMassMetres, double dtSeconds,
                      std::span<DeviceState> states, std::span<DeviceLoad> loads) {
    if (mounts.size() != states.size() || mounts.size() != loads.size())
        throw std::invalid_argument("Vaisseau: il faut un état et une sortie par propulseur");
    if (!(dtSeconds > 0.0) || !std::isfinite(dtSeconds) ||
        !std::isfinite(atmosphericDensityKgM3) || !Finite(centreOfMassMetres))
        throw std::invalid_argument("Vaisseau: pas ou référentiel invalide");

    Wrench result{};
    const double density = std::max(0.0, atmosphericDensityKgM3);
    std::array<unsigned, 2> fanPositions{};
    for (const auto& mount : mounts) {
        if (mount.organ == Organ::Fan)
            fanPositions[mount.side == Side::Left ? 0 : 1] |=
                mount.positionMetres.x >= centreOfMassMetres.x ? 1u : 2u;
    }
    const double speed = Axis(demand.longitudinalSpeedMps / profile.fan.fullBiasSpeedMps);
    const double weight = std::abs(speed);
    const double balance = std::max(0.0, Axis(demand.liftBalance));
    const double fore = 0.5 + (1.0 - balance) *
        std::copysign(0.4 * weight * weight * (3.0 - 2.0 * weight), speed);
    const double maxShare = std::max(fore, 1.0 - fore);
    for (std::size_t i = 0; i < mounts.size(); ++i) {
        const auto& mount = mounts[i];
        const unsigned side = mount.side == Side::Left ? 0u : 1u;
        const auto& hand = demand.hands[side];
        const double share = fanPositions[side] != 3u ? 1.0 :
            (mount.positionMetres.x >= centreOfMassMetres.x ? fore : 1.0 - fore) / maxShare;
        auto& load = loads[i];
        load = mount.organ == Organ::Reactor ?
               Reactor(profile.reactor, mount, hand, demand.powered, dtSeconds, states[i]) :
               Fan(profile.fan, mount, hand, demand.powered, share, density, dtSeconds, states[i]);
        result.forceN = result.forceN + load.forceN;
        result.torqueNm = result.torqueNm + Cross(load.positionMetres - centreOfMassMetres, load.forceN) +
                          load.freeTorqueNm;
        result.inputW += load.inputW;
    }
    return result;
}

Vec3 CentreOfMass(double dryKg, Vec3 dryCentreMetres, double fuelKg,
                  Vec3 fuelCentreMetres) noexcept {
    return (dryCentreMetres * dryKg + fuelCentreMetres * fuelKg) * (1.0 / (dryKg + fuelKg));
}

} // namespace OASIS::Vaisseaux
