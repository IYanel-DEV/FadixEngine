#include "physics/PhysicsWorld.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>
#include <box2d/box2d.h>

#include <iostream>
#include <thread>
#include <unordered_map>

#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace fadix
{
namespace
{
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kDynamicLayer = 1;
constexpr JPH::BroadPhaseLayer kStaticBroadPhase{0};
constexpr JPH::BroadPhaseLayer kDynamicBroadPhase{1};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
{
public:
    [[nodiscard]] std::uint32_t GetNumBroadPhaseLayers() const override { return 2; }
    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == kStaticLayer ? kStaticBroadPhase : kDynamicBroadPhase;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == kStaticBroadPhase ? "Static" : "Dynamic";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhase) const override
    {
        return layer == kDynamicLayer || broadPhase == kDynamicBroadPhase;
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override
    {
        return first == kDynamicLayer || second == kDynamicLayer;
    }
};

// Jolt boxes must have every half extent >= the convex radius, and Box2D
// requires strictly positive extents. Clamp inspector values into a legal range.
constexpr float kMinHalfExtent = 0.01F;

[[nodiscard]] JPH::Vec3 ClampHalfExtent3D(const glm::vec3 halfExtent)
{
    return JPH::Vec3{std::max(halfExtent.x, kMinHalfExtent), std::max(halfExtent.y, kMinHalfExtent),
        std::max(halfExtent.z, kMinHalfExtent)};
}

// Extract the rotation of the entity about the Z axis (the only meaningful
// rotation for a 2D body) from its full 3D orientation quaternion.
[[nodiscard]] float ZRotation(const glm::quat& rotation)
{
    const glm::vec3 rotated = rotation * glm::vec3{1.0F, 0.0F, 0.0F};
    return std::atan2(rotated.y, rotated.x);
}
}

struct PhysicsWorld::State
{
    State()
        : TempAllocator(16U * 1024U * 1024U),
          Jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
              static_cast<int>(std::thread::hardware_concurrency() > 1U
                  ? std::thread::hardware_concurrency() - 1U
                  : 1U))
    {
        std::clog << "[Fadix] Initializing Jolt 3D physics\n";
        Physics3D.Init(65536, 0, 65536, 10240, BroadPhase, ObjectVsBroadPhaseFilter, PairFilter);

        std::clog << "[Fadix] Initializing Box2D physics\n";
        b2WorldDef definition = b2DefaultWorldDef();
        definition.gravity = b2Vec2{0.0F, -9.81F};
        Physics2D = b2CreateWorld(&definition);
        std::clog << "[Fadix] Physics worlds ready\n";
    }

    ~State()
    {
        if (b2World_IsValid(Physics2D))
        {
            b2DestroyWorld(Physics2D);
        }
    }

    BroadPhaseLayers BroadPhase;
    ObjectVsBroadPhase ObjectVsBroadPhaseFilter;
    ObjectPairFilter PairFilter;
    JPH::TempAllocatorImpl TempAllocator;
    JPH::JobSystemThreadPool Jobs;
    JPH::PhysicsSystem Physics3D;
    b2WorldId Physics2D{b2_nullWorldId};
    struct Body3DRecord
    {
        JPH::BodyID Id;
        glm::vec3 HalfExtent{0.5F};
        ColliderShape3D Shape{ColliderShape3D::Box};
        AssetHandle MeshAsset{};
        float Mass{1.0F};
        float Friction{0.8F};
        bool Dynamic{true};
    };
    struct Body2DRecord
    {
        b2BodyId Id;
        glm::vec2 HalfExtent{0.5F};
        bool Dynamic{true};
    };
    struct Body2DNewRecord
    {
        b2BodyId Id;
        Body2DType Type{Body2DType::Dynamic};
        Collider2DShape Shape{Collider2DShape::Box};
        glm::vec2 Size{1.0F};
    };
    struct CharacterRecord
    {
        JPH::Ref<JPH::CharacterVirtual> Controller;
        float Radius{0.5F};
        float Height{2.0F};
        float StepHeight{0.35F};
        float MaxSlopeDegrees{45.0F};
        float MoveSpeed{5.0F};
        float JumpSpeed{5.0F};
        float Gravity{-9.81F};
        float AirControl{0.35F};
        float PushStrength{100.0F};
        glm::vec2 MoveInput{0.0F};
        glm::vec3 RequestedTranslation{0.0F};
        bool JumpRequested{false};
    };

    PhysicsBodyHandle NextHandle{1};
    std::unordered_map<PhysicsBodyHandle, Body3DRecord> Bodies3D;
    std::unordered_map<PhysicsBodyHandle, Body2DRecord> Bodies2D;
    std::unordered_map<PhysicsBodyHandle, Body2DNewRecord> Bodies2DNew;
    std::unordered_map<Uuid, PhysicsBodyHandle> EntityBodies3D;
    std::unordered_map<Uuid, PhysicsBodyHandle> EntityBodies2D;
    std::unordered_map<Uuid, PhysicsBodyHandle> EntityBodies2DNew;
    std::unordered_map<Uuid, CharacterRecord> Characters;
};

