#pragma once
#include <cstdint>
namespace OASIS::RuntimeConfig {
inline constexpr uint32_t WorkerThreads = 5;
inline constexpr uint32_t MaxViews = 4;
inline constexpr uint32_t MaxLights = 128;
// MG2 unique-variant triangle admission, not a limit on drawn triangles.
inline constexpr uint32_t Mg2ReferenceTriangles = 262144u;
inline constexpr uint32_t Mg2DefaultTriangleBudget = 8u*Mg2ReferenceTriangles;
inline constexpr uint32_t Mg2DiagnosticMaxTriangleBudget = 32u*Mg2ReferenceTriangles;
// Transient publication ceilings; no fixed VRAM reservation.
inline constexpr uint32_t Mg2DefaultBuildBudgetMiB = 1024u;
inline constexpr uint32_t Mg2DiagnosticBuildBudgetMiB = 4096u;
}
