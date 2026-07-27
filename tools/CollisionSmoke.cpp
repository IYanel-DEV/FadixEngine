#include "physics/PhysicsWorld.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void Check(const bool condition, const std::string& message)
{
    std::cout << (condition ? "  ok   " : "  FAIL ") << message << '\n';
    failures += condition ? 0 : 1;
}

void Step(fadix::PhysicsWorld& physics)
{
    constexpr float dt = 1.0F / 60.0F;
    for (int frame = 0; frame < 240; ++frame)
    {
        physics.StepFixed(dt);
    }
}

void AutoPrimitiveCollision()
{
    using namespace fadix;
    World world{false};
    const entt::entity floor = world.Create();
    TransformComponent floorTransform;
    floorTransform.Scale = {10.0F, 1.0F, 10.0F};
    world.Registry().emplace<TransformComponent>(floor, floorTransform);
    world.Registry().emplace<MeshComponent>(floor, MeshComponent{MeshKind::Plane});
    JoltBodyComponent floorBody;
    floorBody.Dynamic = false;
    world.Registry().emplace<JoltBodyComponent>(floor, floorBody);

    const entt::entity sphere = world.Create();
    world.Registry().emplace<TransformComponent>(sphere, glm::vec3{0.0F, 5.0F, 0.0F});
    world.Registry().emplace<MeshComponent>(sphere, MeshComponent{MeshKind::Sphere});
    world.Registry().emplace<JoltBodyComponent>(sphere, JoltBodyComponent{});

    PhysicsWorld physics;
    physics.SyncFromWorld(world);
    Step(physics);
    physics.SyncToWorld(world);
    const float y = world.Registry().get<TransformComponent>(sphere).Position.y;
    Check(y > 0.45F && y < 0.65F,
        "Auto sphere collides with the scaled thin plane (y=" + std::to_string(y) + ")");
}

void ExactMeshCollision()
{
    using namespace fadix;
    World world{false};
    const entt::entity floor = world.Create();
    world.Registry().emplace<TransformComponent>(floor);
    const entt::entity sphere = world.Create();
    world.Registry().emplace<TransformComponent>(sphere, glm::vec3{0.0F, 5.0F, 0.0F});

    const std::array vertices{
        glm::vec3{-5.0F, 0.0F, -5.0F}, glm::vec3{-5.0F, 0.0F, 5.0F},
        glm::vec3{5.0F, 0.0F, 5.0F}, glm::vec3{5.0F, 0.0F, -5.0F}};
    const std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};

    PhysicsWorld physics;
    Body3DDesc mesh;
    mesh.Entity = world.GetUuid(floor).value();
    mesh.Shape = ColliderShape3D::Mesh;
    mesh.MeshVertices = vertices;
    mesh.MeshIndices = indices;
    mesh.Dynamic = false;
    Check(physics.CreateBody3D(mesh) != InvalidPhysicsBody, "Static triangle mesh collider created");

    Body3DDesc ball;
    ball.Entity = world.GetUuid(sphere).value();
    ball.Position = {0.0F, 5.0F, 0.0F};
    ball.Shape = ColliderShape3D::Sphere;
    Check(physics.CreateBody3D(ball) != InvalidPhysicsBody, "Dynamic sphere collider created");

    Step(physics);
    physics.SyncToWorld(world);
    const float y = world.Registry().get<TransformComponent>(sphere).Position.y;
    Check(y > 0.40F && y < 0.65F,
        "Sphere collides with exact triangle mesh (y=" + std::to_string(y) + ")");
}

void MassAffectsCollisionResponse()
{
    using namespace fadix;
    World world{false};
    const entt::entity lightEntity = world.Create();
    const entt::entity heavyEntity = world.Create();
    world.Registry().emplace<TransformComponent>(lightEntity, glm::vec3{-0.45F, 5.0F, 0.0F});
    world.Registry().emplace<TransformComponent>(heavyEntity, glm::vec3{0.45F, 5.0F, 0.0F});

    PhysicsWorld physics;
    Body3DDesc light;
    light.Entity = world.GetUuid(lightEntity).value();
    light.Position = {-0.45F, 5.0F, 0.0F};
    light.Shape = ColliderShape3D::Sphere;
    light.Mass = 1.0F;
    Check(physics.CreateBody3D(light) != InvalidPhysicsBody, "Light rigid body created");

    Body3DDesc heavy = light;
    heavy.Entity = world.GetUuid(heavyEntity).value();
    heavy.Position = {0.45F, 5.0F, 0.0F};
    heavy.Mass = 100.0F;
    Check(physics.CreateBody3D(heavy) != InvalidPhysicsBody, "Heavy rigid body created");

    for (int frame = 0; frame < 30; ++frame)
    {
        physics.StepFixed(1.0F / 60.0F);
    }
    physics.SyncToWorld(world);
    const float lightMovement =
        std::abs(world.Registry().get<TransformComponent>(lightEntity).Position.x + 0.45F);
    const float heavyMovement =
        std::abs(world.Registry().get<TransformComponent>(heavyEntity).Position.x - 0.45F);
    Check(lightMovement > heavyMovement * 10.0F,
        "Light body moves more than heavy body after contact (light=" +
            std::to_string(lightMovement) + ", heavy=" + std::to_string(heavyMovement) + ")");
}