PhysicsWorld::PhysicsWorld(ICollisionMeshProvider* meshes)
    : m_Meshes(meshes)
{
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    try
    {
        m_State = std::make_unique<State>();
    }
    catch (...)
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
        throw;
    }
}

PhysicsWorld::~PhysicsWorld()
{
    m_State.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void PhysicsWorld::Step(const float deltaSeconds)
{
    constexpr float fixedDeltaSeconds = 1.0F / 60.0F;
    // Clamp the backlog: physics may run late, but it should not spend next Tuesday catching up.
    m_Accumulator = std::min(m_Accumulator + std::max(deltaSeconds, 0.0F), fixedDeltaSeconds * 4.0F);
    while (m_Accumulator >= fixedDeltaSeconds)
    {
        StepFixed(fixedDeltaSeconds);
        m_Accumulator -= fixedDeltaSeconds;
    }
}

void PhysicsWorld::StepFixed(const float fixedDeltaSeconds)
{
    if (fixedDeltaSeconds <= 0.0F)
    {
        return;
    }
    for (auto& [id, record] : m_State->Characters)
    {
        static_cast<void>(id);
        JPH::CharacterVirtual& character = *record.Controller;
        const bool grounded = character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
        const JPH::Vec3 current = character.GetLinearVelocity();

        glm::vec2 requested{record.RequestedTranslation.x, record.RequestedTranslation.z};
        glm::vec2 desired = record.MoveInput;
        if (glm::dot(requested, requested) > 0.0000001F)
        {
            desired = requested / fixedDeltaSeconds;
        }
        else
        {
            const float length = glm::length(desired);
            if (length > 1.0F)
            {
                desired /= length;
            }
            desired *= record.MoveSpeed;
        }

        if (!grounded)
        {
            const glm::vec2 airborne{current.GetX(), current.GetZ()};
            desired = glm::mix(airborne, desired, record.AirControl);
        }

        float vertical = current.GetY();
        if (grounded && vertical < 0.0F)
        {
            vertical = character.GetGroundVelocity().GetY();
        }
        if (record.JumpRequested && grounded)
        {
            vertical = record.JumpSpeed;
        }
        else
        {
            vertical += record.Gravity * fixedDeltaSeconds;
        }

        const JPH::Vec3 velocity = character.CancelVelocityTowardsSteepSlopes(
            JPH::Vec3{desired.x, vertical, desired.y});
        character.SetLinearVelocity(velocity);
        JPH::CharacterVirtual::ExtendedUpdateSettings update;
        update.mWalkStairsStepUp = JPH::Vec3{0.0F, record.StepHeight, 0.0F};
        update.mStickToFloorStepDown = JPH::Vec3{0.0F, -std::max(record.StepHeight, 0.1F), 0.0F};
        character.ExtendedUpdate(fixedDeltaSeconds, JPH::Vec3{0.0F, record.Gravity, 0.0F}, update,
            m_State->Physics3D.GetDefaultBroadPhaseLayerFilter(kDynamicLayer),
            m_State->Physics3D.GetDefaultLayerFilter(kDynamicLayer), {}, {}, m_State->TempAllocator);
        record.MoveInput = {0.0F, 0.0F};
        record.RequestedTranslation = {0.0F, 0.0F, 0.0F};
        record.JumpRequested = false;
    }

    m_State->Physics3D.Update(fixedDeltaSeconds, 1, &m_State->TempAllocator, &m_State->Jobs);
    b2World_Step(m_State->Physics2D, fixedDeltaSeconds, 4);
}

PhysicsBodyHandle PhysicsWorld::CreateBody3D(const Body3DDesc& description)
{
    const auto motion = description.Dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
    const JPH::ObjectLayer layer = description.Dynamic ? kDynamicLayer : kStaticLayer;
    const JPH::Quat rotation{
        description.Rotation.x, description.Rotation.y, description.Rotation.z, description.Rotation.w};
    const JPH::Vec3 halfExtent = ClampHalfExtent3D(description.HalfExtent);
    JPH::RefConst<JPH::Shape> shape;
    switch (description.Shape)
    {
    case ColliderShape3D::Sphere:
        shape = new JPH::SphereShape(std::max({halfExtent.GetX(), halfExtent.GetY(), halfExtent.GetZ()}));
        break;
    case ColliderShape3D::Capsule:
    {
        const float radius = std::max(halfExtent.GetX(), halfExtent.GetZ());
        shape = new JPH::CapsuleShape(std::max(halfExtent.GetY() - radius, kMinHalfExtent), radius);
        break;
    }
    case ColliderShape3D::Cylinder:
    {
        const float radius = std::max(halfExtent.GetX(), halfExtent.GetZ());
        const float convexRadius = std::min(0.05F, std::min(radius, halfExtent.GetY()) * 0.5F);
        shape = new JPH::CylinderShape(halfExtent.GetY(), radius, convexRadius);
        break;
    }
    case ColliderShape3D::Mesh:
    {
        const std::size_t minimumVertices = description.Dynamic ? 4U : 3U;
        if (description.MeshVertices.size() < minimumVertices ||
            (!description.Dynamic && description.MeshIndices.size() < 3U))
        {
            return InvalidPhysicsBody;
        }
        JPH::ShapeSettings::ShapeResult result;
        if (description.Dynamic)
        {
            JPH::Array<JPH::Vec3> points;
            points.reserve(description.MeshVertices.size());
            for (const glm::vec3& vertex : description.MeshVertices)
            {
                points.emplace_back(vertex.x, vertex.y, vertex.z);
            }
            JPH::ConvexHullShapeSettings hull{points};
            result = hull.Create();
        }
        else
        {
            JPH::VertexList vertices;
            vertices.reserve(description.MeshVertices.size());
            for (const glm::vec3& vertex : description.MeshVertices)
            {
                vertices.emplace_back(vertex.x, vertex.y, vertex.z);
            }
            JPH::IndexedTriangleList triangles;
            triangles.reserve(description.MeshIndices.size() / 3U);
            for (std::size_t index = 0; index + 2 < description.MeshIndices.size(); index += 3)
            {
                const std::uint32_t a = description.MeshIndices[index];
                const std::uint32_t b = description.MeshIndices[index + 1];
                const std::uint32_t c = description.MeshIndices[index + 2];
                if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size())
                {
                    continue;
                }
                triangles.emplace_back(a, description.FlipMeshWinding ? c : b,
                    description.FlipMeshWinding ? b : c, 0U);
            }
            JPH::MeshShapeSettings mesh{std::move(vertices), std::move(triangles)};
            result = mesh.Create();
        }
        if (result.HasError())
        {
            std::clog << "[Fadix] Collider shape creation failed: " << result.GetError() << '\n';
            return InvalidPhysicsBody;
        }
        shape = result.Get();
        break;
    }
    case ColliderShape3D::Plane:
    case ColliderShape3D::Box:
    case ColliderShape3D::Auto:
    {
        const float convexRadius =
            std::min(0.05F, std::min({halfExtent.GetX(), halfExtent.GetY(), halfExtent.GetZ()}) * 0.5F);
        shape = new JPH::BoxShape(halfExtent, convexRadius);
        break;
    }
    }
    JPH::BodyCreationSettings settings{
        shape.GetPtr(),
        JPH::RVec3{description.Position.x, description.Position.y, description.Position.z},
        rotation, motion, layer};
    settings.mEnhancedInternalEdgeRemoval = description.Shape == ColliderShape3D::Mesh;
    settings.mFriction = std::clamp(description.Friction, 0.0F, 2.0F);
    if (description.Dynamic)
    {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass =
            std::clamp(description.Mass, 0.001F, 1000000.0F);
    }
    JPH::BodyInterface& bodies = m_State->Physics3D.GetBodyInterface();
    const JPH::BodyID id = bodies.CreateAndAddBody(
        settings, description.Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid())
    {
        return InvalidPhysicsBody;
    }
    const PhysicsBodyHandle handle = m_State->NextHandle++;
    m_State->Bodies3D.emplace(handle,
        State::Body3DRecord{id, description.HalfExtent, description.Shape, {},
            description.Mass, description.Friction, description.Dynamic});
    m_State->EntityBodies3D.insert_or_assign(description.Entity, handle);
    return handle;
}

