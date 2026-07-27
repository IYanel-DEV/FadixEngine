#include "render/ParticleSystem.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <random>

namespace fadix
{
namespace
{
thread_local std::mt19937 g_Rng{std::random_device{}()};

[[nodiscard]] float RandRange(const float minValue, const float maxValue)
{
    std::uniform_real_distribution<float> dist(minValue, maxValue);
    return dist(g_Rng);
}

[[nodiscard]] glm::vec3 RandomUnitVector()
{
    const float z = RandRange(-1.0F, 1.0F);
    const float t = RandRange(0.0F, glm::two_pi<float>());
    const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
    return {r * std::cos(t), r * std::sin(t), z};
}
}

ParticlePool::ParticlePool(const std::size_t maxTotal)
{
    m_Particles.resize(maxTotal);
}

void ParticlePool::EnsureEmitter(EmitterState& state, const int maxParticles)
{
    const std::size_t count = static_cast<std::size_t>(std::clamp(maxParticles, 1, 4096));
    if (!state.Allocated)
    {
        state.FirstParticle = m_NextFree;
        state.Count = count;
        m_NextFree += count;
        if (m_NextFree > m_Particles.size())
        {
            m_Particles.resize(m_NextFree);
        }
        state.Allocated = true;
        for (std::size_t i = 0; i < state.Count; ++i)
        {
            m_Particles[state.FirstParticle + i] = Particle{};
        }
        return;
    }
    if (state.Count >= count)
    {
        return;
    }
    // Growing an existing slice: append only works if this emitter is at the end.
    // ponytail: if not at end, leave Count and clamp emit to existing slots.
    if (state.FirstParticle + state.Count == m_NextFree)
    {
        const std::size_t extra = count - state.Count;
        m_NextFree += extra;
        if (m_NextFree > m_Particles.size())
        {
            m_Particles.resize(m_NextFree);
        }
        for (std::size_t i = state.Count; i < count; ++i)
        {
            m_Particles[state.FirstParticle + i] = Particle{};
        }
        state.Count = count;
    }
}

void ParticlePool::EmitOne(
    EmitterState& state,
    const ParticleEmitterComponent& emitter,
    const glm::vec3& emitterPosition,
    const glm::quat& emitterRotation)
{
    Particle* slot = nullptr;
    for (std::size_t i = 0; i < state.Count; ++i)
    {
        Particle& particle = m_Particles[state.FirstParticle + i];
        if (!particle.Alive)
        {
            slot = &particle;
            break;
        }
    }
    if (slot == nullptr)
    {
        return;
    }

    const glm::vec3 localDirection = glm::length(emitter.Direction) > 1.0e-4F
        ? glm::normalize(emitter.Direction)
        : glm::vec3{0.0F, 1.0F, 0.0F};
    const glm::vec3 worldAxis = glm::normalize(emitterRotation * localDirection);

    glm::vec3 position = emitterPosition;
    glm::vec3 velocity = worldAxis;

    switch (emitter.Shape)
    {
    case EmitterShape::Sphere:
    {
        const glm::vec3 dir = RandomUnitVector();
        position += dir * RandRange(0.0F, emitter.ShapeRadius);
        velocity = dir;
        break;
    }
    case EmitterShape::Box:
    {
        position += glm::vec3{
            RandRange(-emitter.ShapeRadius, emitter.ShapeRadius),
            RandRange(-emitter.ShapeRadius, emitter.ShapeRadius),
            RandRange(-emitter.ShapeRadius, emitter.ShapeRadius)};
        velocity = worldAxis;
        break;
    }
    case EmitterShape::Cone:
    default:
    {
        const float angle = glm::radians(emitter.ConeAngle);
        const float cone = RandRange(0.0F, angle);
        const float spin = RandRange(0.0F, glm::two_pi<float>());
        const glm::vec3 tangent = glm::abs(worldAxis.y) < 0.99F
            ? glm::normalize(glm::cross(worldAxis, glm::vec3{0.0F, 1.0F, 0.0F}))
            : glm::normalize(glm::cross(worldAxis, glm::vec3{1.0F, 0.0F, 0.0F}));
        const glm::vec3 bitangent = glm::cross(worldAxis, tangent);
        velocity = glm::normalize(
            worldAxis * std::cos(cone) +
            (tangent * std::cos(spin) + bitangent * std::sin(spin)) * std::sin(cone));
        position += velocity * RandRange(0.0F, emitter.ShapeRadius * 0.1F);
        break;
    }
    }

    const float speed = RandRange(emitter.SpeedMin, emitter.SpeedMax);
    slot->Position = position;
    slot->Velocity = velocity * speed;
    slot->Color = emitter.ColorStart;
    slot->Size = emitter.SizeStart;
    slot->Age = 0.0F;
    slot->Lifetime = std::max(RandRange(emitter.LifetimeMin, emitter.LifetimeMax), 0.01F);
    slot->Alive = true;
}

void ParticlePool::Simulate(
    EmitterState& state,
    const ParticleEmitterComponent& emitter,
    const glm::vec3& emitterPosition,
    const glm::quat& emitterRotation,
    const float dt)
{
    EnsureEmitter(state, emitter.MaxParticles);
    if (!emitter.Enabled || dt <= 0.0F)
    {
        return;
    }

    if (emitter.PlayOnStart)
    {
        state.Started = true;
    }

    if (state.Started && !state.Finished)
    {
        state.Accumulator += emitter.EmitRate * dt;
        while (state.Accumulator >= 1.0F)
        {
            state.Accumulator -= 1.0F;
            EmitOne(state, emitter, emitterPosition, emitterRotation);
            if (!emitter.Loop)
            {
                ++state.Emitted;
                if (state.Emitted >= static_cast<std::size_t>(std::max(emitter.MaxParticles, 1)))
                {
                    state.Finished = true;
                    state.Accumulator = 0.0F;
                    break;
                }
            }
        }
    }

    for (std::size_t i = 0; i < state.Count; ++i)
    {
        Particle& particle = m_Particles[state.FirstParticle + i];
        if (!particle.Alive)
        {
            continue;
        }
        particle.Age += dt;
        if (particle.Age >= particle.Lifetime)
        {
            particle.Alive = false;
            continue;
        }
        particle.Velocity += emitter.Gravity * dt;
        particle.Position += particle.Velocity * dt;
        const float t = particle.Age / particle.Lifetime;
        particle.Color = glm::mix(emitter.ColorStart, emitter.ColorEnd, t);
        particle.Size = glm::mix(emitter.SizeStart, emitter.SizeEnd, t);
    }

    if (emitter.Loop)
    {
        state.Finished = false;
        state.Emitted = 0;
    }
}

void ParticlePool::KillEmitter(EmitterState& state)
{
    if (!state.Allocated)
    {
        return;
    }
    for (std::size_t i = 0; i < state.Count; ++i)
    {
        m_Particles[state.FirstParticle + i] = Particle{};
    }
    state.Accumulator = 0.0F;
    state.Emitted = 0;
    state.Started = false;
    state.Finished = false;
}

} // namespace fadix
