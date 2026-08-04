#pragma once

#include "engine/assets/AssetHandle.hpp"
#include "engine/Uuid.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace fadix
{
class IWorld;

using PhysicsBodyHandle = std::uint64_t;
inline constexpr PhysicsBodyHandle InvalidPhysicsBody = 0;

struct CollisionMeshView
{
    std::span<const glm::vec3> Vertices;
    std::span<const std::uint32_t> Indices;
    glm::vec3 BoundsMin{0.0F};
    glm::vec3 BoundsMax{0.0F};
};

class ICollisionMeshProvider
{
public:
    virtual ~ICollisionMeshProvider() = default;
    [[nodiscard]] virtual std::optional<CollisionMeshView> CollisionMesh(
        const AssetHandle& handle) = 0;
};

enum class ColliderShape3D : std::uint8_t
{
    Auto,
    Box,
    Sphere,
    Capsule,
    Cylinder,
    Plane,
    Mesh
};

struct Body3DDesc
{
    Uuid Entity;
    glm::vec3 Position{0.0F};
    glm::quat Rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 HalfExtent{0.5F};
    ColliderShape3D Shape{ColliderShape3D::Box};
    std::span<const glm::vec3> MeshVertices{};
    std::span<const std::uint32_t> MeshIndices{};
    float Mass{1.0F};
    float Friction{0.8F};
    bool FlipMeshWinding{false};
    bool Dynamic{true};
};

struct Body2DDesc
{
    Uuid Entity;
    glm::vec2 Position{0.0F};
    float Rotation{0.0F};
    glm::vec2 HalfExtent{0.5F};
    bool Dynamic{true};
};

// Sensor contact event generated after a Box2D step. SensorEntity carries a
// Collider2DComponent with Sensor=true; VisitorEntity is the touching body.
struct ContactEvent2D
{
    Uuid SensorEntity;
    Uuid VisitorEntity;
    bool Entered{true}; // true = begin touch, false = end touch
};

class IPhysicsWorld
{
public:
    virtual ~IPhysicsWorld() = default;
    virtual void StepFixed(float fixedDeltaSeconds) = 0;
    virtual void SyncFromWorld(const IWorld& world) = 0;
    virtual void SyncToWorld(IWorld& world) const = 0;
    [[nodiscard]] virtual PhysicsBodyHandle CreateBody3D(const Body3DDesc& description) = 0;
    [[nodiscard]] virtual PhysicsBodyHandle CreateBody2D(const Body2DDesc& description) = 0;
    virtual void DestroyBody(PhysicsBodyHandle handle) = 0;
    // Returns all sensor contact events collected since the last call and clears the queue.
    [[nodiscard]] virtual std::vector<ContactEvent2D> DrainContactEvents2D() { return {}; }
};
}
