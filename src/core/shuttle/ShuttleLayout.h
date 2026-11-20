#pragma once

// Transformations locales du catalogue navette, dans le repère du chargeur glTF.
#include "SolarSystem.h"

namespace OASIS::ShuttleLayout {
enum class Motion : uint8_t { Fixed, Nozzle, Rotor, Blade, Pedestal, Grip, Brake, Panel, Crt };
struct Part {
    uint16_t parent, module;
    Pose local;
    XMFLOAT3 scale;
    Motion motion;
    uint8_t slot;
};
inline constexpr uint16_t None=0xffffu;
inline constexpr uint16_t ModuleCount=29;
inline constexpr std::array<uint16_t,3> HullModules{0,1,2};
inline constexpr XMFLOAT3 Parking{2.10f,.020f+.925f*SpectacleInteriorUnitsPerMeter,4.42f};
inline constexpr XMFLOAT3 Eye{0.4f,0.82f,0.f};
inline constexpr XMFLOAT3 ToolAnchor{1.00f,.47f,.49f};
inline constexpr float JetExitX=-.426f;
inline const std::array<Part,183> Parts{{
    {None,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // NAVETTE_LUNAIRE
    {0,None,{{0.400000006f,0.819999993f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // ANCRAGE_OEIL
    {0,None,{{0.f,0.0270000007f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CENTRE_MASSE_SEC
    {0,None,{{-0.200000003f,-0.270000011f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // RESERVOIR
    {0,None,{{2.0250001f,0.208000004f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CAMERA_VISEUR
    {0,None,{{2.3900001f,-0.143000007f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // ORIGINE_TIR
    {0,3,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // POSTE_PILOTAGE
    {0,None,{{-1.54999995f,0.f,1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_G
    {7,4,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_G.SUPPORT
    {7,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Nozzle,0}, // R_AR_G.LACET
    {9,5,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_G.BUSE
    {9,None,{{-0.425999999f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_G.AUDIO_JET
    {0,None,{{1.54999995f,0.f,1.32000005f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_G
    {12,4,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_G.SUPPORT
    {12,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Nozzle,1}, // R_AV_G.LACET
    {14,5,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_G.BUSE
    {14,None,{{-0.425999999f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_G.AUDIO_JET
    {0,None,{{-1.54999995f,0.f,-1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_D
    {17,4,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_D.SUPPORT
    {17,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Nozzle,2}, // R_AR_D.LACET
    {19,5,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_D.BUSE
    {19,None,{{-0.425999999f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AR_D.AUDIO_JET
    {0,None,{{1.54999995f,0.f,-1.32000005f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_D
    {22,4,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_D.SUPPORT
    {22,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Nozzle,3}, // R_AV_D.LACET
    {24,5,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_D.BUSE
    {24,None,{{-0.425999999f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // R_AV_D.AUDIO_JET
    {0,None,{{-0.75999999f,-0.230000004f,1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G
    {27,6,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.CONDUIT
    {27,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Rotor,4}, // H_AR_G.ROTATION
    {29,7,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.MOYEU
    {29,None,{{0.075000003f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_01
    {31,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_01.PAS
    {32,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_01.MESH
    {29,None,{{0.0375000015f,0.f,0.0649519041f},{-0.f,-0.5f,-0.f,0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_02
    {34,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_02.PAS
    {35,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_02.MESH
    {29,None,{{-0.0375000015f,0.f,0.0649519041f},{-0.f,-0.866025448f,-0.f,0.49999997f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_03
    {37,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_03.PAS
    {38,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_03.MESH
    {29,None,{{-0.075000003f,0.f,9.18485139e-18f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_04
    {40,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_04.PAS
    {41,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_04.MESH
    {29,None,{{-0.0375000015f,0.f,-0.0649519041f},{0.f,-0.866025388f,-0.f,-0.50000006f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_05
    {43,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_05.PAS
    {44,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_05.MESH
    {29,None,{{0.0375000015f,0.f,-0.0649519041f},{0.f,-0.50000006f,-0.f,-0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_06
    {46,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,4}, // H_AR_G.PALE_06.PAS
    {47,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.PALE_06.MESH
    {27,None,{{0.f,0.0900000036f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_G.AUDIO_CONDUIT
    {0,None,{{0.75999999f,-0.230000004f,1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G
    {50,6,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.CONDUIT
    {50,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Rotor,5}, // H_AV_G.ROTATION
    {52,7,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.MOYEU
    {52,None,{{0.075000003f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_01
    {54,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_01.PAS
    {55,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_01.MESH
    {52,None,{{0.0375000015f,0.f,0.0649519041f},{-0.f,-0.5f,-0.f,0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_02
    {57,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_02.PAS
    {58,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_02.MESH
    {52,None,{{-0.0375000015f,0.f,0.0649519041f},{-0.f,-0.866025448f,-0.f,0.49999997f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_03
    {60,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_03.PAS
    {61,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_03.MESH
    {52,None,{{-0.075000003f,0.f,9.18485139e-18f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_04
    {63,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_04.PAS
    {64,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_04.MESH
    {52,None,{{-0.0375000015f,0.f,-0.0649519041f},{0.f,-0.866025388f,-0.f,-0.50000006f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_05
    {66,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_05.PAS
    {67,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_05.MESH
    {52,None,{{0.0375000015f,0.f,-0.0649519041f},{0.f,-0.50000006f,-0.f,-0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_06
    {69,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,5}, // H_AV_G.PALE_06.PAS
    {70,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.PALE_06.MESH
    {50,None,{{0.f,0.0900000036f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_G.AUDIO_CONDUIT
    {0,None,{{-0.75999999f,-0.230000004f,-1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D
    {73,6,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.CONDUIT
    {73,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Rotor,6}, // H_AR_D.ROTATION
    {75,7,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.MOYEU
    {75,None,{{0.075000003f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_01
    {77,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_01.PAS
    {78,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_01.MESH
    {75,None,{{0.0375000015f,0.f,0.0649519041f},{-0.f,-0.5f,-0.f,0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_02
    {80,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_02.PAS
    {81,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_02.MESH
    {75,None,{{-0.0375000015f,0.f,0.0649519041f},{-0.f,-0.866025448f,-0.f,0.49999997f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_03
    {83,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_03.PAS
    {84,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_03.MESH
    {75,None,{{-0.075000003f,0.f,9.18485139e-18f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_04
    {86,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_04.PAS
    {87,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_04.MESH
    {75,None,{{-0.0375000015f,0.f,-0.0649519041f},{0.f,-0.866025388f,-0.f,-0.50000006f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_05
    {89,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_05.PAS
    {90,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_05.MESH
    {75,None,{{0.0375000015f,0.f,-0.0649519041f},{0.f,-0.50000006f,-0.f,-0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_06
    {92,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,6}, // H_AR_D.PALE_06.PAS
    {93,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.PALE_06.MESH
    {73,None,{{0.f,0.0900000036f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AR_D.AUDIO_CONDUIT
    {0,None,{{0.75999999f,-0.230000004f,-1.32000005f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D
    {96,6,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.CONDUIT
    {96,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Rotor,7}, // H_AV_D.ROTATION
    {98,7,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.MOYEU
    {98,None,{{0.075000003f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_01
    {100,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_01.PAS
    {101,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_01.MESH
    {98,None,{{0.0375000015f,0.f,0.0649519041f},{-0.f,-0.5f,-0.f,0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_02
    {103,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_02.PAS
    {104,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_02.MESH
    {98,None,{{-0.0375000015f,0.f,0.0649519041f},{-0.f,-0.866025448f,-0.f,0.49999997f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_03
    {106,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_03.PAS
    {107,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_03.MESH
    {98,None,{{-0.075000003f,0.f,9.18485139e-18f},{0.f,-1.f,-0.f,-4.37113883e-08f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_04
    {109,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_04.PAS
    {110,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_04.MESH
    {98,None,{{-0.0375000015f,0.f,-0.0649519041f},{0.f,-0.866025388f,-0.f,-0.50000006f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_05
    {112,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_05.PAS
    {113,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_05.MESH
    {98,None,{{0.0375000015f,0.f,-0.0649519041f},{0.f,-0.50000006f,-0.f,-0.866025388f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_06
    {115,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Blade,7}, // H_AV_D.PALE_06.PAS
    {116,8,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.PALE_06.MESH
    {96,None,{{0.f,0.0900000036f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // H_AV_D.AUDIO_CONDUIT
    {0,None,{{0.589999974f,0.0149999997f,0.49000001f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Pedestal,0}, // COMMANDE_G
    {119,9,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.SOCLE
    {119,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.AVANT_ARRIERE
    {121,10,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.CARDAN
    {121,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.LATERAL
    {123,11,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.COLLIER
    {123,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Grip,0}, // COMMANDE_G.TWIST
    {125,12,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.POIGNEE
    {125,13,{{0.0500000007f,0.254999995f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.GACHETTE
    {125,14,{{0.0130000003f,0.214000002f,-0.0340000018f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_G.GRIP
    {0,None,{{0.589999974f,0.0149999997f,-0.49000001f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Pedestal,1}, // COMMANDE_D
    {129,9,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.SOCLE
    {129,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.AVANT_ARRIERE
    {131,10,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.CARDAN
    {131,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.LATERAL
    {133,11,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.COLLIER
    {133,None,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Grip,1}, // COMMANDE_D.TWIST
    {135,12,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.POIGNEE
    {135,13,{{0.0500000007f,0.254999995f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.GACHETTE
    {135,14,{{0.0130000003f,0.214000002f,0.0340000018f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COMMANDE_D.GRIP
    {0,None,{{1.15499997f,0.279000014f,0.455000013f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,1.00999999f,0.970000029f},Motion::Panel,0}, // CRT_NAV
    {139,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_NAV.BOITIER
    {139,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_NAV.VITRE
    {139,17,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,0}, // CRT_NAV.IMAGE
    {0,None,{{1.19500005f,0.291999996f,0.123000003f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,1.07000005f,1.f},Motion::Panel,1}, // CRT_VOL
    {143,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_VOL.BOITIER
    {143,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_VOL.VITRE
    {143,18,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,1}, // CRT_VOL.IMAGE
    {0,None,{{1.148f,0.279000014f,-0.389999986f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,1.11000001f,1.61000001f},Motion::Panel,2}, // CRT_VISEUR
    {147,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_VISEUR.BOITIER
    {147,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_VISEUR.VITRE
    {147,19,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,2}, // CRT_VISEUR.IMAGE
    {0,None,{{1.07500005f,0.0240000002f,0.555000007f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,0.930000007f,0.839999974f},Motion::Panel,3}, // CRT_SYSTEMES
    {151,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_SYSTEMES.BOITIER
    {151,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_SYSTEMES.VITRE
    {151,20,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,3}, // CRT_SYSTEMES.IMAGE
    {0,None,{{1.09599996f,0.0120000001f,0.25999999f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,0.920000017f,0.810000002f},Motion::Panel,4}, // CRT_CARBURANT
    {155,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_CARBURANT.BOITIER
    {155,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_CARBURANT.VITRE
    {155,21,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,4}, // CRT_CARBURANT.IMAGE
    {0,None,{{1.09599996f,0.0120000001f,-0.0599999987f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,0.920000017f,1.10000002f},Motion::Panel,5}, // CRT_INTEGRITE
    {159,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_INTEGRITE.BOITIER
    {159,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_INTEGRITE.VITRE
    {159,22,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,5}, // CRT_INTEGRITE.IMAGE
    {0,None,{{1.07500005f,0.0240000002f,-0.492000014f},{-0.f,-0.f,-0.12467473f,0.992197692f}},{1.f,0.930000007f,0.839999974f},Motion::Panel,6}, // CRT_MUNITIONS
    {163,15,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_MUNITIONS.BOITIER
    {163,16,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // CRT_MUNITIONS.VITRE
    {163,23,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Crt,6}, // CRT_MUNITIONS.IMAGE
    {0,None,{{0.f,-0.0199999996f,1.75399995f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Brake,0}, // FREIN_G.CHARNIERE
    {167,24,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // FREIN_G.PANNEAU
    {0,None,{{0.f,-0.0199999996f,-1.75399995f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Brake,1}, // FREIN_D.CHARNIERE
    {169,24,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,-1.f},Motion::Fixed,0}, // FREIN_D.PANNEAU
    {0,None,{{1.45000005f,-0.51700002f,0.f},{-0.f,-0.f,0.266731441f,0.963770866f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AV
    {171,25,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AV.PATIN
    {0,None,{{-1.17999995f,-0.569000006f,0.600000024f},{-0.f,-0.f,0.266731441f,0.963770866f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AR_G
    {173,25,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AR_G.PATIN
    {0,None,{{-1.17999995f,-0.569000006f,-0.600000024f},{-0.f,-0.f,0.266731441f,0.963770866f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AR_D
    {175,25,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAIN_AR_D.PATIN
    {0,None,{{-1.86000001f,0.257999986f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAPPE.CHARNIERE
    {177,26,{{0.f,0.f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TRAPPE.PANNEAU
    {0,27,{{1.05299997f,-0.119000003f,0.400000006f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // VARIATEUR
    {0,28,{{1.02600002f,-0.128000006f,-0.175999999f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // TEST_VOYANTS
    {0,28,{{1.02600002f,-0.128000006f,0.222000003f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // COULEUR_FOND
    {0,None,{{1.10800004f,0.159999996f,0.f},{-0.f,-0.f,-0.f,1.f}},{1.f,1.f,1.f},Motion::Fixed,0}, // AUDIO_IGNITION
}};
} // namespace OASIS::ShuttleLayout