PhysicsBodyHandle PhysicsWorld::CreateBody2D(const Body2DDesc& description)
{
    b2BodyDef definition = b2DefaultBodyDef();
    definition.type = description.Dynamic ? b2_dynamicBody : b2_staticBody;
    definition.position = b2Vec2{description.Position.x, description.Position.y};
    definition.rotation = b2MakeRot(description.Rotation);
    const b2BodyId id = b2CreateBody(m_State->Physics2D, &definition);
    if (!b2Body_IsValid(id))
    {
        return InvalidPhysicsBody;
    }
    b2ShapeDef shape = b2DefaultShapeDef();
    shape.density = description.Dynamic ? 1.0F : 0.0F;
    const b2Polygon box = b2MakeBox(
        std::max(description.HalfExtent.x, kMinHalfExtent), std::max(description.HalfExtent.y, kMinHalfExtent));
    b2CreatePolygonShape(id, &shape, &box);
    const PhysicsBodyHandle handle = m_State->NextHandle++;
    m_State->Bodies2D.emplace(handle, State::Body2DRecord{id, description.HalfExtent, description.Dynamic});
    m_State->EntityBodies2D.insert_or_assign(description.Entity, handle);
    return handle;
}

void PhysicsWorld::DestroyBody(const PhysicsBodyHandle handle)
{
    if (const auto iterator = m_State->Bodies3D.find(handle); iterator != m_State->Bodies3D.end())
    {
        JPH::BodyInterface& bodies = m_State->Physics3D.GetBodyInterface();
        bodies.RemoveBody(iterator->second.Id);
        bodies.DestroyBody(iterator->second.Id);
        m_State->Bodies3D.erase(iterator);
    }
    if (const auto iterator = m_State->Bodies2D.find(handle); iterator != m_State->Bodies2D.end())
    {
        if (b2Body_IsValid(iterator->second.Id))
        {
            b2DestroyBody(iterator->second.Id);
        }
        m_State->Bodies2D.erase(iterator);
    }
    if (const auto iterator = m_State->Bodies2DNew.find(handle);
        iterator != m_State->Bodies2DNew.end())
    {
        if (b2Body_IsValid(iterator->second.Id))
        {
            b2DestroyBody(iterator->second.Id);
        }
        m_State->Bodies2DNew.erase(iterator);
    }
    std::erase_if(m_State->EntityBodies3D, [handle](const auto& item) { return item.second == handle; });
    std::erase_if(m_State->EntityBodies2D, [handle](const auto& item) { return item.second == handle; });
    std::erase_if(m_State->EntityBodies2DNew, [handle](const auto& item) { return item.second == handle; });
}

