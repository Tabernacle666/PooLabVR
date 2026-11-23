#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace OASIS::Vaisseaux {

struct Vec3 {
    double x{}, y{}, z{};
};

// Axes locaux directs. X vers l'avant, Y vers la gauche, Z vers le haut.
Vec3 operator+(Vec3 a, Vec3 b) noexcept;
Vec3 operator-(Vec3 a, Vec3 b) noexcept;
Vec3 operator*(Vec3 v, double scale) noexcept;
Vec3 Cross(Vec3 a, Vec3 b) noexcept;

enum class Organ : unsigned char { Reactor, Fan };
enum class Side : unsigned char { Left, Right };

struct Mount {
    const char* node;
    Organ organ;
    Side side;
    Vec3 positionMetres;
    // Direction de la poussée neutre X (réacteur) ou signe de rotation Z (hélice).
    int direction;
};

struct PowerPoint {
    double rpmFraction, forceN, inputW;
};
using PowerCurve = std::array<PowerPoint, 5>;

struct ReactorProfile {
    double idleRpm, maxRpm;
    double spinUpSeconds, spinDownSeconds, startLagSeconds;
    double gimbalMinRad, gimbalMaxRad, gimbalRateRadS;
    PowerCurve curve;
};

struct FanProfile {
    double idleRpm, maxRpm;
    double spinUpSeconds, spinDownSeconds;
    double pitchMinRad, pitchMaxRad, pitchRateRadS;
    double rotorInertiaKgM2, referenceDensityKgM3, vacuumGasMaxN;
    PowerCurve curve;
    double fullBiasSpeedMps{10.};
};

struct PropulsionProfile {
    ReactorProfile reactor;
    FanProfile fan;
};

bool Valid(const PropulsionProfile& profile) noexcept;

struct HandDemand {
    // Positif : avancer, incliner le manche latéralement, monter, respectivement.
    double longitudinal{}, lateral{}, vertical{};
};

struct FlightDemand {
    std::array<HandDemand, 2> hands;
    bool powered{};
    double longitudinalSpeedMps{}; // Retained for telemetry; fan bias follows the handles.
    double liftBalance{}; // 0 follows each handle (10/90..90/10); 1 splits lift 50/50.
};

struct DeviceState {
    double rpm{}, gimbalRad{}, pitchRad{}, rotorPhaseRad{}, startRemainingSeconds{};
    bool requestingThrust{};
};

struct DeviceLoad {
    Vec3 positionMetres{};
    Vec3 forceN{};
    Vec3 freeTorqueNm{};
    double rpm{}, gimbalRad{}, pitchRad{}, rotorPhaseRad{}, inputW{}, gasForceN{};
    int rotorDirection{};
};

struct Wrench {
    Vec3 forceN{}, torqueNm{};
    double inputW{};
};

// L'appelant conserve un état par ancrage et applique les forces au corps rigide
// au pas du solveur. Le point de masse et la densité viennent du monde actif.
// Les tableaux de mounts, states et loads doivent présenter la même taille.
Wrench StepPropulsion(const PropulsionProfile& profile, std::span<const Mount> mounts,
                      const FlightDemand& demand, double atmosphericDensityKgM3,
                      Vec3 centreOfMassMetres, double dtSeconds,
                      std::span<DeviceState> states, std::span<DeviceLoad> loads);

Vec3 CentreOfMass(double dryKg, Vec3 dryCentreMetres, double fuelKg,
                  Vec3 fuelCentreMetres) noexcept;

} // namespace OASIS::Vaisseaux
