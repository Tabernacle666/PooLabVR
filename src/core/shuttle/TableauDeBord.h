#pragma once

#include "PropulsionVaisseau.h"

#include <array>
#include <cstdint>
#include <limits>

namespace OASIS::Vaisseaux {

struct Theme {
    const char* name;
    std::array<float, 3> backgroundLinear;
};

struct MapDot {
    double x{}, y{};
};

template<std::size_t BodyCount>
struct SolarMapCache {
    std::array<MapDot, BodyCount> dots{};
    MapDot craft{};
    std::uint32_t activeBody{std::numeric_limits<std::uint32_t>::max()};
    double periodSeconds{0.5};
    double nextSampleSeconds{};
    double lastSampleSeconds{-std::numeric_limits<double>::infinity()};
    std::uint64_t revision{};

    // sampleBody et craftProjected utilisent la même projection du repère cosmos.
    // Appeler uniquement pendant que la carte est affichée.
    template<class SampleBody>
    bool Refresh(double displaySeconds, MapDot craftProjected, std::uint32_t body, SampleBody sampleBody) {
        const bool timelineBackwards = displaySeconds < lastSampleSeconds;
        if (revision && !timelineBackwards && body == activeBody && displaySeconds < nextSampleSeconds)
            return false;
        for (std::size_t i = 0; i < BodyCount; ++i)
            dots[i] = sampleBody(i);
        craft = craftProjected;
        activeBody = body;
        lastSampleSeconds = displaySeconds;
        nextSampleSeconds = displaySeconds + periodSeconds;
        ++revision;
        return true;
    }
};

} // namespace OASIS::Vaisseaux
