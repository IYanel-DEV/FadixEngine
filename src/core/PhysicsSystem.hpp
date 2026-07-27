#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "PhysicsComponents.hpp"
#include "Components.hpp"

// =============================================================================
// Fadix Engine — PhysicsSystem  (Jolt 3D + Box2D v3 2D)
// =============================================================================
//
// Lifecycle (call order each frame):
//   1. Init(gravity)          — once at Play start / after hot-reload.
//   2. SyncBodies(registry)   — reconcile ECS ↔ physics handles each frame.
//   3. PreStep(registry)      — push Inspector-dirtied transforms into engines.
//   4. Step(deltaTime)        — fixed-step accumulators drive both worlds.
//   5. PostStep(registry)     — pull simulated transforms back into ECS.
//   6. Shutdown()             — tear down both worlds before Stop or exit.
//
// Jolt and Box2D headers are confined to PhysicsSystem.cpp via the JoltContext
// pimpl and the uint64 b2WorldId slot — this header is safe to include anywhere.
// =============================================================================

class PhysicsSystem
{
public:
    PhysicsSystem() = default;

    PhysicsSystem(const PhysicsSystem&)            = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;
    PhysicsSystem(PhysicsSystem&&)                 = delete;
    PhysicsSystem& operator=(PhysicsSystem&&)      = delete;

    ~PhysicsSystem() { Shutdown(); }

    // -------------------------------------------------------------------------
    // Lifecycle
    // -------------------------------------------------------------------------
    void Init(const glm::vec3& gravity = glm::vec3(0.0f, -9.81f, 0.0f));
    void Shutdown();
    bool IsInitialised() const { return m_Initialised; }

    // -------------------------------------------------------------------------
    // Per-frame update
    // -------------------------------------------------------------------------

    static constexpr float kFixedStep = 1.0f / 60.0f;

    void Step(float deltaTime);

    // -------------------------------------------------------------------------
    // ECS ↔ physics bridge
    // -------------------------------------------------------------------------

    // Create/destroy Jolt and Box2D bodies to match current ECS state.
    // Entities with a collider but no RigidBody receive an implicit Static body.
    // Safe to call every frame — no-ops when handles are already live.
    void SyncBodies(entt::registry& registry);

    // Null out every runtime physics handle stored in ECS components.
    // Call after the physics world is destroyed while the registry lives on
    // (e.g. Scene::Clone for Play snapshot, hot-reload) so SyncBodies recreates
    // bodies from scratch instead of treating stale handles as live.
    static void ResetHandles(entt::registry& registry);

    // Push Inspector-dirtied transforms into the physics engines.
    void PreStep(entt::registry& registry);

    // Read back simulated transforms into TransformComponent.
    // Also snapshots velocity into the component fields for serialisation.
    void PostStep(entt::registry& registry);

    // -------------------------------------------------------------------------
    // Hot-reload state serialisation (3D Jolt world only)
    // -------------------------------------------------------------------------
    void SerializeState(entt::registry& registry, const std::string& path) const;
    void DeserializeState(entt::registry& registry, const std::string& path);

    // -------------------------------------------------------------------------
    // Settings
    // -------------------------------------------------------------------------
    glm::vec3 GetGravity() const { return m_Gravity; }
    void      SetGravity(const glm::vec3& g);

private:
    // -- Jolt internals (pimpl — defined only in PhysicsSystem.cpp) -----------
    struct JoltContext;
    std::unique_ptr<JoltContext> m_Jolt;

    // -- Box2D v3 world (memcpy in/out of b2WorldId in .cpp) ------------------
    // b2WorldId is {int32_t, uint16_t, uint16_t} = 8 bytes.
    uint64_t m_B2World      = 0;
    bool     m_B2Initialised = false;

    bool      m_Initialised   = false;
    glm::vec3 m_Gravity       = glm::vec3(0.0f, -9.81f, 0.0f);
    float     m_Accumulator3D = 0.0f;
    float     m_Accumulator2D = 0.0f;

    // -- 3D helpers (Jolt) ----------------------------------------------------
    void Create3DBody(entt::registry& registry, entt::entity entity,
                      RigidBody3DComponent& rb, const TransformComponent& tc);
    void Destroy3DBody(RigidBody3DComponent& rb);
    void Attach3DBoxShape(RigidBody3DComponent& rb, const BoxCollider3DComponent& bc,
                          const glm::vec3& worldScale);
    void Attach3DSphereShape(RigidBody3DComponent& rb, const SphereCollider3DComponent& sc,
                             const glm::vec3& worldScale);

    // -- 2D helpers (Box2D v3) ------------------------------------------------
    void Create2DBody(entt::entity entity, RigidBody2DComponent& rb,
                      const TransformComponent& tc);
    void Destroy2DBody(RigidBody2DComponent& rb);
    void Attach2DBoxShape(RigidBody2DComponent& rb, BoxCollider2DComponent& bc);
};
