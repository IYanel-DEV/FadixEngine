// Standalone proof that the Lua dependency links and that a script can read and
// mutate an entity's TransformComponent. Run as the fadix_lua_smoke target.
#include "scripting/LuaVM.hpp"
#include "runtime/AnimationRuntime.hpp"
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
    registry.emplace<AnimatorComponent>(entity);
    TransformAnimatorComponent transformAnimator;
    AnimationClipAsset walk;
    walk.Name = "Walk";
    walk.Duration = 2.0F;
    AnimationChannel walkPosition;
    walkPosition.JointIndex = -1;
    walkPosition.Target = AnimationChannel::Property::Translation;
    walkPosition.Keyframes.push_back({0.0F, glm::vec4{0.0F}});
    walkPosition.Keyframes.push_back({2.0F, glm::vec4{0.0F}});
    walk.Channels.push_back(std::move(walkPosition));
    AnimationClipAsset dash;
    dash.Name = "Dash";
    dash.Duration = 2.0F;
    AnimationChannel dashPosition;
    dashPosition.JointIndex = -1;
    dashPosition.Target = AnimationChannel::Property::Translation;
    dashPosition.Keyframes.push_back({0.0F, glm::vec4{10.0F, 0.0F, 0.0F, 0.0F}});
    dashPosition.Keyframes.push_back({2.0F, glm::vec4{10.0F, 0.0F, 0.0F, 0.0F}});
    dash.Channels.push_back(std::move(dashPosition));
    transformAnimator.Clips.push_back(std::move(walk));
    transformAnimator.Clips.push_back(std::move(dash));
    transformAnimator.ClipName = "Walk";
    registry.emplace<TransformAnimatorComponent>(entity, std::move(transformAnimator));

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
    if not e:playAnimation("Run") or not e:isAnimationPlaying() then
        error("playAnimation should start the requested clip")
    end
    if not e:seekAnimation(1.25) or not e:pauseAnimation() then
        error("seek and pause should control the selected clip")
    end
    if e:getCurrentAnimation() ~= "Run" or math.abs(e:getAnimationTime() - 1.25) > 0.001 then
        error("animation queries should report skeletal state")
    end
    if not e:resumeAnimation() or not e:playAnimation("Walk") then
        error("resume and named transform playback should work")
    end
    e:seekAnimation(0.4)
    if not e:crossFadeAnimation("Dash", 1.0) then
        error("crossFadeAnimation should start a named transition")
    end
    if e:getCurrentAnimation() ~= "Dash" or math.abs(e:getAnimationTime()) > 0.001 then
        error("animation queries should report the crossfade destination")
    end
    e:setAnimationSpeed(0.5)
end