float RampSlideDistance(const float friction)
{
    using namespace fadix;
    constexpr float angle = 12.0F * 3.14159265358979323846F / 180.0F;
    const glm::quat rotation{std::cos(angle * 0.5F), 0.0F, 0.0F, std::sin(angle * 0.5F)};
    const glm::vec3 normal{-std::sin(angle), std::cos(angle), 0.0F};
    const glm::vec3 start = normal * 0.61F;

    World world{false};
    const entt::entity rampEntity = world.Create();
    const entt::entity boxEntity = world.Create();
    world.Registry().emplace<TransformComponent>(rampEntity);
    world.Registry().emplace<TransformComponent>(boxEntity, start, rotation);

    PhysicsWorld physics;
    Body3DDesc ramp;
    ramp.Entity = world.GetUuid(rampEntity).value();
    ramp.Rotation = rotation;
    ramp.HalfExtent = {5.0F, 0.1F, 2.0F};
    ramp.Friction = friction;
    ramp.Dynamic = false;
    static_cast<void>(physics.CreateBody3D(ramp));

    Body3DDesc box;
    box.Entity = world.GetUuid(boxEntity).value();
    box.Position = start;
    box.Rotation = rotation;
    box.HalfExtent = {0.5F, 0.5F, 0.5F};
    box.Friction = friction;
    static_cast<void>(physics.CreateBody3D(box));

    for (int frame = 0; frame < 180; ++frame)
    {
        physics.StepFixed(1.0F / 60.0F);
    }
    physics.SyncToWorld(world);
    return std::abs(world.Registry().get<TransformComponent>(boxEntity).Position.x - start.x);
}

void FrictionControlsSliding()
{
    const float normalSlide = RampSlideDistance(0.8F);
    const float iceSlide = RampSlideDistance(0.0F);
    Check(iceSlide > normalSlide + 1.0F,
        "Ice friction slides farther than the normal default (normal=" +
            std::to_string(normalSlide) + ", ice=" + std::to_string(iceSlide) + ")");
}

void CharacterControllerCollision()
{
    using namespace fadix;
    constexpr float dt = 1.0F / 60.0F;
    World world{false};

    const entt::entity floor = world.Create();
    TransformComponent floorTransform;
    floorTransform.Scale = {10.0F, 1.0F, 10.0F};
    world.Registry().emplace<TransformComponent>(floor, floorTransform);
    world.Registry().emplace<MeshComponent>(floor, MeshComponent{MeshKind::Plane});
    JoltBodyComponent staticBody;
    staticBody.Dynamic = false;
    world.Registry().emplace<JoltBodyComponent>(floor, staticBody);

    const entt::entity wall = world.Create();
    TransformComponent wallTransform;
    wallTransform.Position = {2.0F, 1.0F, 0.0F};
    wallTransform.Scale = {1.0F, 2.0F, 6.0F};
    world.Registry().emplace<TransformComponent>(wall, wallTransform);
    world.Registry().emplace<MeshComponent>(wall, MeshComponent{MeshKind::Cube});
    world.Registry().emplace<JoltBodyComponent>(wall, staticBody);

    const entt::entity player = world.Create();
    world.Registry().emplace<TransformComponent>(player, glm::vec3{0.0F, 3.0F, 0.0F});
    world.Registry().emplace<CharacterControllerComponent>(player);

    PhysicsWorld physics;
    for (int frame = 0; frame < 120; ++frame)
    {
        physics.SyncFromWorld(world);
        physics.StepFixed(dt);
        physics.SyncToWorld(world);
    }
    Check(world.Registry().get<CharacterControllerComponent>(player).Grounded,
        "Character falls onto the floor and reports grounded");

    for (int frame = 0; frame < 6; ++frame)
    {
        world.Registry().get<CharacterControllerComponent>(player).MoveInput = {1.0F, 0.0F};
        physics.SyncFromWorld(world);
        physics.StepFixed(dt);
        physics.SyncToWorld(world);
    }
    Check(world.Registry().get<TransformComponent>(player).Position.x > 0.35F,
        "Character movement input drives the controller");
    for (int frame = 0; frame < 234; ++frame)
    {
        // Compatibility with existing PlayerController scripts that move the
        // Transform directly: PhysicsWorld turns the requested delta into a sweep.
        world.Registry().get<TransformComponent>(player).Position.x += 5.0F * dt;
        physics.SyncFromWorld(world);
        physics.StepFixed(dt);
        physics.SyncToWorld(world);
    }
    const TransformComponent& stopped = world.Registry().get<TransformComponent>(player);
    Check(stopped.Position.x > 0.35F && stopped.Position.x < 1.1F,
        "Character stays blocked by a wall (x=" + std::to_string(stopped.Position.x) + ")");
    Check(std::abs(stopped.Position.y - 1.02F) < 0.12F,
        "Character remains upright on the floor (y=" + std::to_string(stopped.Position.y) + ")");
    Check(world.Registry().get<CharacterControllerComponent>(player).Grounded,
        "Character remains grounded against a wall");

    world.Registry().get<CharacterControllerComponent>(player).JumpRequested = true;
    physics.SyncFromWorld(world);
    physics.StepFixed(dt);
    physics.SyncToWorld(world);
    for (int frame = 0; frame < 12; ++frame)
    {
        physics.SyncFromWorld(world);
        physics.StepFixed(dt);
        physics.SyncToWorld(world);
    }
    Check(world.Registry().get<TransformComponent>(player).Position.y > 1.25F,
        "Grounded character can jump");
}
}

int main()
{
    std::cout << "Fadix collision smoke\n";
    AutoPrimitiveCollision();
    ExactMeshCollision();
    MassAffectsCollisionResponse();
    FrictionControlsSliding();
    CharacterControllerCollision();
    return failures == 0 ? 0 : 1;
}
