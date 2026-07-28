// Proves ScriptRunner drives ScriptComponent entities end to end: OnStart /
// OnUpdate mutate the transform through the Lua entity API, and a script that
// calls entity:destroy() removes the entity via the deferred-destroy queue.
#include "runtime/Components.hpp"
#include "scripting/ScriptRunner.hpp"

#include <entt/entity/registry.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace
{
int g_Failures = 0;

void Check(const bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
    {
        ++g_Failures;
    }
}

const char* MoverSource()
{
    return "function OnStart(entity)\n"
           "  entity:setPosition(1.0, 0.0, 0.0)\n"
           "end\n"
           "function OnUpdate(entity, dt)\n"
           "  local x, y, z = entity:getPosition()\n"
           "  entity:setPosition(x + dt, y, z)\n"
           "end\n";
}

const char* SuicideSource()
{
    return "function OnUpdate(entity, dt)\n"
           "  entity:destroy()\n"
           "end\n";
}

// Gameplay API scripts: find an entity by name, spawn a prefab, load a scene.
const char* FinderSource()
{
    return "function OnStart(entity)\n"
           "  local target = World.find(\"Target\")\n"
           "  if target then target:setPosition(42.0, 0.0, 0.0) end\n"
           "end\n";
}

const char* SpawnerSource()
{
    return "function OnStart(entity)\n"
           "  local e = Prefab.spawn(\"Pickup\", 5.0, 6.0, 7.0)\n"
           "  if e then e:setPosition(9.0, 9.0, 9.0) end\n"
           "end\n";
}

const char* LoaderSource()
{
    return "function OnUpdate(entity, dt)\n"
           "  Scene.load(\"Scenes/Level2.scene\")\n"
           "end\n";
}
}

int main()
{
    entt::registry registry;

    const entt::entity mover = registry.create();
    registry.emplace<fadix::NameComponent>(mover, fadix::NameComponent{"Hero"});
    registry.emplace<fadix::TransformComponent>(mover);
    registry.emplace<fadix::ScriptComponent>(
        mover, fadix::ScriptComponent{std::vector<std::string>{"mover"}, true});

    const entt::entity doomed = registry.create();
    registry.emplace<fadix::NameComponent>(doomed, fadix::NameComponent{"Doomed"});
    registry.emplace<fadix::TransformComponent>(doomed);
    registry.emplace<fadix::ScriptComponent>(
        doomed, fadix::ScriptComponent{std::vector<std::string>{"suicide"}, true});

    // A disabled script must not run.
    const entt::entity sleeper = registry.create();
    registry.emplace<fadix::TransformComponent>(sleeper);
    registry.emplace<fadix::ScriptComponent>(
        sleeper, fadix::ScriptComponent{std::vector<std::string>{"mover"}, false});

    // Gameplay API: a named target found by World.find, a finder that moves it,
    // a spawner that calls Prefab.spawn, and a loader that calls Scene.load.
    const entt::entity target = registry.create();
    registry.emplace<fadix::NameComponent>(target, fadix::NameComponent{"Target"});
    registry.emplace<fadix::TransformComponent>(target);

    const entt::entity finder = registry.create();
    registry.emplace<fadix::TransformComponent>(finder);
    registry.emplace<fadix::ScriptComponent>(
        finder, fadix::ScriptComponent{std::vector<std::string>{"finder"}, true});

    const entt::entity spawner = registry.create();
    registry.emplace<fadix::TransformComponent>(spawner);
    registry.emplace<fadix::ScriptComponent>(
        spawner, fadix::ScriptComponent{std::vector<std::string>{"spawner"}, true});

    const entt::entity loader = registry.create();
    registry.emplace<fadix::TransformComponent>(loader);
    registry.emplace<fadix::ScriptComponent>(
        loader, fadix::ScriptComponent{std::vector<std::string>{"loader"}, true});

    fadix::ScriptRunner runner;
    runner.SetLogger([](const std::string& message, const char* severity) {
        std::printf("  lua[%s]: %s\n", severity, message.c_str());
    });

    // Stand in for PlaySession's game services: spawn records the created entity
    // at the requested position; load records the requested scene path.
    std::string spawnPath;
    std::string loadedScene;
    entt::entity spawnedEntity = entt::null;
    runner.SetGameCallbacks(
        [&](const std::string& path, float x, float y, float z) -> std::optional<entt::entity> {
            spawnPath = path;
            const entt::entity created = registry.create();
            registry.emplace<fadix::NameComponent>(created, fadix::NameComponent{"Pickup"});
            auto& transform = registry.emplace<fadix::TransformComponent>(created);
            transform.Position = {x, y, z};
            spawnedEntity = created;
            return created;
        },
        [&](const std::string& path) { loadedScene = path; });

    const auto resolver =
        [](const std::string& name) -> std::optional<fadix::ScriptRunner::ResolvedScript> {
        if (name == "mover")
        {
            return fadix::ScriptRunner::ResolvedScript{MoverSource(), fadix::ScriptLanguage::Lua, {}};
        }
        if (name == "suicide")
        {
            return fadix::ScriptRunner::ResolvedScript{
                SuicideSource(), fadix::ScriptLanguage::Lua, {}};
        }
        if (name == "finder")
        {
            return fadix::ScriptRunner::ResolvedScript{
                FinderSource(), fadix::ScriptLanguage::Lua, {}};
        }
        if (name == "spawner")
        {
            return fadix::ScriptRunner::ResolvedScript{
                SpawnerSource(), fadix::ScriptLanguage::Lua, {}};
        }
        if (name == "loader")
        {
            return fadix::ScriptRunner::ResolvedScript{
                LoaderSource(), fadix::ScriptLanguage::Lua, {}};
        }
        return std::nullopt;
    };

    runner.Start(registry, resolver);
    Check(std::fabs(registry.get<fadix::TransformComponent>(target).Position.x - 42.0F) < 1e-4F,
        "World.find located Target and OnStart moved it");
    Check(spawnPath == "Pickup", "Prefab.spawn passed the prefab path");
    Check(registry.valid(spawnedEntity)
            && std::fabs(registry.get<fadix::TransformComponent>(spawnedEntity).Position.x - 9.0F)
                < 1e-4F,
        "Prefab.spawn returned a live entity handle the script could mutate");
    Check(std::fabs(registry.get<fadix::TransformComponent>(mover).Position.x - 1.0F) < 1e-4F,
        "OnStart set mover.x to 1.0");
    Check(std::fabs(registry.get<fadix::TransformComponent>(sleeper).Position.x) < 1e-4F,
        "disabled script did not run");
    Check(registry.valid(doomed), "doomed entity alive before update");

    runner.Update(registry, 0.5F);
    Check(std::fabs(registry.get<fadix::TransformComponent>(mover).Position.x - 1.5F) < 1e-4F,
        "OnUpdate advanced mover.x to 1.5");
    Check(!registry.valid(doomed), "entity:destroy() removed the doomed entity");
    Check(loadedScene == "Scenes/Level2.scene", "Scene.load requested the level transition");

    runner.Update(registry, 0.5F);
    Check(std::fabs(registry.get<fadix::TransformComponent>(mover).Position.x - 2.0F) < 1e-4F,
        "second OnUpdate advanced mover.x to 2.0");

    runner.Stop(registry);

    if (g_Failures == 0)
    {
        std::printf("fadix_script_runtime_smoke: OK\n");
        return 0;
    }
    std::printf("fadix_script_runtime_smoke: %d FAILURE(S)\n", g_Failures);
    return 1;
}
