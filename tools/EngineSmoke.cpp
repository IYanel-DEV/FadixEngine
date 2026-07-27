// Headless smoke tests for the pieces that cannot be verified by clicking in
// the editor: transactional scene save/load (Subtask 1) and physics collider
// fidelity (Subtask 3). Exits non-zero if any check fails.

#include "editor/command/EntityCommands.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef FADIX_ENABLE_PHYSICS
#include "physics/PhysicsWorld.hpp"
#endif

#include "assets/MaterialAssetJson.hpp"
#include "assets/ScriptDatabase.hpp"
#include "assets/TextureProcessor.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

namespace
{
int g_Failures = 0;

void Check(const bool condition, const std::string& label)
{
    if (condition)
    {
        std::cout << "  ok   " << label << '\n';
    }
    else
    {
        std::cerr << "  FAIL " << label << '\n';
        ++g_Failures;
    }
}

std::size_t EntityCount(const fadix::IWorld& world)
{
    std::size_t count = 0;
    for (const auto entity : world.Registry().view<const fadix::UuidComponent>())
    {
        static_cast<void>(entity);
        ++count;
    }
    return count;
}

std::filesystem::path TempScene(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

void SceneTests()
{
    using namespace fadix;
    std::cout << "[scene] transactional save/load\n";

    // Build an authored world with a couple of entities.
    World authored{false};
    const entt::entity alpha = authored.Create();
    authored.Registry().emplace<NameComponent>(alpha, "Alpha");
    authored.Registry().emplace<TransformComponent>(alpha, glm::vec3{1.0F, 2.0F, 3.0F});
    authored.Registry().emplace<MeshComponent>(alpha, MeshKind::Cube, glm::vec3{0.2F, 0.4F, 0.8F}, 0.3F, 0.7F);
    authored.Registry().emplace<DirectionalLightComponent>(alpha, glm::vec3{1.0F, 0.9F, 0.7F}, 4.0F);
    const Uuid alphaId = authored.GetUuid(alpha).value();

    const entt::entity beta = authored.Create();
    authored.Registry().emplace<NameComponent>(beta, "Beta");
    authored.Registry().emplace<TransformComponent>(beta);
    JoltBodyComponent body;
    body.HalfExtent = glm::vec3{2.0F, 3.0F, 4.0F};
    body.Shape = ColliderShape3D::Capsule;
    body.AutoFit = false;
    body.Dynamic = false;
    authored.Registry().emplace<JoltBodyComponent>(beta, body);

    SceneService service;
    const auto scenePath = TempScene("fadix_smoke_roundtrip.scene");
    SceneDocument saveDoc;
    Check(static_cast<bool>(service.SaveAs(saveDoc, authored, scenePath)), "save scene");
    Check(std::filesystem::exists(scenePath), "scene file exists after save");
    Check(!std::filesystem::exists(scenePath.string() + ".tmp"), "no leftover .tmp after save");

    // Round-trip into a fresh world.
    World loaded{false};
    SceneDocument loadDoc;
    const auto loadResult = service.Load(loadDoc, loaded, scenePath);
    Check(static_cast<bool>(loadResult), std::string{"load scene ("} + loadResult.ErrorMessage() + ")");
    Check(EntityCount(loaded) == 2, "round-trip entity count matches");
    if (const auto found = loaded.Find(alphaId))
    {
        const auto* transform = loaded.Registry().try_get<TransformComponent>(*found);
        Check(transform != nullptr && std::abs(transform->Position.y - 2.0F) < 1e-4F,
            "round-trip transform preserved");
        const auto* mesh = loaded.Registry().try_get<MeshComponent>(*found);
        Check(mesh != nullptr && std::abs(mesh->BaseColor.z - 0.8F) < 1e-4F &&
                std::abs(mesh->Roughness - 0.7F) < 1e-4F,
            "round-trip mesh material preserved");
        const auto* light = loaded.Registry().try_get<DirectionalLightComponent>(*found);
        Check(light != nullptr && std::abs(light->Intensity - 4.0F) < 1e-4F,
            "round-trip directional light preserved");
    }
    else
    {
        Check(false, "round-trip preserved entity UUID");
    }
    if (const auto found = loaded.Find(authored.GetUuid(beta).value()))
    {
        const auto* jolt = loaded.Registry().try_get<JoltBodyComponent>(*found);
        Check(jolt != nullptr && std::abs(jolt->HalfExtent.z - 4.0F) < 1e-4F &&
                jolt->Shape == ColliderShape3D::Capsule && !jolt->AutoFit,
            "round-trip collider shape and fit settings preserved");
    }

    // Malformed scene must be rejected AND must not disturb the live world.
    World live{false};
    static_cast<void>(live.Create());
    static_cast<void>(live.Create());
    const std::size_t before = EntityCount(live);
    const auto badPath = TempScene("fadix_smoke_bad.scene");
    {
        std::ofstream bad{badPath, std::ios::trunc};
        bad << "FADIX_SCENE 1\nENTITY not-a-valid-uuid \"Broken\" 1 END\n";
    }
    SceneDocument badDoc;
    const auto badResult = service.Load(badDoc, live, badPath);
    Check(!static_cast<bool>(badResult), "malformed scene rejected");
    Check(EntityCount(live) == before, "live world preserved after malformed load");

    // Overwriting an existing scene keeps content correct and leaves no temp.
    World second{false};
    static_cast<void>(second.Create());
    static_cast<void>(second.Create());
    static_cast<void>(second.Create());
    SceneDocument overwriteDoc;
    Check(static_cast<bool>(service.SaveAs(overwriteDoc, second, scenePath)), "overwrite existing scene");
    Check(!std::filesystem::exists(scenePath.string() + ".tmp"), "no leftover .tmp after overwrite");
    World reloaded{false};
    SceneDocument reloadDoc;
    Check(static_cast<bool>(service.Load(reloadDoc, reloaded, scenePath)), "reload overwritten scene");
    Check(EntityCount(reloaded) == 3, "overwrite content replaced atomically");

    std::error_code ec;
    std::filesystem::remove(scenePath, ec);
    std::filesystem::remove(badPath, ec);
}

bool NearlyEqual(const float a, const float b) { return std::abs(a - b) < 1e-4F; }
bool NearlyEqual(const glm::vec3 a, const glm::vec3 b)
{
    return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y) && NearlyEqual(a.z, b.z);
}

bool NearlyEqual(const glm::vec2 a, const glm::vec2 b)
{
    return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y);
}

