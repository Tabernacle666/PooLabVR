#pragma once
#include <cstdint>
namespace OASIS::RuntimeConfig {
inline constexpr uint32_t WorkerThreads = 5;
inline constexpr uint32_t MaxViews = 4;
inline constexpr uint32_t MaxLights = 128;
// MG2 production has no triangle-count or fixed construction-byte ceiling.
// Admission of GPU allocations is governed by the live DXGI memory budget.
// Zero is the absence of an explicit diagnostic triangle restriction.
inline constexpr uint32_t Mg2DefaultTriangleBudget = 0u;
}
