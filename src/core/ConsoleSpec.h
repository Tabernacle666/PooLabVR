#pragma once
#include <cstdint>
namespace lab::ConsoleSpec {
inline constexpr uint32_t MinPhysicalCores = 6;
inline constexpr uint32_t WorkerThreads = 5;
inline constexpr uint64_t GiB = 1024ull * 1024ull * 1024ull;
inline constexpr uint64_t MiB = 1024ull * 1024ull;
inline constexpr uint64_t MinDedicatedVram = 15ull * GiB;
inline constexpr uint32_t FrameSlots = 3;
inline constexpr uint32_t MaxViews = 4;
inline constexpr uint32_t MaxLights = 128;
inline constexpr uint32_t MaxTextures = 1024;
inline constexpr uint32_t MaxSamplers = 1021;
inline constexpr uint32_t MaxMaterials = 1024;
inline constexpr uint32_t MaxMeshes = 4096;
inline constexpr uint32_t MaxObjects = 8192;
inline constexpr uint32_t MaxVisualStreams = 32;
inline constexpr uint32_t MaxVisualEffects = 32;
inline constexpr uint32_t MaxVisualCaptures = 2;
inline constexpr uint32_t MaxDynamicVerticesPerFrame = 2'000'000;
inline constexpr uint32_t MaxCaptureWidth = 1024;
inline constexpr uint32_t MaxCaptureHeight = 1024;
inline constexpr uint32_t MaxTextureEdge = 8192;
inline constexpr uint32_t MaxHeightParallaxSteps = 96;
inline constexpr float HeightTessPixelsPerSegment=12.0f;
inline constexpr uint32_t MaxHeightTessellation = 32;
inline constexpr uint64_t MaxWorldStaticGpuBytes = 4ull * GiB;
inline constexpr uint64_t MaxWorldDynamicGpuBytes = 512ull * MiB;
inline constexpr uint32_t LegacyPresentationResourceSlots = 4096;
inline constexpr uint32_t LegacyPresentationDraws = 256;
uint32_t PhysicalCoreCount();
void ValidateCpu();
}
