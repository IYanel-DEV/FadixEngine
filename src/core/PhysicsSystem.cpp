#include "core/PhysicsSystem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>
#include <filesystem>
#include <thread>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "core/Scene.hpp"

// =============================================================================
// Jolt Physics includes
// =============================================================================
#ifdef JOLT_INCLUDED

// Jolt requires this pragma before any Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>

JPH_SUPPRESS_WARNINGS

#endif // JOLT_INCLUDED

// =============================================================================
// Box2D v3 includes
// =============================================================================
#ifdef BOX2D_INCLUDED
#  include <box2d/box2d.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

// =============================================================================
// Jolt layer / broadphase setup  (file-local, compiled only with Jolt)
// =============================================================================
#ifdef JOLT_INCLUDED

namespace {

namespace Layers {
    static constexpr JPH::ObjectLayer NON_MOVING = 0;
    static constexpr JPH::ObjectLayer MOVING     = 1;
    static constexpr JPH::uint        NUM_LAYERS = 2;
}

namespace BPLayers {
    static constexpr JPH::BroadPhaseLayer NON_MOVING{0};
    static constexpr JPH::BroadPhaseLayer MOVING{1};
    static constexpr JPH::uint            NUM_LAYERS = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface
{
public:
    JPH::uint GetNumBroadPhaseLayers() const override
    {
        return BPLayers::NUM_LAYERS;
    }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override
    {
        return layer == Layers::NON_MOVING ? BPLayers::NON_MOVING : BPLayers::MOVING;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override
    {
        return layer == BPLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer obj, JPH::BroadPhaseLayer bp) const override
    {
        if (obj == Layers::NON_MOVING) return bp == BPLayers::MOVING;
        return true; // MOVING collides with everything
    }
};

class ObjLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override
    {
        if (a == Layers::NON_MOVING) return b == Layers::MOVING;
        return true;
    }
};

} // namespace

struct PhysicsSystem::JoltContext
{
    JPH::TempAllocatorImpl   tempAlloc{ 10u * 1024u * 1024u };
    JPH::JobSystemThreadPool jobSystem{
        JPH::cMaxPhysicsJobs,
        JPH::cMaxPhysicsBarriers,
        static_cast<int>(std::thread::hardware_concurrency()) - 1
    };
    BPLayerInterfaceImpl bpLayerInterface;
    ObjVsBPFilter        objVsBP;
    ObjLayerPairFilter   objLayerPair;
    JPH::PhysicsSystem   physicsSystem;
};

#endif // JOLT_INCLUDED

// =============================================================================
// GLM ↔ Jolt conversion helpers
// =============================================================================
#ifdef JOLT_INCLUDED

static inline JPH::Vec3 ToJph(const glm::vec3& v)
{
    return JPH::Vec3(v.x, v.y, v.z);
}

static inline glm::vec3 FromJph(JPH::Vec3Arg v)
{
    return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

static inline JPH::Quat EulerToJphQuat(const glm::vec3& eulerDeg)
{
    const glm::quat q = glm::quat(glm::radians(eulerDeg));
    return JPH::Quat(q.x, q.y, q.z, q.w);
}

static inline glm::vec3 JphQuatToEuler(JPH::QuatArg q)
{
    const glm::quat gq(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
    return glm::degrees(glm::eulerAngles(gq));
}

static inline JPH::ObjectLayer LayerFor(MotionType3D mt)
{
    return mt == MotionType3D::Static ? Layers::NON_MOVING : Layers::MOVING;
}

static inline JPH::EMotionType JphMotionType(MotionType3D mt)
{
    switch (mt)
    {
        case MotionType3D::Static:    return JPH::EMotionType::Static;
        case MotionType3D::Kinematic: return JPH::EMotionType::Kinematic;
        case MotionType3D::Dynamic:
        default:                      return JPH::EMotionType::Dynamic;
    }
}

#endif // JOLT_INCLUDED

// =============================================================================
// World-space helpers
// =============================================================================
namespace {

[[maybe_unused]]
void DecomposeTRS(const glm::mat4& m, glm::vec3& outPos, glm::vec3& outEulerDeg)
{
    outPos = glm::vec3(m[3]);
    const glm::vec3 scale(glm::length(glm::vec3(m[0])),
                          glm::length(glm::vec3(m[1])),
                          glm::length(glm::vec3(m[2])));
    glm::mat3 rot(m);
    if (scale.x > 1e-6f) rot[0] /= scale.x;
    if (scale.y > 1e-6f) rot[1] /= scale.y;
    if (scale.z > 1e-6f) rot[2] /= scale.z;
    outEulerDeg = glm::degrees(glm::eulerAngles(glm::quat_cast(rot)));
}

bool HasParent(const entt::registry& registry, entt::entity entity)
{
    const auto* rel = registry.try_get<RelationshipComponent>(entity);
    return rel && rel->Parent != entt::null;
}

} // namespace

// =============================================================================
// Box2D v3 world handle helpers
// =============================================================================
#ifdef BOX2D_INCLUDED

static b2WorldId B2World(uint64_t stored)
{
    b2WorldId id;
    std::memcpy(&id, &stored, sizeof(id));
    return id;
}

static uint64_t StoreB2World(b2WorldId id)
{
    uint64_t out = 0;
    std::memcpy(&out, &id, sizeof(id));
    return out;
}

static b2BodyId B2Body(const Fadix2DBodyId& stored)
{
    b2BodyId id;
    std::memcpy(&id, &stored, sizeof(id));
    return id;
}

static Fadix2DBodyId StoreB2Body(b2BodyId id)
{
    Fadix2DBodyId out{};
    std::memcpy(&out, &id, sizeof(id));
    return out;
}

static b2ShapeId B2Shape(const Fadix2DShapeId& stored)
{
    b2ShapeId id;
    std::memcpy(&id, &stored, sizeof(id));
    return id;
}

static Fadix2DShapeId StoreB2Shape(b2ShapeId id)
{
    Fadix2DShapeId out{};
    std::memcpy(&out, &id, sizeof(id));
    return out;
}

static b2BodyType B2MotionType(MotionType2D mt)
{
    switch (mt)
    {
        case MotionType2D::Static:    return b2_staticBody;
        case MotionType2D::Kinematic: return b2_kinematicBody;
        case MotionType2D::Dynamic:
        default:                      return b2_dynamicBody;
    }
}

static bool B2BodyIsNull(const Fadix2DBodyId& id)
{
    return id.index1 == 0 && id.world0 == 0 && id.revision == 0;
}

static bool B2ShapeIsNull(const Fadix2DShapeId& id)
{
    return id.index1 == 0 && id.world0 == 0 && id.revision == 0;
}

#endif // BOX2D_INCLUDED

// =============================================================================
// PhysicsSystem::Init
// =============================================================================
void PhysicsSystem::Init(const glm::vec3& gravity)
{
    if (m_Initialised) Shutdown();

    m_Gravity       = gravity;
    m_Accumulator3D = 0.0f;
    m_Accumulator2D = 0.0f;

#ifdef JOLT_INCLUDED
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    m_Jolt = std::make_unique<JoltContext>();
    m_Jolt->physicsSystem.Init(
        /*maxBodies=*/       1024,
        /*numBodyMutexes=*/  0,
        /*maxBodyPairs=*/    1024,
        /*maxContactConstraints=*/ 1024,
        m_Jolt->bpLayerInterface,
        m_Jolt->objVsBP,
        m_Jolt->objLayerPair);
    m_Jolt->physicsSystem.SetGravity(ToJph(gravity));

    std::cout << "[Physics] Jolt 3D world created (gravity "
              << gravity.x << ',' << gravity.y << ',' << gravity.z << ")\n";
#endif

#ifdef BOX2D_INCLUDED
    b2WorldDef def = b2DefaultWorldDef();
    def.gravity    = { 0.0f, gravity.y };
    m_B2World      = StoreB2World(b2CreateWorld(&def));
    m_B2Initialised = true;

    std::cout << "[Physics] Box2D v3 2D world created (gravity.y = " << gravity.y << ")\n";
#endif

    m_Initialised = true;
}

// =============================================================================
// PhysicsSystem::Shutdown
// =============================================================================
void PhysicsSystem::Shutdown()
{
    if (!m_Initialised) return;

#ifdef JOLT_INCLUDED
    m_Jolt.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
#endif

#ifdef BOX2D_INCLUDED
    if (m_B2Initialised)
    {
        b2DestroyWorld(B2World(m_B2World));
        m_B2World       = 0;
        m_B2Initialised = false;
    }
#endif

    m_Initialised   = false;
    m_Accumulator3D = 0.0f;
    m_Accumulator2D = 0.0f;
}

// =============================================================================
// PhysicsSystem::SetGravity
// =============================================================================
void PhysicsSystem::SetGravity(const glm::vec3& g)
{
    m_Gravity = g;
#ifdef JOLT_INCLUDED
    if (m_Jolt)
        m_Jolt->physicsSystem.SetGravity(ToJph(g));
#endif
#ifdef BOX2D_INCLUDED
    if (m_B2Initialised)
        b2World_SetGravity(B2World(m_B2World), { 0.0f, g.y });
#endif
}

// =============================================================================
// PhysicsSystem::Step
// =============================================================================
void PhysicsSystem::Step(float deltaTime)
{
    if (!m_Initialised) return;

    constexpr int kMaxSubsteps = 4;

#ifdef JOLT_INCLUDED
    if (m_Jolt)
    {
        m_Accumulator3D += deltaTime;
        int steps = 0;
        while (m_Accumulator3D >= kFixedStep && steps < kMaxSubsteps)
        {
            m_Jolt->physicsSystem.Update(kFixedStep, 1,
                                         &m_Jolt->tempAlloc,
                                         &m_Jolt->jobSystem);
            m_Accumulator3D -= kFixedStep;
            ++steps;
        }
    }
#endif

#ifdef BOX2D_INCLUDED
    if (m_B2Initialised)
    {
        m_Accumulator2D += deltaTime;
        int steps = 0;
        while (m_Accumulator2D >= kFixedStep && steps < kMaxSubsteps)
        {
            b2World_Step(B2World(m_B2World), kFixedStep, 4);
            m_Accumulator2D -= kFixedStep;
            ++steps;
        }
    }
#endif
}

// =============================================================================
// PhysicsSystem::Create3DBody  (internal)
// =============================================================================
void PhysicsSystem::Create3DBody(entt::registry& registry,
                                 entt::entity    entity,
                                 RigidBody3DComponent& rb,
                                 const TransformComponent& tc)
{
#ifdef JOLT_INCLUDED
    if (!m_Jolt) return;

    glm::vec3 worldPos = tc.Position;
    glm::vec3 worldRot = tc.Rotation;
    if (HasParent(registry, entity))
        DecomposeTRS(Scene::GetWorldMatrix(registry, entity), worldPos, worldRot);

    // Build the shape — prefer box, then sphere, then a tiny unit box as fallback.
    JPH::Ref<JPH::Shape> shape;
    if (registry.all_of<BoxCollider3DComponent>(entity))
    {
        const auto& bc = registry.get<BoxCollider3DComponent>(entity);
        glm::vec3 worldScale(1.0f);
        if (HasParent(registry, entity))
        {
            const glm::mat4 w = Scene::GetWorldMatrix(registry, entity);
            worldScale = glm::vec3(glm::length(glm::vec3(w[0])),
                                   glm::length(glm::vec3(w[1])),
                                   glm::length(glm::vec3(w[2])));
        }
        else
        {
            worldScale = tc.Scale;
        }
        const glm::vec3 half = glm::max(bc.HalfExtents * glm::abs(worldScale),
                                        glm::vec3(0.001f));
        JPH::BoxShapeSettings ss(ToJph(half), 0.0f);
        ss.mDensity     = bc.Density;
        ss.mFriction    = bc.Friction;
        ss.mRestitution = bc.Restitution;
        auto res = ss.Create();
        if (res.IsValid()) shape = res.Get();
    }
    else if (registry.all_of<SphereCollider3DComponent>(entity))
    {
        const auto& sc   = registry.get<SphereCollider3DComponent>(entity);
        const float maxS = HasParent(registry, entity)
            ? glm::compMax(glm::abs(glm::vec3(
                  glm::length(glm::vec3(Scene::GetWorldMatrix(registry, entity)[0])),
                  glm::length(glm::vec3(Scene::GetWorldMatrix(registry, entity)[1])),
                  glm::length(glm::vec3(Scene::GetWorldMatrix(registry, entity)[2])))))
            : glm::compMax(glm::abs(tc.Scale));
        JPH::SphereShapeSettings ss(std::max(sc.Radius * maxS, 0.001f));
        ss.mDensity     = sc.Density;
        ss.mFriction    = sc.Friction;
        ss.mRestitution = sc.Restitution;
        auto res = ss.Create();
        if (res.IsValid()) shape = res.Get();
    }

    if (!shape)
    {
        JPH::BoxShapeSettings ss(JPH::Vec3(0.5f, 0.5f, 0.5f), 0.0f);
        auto res = ss.Create();
        if (res.IsValid()) shape = res.Get();
    }

    JPH::BodyCreationSettings bcs(
        shape,
        JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
        EulerToJphQuat(worldRot),
        JphMotionType(rb.MotionType),
        LayerFor(rb.MotionType));

    bcs.mLinearDamping     = rb.LinearDamping;
    bcs.mAngularDamping    = rb.AngularDamping;
    bcs.mGravityFactor     = rb.GravityFactor;
    if (rb.MotionType == MotionType3D::Dynamic && rb.Mass > 0.0f)
    {
        bcs.mOverrideMassProperties        = JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass  = rb.Mass;
    }

    auto& bi  = m_Jolt->physicsSystem.GetBodyInterface();
    JPH::Body* body = bi.CreateBody(bcs);
    if (body)
    {
        bi.AddBody(body->GetID(), JPH::EActivation::Activate);
        rb.BodyId = body->GetID().GetIndexAndSequenceNumber();
    }
#else
    static_cast<void>(registry);
    static_cast<void>(entity);
    static_cast<void>(rb);
    static_cast<void>(tc);
#endif
}

// =============================================================================
// PhysicsSystem::Destroy3DBody  (internal)
// =============================================================================
void PhysicsSystem::Destroy3DBody(RigidBody3DComponent& rb)
{
#ifdef JOLT_INCLUDED
    if (!m_Jolt) return;
    if (rb.BodyId == 0xFFFFFFFFu) return;

    auto& bi = m_Jolt->physicsSystem.GetBodyInterface();
    const JPH::BodyID id(rb.BodyId);
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    rb.BodyId = 0xFFFFFFFFu;
#else
    static_cast<void>(rb);
#endif
}

// =============================================================================
// PhysicsSystem::Attach3DBoxShape / Attach3DSphereShape
// (not called directly in the new design — shapes baked in Create3DBody —
//  kept for late-attachment from SyncBodies if needed)
// =============================================================================
void PhysicsSystem::Attach3DBoxShape(RigidBody3DComponent&       rb,
                                     const BoxCollider3DComponent& bc,
                                     const glm::vec3&              worldScale)
{
    static_cast<void>(rb);
    static_cast<void>(bc);
    static_cast<void>(worldScale);
    // Jolt bakes shapes into bodies at creation. Rebuild body via Destroy+Create
    // if collider dimensions change. This stub satisfies the header declaration.
}

void PhysicsSystem::Attach3DSphereShape(RigidBody3DComponent&         rb,
                                        const SphereCollider3DComponent& sc,
                                        const glm::vec3&                 worldScale)
{
    static_cast<void>(rb);
    static_cast<void>(sc);
    static_cast<void>(worldScale);
}

// =============================================================================
// PhysicsSystem::Create2DBody  (internal)
// =============================================================================
void PhysicsSystem::Create2DBody(entt::entity           entity,
                                 RigidBody2DComponent&  rb,
                                 const TransformComponent& tc)
{
#ifdef BOX2D_INCLUDED
    if (!m_B2Initialised) return;

    b2BodyDef def    = b2DefaultBodyDef();
    def.type         = B2MotionType(rb.MotionType);
    def.position     = { tc.Position.x, tc.Position.y };
    def.angle        = glm::radians(tc.Rotation.z);
    def.fixedRotation = rb.FixedRotation;
    def.gravityScale = rb.GravityScale;
    def.linearDamping = rb.LinearDamping;
    def.angularDamping = rb.AngularDamping;

    const b2BodyId bodyId = b2CreateBody(B2World(m_B2World), &def);
    rb.BodyId = StoreB2Body(bodyId);

    static_cast<void>(entity);
#else
    static_cast<void>(entity);
    static_cast<void>(rb);
    static_cast<void>(tc);
#endif
}

// =============================================================================
// PhysicsSystem::Destroy2DBody  (internal)
// =============================================================================
void PhysicsSystem::Destroy2DBody(RigidBody2DComponent& rb)
{
#ifdef BOX2D_INCLUDED
    if (B2BodyIsNull(rb.BodyId)) return;
    b2DestroyBody(B2Body(rb.BodyId));
    rb.BodyId = {};
#else
    static_cast<void>(rb);
#endif
}

// =============================================================================
// PhysicsSystem::Attach2DBoxShape  (internal)
// =============================================================================
void PhysicsSystem::Attach2DBoxShape(RigidBody2DComponent& rb,
                                     BoxCollider2DComponent& bc)
{
#ifdef BOX2D_INCLUDED
    if (B2BodyIsNull(rb.BodyId)) return;

    b2ShapeDef sd  = b2DefaultShapeDef();
    sd.friction    = bc.Friction;
    sd.restitution = bc.Restitution;
    sd.density     = bc.Density;

    const b2Polygon box = b2MakeBox(
        std::max(bc.HalfExtents.x, 0.001f),
        std::max(bc.HalfExtents.y, 0.001f));

    bc.ShapeId = StoreB2Shape(
        b2CreatePolygonShape(B2Body(rb.BodyId), &sd, &box));
#else
    static_cast<void>(rb);
    static_cast<void>(bc);
#endif
}

// =============================================================================
// PhysicsSystem::SyncBodies
// =============================================================================
void PhysicsSystem::SyncBodies(entt::registry& registry)
{
    if (!m_Initialised) return;

    // -------------------------------------------------------------------------
    // 3D pass (Jolt)
    // -------------------------------------------------------------------------
    {
        // Implicit static bodies for collider-only entities.
        std::vector<entt::entity> needsBody3D;
        for (auto e : registry.view<BoxCollider3DComponent, TransformComponent>(
                 entt::exclude<RigidBody3DComponent>))
            needsBody3D.push_back(e);
        for (auto e : registry.view<SphereCollider3DComponent, TransformComponent>(
                 entt::exclude<RigidBody3DComponent>))
            needsBody3D.push_back(e);

        for (entt::entity e : needsBody3D)
        {
            if (registry.all_of<RigidBody3DComponent>(e)) continue;
            auto& rb      = registry.emplace<RigidBody3DComponent>(e);
            rb.MotionType = MotionType3D::Static;
            rb.Mass       = 0.0f;
        }

        auto view3 = registry.view<RigidBody3DComponent, TransformComponent>();
        for (auto entity : view3)
        {
            auto& rb = view3.get<RigidBody3DComponent>(entity);
            auto& tc = view3.get<TransformComponent>(entity);

            if (rb.BodyId == 0xFFFFFFFFu)
                Create3DBody(registry, entity, rb, tc);
        }
    }

    // -------------------------------------------------------------------------
    // 2D pass (Box2D v3)
    // -------------------------------------------------------------------------
    {
        std::vector<entt::entity> needsBody2D;
        for (auto e : registry.view<BoxCollider2DComponent, TransformComponent>(
                 entt::exclude<RigidBody2DComponent>))
            needsBody2D.push_back(e);

        for (entt::entity e : needsBody2D)
        {
            if (registry.all_of<RigidBody2DComponent>(e)) continue;
            auto& rb      = registry.emplace<RigidBody2DComponent>(e);
            rb.MotionType = MotionType2D::Static;
        }

        auto view2 = registry.view<RigidBody2DComponent, TransformComponent>();
        for (auto entity : view2)
        {
            auto& rb = view2.get<RigidBody2DComponent>(entity);
            auto& tc = view2.get<TransformComponent>(entity);

            if (B2BodyIsNull(rb.BodyId))
            {
                Create2DBody(entity, rb, tc);

                if (auto* bc = registry.try_get<BoxCollider2DComponent>(entity);
                    bc && B2ShapeIsNull(bc->ShapeId))
                    Attach2DBoxShape(rb, *bc);
            }
            else
            {
                // Late shape attachment.
                if (auto* bc = registry.try_get<BoxCollider2DComponent>(entity);
                    bc && B2ShapeIsNull(bc->ShapeId))
                    Attach2DBoxShape(rb, *bc);
            }
        }
    }
}

// =============================================================================
// PhysicsSystem::ResetHandles
// =============================================================================
void PhysicsSystem::ResetHandles(entt::registry& registry)
{
    for (auto e : registry.view<RigidBody3DComponent>())
        registry.get<RigidBody3DComponent>(e).BodyId = 0xFFFFFFFFu;

    for (auto e : registry.view<RigidBody2DComponent>())
        registry.get<RigidBody2DComponent>(e).BodyId = {};

    for (auto e : registry.view<BoxCollider2DComponent>())
        registry.get<BoxCollider2DComponent>(e).ShapeId = {};

    // BoxCollider3D / SphereCollider3D: no runtime handle to reset (Jolt bakes
    // shapes into the body; body destruction covers the shape).
}

// =============================================================================
// PhysicsSystem::PreStep
// =============================================================================
void PhysicsSystem::PreStep(entt::registry& registry)
{
    if (!m_Initialised) return;

    // 3D
#ifdef JOLT_INCLUDED
    if (m_Jolt)
    {
        auto& bi = m_Jolt->physicsSystem.GetBodyInterface();
        auto view3 = registry.view<RigidBody3DComponent, TransformComponent>();
        for (auto entity : view3)
        {
            auto& rb = view3.get<RigidBody3DComponent>(entity);
            auto& tc = view3.get<TransformComponent>(entity);
            if (!rb.TransformDirty || rb.BodyId == 0xFFFFFFFFu) continue;
            rb.TransformDirty = false;

            glm::vec3 worldPos = tc.Position;
            glm::vec3 worldRot = tc.Rotation;
            if (HasParent(registry, entity))
                DecomposeTRS(Scene::GetWorldMatrix(registry, entity),
                             worldPos, worldRot);

            bi.SetPositionAndRotation(
                JPH::BodyID(rb.BodyId),
                JPH::RVec3(worldPos.x, worldPos.y, worldPos.z),
                EulerToJphQuat(worldRot),
                JPH::EActivation::Activate);
        }
    }
#endif

    // 2D
#ifdef BOX2D_INCLUDED
    if (m_B2Initialised)
    {
        auto view2 = registry.view<RigidBody2DComponent, TransformComponent>();
        for (auto entity : view2)
        {
            auto& rb = view2.get<RigidBody2DComponent>(entity);
            auto& tc = view2.get<TransformComponent>(entity);
            if (!rb.TransformDirty || B2BodyIsNull(rb.BodyId)) continue;
            rb.TransformDirty = false;

            b2Body_SetTransform(B2Body(rb.BodyId),
                                { tc.Position.x, tc.Position.y },
                                b2MakeRot(glm::radians(tc.Rotation.z)));
        }
    }
#endif
}

// =============================================================================
// PhysicsSystem::PostStep
// =============================================================================
void PhysicsSystem::PostStep(entt::registry& registry)
{
    if (!m_Initialised) return;

    // 3D
#ifdef JOLT_INCLUDED
    if (m_Jolt)
    {
        const auto& bi = m_Jolt->physicsSystem.GetBodyInterface();
        auto view3 = registry.view<RigidBody3DComponent, TransformComponent>();
        for (auto entity : view3)
        {
            auto& rb = view3.get<RigidBody3DComponent>(entity);
            auto& tc = view3.get<TransformComponent>(entity);

            if (rb.MotionType == MotionType3D::Static) continue;
            if (rb.BodyId == 0xFFFFFFFFu) continue;

            const JPH::BodyID id(rb.BodyId);
            JPH::RVec3        jPos;
            JPH::Quat         jRot;
            bi.GetPositionAndRotation(id, jPos, jRot);

            const glm::vec3 worldPos(jPos.GetX(), jPos.GetY(), jPos.GetZ());
            const glm::vec3 worldRot = JphQuatToEuler(jRot);

            if (HasParent(registry, entity))
            {
                const auto& rel = registry.get<RelationshipComponent>(entity);
                const glm::mat4 parentWorld =
                    Scene::GetWorldMatrix(registry, rel.Parent);
                const glm::mat4 bodyWorld =
                    glm::translate(glm::mat4(1.0f), worldPos)
                    * glm::mat4_cast(glm::quat(glm::radians(worldRot)))
                    * glm::scale(glm::mat4(1.0f), tc.Scale);
                glm::vec3 localPos, localRot;
                DecomposeTRS(glm::inverse(parentWorld) * bodyWorld, localPos, localRot);
                tc.Position = localPos;
                tc.Rotation = localRot;
            }
            else
            {
                tc.Position = worldPos;
                tc.Rotation = worldRot;
            }

            rb.LinearVelocity  = FromJph(bi.GetLinearVelocity(id));
            rb.AngularVelocity = FromJph(bi.GetAngularVelocity(id));
        }
    }
#endif

    // 2D
#ifdef BOX2D_INCLUDED
    if (m_B2Initialised)
    {
        auto view2 = registry.view<RigidBody2DComponent, TransformComponent>();
        for (auto entity : view2)
        {
            auto& rb = view2.get<RigidBody2DComponent>(entity);
            auto& tc = view2.get<TransformComponent>(entity);

            if (rb.MotionType == MotionType2D::Static) continue;
            if (B2BodyIsNull(rb.BodyId)) continue;

            const b2BodyId bid = B2Body(rb.BodyId);
            const b2Vec2   pos = b2Body_GetPosition(bid);
            const float    ang = b2Rot_GetAngle(b2Body_GetRotation(bid));
            const b2Vec2   lv  = b2Body_GetLinearVelocity(bid);

            tc.Position.x  = pos.x;
            tc.Position.y  = pos.y;
            tc.Rotation.z  = glm::degrees(ang);

            rb.LinVelX = lv.x;
            rb.LinVelY = lv.y;
            rb.AngVel  = b2Body_GetAngularVelocity(bid);
        }
    }
#endif
}

// =============================================================================
// PhysicsSystem::SerializeState
// =============================================================================
void PhysicsSystem::SerializeState(entt::registry& registry,
                                   const std::string& path) const
{
    if (!m_Initialised) return;

    json root = json::array();
    for (auto entity : registry.view<RigidBody3DComponent>())
    {
        const auto& rb = registry.get<RigidBody3DComponent>(entity);
        json je;
        je["id"]       = static_cast<uint32_t>(entt::to_integral(entity));
        je["bodyType"] = static_cast<int>(rb.MotionType);
        je["linVel"]   = { rb.LinearVelocity.x, rb.LinearVelocity.y, rb.LinearVelocity.z };
        je["angVel"]   = { rb.AngularVelocity.x, rb.AngularVelocity.y, rb.AngularVelocity.z };
        root.push_back(std::move(je));
    }

    std::error_code ec;
    fs::create_directories("bin", ec);
    std::ofstream out(path);
    if (out.is_open())
        out << root.dump(2) << '\n';
}

// =============================================================================
// PhysicsSystem::DeserializeState
// =============================================================================
void PhysicsSystem::DeserializeState(entt::registry& registry,
                                     const std::string& path)
{
    if (!m_Initialised) return;

    std::ifstream in(path);
    if (!in.is_open()) return;

    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_array()) return;

    for (const auto& je : root)
    {
        if (!je.is_object() || !je.contains("id")) continue;

        const entt::entity handle =
            static_cast<entt::entity>(je["id"].get<uint32_t>());
        if (!registry.valid(handle)) continue;

        auto* rb = registry.try_get<RigidBody3DComponent>(handle);
        if (!rb) continue;

        auto readVec = [&](const char* key) -> glm::vec3
        {
            if (!je.contains(key) || !je[key].is_array() || je[key].size() < 3)
                return glm::vec3(0.0f);
            return glm::vec3(je[key][0].get<float>(),
                             je[key][1].get<float>(),
                             je[key][2].get<float>());
        };

        rb->LinearVelocity  = readVec("linVel");
        rb->AngularVelocity = readVec("angVel");

#ifdef JOLT_INCLUDED
        if (m_Jolt && rb->BodyId != 0xFFFFFFFFu)
        {
            auto& bi = m_Jolt->physicsSystem.GetBodyInterface();
            bi.SetLinearVelocity(JPH::BodyID(rb->BodyId), ToJph(rb->LinearVelocity));
            bi.SetAngularVelocity(JPH::BodyID(rb->BodyId), ToJph(rb->AngularVelocity));
        }
#endif
    }
}
