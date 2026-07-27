#pragma once

#include "runtime/Components.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <vector>

namespace fadix
{

struct Particle
{
    glm::vec3 Position{0.0F};
    glm::vec3 Velocity{0.0F};
    glm::vec4 Color{1.0F};
    float Size{0.1F};
    float Age{0.0F};
    float Lifetime{1.0F};
    bool Alive{false};
};

// Shared CPU particle pool. Emitters reserve slices; Simulate advances one emitter.
class ParticlePool
{
public:
    explicit ParticlePool(std::size_t maxTotal = 8192);

    struct EmitterState
    {
        std::size_t FirstParticle{0};
        std::size_t Count{0};
        float Accumulator{0.0F};
        std::size_t Emitted{0};
        bool Started{false};
        bool Finished{false};
        bool Allocated{false};
    };

    // Allocates a unique slice the first time; resizes the slice if maxParticles grows.
    void EnsureEmitter(EmitterState& state, int maxParticles);

    void Simulate(
        EmitterState& state,
        const ParticleEmitterComponent& emitter,
        const glm::vec3& emitterPosition,
        const glm::quat& emitterRotation,
        float dt);

    void KillEmitter(EmitterState& state);

    [[nodiscard]] const std::vector<Particle>& Particles() const { return m_Particles; }

private:
    void EmitOne(
        EmitterState& state,
        const ParticleEmitterComponent& emitter,
        const glm::vec3& emitterPosition,
        const glm::quat& emitterRotation);

    std::vector<Particle> m_Particles;
    std::size_t m_NextFree{0};
};

} // namespace fadix
