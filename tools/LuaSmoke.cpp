// Standalone proof that the Lua dependency links and that a script can read and
// mutate an entity's TransformComponent. Run as the fadix_lua_smoke target.
#include "scripting/LuaVM.hpp"
#include "runtime/Components.hpp"

#include <entt/entity/registry.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

int main()
{
    using namespace fadix;

    entt::registry registry;
    const entt::entity entity = registry.create();
    registry.emplace<NameComponent>(entity, NameComponent{"Hero"});
    registry.emplace<TransformComponent>(entity);
    CharacterControllerComponent character;
    character.Grounded = true;
    registry.emplace<CharacterControllerComponent>(entity, character);

    LuaVM vm;
    std::string captured;
    vm.SetLogger([&captured](const std::string& message, const char*) { captured += message; });

    const bool compiled = vm.Compile("smoke", R"LUA(
function OnStart(e)
    print("start " .. e:getName() .. " id " .. e.id)
    -- SDL not initialized here; isDown must still be callable and return false.
    if Input.isDown("W") then
        error("Input.isDown should be false without SDL keyboard")
    end
end

function OnUpdate(e, dt)
    local x, y, z = e:getPosition()
    e:setPosition(x + dt, y, z)
    e:moveCharacter(1.0, -0.5)
    if e:isCharacterGrounded() then
        e:jumpCharacter()
    end
end
)LUA");
    assert(compiled && "script should compile");

    // Compile accepts unresolved globals; they are not syntax errors.
    LuaVM syntaxOnly;
    assert(syntaxOnly.Compile("globals", "print(notARealGlobal)\n")
        && "unresolved globals must not fail Lua compile");

    const int instance = vm.Instantiate("smoke");
    assert(instance >= 0 && "script should instantiate");

    const ScriptEntityHandle handle{&registry, entity, nullptr};
    vm.CallStart(instance, handle);
    vm.CallUpdate(instance, handle, 2.0F);
    vm.CallUpdate(instance, handle, 0.5F);

    const auto& transform = registry.get<TransformComponent>(entity);
    assert(std::fabs(transform.Position.x - 2.5F) < 1e-5F && "OnUpdate must move the entity");
    const auto& movedCharacter = registry.get<CharacterControllerComponent>(entity);
    assert(movedCharacter.MoveInput == glm::vec2(1.0F, -0.5F)
        && "Lua must send character movement input");
    assert(movedCharacter.JumpRequested && "Lua must request a grounded character jump");
    assert(captured.find("start Hero") != std::string::npos && "print must reach the logger");
    assert(!vm.HasError() && "no script errors expected");

    std::puts("fadix_lua_smoke: OK");
    return 0;
}
