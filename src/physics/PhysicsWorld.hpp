#pragma once

#include "engine/physics/IPhysicsWorld.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace fadix
{
class PhysicsWorld final : public IPhysicsWorld
{
public:
    explicit PhysicsWorld(ICollisionMeshProvider* meshes = nullptr);
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void Step(float deltaSeconds);
    void StepFixed(float fixedDeltaSeconds) override;
    void SyncFromWorld(const IWorld& world) override;
    void SyncToWorld(IWorld& world) const override;
    [[nodiscard]] PhysicsBodyHandle CreateBody3D(const Body3DDesc& description) override;
    [[nodiscard]] PhysicsBodyHandle CreateBody2D(const Body2DDesc& description) override;
    void DestroyBody(PhysicsBodyHandle handle) override;

private:
    void ReconcileRemovedBodies(
        std::unordered_map<Uuid, PhysicsBodyHandle>& entityBodies, const std::vector<Uuid>& seen);

    struct State;
    std::unique_ptr<State> m_State;
    ICollisionMeshProvider* m_Meshes{};
    float m_Accumulator{0.0F};
};

[[nodiscard]] std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld(
    ICollisionMeshProvider* meshes = nullptr);
}
