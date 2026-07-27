#pragma once

#include <cstdint>
#include <glm/glm.hpp>

// =============================================================================
// Fadix Engine — Physics ECS Components (Jolt 3D + Box2D v3 2D)
// =============================================================================
//
// All Jolt and Box2D library headers are confined to PhysicsSystem.cpp.
// Components store opaque POD values only:
//   - Jolt  BodyID  → uint32_t   (JPH::BodyID is a uint32_t wrapper;
//                                  0xFFFFFFFF == cInvalidBodyID)
//   - Box2D handles → Fadix2DBodyId / Fadix2DShapeId  (byte-identical POD
//                     clones of b2BodyId / b2ShapeId from Box2D v3)
//
// PhysicsSystem.cpp casts between these and the real library types.
// =============================================================================

// ---------------------------------------------------------------------------
// Motion type enums — shared naming convention for 3D and 2D
// ---------------------------------------------------------------------------
enum class MotionType3D : int
{
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

enum class MotionType2D : int
{
    Static    = 0,
    Kinematic = 1,
    Dynamic   = 2,
};

// ---------------------------------------------------------------------------
// Opaque Box2D v3 handle clones
// b2BodyId / b2ShapeId layout in Box2D v3: { int32_t index1, uint16_t world0,
// uint16_t revision } — 8 bytes total, identical to these structs.
// ---------------------------------------------------------------------------
struct Fadix2DBodyId
{
    int32_t  index1   = 0;
    uint16_t world0   = 0;
    uint16_t revision = 0;
};

struct Fadix2DShapeId
{
    int32_t  index1   = 0;
    uint16_t world0   = 0;
    uint16_t revision = 0;
};

// ---------------------------------------------------------------------------
// RigidBody3DComponent  (Jolt Physics)
// ---------------------------------------------------------------------------
struct RigidBody3DComponent
{
    uint32_t     BodyId         = 0xFFFFFFFFu; // JPH::BodyID::cInvalidBodyID
    MotionType3D MotionType     = MotionType3D::Dynamic;
    float        Mass           = 1.0f;
    float        Friction       = 0.6f;
    float        Restitution    = 0.0f;
    float        GravityFactor  = 1.0f;
    float        LinearDamping  = 0.05f;
    float        AngularDamping = 0.05f;

    // Runtime snapshots written by PostStep; read by hot-reload serialiser.
    glm::vec3    LinearVelocity  = glm::vec3(0.0f);
    glm::vec3    AngularVelocity = glm::vec3(0.0f);

    // Set by Inspector/gizmo so PreStep can push the new pose into Jolt.
    bool         TransformDirty  = false;
};

// ---------------------------------------------------------------------------
// BoxCollider3DComponent  (Jolt Physics)
// Shapes are baked into the body at creation — no per-shape runtime handle.
// ---------------------------------------------------------------------------
struct BoxCollider3DComponent
{
    glm::vec3 HalfExtents = glm::vec3(0.5f);
    float     Friction    = 0.6f;
    float     Restitution = 0.0f;
    float     Density     = 1.0f;
};

// ---------------------------------------------------------------------------
// SphereCollider3DComponent  (Jolt Physics)
// ---------------------------------------------------------------------------
struct SphereCollider3DComponent
{
    float Radius      = 0.5f;
    float Friction    = 0.6f;
    float Restitution = 0.0f;
    float Density     = 1.0f;
};

// ---------------------------------------------------------------------------
// RigidBody2DComponent  (Box2D v3)
// ---------------------------------------------------------------------------
struct RigidBody2DComponent
{
    Fadix2DBodyId BodyId         = {};
    MotionType2D  MotionType     = MotionType2D::Dynamic;
    bool          FixedRotation  = false;
    float         GravityScale   = 1.0f;
    float         LinearDamping  = 0.0f;
    float         AngularDamping = 0.01f;

    // Runtime snapshots (written by PostStep).
    float         LinVelX = 0.0f;
    float         LinVelY = 0.0f;
    float         AngVel  = 0.0f;

    bool          TransformDirty = false;
};

// ---------------------------------------------------------------------------
// BoxCollider2DComponent  (Box2D v3)
// ---------------------------------------------------------------------------
struct BoxCollider2DComponent
{
    Fadix2DShapeId ShapeId     = {};
    glm::vec2      HalfExtents = glm::vec2(0.5f);
    float          Friction    = 0.6f;
    float          Restitution = 0.0f;
    float          Density     = 1.0f;
};
