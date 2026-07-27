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

    fadix::ScriptRunner runner;
    runner.SetLogger([](const std::string& message, const char* severity) {
        std::printf("  lua[%s]: %s\n", severity, message.c_str());
    });

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
        return std::nullopt;
    };

    runner.Start(registry, resolver);
    Check(std::fabs(registry.get<fadix::TransformComponent>(mover).Position.x - 1.0F) < 1e-4F,
        "OnStart set mover.x to 1.0");
    Check(std::fabs(registry.get<fadix::TransformComponent>(sleeper).Position.x) < 1e-4F,
        "disabled script did not run");
    Check(registry.valid(doomed), "doomed entity alive before update");

    runner.Update(registry, 0.5F);
    Check(std::fabs(registry.get<fadix::TransformComponent>(mover).Position.x - 1.5F) < 1e-4F,
        "OnUpdate advanced mover.x to 1.5");
    Check(!registry.valid(doomed), "entity:destroy() removed the doomed entity");

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