function OnUpdate(e, dt)
    local x, y, z = e:getPosition()
    e:setPosition(x + dt, y, z)
    e:moveCharacter(1.0, -0.5)
    if e:isCharacterGrounded() then
        e:jumpCharacter()
    end
    if dt < 1.0 then
        e:stopAnimation()
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
    UpdateTransformAnimations(registry, 0.5F);
    auto& blendedTransform = registry.get<TransformComponent>(entity);
    assert(std::fabs(blendedTransform.Position.x - 5.0F) < 1e-4F &&
        "runtime must blend halfway between transform clips");
    const auto& blendingAnimator = registry.get<TransformAnimatorComponent>(entity);
    assert(blendingAnimator.BlendFromClipName == "Walk" &&
        std::fabs(blendingAnimator.BlendElapsed - 0.5F) < 1e-4F &&
        "runtime must retain the source clip during a crossfade");
    blendedTransform.Position = glm::vec3{0.0F};
    vm.CallUpdate(instance, handle, 2.0F);
    vm.CallUpdate(instance, handle, 0.5F);

    const auto& transform = registry.get<TransformComponent>(entity);
    assert(std::fabs(transform.Position.x - 2.5F) < 1e-5F && "OnUpdate must move the entity");
    const auto& movedCharacter = registry.get<CharacterControllerComponent>(entity);
    assert(movedCharacter.MoveInput == glm::vec2(1.0F, -0.5F)
        && "Lua must send character movement input");
    assert(movedCharacter.JumpRequested && "Lua must request a grounded character jump");
    const auto& animator = registry.get<AnimatorComponent>(entity);
    assert(animator.ClipName == "Run" && "Lua must select the requested animation clip");
    assert(!animator.Playing && !animator.Paused && animator.CurrentTime == 0.0F &&
        "Lua stop must reset skeletal animation playback");
    const auto& transformAnimatorResult = registry.get<TransformAnimatorComponent>(entity);
    assert(transformAnimatorResult.ClipName == "Dash" && !transformAnimatorResult.Playing &&
        !transformAnimatorResult.Paused && transformAnimatorResult.CurrentTime == 0.0F &&
        transformAnimatorResult.BlendFromClipName.empty() &&
        std::fabs(transformAnimatorResult.Speed - 0.5F) < 1e-5F &&
        "Lua must select and control a named transform clip");

    const entt::entity controllerEntity = registry.create();
    registry.emplace<TransformComponent>(controllerEntity);
    TransformAnimatorComponent controllerAnimator;
    controllerAnimator.Clips = transformAnimatorResult.Clips;
    controllerAnimator.ClipName = "Walk";
    controllerAnimator.Controller.Name = "Hero Controller";
    controllerAnimator.Controller.EntryState = "Idle";
    controllerAnimator.Controller.States = {
        {"Idle", "Walk", {20.0F, 20.0F}}, {"Move", "Dash", {220.0F, 20.0F}}};
    controllerAnimator.Controller.Parameters = {
        {"Moving", AnimatorParameterType::Bool, false, 0.0F, 0},
        {"Speed", AnimatorParameterType::Float, false, 0.0F, 0},
        {"Mode", AnimatorParameterType::Int, false, 0.0F, 0},
        {"Jump", AnimatorParameterType::Trigger, false, 0.0F, 0}};
    AnimatorTransition moveTransition;
    moveTransition.From = "Idle";
    moveTransition.To = "Move";
    moveTransition.Duration = 0.2F;
    moveTransition.Conditions.push_back({"Moving", AnimatorComparison::Equal, 1.0F});
    controllerAnimator.Controller.Transitions.push_back(std::move(moveTransition));
    AnimatorTransition jumpTransition;
    jumpTransition.From = "Move";
    jumpTransition.To = "Idle";
    jumpTransition.Conditions.push_back({"Jump", AnimatorComparison::Equal, 1.0F});
    controllerAnimator.Controller.Transitions.push_back(std::move(jumpTransition));
    registry.emplace<TransformAnimatorComponent>(controllerEntity, std::move(controllerAnimator));

    LuaVM controllerVm;
    assert(controllerVm.Compile("controller", R"LUA(
function OnStart(e)
    if not e:startAnimator() then error("controller should start") end
    if not e:setAnimatorBool("Moving", true) then error("bool parameter should exist") end
    if not e:setAnimatorFloat("Speed", 3.5) then error("float parameter should exist") end
    if not e:setAnimatorInt("Mode", 2) then error("int parameter should exist") end
    if not e:triggerAnimator("Jump") then error("trigger parameter should exist") end
end
)LUA"));
    const int controllerInstance = controllerVm.Instantiate("controller");
    assert(controllerInstance >= 0);
    controllerVm.CallStart(controllerInstance,
        ScriptEntityHandle{&registry, controllerEntity, nullptr});
    UpdateTransformAnimations(registry, 0.1F);
    const auto& transitioned = registry.get<TransformAnimatorComponent>(controllerEntity);
    assert(transitioned.ActiveState == "Move" && transitioned.ClipName == "Dash" &&
        transitioned.BlendFromClipName == "Walk" &&
        "Animator conditions must transition through the existing crossfade path");
    assert(FindAnimatorParameter(transitioned.Controller, "Speed")->FloatValue == 3.5F &&
        FindAnimatorParameter(transitioned.Controller, "Mode")->IntValue == 2 &&
        FindAnimatorParameter(transitioned.Controller, "Jump")->BoolValue &&
        "Lua must set all Animator Controller parameter types");
    UpdateTransformAnimations(registry, 0.1F);
    const auto& triggered = registry.get<TransformAnimatorComponent>(controllerEntity);
    assert(triggered.ActiveState == "Idle" &&
        !FindAnimatorParameter(triggered.Controller, "Jump")->BoolValue &&
        "Trigger parameters must reset after their transition is taken");
    assert(captured.find("start Hero") != std::string::npos && "print must reach the logger");
    assert(!vm.HasError() && "no script errors expected");

    std::puts("fadix_lua_smoke: OK");
    return 0;
}