void NewComponentRoundTripTests()
{
    using namespace fadix;
    std::cout << "[components] point/spot/environment/visibility round-trip\n";

    World authored{false};
    const entt::entity point = authored.Create();
    authored.Registry().emplace<NameComponent>(point, "Point");
    authored.Registry().emplace<TransformComponent>(point, glm::vec3{1.0F, 2.0F, 3.0F});
    PointLightComponent pointLight;
    pointLight.Color = {0.9F, 0.3F, 0.2F};
    pointLight.Intensity = 25.0F;
    pointLight.Range = 7.5F;
    pointLight.FalloffExponent = 3.0F;
    pointLight.Enabled = false;
    authored.Registry().emplace<PointLightComponent>(point, pointLight);

    const entt::entity spot = authored.Create();
    authored.Registry().emplace<NameComponent>(spot, "Spot");
    authored.Registry().emplace<TransformComponent>(spot);
    SpotLightComponent spotLight;
    spotLight.Color = {0.2F, 0.6F, 1.0F};
    spotLight.Intensity = 40.0F;
    spotLight.Range = 22.0F;
    spotLight.InnerConeDegrees = 12.0F;
    spotLight.OuterConeDegrees = 44.0F;
    spotLight.FalloffExponent = 1.5F;
    authored.Registry().emplace<SpotLightComponent>(spot, spotLight);

    const entt::entity env = authored.Create();
    authored.Registry().emplace<NameComponent>(env, "Environment");
    authored.Registry().emplace<TransformComponent>(env);
    EnvironmentComponent environment;
    environment.SkyZenithColor = {0.1F, 0.2F, 0.3F};
    environment.SkyHorizonColor = {0.4F, 0.5F, 0.6F};
    environment.GroundColor = {0.15F, 0.14F, 0.13F};
    environment.AmbientColor = {0.3F, 0.4F, 0.5F};
    environment.AmbientIntensity = 1.4F;
    environment.Exposure = 1.8F;
    environment.FogEnabled = true;
    environment.FogColor = {0.7F, 0.7F, 0.8F};
    environment.FogDensity = 0.05F;
    environment.FogStart = 12.0F;
    environment.FogEnd = 90.0F;
    environment.Primary = true;
    environment.Priority = 3;
    authored.Registry().emplace<EnvironmentComponent>(env, environment);

    const entt::entity hidden = authored.Create();
    authored.Registry().emplace<NameComponent>(hidden, "Hidden");
    authored.Registry().emplace<TransformComponent>(hidden);
    authored.Registry().emplace<VisibilityComponent>(hidden, false, true, false);

    SceneService service;
    const auto path = TempScene("fadix_smoke_new_components.scene");
    SceneDocument saveDoc;
    Check(static_cast<bool>(service.SaveAs(saveDoc, authored, path)), "save new-component scene");

    World loaded{false};
    SceneDocument loadDoc;
    Check(static_cast<bool>(service.Load(loadDoc, loaded, path)), "load new-component scene");
    if (const auto found = loaded.Find(authored.GetUuid(point).value()))
    {
        const auto* value = loaded.Registry().try_get<PointLightComponent>(*found);
        Check(value != nullptr && NearlyEqual(value->Color, pointLight.Color) &&
                NearlyEqual(value->Intensity, 25.0F) && NearlyEqual(value->Range, 7.5F) &&
                NearlyEqual(value->FalloffExponent, 3.0F) && !value->Enabled,
            "point light round-trips every field");
    }
    else
    {
        Check(false, "point light entity preserved");
    }
    if (const auto found = loaded.Find(authored.GetUuid(spot).value()))
    {
        const auto* value = loaded.Registry().try_get<SpotLightComponent>(*found);
        Check(value != nullptr && NearlyEqual(value->Color, spotLight.Color) &&
                NearlyEqual(value->Intensity, 40.0F) && NearlyEqual(value->Range, 22.0F) &&
                NearlyEqual(value->InnerConeDegrees, 12.0F) &&
                NearlyEqual(value->OuterConeDegrees, 44.0F) &&
                NearlyEqual(value->FalloffExponent, 1.5F) && value->Enabled,
            "spot light round-trips every field");
    }
    else
    {
        Check(false, "spot light entity preserved");
    }
    if (const auto found = loaded.Find(authored.GetUuid(env).value()))
    {
        const auto* value = loaded.Registry().try_get<EnvironmentComponent>(*found);
        Check(value != nullptr && NearlyEqual(value->SkyZenithColor, environment.SkyZenithColor) &&
                NearlyEqual(value->SkyHorizonColor, environment.SkyHorizonColor) &&
                NearlyEqual(value->GroundColor, environment.GroundColor) &&
                NearlyEqual(value->AmbientColor, environment.AmbientColor) &&
                NearlyEqual(value->AmbientIntensity, 1.4F) && NearlyEqual(value->Exposure, 1.8F) &&
                value->FogEnabled && NearlyEqual(value->FogColor, environment.FogColor) &&
                NearlyEqual(value->FogDensity, 0.05F) && NearlyEqual(value->FogStart, 12.0F) &&
                NearlyEqual(value->FogEnd, 90.0F) && value->Primary && value->Priority == 3,
            "environment round-trips every field");
    }
    else
    {
        Check(false, "environment entity preserved");
    }
    if (const auto found = loaded.Find(authored.GetUuid(hidden).value()))
    {
        const auto* value = loaded.Registry().try_get<VisibilityComponent>(*found);
        Check(value != nullptr && !value->VisibleInEditor && value->VisibleInGame &&
                !value->Selectable,
            "visibility round-trips every field");
    }
    else
    {
        Check(false, "visibility entity preserved");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void MeshKindTests()
{
    using namespace fadix;
    std::cout << "[components] mesh kinds\n";

    World authored{false};
    std::vector<Uuid> ids;
    for (unsigned kind = 0; kind < MeshKindCount; ++kind)
    {
        const entt::entity entity = authored.Create();
        authored.Registry().emplace<NameComponent>(entity, MeshKindName(static_cast<MeshKind>(kind)));
        authored.Registry().emplace<TransformComponent>(entity);
        MeshComponent mesh;
        mesh.Kind = static_cast<MeshKind>(kind);
        authored.Registry().emplace<MeshComponent>(entity, mesh);
        ids.push_back(authored.GetUuid(entity).value());
    }

    SceneService service;
    const auto path = TempScene("fadix_smoke_meshkinds.scene");
    SceneDocument saveDoc;
    Check(static_cast<bool>(service.SaveAs(saveDoc, authored, path)), "save mesh-kind scene");
    World loaded{false};
    SceneDocument loadDoc;
    Check(static_cast<bool>(service.Load(loadDoc, loaded, path)), "load mesh-kind scene");
    bool allMatch = true;
    for (unsigned kind = 0; kind < MeshKindCount; ++kind)
    {
        const auto found = loaded.Find(ids[kind]);
        const auto* mesh = found ? loaded.Registry().try_get<MeshComponent>(*found) : nullptr;
        allMatch = allMatch && mesh != nullptr && mesh->Kind == static_cast<MeshKind>(kind);
    }
    Check(allMatch, "every MeshKind value round-trips");

    // A scene carrying an out-of-range kind must degrade safely to Cube.
    {
        std::ofstream legacy{path, std::ios::trunc};
        legacy << "FADIX_SCENE 1\n"
               << "ENTITY 01234567-89ab-cdef-0123-456789abcdef \"Old\" 1 "
                  "0 0 0 1 0 0 0 1 1 1 - M 99 0.5 0.5 0.5 0.1 0.6 END\n";
    }
    World legacyWorld{false};
    SceneDocument legacyDoc;
    Check(static_cast<bool>(service.Load(legacyDoc, legacyWorld, path)), "unknown mesh kind loads");
    bool fellBack = false;
    for (const auto [entity, mesh] : legacyWorld.Registry().view<const MeshComponent>().each())
    {
        fellBack = mesh.Kind == MeshKind::Cube;
        static_cast<void>(entity);
    }
    Check(fellBack, "unknown mesh kind falls back to Cube");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void SanitizationTests()
{
    using namespace fadix;
    std::cout << "[components] invalid value sanitization\n";

    // A scene with hostile values loads with everything clamped into range.
    SceneService service;
    const auto path = TempScene("fadix_smoke_sanitize.scene");
    {
        std::ofstream bad{path, std::ios::trunc};
        bad << "FADIX_SCENE 1\n"
            << "ENTITY 01234567-89ab-cdef-0123-456789abcdef \"Bad\" 1 "
               "0 0 0 1 0 0 0 1 1 1 - "
               "P 5 -2 0.5 -10 -3 0 1 "
               "S 2 2 2 -1 -5 80 40 -2 1 END\n";
    }
    World loaded{false};
    SceneDocument doc;
    Check(static_cast<bool>(service.Load(doc, loaded, path)), "hostile scene still loads");
    for (const auto [entity, light] : loaded.Registry().view<const PointLightComponent>().each())
    {
        Check(light.Color.x <= 1.0F && light.Color.y >= 0.0F && light.Intensity >= 0.0F &&
                light.Range > 0.0F && light.FalloffExponent > 0.0F,
            "point light values sanitized on load");
        static_cast<void>(entity);
    }
    for (const auto [entity, light] : loaded.Registry().view<const SpotLightComponent>().each())
    {
        Check(light.Intensity >= 0.0F && light.Range > 0.0F &&
                light.OuterConeDegrees > light.InnerConeDegrees &&
                light.OuterConeDegrees < 90.0F && light.InnerConeDegrees >= 0.0F,
            "spot light cone ordering sanitized on load");
        static_cast<void>(entity);
    }

    // Old scene text without any of the new markers must still load.
    {
        std::ofstream old{path, std::ios::trunc};
        old << "FADIX_SCENE 1\n"
            << "ENTITY 00112233-4455-6677-8899-aabbccddeeff \"Legacy\" 1 "
               "1 2 3 1 0 0 0 1 1 1 - M 0 0.6 0.6 0.6 0.1 0.5 L 1 1 1 3 END\n";
    }
    World legacy{false};
    Check(static_cast<bool>(service.Load(doc, legacy, path)), "pre-batch scene still loads");
    Check(EntityCount(legacy) == 1, "pre-batch scene entity restored");

    // Unknown future component records must not corrupt other entities.
    {
        std::ofstream future{path, std::ios::trunc};
        future << "FADIX_SCENE 1\n"
               << "ENTITY 00112233-4455-6677-8899-aabbccddee01 \"Future\" 1 "
                  "0 0 0 1 0 0 0 1 1 1 - Z 1 2 3 END\n"
               << "ENTITY 00112233-4455-6677-8899-aabbccddee02 \"Safe\" 1 "
                  "0 0 0 1 0 0 0 1 1 1 - M 1 0.5 0.5 0.5 0 0.5 END\n";
    }
    World future{false};
    Check(static_cast<bool>(service.Load(doc, future, path)), "unknown component record tolerated");
    Check(EntityCount(future) == 2, "entities after unknown record survive");

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void CloneAndSnapshotTests()
{
    using namespace fadix;
    std::cout << "[world] clone, duplicate, snapshot removal/restore\n";

    World world{false};
    const entt::entity entity = world.Create();
    world.Registry().emplace<NameComponent>(entity, "Lamp");
    world.Registry().emplace<TransformComponent>(entity, glm::vec3{4.0F, 5.0F, 6.0F});
    PointLightComponent pointLight;
    pointLight.Range = 9.0F;
    world.Registry().emplace<PointLightComponent>(entity, pointLight);
    SpotLightComponent spotLight;
    spotLight.OuterConeDegrees = 50.0F;
    world.Registry().emplace<SpotLightComponent>(entity, spotLight);
    EnvironmentComponent environment;
    environment.Priority = 7;
    world.Registry().emplace<EnvironmentComponent>(entity, environment);
    world.Registry().emplace<VisibilityComponent>(entity, true, false, true);
    const Uuid id = world.GetUuid(entity).value();

    // Play-mode staging path uses World::Clone.
    const std::unique_ptr<IWorld> clone = world.Clone();
    const auto clonedEntity = clone->Find(id);
    Check(clonedEntity.has_value(), "clone preserves entity");
    if (clonedEntity)
    {
        const auto& registry = clone->Registry();
        const auto* clonedPoint = registry.try_get<PointLightComponent>(*clonedEntity);
        const auto* clonedSpot = registry.try_get<SpotLightComponent>(*clonedEntity);
        const auto* clonedEnv = registry.try_get<EnvironmentComponent>(*clonedEntity);
        const auto* clonedVisibility = registry.try_get<VisibilityComponent>(*clonedEntity);
        Check(clonedPoint != nullptr && NearlyEqual(clonedPoint->Range, 9.0F) &&
                clonedSpot != nullptr && NearlyEqual(clonedSpot->OuterConeDegrees, 50.0F) &&
                clonedEnv != nullptr && clonedEnv->Priority == 7 &&
                clonedVisibility != nullptr && !clonedVisibility->VisibleInGame,
            "clone copies all new components");
    }
    // Mutating the clone must never touch the authored world.
    if (clonedEntity)
    {
        clone->Registry().get<PointLightComponent>(*clonedEntity).Range = 1.0F;
        Check(NearlyEqual(world.Registry().get<PointLightComponent>(entity).Range, 9.0F),
            "authored world untouched after clone edit");
    }

    // Duplicate preserves the new components.
    DuplicateEntityCommand duplicate{world, id};
    duplicate.Execute();
    const auto duplicated = world.Find(duplicate.DuplicateId());
    Check(duplicated.has_value(), "duplicate created");
    if (duplicated)
    {
        const auto* dupPoint = world.Registry().try_get<PointLightComponent>(*duplicated);
        const auto* dupVisibility = world.Registry().try_get<VisibilityComponent>(*duplicated);
        Check(dupPoint != nullptr && NearlyEqual(dupPoint->Range, 9.0F) &&
                dupVisibility != nullptr && !dupVisibility->VisibleInGame,
            "duplicate copies new components");
    }
    duplicate.Undo();
    Check(!world.Find(duplicate.DuplicateId()).has_value(), "duplicate undo removes copy");

    // Component removal + undo through the snapshot command.
    const auto before = EntitySnapshot::Capture(world, id);
    world.Registry().remove<PointLightComponent>(entity);
    world.Registry().remove<EnvironmentComponent>(entity);
    const auto after = EntitySnapshot::Capture(world, id);
    Check(before.has_value() && after.has_value(), "snapshots captured");
    if (before && after)
    {
        SnapshotEntityCommand command{world, *before, *after, "Remove Component"};
        command.Undo();
        Check(world.Registry().all_of<PointLightComponent>(entity) &&
                world.Registry().all_of<EnvironmentComponent>(entity),
            "undo restores removed components");
        command.Execute();
        Check(!world.Registry().all_of<PointLightComponent>(entity) &&
                !world.Registry().all_of<EnvironmentComponent>(entity),
            "redo removes components again");
    }
}

#ifdef FADIX_ENABLE_PHYSICS
float ZAngle(const glm::quat& q)
{
    const glm::vec3 rotated = q * glm::vec3{1.0F, 0.0F, 0.0F};
    return std::atan2(rotated.y, rotated.x);
}

void StepFor(fadix::PhysicsWorld& physics, const float seconds)
{
    constexpr float dt = 1.0F / 60.0F;
    for (float t = 0.0F; t < seconds; t += dt)
    {
        physics.StepFixed(dt);
    }
}

void Physics3DTest()
{
    using namespace fadix;
    std::cout << "[physics] Jolt collider size\n";
    World world{false};
    // Wide, tall static floor: its top surface is at y = 1.0.
    const entt::entity floor = world.Create();
    world.Registry().emplace<TransformComponent>(floor, glm::vec3{0.0F, 0.0F, 0.0F});
    JoltBodyComponent floorBody;
    floorBody.HalfExtent = glm::vec3{5.0F, 1.0F, 5.0F};
    floorBody.Dynamic = false;
    world.Registry().emplace<JoltBodyComponent>(floor, floorBody);
    // Small dynamic box dropped from above.
    const entt::entity box = world.Create();
    world.Registry().emplace<TransformComponent>(box, glm::vec3{0.0F, 5.0F, 0.0F});
    JoltBodyComponent boxBody;
    boxBody.HalfExtent = glm::vec3{0.5F, 0.5F, 0.5F};
    boxBody.Dynamic = true;
    world.Registry().emplace<JoltBodyComponent>(box, boxBody);

    PhysicsWorld physics;
    physics.SyncFromWorld(world);
    StepFor(physics, 4.0F);
    physics.SyncToWorld(world);

    const float restY = world.Registry().get<TransformComponent>(box).Position.y;
    // Floor top (1.0) + box half extent (0.5) = 1.5. If HalfExtent were ignored
    // (all boxes 0.5), the box would rest near y = 1.0 instead.
    Check(restY > 1.3F && restY < 1.7F,
        "3D box rests on sized floor (y=" + std::to_string(restY) + ", expected ~1.5)");
}

void Physics2DTest()
{
    using namespace fadix;
    std::cout << "[physics] Box2D collider size + rotation\n";
    {
        World world{false};
        const entt::entity ground = world.Create();
        world.Registry().emplace<TransformComponent>(ground, glm::vec3{0.0F, 0.0F, 0.0F});
        Box2DBodyComponent groundBody;
        groundBody.HalfExtent = glm::vec2{10.0F, 1.0F};
        groundBody.Dynamic = false;
        world.Registry().emplace<Box2DBodyComponent>(ground, groundBody);

        const entt::entity box = world.Create();
        world.Registry().emplace<TransformComponent>(box, glm::vec3{0.0F, 5.0F, 0.0F});
        Box2DBodyComponent boxBody;
        boxBody.HalfExtent = glm::vec2{0.5F, 0.5F};
        boxBody.Dynamic = true;
        world.Registry().emplace<Box2DBodyComponent>(box, boxBody);

        PhysicsWorld physics;
        physics.SyncFromWorld(world);
        StepFor(physics, 4.0F);
        physics.SyncToWorld(world);
        const float restY = world.Registry().get<TransformComponent>(box).Position.y;
        Check(restY > 1.3F && restY < 1.7F,
            "2D box rests on sized ground (y=" + std::to_string(restY) + ", expected ~1.5)");
    }
    {
        // A static body's Z rotation must survive the round trip through Box2D.
        World world{false};
        const entt::entity spinner = world.Create();
        TransformComponent transform;
        transform.Rotation = glm::angleAxis(0.5F, glm::vec3{0.0F, 0.0F, 1.0F});
        world.Registry().emplace<TransformComponent>(spinner, transform);
        Box2DBodyComponent staticBody;
        staticBody.Dynamic = false;
        world.Registry().emplace<Box2DBodyComponent>(spinner, staticBody);

        PhysicsWorld physics;
        physics.SyncFromWorld(world);
        physics.SyncToWorld(world);
        const float angle = ZAngle(world.Registry().get<TransformComponent>(spinner).Rotation);
        Check(std::abs(angle - 0.5F) < 1e-3F,
            "2D Z rotation round-trips (angle=" + std::to_string(angle) + ", expected 0.5)");
    }
}

void PhysicsRebuildTest()
{
    using namespace fadix;
    std::cout << "[physics] rebuild on dynamic/static change\n";
    World world{false};
    const entt::entity box = world.Create();
    world.Registry().emplace<TransformComponent>(box, glm::vec3{0.0F, 5.0F, 0.0F});
    JoltBodyComponent body;
    body.Dynamic = true;
    world.Registry().emplace<JoltBodyComponent>(box, body);

    PhysicsWorld physics;
    physics.SyncFromWorld(world);
    StepFor(physics, 1.0F);
    physics.SyncToWorld(world);
    const float fellY = world.Registry().get<TransformComponent>(box).Position.y;
    Check(fellY < 4.9F, "dynamic body fell under gravity (y=" + std::to_string(fellY) + ")");

    // Flip to static; SyncFromWorld must rebuild the body so it stops falling.
    world.Registry().get<JoltBodyComponent>(box).Dynamic = false;
    physics.SyncFromWorld(world);
    StepFor(physics, 1.0F);
    physics.SyncToWorld(world);
    const float frozenY = world.Registry().get<TransformComponent>(box).Position.y;
    Check(std::abs(frozenY - fellY) < 0.05F,
        "rebuilt static body no longer falls (dy=" + std::to_string(frozenY - fellY) + ")");
}
#endif
}

void AssetMaterialJsonTests()
{
    using namespace fadix;
    std::cout << "[assets] material json round-trip\n";

    const auto path = TempScene("fadix_smoke_material.material");
    const AssetHandle handle = AssetHandle::Generate();
    MaterialAsset authored = DefaultMaterialAsset(handle, "SmokeMaterial");
    authored.BaseColor = {0.2F, 0.4F, 0.8F, 0.9F};
    authored.Metallic = 0.35F;
    authored.Roughness = 0.25F;
    authored.EmissiveColor = {0.1F, 0.2F, 0.3F};
    authored.EmissiveIntensity = 2.5F;
    authored.AlphaMode = AlphaMode::Mask;
    authored.AlphaCutoff = 0.42F;
    authored.DoubleSided = true;
    authored.UVScale = {2.0F, 3.0F};
    authored.UVOffset = {0.25F, 0.5F};
    authored.BaseColorTexture = AssetHandle::Generate();

    Check(static_cast<bool>(SaveMaterialAsset(path, authored)), "save material json");
    const Result<MaterialAsset> loaded = LoadMaterialAsset(path, handle);
    Check(static_cast<bool>(loaded), "load material json");
    if (loaded)
    {
        const MaterialAsset& value = loaded.Value();
        Check(value.Name == "SmokeMaterial", "material name round-trips");
        Check(NearlyEqual(value.BaseColor.x, 0.2F) && NearlyEqual(value.BaseColor.w, 0.9F),
            "material base color round-trips");
        Check(NearlyEqual(value.Metallic, 0.35F) && NearlyEqual(value.Roughness, 0.25F),
            "material metallic/roughness round-trips");
        Check(NearlyEqual(value.EmissiveIntensity, 2.5F), "material emissive intensity round-trips");
        Check(value.AlphaMode == AlphaMode::Mask && NearlyEqual(value.AlphaCutoff, 0.42F),
            "material alpha settings round-trips");
        Check(value.DoubleSided, "material double-sided round-trips");
        Check(NearlyEqual(value.UVScale, authored.UVScale) &&
                NearlyEqual(value.UVOffset, authored.UVOffset),
            "material uv transform round-trips");
        Check(value.BaseColorTexture == authored.BaseColorTexture,
            "material texture handle round-trips");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void ProcessedTextureTests()
{
    using namespace fadix;
    std::cout << "[assets] processed texture header validation\n";

    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    Check(surface != nullptr, "create rgba surface");
    if (surface != nullptr)
    {
        std::memset(surface->pixels, 200, static_cast<std::size_t>(surface->w * surface->h * 4));
        TextureImportSettings settings;
        settings.GenerateMipmaps = true;
        settings.MaxSize = 4096;
        const Result<ProcessedTextureData> processed = ProcessTextureSurface(surface, settings);
        SDL_DestroySurface(surface);
        Check(static_cast<bool>(processed), "process texture surface");
        if (processed)
        {
            Check(processed.Value().Header.Width == 4U && processed.Value().Header.Height == 4U,
                "processed texture dimensions preserved");
            Check(processed.Value().Header.MipCount >= 1U, "processed texture has mip chain");
            const auto path = TempScene("fadix_smoke_texture.ftex");
            Check(static_cast<bool>(WriteProcessedTexture(path, processed.Value())),
                "write processed texture");
            const Result<ProcessedTextureData> reloaded = ReadProcessedTexture(path);
            Check(static_cast<bool>(reloaded), "read processed texture");
            if (reloaded)
            {
                Check(reloaded.Value().Pixels.size() == processed.Value().Pixels.size(),
                    "processed texture payload stable");
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    ProcessedTextureHeader bad{};
    bad.Magic = ProcessedTextureMagic;
    bad.Version = 2U;
    bad.Width = 999999U;
    bad.Height = 999999U;
    bad.DataSize = 16U;
    Check(!ValidateProcessedHeader(bad, 64U), "reject corrupt processed header");
}

void MaterialSceneHandleTests()
{
    using namespace fadix;
    std::cout << "[assets] mesh material handle round-trip\n";

    World authored{false};
    const entt::entity entity = authored.Create();
    authored.Registry().emplace<NameComponent>(entity, "Mesh");
    authored.Registry().emplace<TransformComponent>(entity);
    MeshComponent mesh;
    mesh.Material = AssetHandle::Generate();
    mesh.ImportedMesh = AssetHandle::Generate();
    mesh.BaseColor = {0.1F, 0.2F, 0.3F};
    authored.Registry().emplace<MeshComponent>(entity, mesh);
    const Uuid id = authored.GetUuid(entity).value();

    SceneService service;
    const auto path = TempScene("fadix_smoke_material_scene.scene");
    SceneDocument saveDoc;
    Check(static_cast<bool>(service.SaveAs(saveDoc, authored, path)), "save material scene");

    World loaded{false};
    SceneDocument loadDoc;
    Check(static_cast<bool>(service.Load(loadDoc, loaded, path)), "load material scene");
    if (const auto found = loaded.Find(id))
    {
        const auto* loadedMesh = loaded.Registry().try_get<MeshComponent>(*found);
        Check(loadedMesh != nullptr && loadedMesh->Material == mesh.Material,
            "mesh material handle round-trips through scene");
        Check(loadedMesh != nullptr && loadedMesh->ImportedMesh == mesh.ImportedMesh,
            "mesh imported-mesh handle round-trips through scene");
        Check(loadedMesh != nullptr && NearlyEqual(loadedMesh->BaseColor, mesh.BaseColor),
            "legacy mesh color still round-trips");
    }
    else
    {
        Check(false, "material scene entity preserved");
    }

    const std::unique_ptr<IWorld> clone = authored.Clone();
    if (const auto cloned = clone->Find(id))
    {
        const auto* clonedMesh = clone->Registry().try_get<MeshComponent>(*cloned);
        Check(clonedMesh != nullptr && clonedMesh->Material == mesh.Material,
            "mesh material handle survives world clone");
    }

    DuplicateEntityCommand duplicate{authored, id};
    duplicate.Execute();
    if (const auto duplicated = authored.Find(duplicate.DuplicateId()))
    {
        const auto* dupMesh = authored.Registry().try_get<MeshComponent>(*duplicated);
        Check(dupMesh != nullptr && dupMesh->Material == mesh.Material,
            "mesh material handle survives entity duplicate");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void ScriptComponentTests()
{
    using namespace fadix;
    std::cout << "[components] script component round-trip\n";

    World authored{false};
    const entt::entity scripted = authored.Create();
    authored.Registry().emplace<NameComponent>(scripted, "Scripted");
    authored.Registry().emplace<TransformComponent>(scripted);
    ScriptComponent script;
    // A name with a space exercises the quoted serialization path.
    script.ScriptNames = {"player", "camera controller"};
    script.Enabled = false;
    authored.Registry().emplace<ScriptComponent>(scripted, script);
    const Uuid id = authored.GetUuid(scripted).value();

    SceneService service;
    const auto path = TempScene("fadix_smoke_script.scene");
    SceneDocument saveDoc;
    Check(static_cast<bool>(service.SaveAs(saveDoc, authored, path)), "save script scene");

    World loaded{false};
    SceneDocument loadDoc;
    Check(static_cast<bool>(service.Load(loadDoc, loaded, path)), "load script scene");
    if (const auto found = loaded.Find(id))
    {
        const auto* value = loaded.Registry().try_get<ScriptComponent>(*found);
        Check(value != nullptr && value->ScriptNames.size() == 2 &&
                value->ScriptNames[0] == "player" &&
                value->ScriptNames[1] == "camera controller" && !value->Enabled,
            "script names + enabled round-trip (order + spaces preserved)");
    }
    else
    {
        Check(false, "script entity preserved");
    }

    const std::unique_ptr<IWorld> clone = authored.Clone();
    if (const auto cloned = clone->Find(id))
    {
        const auto* value = clone->Registry().try_get<ScriptComponent>(*cloned);
        Check(value != nullptr && value->ScriptNames.size() == 2 &&
                value->ScriptNames[1] == "camera controller",
            "script component survives world clone");
    }
    else
    {
        Check(false, "script entity survives clone");
    }

    const auto before = EntitySnapshot::Capture(authored, id);
    authored.Registry().remove<ScriptComponent>(scripted);
    const auto after = EntitySnapshot::Capture(authored, id);
    if (before && after)
    {
        SnapshotEntityCommand command{authored, *before, *after, "Remove Script"};
        command.Undo();
        const auto* restored = authored.Registry().try_get<ScriptComponent>(scripted);
        Check(restored != nullptr && restored->ScriptNames.size() == 2,
            "undo restores removed script component");
    }
    else
    {
        Check(false, "script snapshots captured");
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void ScriptDatabaseTests()
{
    using namespace fadix;
    std::cout << "[assets] script database CRUD\n";

    const auto folder = std::filesystem::temp_directory_path() / "fadix_smoke_scripts";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);

    ScriptDatabase database;
    database.ScanFolder(folder);
    Check(database.All().empty(), "fresh scripts folder scans empty");

    ScriptAsset* lua = database.CreateNew("hero", ScriptLanguage::Lua);
    ScriptAsset* cpp = database.CreateNew("spawner", ScriptLanguage::Cpp);
    Check(lua != nullptr && cpp != nullptr, "create lua + cpp scripts");
    Check(database.All().size() == 2, "database holds both scripts");
    Check(std::filesystem::exists(folder / "hero.lua") &&
            std::filesystem::exists(folder / "spawner.cpp"),
        "script files created on disk");
    Check(database.CreateNew("hero", ScriptLanguage::Lua) == nullptr, "duplicate name rejected");

    if (lua != nullptr)
    {
        const std::string edited = "-- edited\nfunction OnStart(e) end\n";
        lua->SourceCode.assign(edited.begin(), edited.end());
        lua->Dirty = true;
        Check(database.Save("hero"), "save edited lua");
    }
    database.Refresh();
    Check(database.All().size() == 2, "rescan finds both scripts");
    if (auto reopened = database.Get("hero"))
    {
        Check(database.LoadSource(reopened->get()), "load lua source from disk");
        const std::string text{
            reopened->get().SourceCode.begin(), reopened->get().SourceCode.end()};
        Check(text.find("-- edited") != std::string::npos, "edited lua source persisted");
    }
    else
    {
        Check(false, "reopen hero after rescan");
    }

    Check(database.Rename("spawner", "factory"), "rename cpp script");
    Check(std::filesystem::exists(folder / "factory.cpp") &&
            !std::filesystem::exists(folder / "spawner.cpp"),
        "rename moved the file");
    Check(database.Delete("factory"), "delete script");
    Check(!std::filesystem::exists(folder / "factory.cpp"), "delete removed the file");
    Check(database.All().size() == 1, "database size after delete");

    std::filesystem::remove_all(folder, ec);
}

void PostProcessTests()
{
    using namespace fadix;
    std::cout << "[postprocess] component defaults + sanitize\n";

    const EnvironmentComponent defaults;
    Check(defaults.BloomEnabled, "bloom enabled by default");
    Check(defaults.TonemapEnabled, "tonemap enabled by default");
    Check(defaults.FxaaEnabled, "fxaa enabled by default");
    Check(defaults.AntiAlias == AntiAliasMode::Auto, "anti-alias auto by default");
    Check(std::abs(defaults.TaaHistoryWeight - 0.9F) < 1e-4F, "default taa history weight 0.9");
    Check(!defaults.ColorGradingEnabled, "color grading off by default");
    Check(std::abs(defaults.BloomIntensity - 0.3F) < 1e-4F, "default bloom intensity 0.3");
    Check(defaults.BloomPasses == 5, "default bloom passes 5");

    EnvironmentComponent wild;
    wild.BloomThreshold = 999.0F;
    wild.BloomIntensity = -3.0F;
    wild.BloomPasses = 40;
    wild.Lift = glm::vec3{-9.0F, 0.0F, 9.0F};
    wild.Gamma = glm::vec3{0.0F, 1.0F, 50.0F};
    wild.Gain = glm::vec3{-1.0F, 1.0F, 99.0F};
    wild.FxaaStrength = 25.0F;
    wild.TaaHistoryWeight = -999.0F;
    wild.TaaSharpness = 999.0F;
    Sanitize(wild);
    Check(std::abs(wild.BloomThreshold - 10.0F) < 1e-4F, "bloom threshold clamped to 10");
    Check(std::abs(wild.BloomIntensity - 0.0F) < 1e-4F, "bloom intensity clamped to 0");
    Check(wild.BloomPasses == 8, "bloom passes clamped to 8");
    Check(std::abs(wild.Lift.x + 1.0F) < 1e-4F && std::abs(wild.Lift.z - 1.0F) < 1e-4F, "lift clamped to [-1,1]");
    Check(std::abs(wild.Gamma.x - 0.1F) < 1e-4F && std::abs(wild.Gamma.z - 3.0F) < 1e-4F, "gamma clamped to [0.1,3]");
    Check(std::abs(wild.Gain.x - 0.0F) < 1e-4F && std::abs(wild.Gain.z - 3.0F) < 1e-4F, "gain clamped to [0,3]");
    Check(std::abs(wild.FxaaStrength - 2.0F) < 1e-4F, "fxaa strength clamped to 2");
    Check(std::abs(wild.TaaHistoryWeight - 0.7F) < 1e-4F, "taa history weight clamped to 0.7");
    Check(std::abs(wild.TaaSharpness - 1.0F) < 1e-4F, "taa sharpness clamped to 1");

    std::cout << "[postprocess] environment serialization round-trip\n";
    World envWorld{false};
    const entt::entity envEntity = envWorld.Create();
    EnvironmentComponent authoredEnv;
    authoredEnv.Primary = true;
    authoredEnv.BloomEnabled = false;
    authoredEnv.BloomThreshold = 2.5F;
    authoredEnv.BloomIntensity = 0.7F;
    authoredEnv.BloomPasses = 3;
    authoredEnv.TonemapEnabled = false;
    authoredEnv.ColorGradingEnabled = true;
    authoredEnv.Lift = glm::vec3{0.1F, -0.2F, 0.3F};
    authoredEnv.Gamma = glm::vec3{1.2F, 0.9F, 1.1F};
    authoredEnv.Gain = glm::vec3{1.3F, 1.0F, 0.8F};
    authoredEnv.FxaaEnabled = false;
    authoredEnv.FxaaStrength = 0.5F;
    authoredEnv.AntiAlias = AntiAliasMode::Taa;
    authoredEnv.TaaHistoryWeight = 0.85F;
    authoredEnv.TaaSharpness = 0.25F;
    envWorld.Registry().emplace<EnvironmentComponent>(envEntity, authoredEnv);
    const Uuid envId = envWorld.GetUuid(envEntity).value();

    SceneService envService;
    const auto envPath = TempScene("fadix_smoke_env_pp.scene");
    SceneDocument envSaveDoc;
    Check(static_cast<bool>(envService.SaveAs(envSaveDoc, envWorld, envPath)), "save env scene");

    World envLoaded{false};
    SceneDocument envLoadDoc;
    Check(static_cast<bool>(envService.Load(envLoadDoc, envLoaded, envPath)), "load env scene");
    if (const auto found = envLoaded.Find(envId))
    {
        const auto* env = envLoaded.Registry().try_get<EnvironmentComponent>(*found);
        Check(env != nullptr && !env->BloomEnabled, "round-trip bloom disabled");
        Check(env != nullptr && std::abs(env->BloomThreshold - 2.5F) < 1e-4F, "round-trip bloom threshold");
        Check(env != nullptr && std::abs(env->BloomIntensity - 0.7F) < 1e-4F, "round-trip bloom intensity");
        Check(env != nullptr && env->BloomPasses == 3, "round-trip bloom passes");
        Check(env != nullptr && !env->TonemapEnabled, "round-trip tonemap disabled");
        Check(env != nullptr && env->ColorGradingEnabled, "round-trip color grading on");
        Check(env != nullptr && NearlyEqual(env->Lift, glm::vec3{0.1F, -0.2F, 0.3F}), "round-trip lift");
        Check(env != nullptr && NearlyEqual(env->Gamma, glm::vec3{1.2F, 0.9F, 1.1F}), "round-trip gamma");
        Check(env != nullptr && std::abs(env->Gain.x - 1.3F) < 1e-4F, "round-trip gain.x");
        Check(env != nullptr && !env->FxaaEnabled, "round-trip fxaa disabled");
        Check(env != nullptr && std::abs(env->FxaaStrength - 0.5F) < 1e-4F, "round-trip fxaa strength");
        Check(env != nullptr && env->AntiAlias == AntiAliasMode::Taa, "round-trip anti-alias taa");
        Check(env != nullptr && std::abs(env->TaaHistoryWeight - 0.85F) < 1e-4F,
            "round-trip taa history weight");
        Check(env != nullptr && std::abs(env->TaaSharpness - 0.25F) < 1e-4F, "round-trip taa sharpness");
    }
    else
    {
        Check(false, "round-trip env entity found");
    }

    // Back-compat: an old scene whose E row lacks post-processing tokens must
    // load successfully and fill defaults, not fail with "Truncated entity".
    std::cout << "[postprocess] old-scene back-compat (no post tokens)\n";
    {
        // Load the newly-saved scene as raw text, strip the post-processing
        // tokens from the E row, and re-save it as a simulated pre-change file.
        std::ifstream inFile{envPath};
        std::string fileText{std::istreambuf_iterator<char>(inFile), {}};
        inFile.close();

        std::istringstream textStream{fileText};
        std::ostringstream stripped;
        std::string fileLine;
        while (std::getline(textStream, fileLine))
        {
            // Find the E row: split by whitespace, locate "E" token, keep only
            // the 23 legacy value tokens after it, then append END.
            std::istringstream lineStream{fileLine};
            std::vector<std::string> tokens;
            std::string tok;
            while (lineStream >> tok)
            {
                tokens.push_back(tok);
            }
            // The E marker is written after the parent UUID field.
            // Walk tokens to find "E".
            std::size_t eIdx = tokens.size();
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i] == "E")
                {
                    eIdx = i;
                    break;
                }
            }
            if (eIdx < tokens.size())
            {
                // Keep tokens[0..eIdx+23] (E + 23 legacy values), drop post tokens + END,
                // then append END.
                const std::size_t keepUntil = eIdx + 23; // inclusive
                for (std::size_t i = 0; i <= keepUntil && i < tokens.size(); ++i)
                {
                    stripped << tokens[i] << ' ';
                }
                stripped << "END\n";
            }
            else
            {
                stripped << fileLine << '\n';
            }
        }

        const auto legacyPath = TempScene("fadix_smoke_env_pp_legacy.scene");
        {
            std::ofstream legacyFile{legacyPath, std::ios::trunc};
            legacyFile << stripped.str();
        }

        World legacyWorld{false};
        SceneDocument legacyDoc;
        const auto legacyResult = envService.Load(legacyDoc, legacyWorld, legacyPath);
        Check(static_cast<bool>(legacyResult), "old scene without post tokens loads");
        if (legacyResult)
        {
            // The entity UUID matches the one we saved (envId).
            if (const auto found = legacyWorld.Find(envId))
            {
                const auto* env = legacyWorld.Registry().try_get<EnvironmentComponent>(*found);
                Check(env != nullptr && env->BloomEnabled, "old scene keeps bloom default");
                Check(env != nullptr && std::abs(env->BloomIntensity - 0.3F) < 1e-4F,
                    "old scene keeps bloom intensity default");
                Check(env != nullptr && env->FxaaEnabled, "old scene keeps fxaa default");
                Check(env != nullptr && env->AntiAlias == AntiAliasMode::Auto,
                    "old scene infers anti-alias auto from fxaa enabled");
            }
            else
            {
                Check(false, "old scene entity found by UUID");
            }
        }

        std::error_code legacyEc;
        std::filesystem::remove(legacyPath, legacyEc);
    }

    // Scenes with FXAA tokens but no AntiAlias field infer mode from FxaaEnabled.
    std::cout << "[postprocess] fxaa-token back-compat (no anti-alias tokens)\n";
    {
        std::ifstream inFile{envPath};
        std::string fileText{std::istreambuf_iterator<char>(inFile), {}};
        inFile.close();

        std::istringstream textStream{fileText};
        std::ostringstream stripped;
        std::string fileLine;
        while (std::getline(textStream, fileLine))
        {
            std::istringstream lineStream{fileLine};
            std::vector<std::string> tokens;
            std::string tok;
            while (lineStream >> tok)
            {
                tokens.push_back(tok);
            }
            std::size_t eIdx = tokens.size();
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                if (tokens[i] == "E")
                {
                    eIdx = i;
                    break;
                }
            }
            if (eIdx < tokens.size())
            {
                // E + 23 legacy + 17 post tokens through FxaaStrength.
                const std::size_t keepUntil = eIdx + 40;
                for (std::size_t i = 0; i <= keepUntil && i < tokens.size(); ++i)
                {
                    stripped << tokens[i] << ' ';
                }
                stripped << "END\n";
            }
            else
            {
                stripped << fileLine << '\n';
            }
        }

        const auto fxaaOnlyPath = TempScene("fadix_smoke_env_pp_fxaa_only.scene");
        {
            std::ofstream fxaaOnlyFile{fxaaOnlyPath, std::ios::trunc};
            fxaaOnlyFile << stripped.str();
        }

        World fxaaOnlyWorld{false};
        SceneDocument fxaaOnlyDoc;
        Check(static_cast<bool>(envService.Load(fxaaOnlyDoc, fxaaOnlyWorld, fxaaOnlyPath)),
            "fxaa-only scene loads");
        if (const auto found = fxaaOnlyWorld.Find(envId))
        {
            const auto* env = fxaaOnlyWorld.Registry().try_get<EnvironmentComponent>(*found);
            Check(env != nullptr && env->AntiAlias == AntiAliasMode::Off,
                "fxaa disabled infers anti-alias off");
        }
        else
        {
            Check(false, "fxaa-only scene entity found");
        }

        std::error_code fxaaOnlyEc;
        std::filesystem::remove(fxaaOnlyPath, fxaaOnlyEc);
    }

    std::error_code envEc;
    std::filesystem::remove(envPath, envEc);
}

int main()
{
    SceneTests();
    NewComponentRoundTripTests();
    MeshKindTests();
    SanitizationTests();
    CloneAndSnapshotTests();
    AssetMaterialJsonTests();
    ProcessedTextureTests();
    MaterialSceneHandleTests();
    ScriptComponentTests();
    ScriptDatabaseTests();
    PostProcessTests();
#ifdef FADIX_ENABLE_PHYSICS
    Physics3DTest();
    Physics2DTest();
    PhysicsRebuildTest();
#else
    std::cout << "[physics] skipped (FADIX_ENABLE_PHYSICS off)\n";
#endif

    if (g_Failures == 0)
    {
        std::cout << "ALL SMOKE CHECKS PASSED\n";
        return 0;
    }
    std::cerr << g_Failures << " SMOKE CHECK(S) FAILED\n";
    return 1;
}
