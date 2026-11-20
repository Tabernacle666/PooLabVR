#pragma once

#include "Core.h"
#include "shuttle/ProfilNavette.h"
#include <span>

namespace OASIS {
class ShuttleWake {
public:
    enum class Kind {Smoke,Wash,Dust};
    using Triangle=std::array<XMFLOAT3,3>;
    struct Particle {
        // Dust is local to the canopy. Motor wakes stay in the surrounding flight frame.
        XMFLOAT3 origin{},velocity{},axis{};
        float age{},life{},arrival{},strength{};
        Kind kind{};
        bool glass{};
    };
    struct Impact {
        XMFLOAT3 position{};
        float strength{};
        bool glass{};
    };

    static constexpr size_t EmitterCount=Vaisseaux::Lunaire::Mounts.size();
    void Step(const FrameInput&,FXMMATRIX,XMFLOAT3 velocity,
              std::span<const Vaisseaux::DeviceLoad,EmitterCount>,float density,std::span<const Triangle>);
    void Clear() noexcept;
    const auto& Particles() const noexcept {return particles_;}
    const Impact& LastImpact() const noexcept {return impact_;}
private:
    static constexpr size_t Capacity=96;
    std::array<Particle,Capacity> particles_{};
    std::array<float,EmitterCount> emitterPhase_{};
    Impact impact_{};
    size_t next_{};
    uint32_t seed_{0x918f051du},body_{Invalid};
    double time_{-1.};
    float dustPhase_{};

    float Random() noexcept;
    void Emit(Particle) noexcept;
    void EmitDust(float speed,std::span<const Triangle>);
};
}