void PhysicsWorld::SyncFromWorld(const IWorld& world)
{
    const entt::registry& registry = world.Registry();

    // --- 3D (Jolt) ---
    std::vector<Uuid> seen3D;
    for (const auto [entity, id, transform, body] :
         registry.view<const UuidComponent, const TransformComponent, const JoltBodyComponent>().each())
    {
        if (registry.all_of<CharacterControllerComponent>(entity))
        {
            continue;
        }
        seen3D.push_back(id.Id);
        const MeshComponent* mesh = registry.try_get<MeshComponent>(entity);
        ColliderShape3D shape = ResolveColliderShape3D(body, mesh);
        std::optional<CollisionMeshView> collisionMesh;
        if (shape == ColliderShape3D::Mesh && mesh != nullptr && mesh->ImportedMesh.IsValid() &&
            m_Meshes != nullptr)
        {
            collisionMesh = m_Meshes->CollisionMesh(mesh->ImportedMesh);
        }
        // Invalid/unavailable mesh data must still collide; use a fitted box and
        // retry the mesh on a later sync once the asset becomes available.
        if (shape == ColliderShape3D::Mesh &&
            !collisionMesh)
        {
            shape = ColliderShape3D::Box;
        }
        glm::vec3 localHalfExtent = body.HalfExtent;
        if (body.AutoFit && mesh != nullptr)
        {
            localHalfExtent = collisionMesh
                ? (collisionMesh->BoundsMax - collisionMesh->BoundsMin) * 0.5F
                : DefaultColliderHalfExtent(mesh->Kind);
        }
        const glm::vec3 absoluteScale = glm::abs(transform.Scale);
        const glm::vec3 halfExtent = glm::max(localHalfExtent * absoluteScale, glm::vec3{kMinHalfExtent});
        const float mass = std::clamp(body.Mass, 0.001F, 1000000.0F);
        const float friction = std::clamp(body.Friction, 0.0F, 2.0F);
        const AssetHandle meshAsset = collisionMesh && mesh != nullptr ? mesh->ImportedMesh : AssetHandle{};
        auto found = m_State->EntityBodies3D.find(id.Id);
        // Rebuild (do not recreate every frame) only when the collider size or
        // simulation mode actually changed; otherwise reuse the existing body.
        if (found != m_State->EntityBodies3D.end())
        {
            const auto record = m_State->Bodies3D.find(found->second);
            if (record != m_State->Bodies3D.end() &&
                (record->second.HalfExtent != halfExtent || record->second.Shape != shape ||
                    record->second.MeshAsset != meshAsset || record->second.Mass != mass ||
                    record->second.Friction != friction ||
                    record->second.Dynamic != body.Dynamic))
            {
                DestroyBody(found->second);
                found = m_State->EntityBodies3D.end();
            }
        }
        PhysicsBodyHandle handle = found != m_State->EntityBodies3D.end()
            ? found->second
            : InvalidPhysicsBody;
        if (handle == InvalidPhysicsBody)
        {
            std::vector<glm::vec3> scaledVertices;
            if (shape == ColliderShape3D::Mesh && collisionMesh)
            {
                glm::vec3 meshScale = transform.Scale;
                if (!body.AutoFit)
                {
                    const glm::vec3 sourceHalf = glm::max(
                        (collisionMesh->BoundsMax - collisionMesh->BoundsMin) * 0.5F,
                        glm::vec3{kMinHalfExtent});
                    meshScale *= body.HalfExtent / sourceHalf;
                }
                scaledVertices.reserve(collisionMesh->Vertices.size());
                for (const glm::vec3& vertex : collisionMesh->Vertices)
                {
                    scaledVertices.push_back(vertex * meshScale);
                }
            }
            Body3DDesc description;
            description.Entity = id.Id;
            description.Position = transform.Position;
            description.Rotation = transform.Rotation;
            description.HalfExtent = halfExtent;
            description.Shape = shape;
            description.MeshVertices = scaledVertices;
            description.MeshIndices = collisionMesh
                ? collisionMesh->Indices
                : std::span<const std::uint32_t>{};
            description.Mass = mass;
            description.Friction = friction;
            description.FlipMeshWinding =
                transform.Scale.x * transform.Scale.y * transform.Scale.z < 0.0F;
            description.Dynamic = body.Dynamic;
            handle = CreateBody3D(description);
            if (const auto record = m_State->Bodies3D.find(handle);
                record != m_State->Bodies3D.end())
            {
                record->second.MeshAsset = meshAsset;
            }
        }
        const auto physical = m_State->Bodies3D.find(handle);
        if (physical != m_State->Bodies3D.end())
        {
            m_State->Physics3D.GetBodyInterface().SetPositionAndRotation(
                physical->second.Id,
                JPH::RVec3{transform.Position.x, transform.Position.y, transform.Position.z},
                JPH::Quat{transform.Rotation.x, transform.Rotation.y, transform.Rotation.z, transform.Rotation.w},
                body.Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
        }
        static_cast<void>(entity);
    }
    ReconcileRemovedBodies(m_State->EntityBodies3D, seen3D);

    // --- Upright gameplay characters (Jolt CharacterVirtual) ---
    std::vector<Uuid> seenCharacters;
    for (const auto [entity, id, transform, authored] :
         registry.view<const UuidComponent, const TransformComponent,
             const CharacterControllerComponent>().each())
    {
        static_cast<void>(entity);
        seenCharacters.push_back(id.Id);
        CharacterControllerComponent settings = authored;
        SanitizeCharacterController(settings);
        auto found = m_State->Characters.find(id.Id);
        if (found != m_State->Characters.end())
        {
            const auto& current = found->second;
            if (current.Radius != settings.Radius || current.Height != settings.Height ||
                current.StepHeight != settings.StepHeight ||
                current.MaxSlopeDegrees != settings.MaxSlopeDegrees ||
                current.MoveSpeed != settings.MoveSpeed || current.JumpSpeed != settings.JumpSpeed ||
                current.Gravity != settings.Gravity || current.AirControl != settings.AirControl ||
                current.PushStrength != settings.PushStrength)
            {
                m_State->Characters.erase(found);
                found = m_State->Characters.end();
            }
        }

        if (found == m_State->Characters.end())
        {
            const float cylinderHalf =
                std::max((settings.Height - settings.Radius * 2.0F) * 0.5F, kMinHalfExtent);
            const JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(cylinderHalf, settings.Radius);
            const JPH::RefConst<JPH::Shape> shape = JPH::RotatedTranslatedShapeSettings(
                JPH::Vec3{0.0F, settings.Height * 0.5F, 0.0F}, JPH::Quat::sIdentity(), capsule)
                                                        .Create()
                                                        .Get();
            constexpr float innerScale = 0.9F;
            const JPH::RefConst<JPH::Shape> innerCapsule =
                new JPH::CapsuleShape(cylinderHalf * innerScale, settings.Radius * innerScale);
            const JPH::RefConst<JPH::Shape> innerShape = JPH::RotatedTranslatedShapeSettings(
                JPH::Vec3{0.0F, settings.Height * 0.5F, 0.0F}, JPH::Quat::sIdentity(), innerCapsule)
                                                             .Create()
                                                             .Get();
            JPH::CharacterVirtualSettings characterSettings;
            characterSettings.mShape = shape;
            characterSettings.mInnerBodyShape = innerShape;
            characterSettings.mInnerBodyLayer = kDynamicLayer;
            characterSettings.mSupportingVolume =
                JPH::Plane{JPH::Vec3::sAxisY(), -settings.Radius};
            characterSettings.mMaxSlopeAngle = JPH::DegreesToRadians(settings.MaxSlopeDegrees);
            characterSettings.mMaxStrength = settings.PushStrength;
            characterSettings.mEnhancedInternalEdgeRemoval = true;
            const JPH::RVec3 feet{transform.Position.x,
                transform.Position.y - settings.Height * 0.5F, transform.Position.z};
            JPH::Ref<JPH::CharacterVirtual> controller = new JPH::CharacterVirtual(
                &characterSettings, feet, JPH::Quat::sIdentity(), &m_State->Physics3D);
            State::CharacterRecord record;
            record.Controller = std::move(controller);
            record.Radius = settings.Radius;
            record.Height = settings.Height;
            record.StepHeight = settings.StepHeight;
            record.MaxSlopeDegrees = settings.MaxSlopeDegrees;
            record.MoveSpeed = settings.MoveSpeed;
            record.JumpSpeed = settings.JumpSpeed;
            record.Gravity = settings.Gravity;
            record.AirControl = settings.AirControl;
            record.PushStrength = settings.PushStrength;
            found = m_State->Characters.emplace(id.Id, std::move(record)).first;
        }

        State::CharacterRecord& record = found->second;
        const JPH::RVec3 feet = record.Controller->GetPosition();
        const glm::vec3 physicalCenter{feet.GetX(), feet.GetY() + record.Height * 0.5F, feet.GetZ()};
        record.RequestedTranslation += glm::vec3{
            transform.Position.x - physicalCenter.x, 0.0F, transform.Position.z - physicalCenter.z};
        record.MoveInput = settings.MoveInput;
        record.JumpRequested = record.JumpRequested || settings.JumpRequested;
    }
    std::erase_if(m_State->Characters, [&seenCharacters](const auto& item) {
        return std::find(seenCharacters.begin(), seenCharacters.end(), item.first) == seenCharacters.end();
    });

    // --- 2D (Box2D) ---
    std::vector<Uuid> seen2D;
    for (const auto [entity, id, transform, body] :
         registry.view<const UuidComponent, const TransformComponent, const Box2DBodyComponent>().each())
    {
        seen2D.push_back(id.Id);
        const float angle = ZRotation(transform.Rotation);
        auto found = m_State->EntityBodies2D.find(id.Id);
        if (found != m_State->EntityBodies2D.end())
        {
            const auto record = m_State->Bodies2D.find(found->second);
            if (record != m_State->Bodies2D.end() &&
                (record->second.HalfExtent != body.HalfExtent || record->second.Dynamic != body.Dynamic))
            {
                DestroyBody(found->second);
                found = m_State->EntityBodies2D.end();
            }
        }
        const PhysicsBodyHandle handle = found == m_State->EntityBodies2D.end()
            ? CreateBody2D({id.Id, {transform.Position.x, transform.Position.y}, angle, body.HalfExtent, body.Dynamic})
            : found->second;
        const auto physical = m_State->Bodies2D.find(handle);
        if (physical != m_State->Bodies2D.end())
        {
            b2Body_SetTransform(
                physical->second.Id, {transform.Position.x, transform.Position.y}, b2MakeRot(angle));
        }
        static_cast<void>(entity);
    }
    ReconcileRemovedBodies(m_State->EntityBodies2D, seen2D);

    // --- 2D new-style (RigidBody2D + Collider2D) ---
    std::vector<Uuid> seen2DNew;
    for (const auto [entity, id, transform, rb] :
         registry.view<const UuidComponent, const TransformComponent, const RigidBody2DComponent>()
             .each())
    {
        const Collider2DComponent* col =
            registry.try_get<const Collider2DComponent>(entity);
        const Collider2DShape shape = col ? col->Shape : Collider2DShape::Box;
        const glm::vec2 size = col ? col->Size : glm::vec2{0.5F};
        seen2DNew.push_back(id.Id);
        const float angle = ZRotation(transform.Rotation);
        auto found = m_State->EntityBodies2DNew.find(id.Id);
        if (found != m_State->EntityBodies2DNew.end())
        {
            const auto record = m_State->Bodies2DNew.find(found->second);
            if (record != m_State->Bodies2DNew.end() &&
                (record->second.Type != rb.Type || record->second.Shape != shape ||
                 record->second.Size != size))
            {
                DestroyBody(found->second);
                found = m_State->EntityBodies2DNew.end();
            }
        }
        if (found == m_State->EntityBodies2DNew.end())
        {
            b2BodyDef def = b2DefaultBodyDef();
            switch (rb.Type)
            {
            case Body2DType::Static: def.type = b2_staticBody; break;
            case Body2DType::Kinematic: def.type = b2_kinematicBody; break;
            case Body2DType::Dynamic: def.type = b2_dynamicBody; break;
            }
            def.position = b2Vec2{transform.Position.x, transform.Position.y};
            def.rotation = b2MakeRot(angle);
            if (rb.Type == Body2DType::Dynamic)
            {
                def.linearVelocity = {rb.InitialLinearVelocity.x, rb.InitialLinearVelocity.y};
                def.angularVelocity = rb.InitialAngularVelocity;
            }
            def.fixedRotation = rb.FixedRotation;
            def.linearDamping = rb.LinearDamping;
            def.angularDamping = rb.AngularDamping;
            const b2BodyId bid = b2CreateBody(m_State->Physics2D, &def);
            if (b2Body_IsValid(bid))
            {
                b2ShapeDef sd = b2DefaultShapeDef();
                sd.density = (rb.Type == Body2DType::Dynamic) ? std::max(col ? col->Density : 1.0F, 0.0F) : 0.0F;
                sd.material.friction = col ? col->Friction : 0.6F;
                sd.material.restitution = col ? col->Restitution : 0.0F;
                sd.isSensor = col && col->Sensor;
                if (shape == Collider2DShape::Circle)
                {
                    const float radius = std::max(size.x * 0.5F, kMinHalfExtent);
                    const b2Circle circle{{col ? col->Offset.x : 0.0F, col ? col->Offset.y : 0.0F}, radius};
                    b2CreateCircleShape(bid, &sd, &circle);
                }
                else
                {
                    const glm::vec2 half{std::max(size.x * 0.5F, kMinHalfExtent),
                        std::max(size.y * 0.5F, kMinHalfExtent)};
                    const b2Polygon box = b2MakeOffsetBox(
                        half.x, half.y,
                        {col ? col->Offset.x : 0.0F, col ? col->Offset.y : 0.0F},
                        b2MakeRot(0.0F));
                    b2CreatePolygonShape(bid, &sd, &box);
                }
                if (rb.Type == Body2DType::Dynamic && rb.Mass > 0.0F)
                {
                    b2MassData md = b2Body_GetMassData(bid);
                    md.mass = rb.Mass;
                    b2Body_SetMassData(bid, md);
                }
                const PhysicsBodyHandle handle = m_State->NextHandle++;
                m_State->Bodies2DNew.emplace(handle, State::Body2DNewRecord{bid, rb.Type, shape, size});
                m_State->EntityBodies2DNew.insert_or_assign(id.Id, handle);
            }
        }
        else
        {
            const auto physical = m_State->Bodies2DNew.find(found->second);
            if (physical != m_State->Bodies2DNew.end())
            {
                b2Body_SetTransform(
                    physical->second.Id,
                    {transform.Position.x, transform.Position.y},
                    b2MakeRot(angle));
            }
        }
        static_cast<void>(entity);
    }
    ReconcileRemovedBodies(m_State->EntityBodies2DNew, seen2DNew);
}

void PhysicsWorld::ReconcileRemovedBodies(
    std::unordered_map<Uuid, PhysicsBodyHandle>& entityBodies, const std::vector<Uuid>& seen)
{
    std::vector<PhysicsBodyHandle> stale;
    for (const auto& [id, handle] : entityBodies)
    {
        if (std::find(seen.begin(), seen.end(), id) == seen.end())
        {
            stale.push_back(handle);
        }
    }
    for (const PhysicsBodyHandle handle : stale)
    {
        DestroyBody(handle);
    }
}

void PhysicsWorld::SyncToWorld(IWorld& world) const
{
    entt::registry& registry = world.Registry();
    for (const auto& [id, handle] : m_State->EntityBodies3D)
    {
        const auto entity = world.Find(id);
        const auto body = m_State->Bodies3D.find(handle);
        if (!entity || body == m_State->Bodies3D.end())
        {
            continue;
        }
        if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
        {
            const JPH::RVec3 position = m_State->Physics3D.GetBodyInterface().GetPosition(body->second.Id);
            const JPH::Quat rotation = m_State->Physics3D.GetBodyInterface().GetRotation(body->second.Id);
            transform->Position = {position.GetX(), position.GetY(), position.GetZ()};
            transform->Rotation = {rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()};
        }
    }
    for (const auto& [id, handle] : m_State->EntityBodies2D)
    {
        const auto entity = world.Find(id);
        const auto body = m_State->Bodies2D.find(handle);
        if (!entity || body == m_State->Bodies2D.end())
        {
            continue;
        }
        if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
        {
            const b2Vec2 position = b2Body_GetPosition(body->second.Id);
            const b2Rot rotation = b2Body_GetRotation(body->second.Id);
            transform->Position.x = position.x;
            transform->Position.y = position.y;
            transform->Rotation = glm::angleAxis(b2Rot_GetAngle(rotation), glm::vec3{0.0F, 0.0F, 1.0F});
        }
    }
    for (const auto& [id, handle] : m_State->EntityBodies2DNew)
    {
        const auto entity = world.Find(id);
        const auto body = m_State->Bodies2DNew.find(handle);
        if (!entity || body == m_State->Bodies2DNew.end())
        {
            continue;
        }
        if (body->second.Type == Body2DType::Static)
        {
            continue;
        }
        if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
        {
            const b2Vec2 position = b2Body_GetPosition(body->second.Id);
            const b2Rot rotation = b2Body_GetRotation(body->second.Id);
            transform->Position.x = position.x;
            transform->Position.y = position.y;
            transform->Rotation = glm::angleAxis(b2Rot_GetAngle(rotation), glm::vec3{0.0F, 0.0F, 1.0F});
        }
    }
    for (auto& [id, record] : m_State->Characters)
    {
        const auto entity = world.Find(id);
        if (!entity)
        {
            continue;
        }
        if (TransformComponent* transform = registry.try_get<TransformComponent>(*entity))
        {
            const JPH::RVec3 feet = record.Controller->GetPosition();
            transform->Position =
                {feet.GetX(), feet.GetY() + record.Height * 0.5F, feet.GetZ()};
        }
        if (CharacterControllerComponent* character =
                registry.try_get<CharacterControllerComponent>(*entity))
        {
            character->Grounded = record.Controller->GetGroundState() ==
                JPH::CharacterBase::EGroundState::OnGround;
            character->MoveInput = {0.0F, 0.0F};
            character->JumpRequested = false;
        }
    }
}

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorld(ICollisionMeshProvider* meshes)
{
    return std::make_unique<PhysicsWorld>(meshes);
}

}
