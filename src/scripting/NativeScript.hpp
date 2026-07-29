#pragma once

#include "runtime/Components.hpp"
#include "scripting/LuaVM.hpp" // ScriptEntityHandle

#include <entt/entity/registry.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace fadix
{
// Handle passed to native (C++) scripts. Mirrors the Lua entity API one to one
// so a script ported between the two languages behaves identically. Rotation is
// exchanged as euler degrees, matching LuaEntity in LuaVM.cpp.
class ScriptEntity
{
public:
    explicit ScriptEntity(const ScriptEntityHandle& handle) : m_Handle(handle) {}

    [[nodiscard]] std::uint32_t id() const
    {
        return static_cast<std::uint32_t>(m_Handle.Entity);
    }

    [[nodiscard]] std::string getName() const
    {
        const auto* name = Try<NameComponent>();
        return name != nullptr ? name->Name : std::string{};
    }

    [[nodiscard]] glm::vec3 getPosition() const
    {
        const auto* transform = Try<TransformComponent>();
        return transform != nullptr ? transform->Position : glm::vec3{0.0F};
    }
    void setPosition(const glm::vec3& value)
    {
        if (auto* transform = Try<TransformComponent>())
        {
            transform->Position = value;
        }
    }

    [[nodiscard]] glm::vec3 getRotation() const // euler degrees
    {
        const auto* transform = Try<TransformComponent>();
        return transform != nullptr ? glm::degrees(glm::eulerAngles(transform->Rotation))
                                    : glm::vec3{0.0F};
    }
    void setRotation(const glm::vec3& eulerDegrees)
    {
        if (auto* transform = Try<TransformComponent>())
        {
            transform->Rotation = glm::quat(glm::radians(eulerDegrees));
        }
    }

    [[nodiscard]] glm::vec3 getScale() const
    {
        const auto* transform = Try<TransformComponent>();
        return transform != nullptr ? transform->Scale : glm::vec3{1.0F};
    }
    void setScale(const glm::vec3& value)
    {
        if (auto* transform = Try<TransformComponent>())
        {
            transform->Scale = value;
        }
    }

    void moveCharacter(const glm::vec2& input)
    {
        if (auto* character = Try<CharacterControllerComponent>())
        {
            character->MoveInput = input;
        }
    }

    void jumpCharacter()
    {
        if (auto* character = Try<CharacterControllerComponent>())
        {
            character->JumpRequested = true;
        }
    }

    [[nodiscard]] bool isCharacterGrounded() const
    {
        const auto* character = Try<CharacterControllerComponent>();
        return character != nullptr && character->Grounded;
    }

    void destroy()
    {
        if (m_Handle.PendingDestroy != nullptr)
        {
            m_Handle.PendingDestroy->push_back(m_Handle.Entity);
        }
    }

    // Returns a handle to ScriptComponent::Target, or nullopt if unset/missing.
    [[nodiscard]] std::optional<ScriptEntityHandle> getTarget() const
    {
        const ScriptComponent* scripts = Try<ScriptComponent>();
        if (scripts == nullptr || !scripts->Target.IsValid() || m_Handle.Registry == nullptr)
        {
            return std::nullopt;
        }
        for (const auto [other, uuid] : m_Handle.Registry->view<UuidComponent>().each())
        {
            if (uuid.Id == scripts->Target)
            {
                return ScriptEntityHandle{m_Handle.Registry, other, m_Handle.PendingDestroy};
            }
        }
        return std::nullopt;
    }

private:
    template <typename T>
    [[nodiscard]] T* Try() const
    {
        return m_Handle.Registry != nullptr ? m_Handle.Registry->try_get<T>(m_Handle.Entity)
                                            : nullptr;
    }

    ScriptEntityHandle m_Handle;
};

// Base class every native script derives from. The compiled DLL exports a
// factory the loader looks up by name:
//   extern "C" __declspec(dllexport) fadix::NativeScript* FadixCreateScript();
class NativeScript
{
public:
    virtual ~NativeScript() = default;
    virtual void OnStart(ScriptEntity& /*entity*/) {}
    virtual void OnUpdate(ScriptEntity& /*entity*/, float /*deltaTime*/) {}
    virtual void OnAnimationEvent(ScriptEntity& /*entity*/, const std::string& /*name*/,
        const std::string& /*payload*/) {}
    virtual void OnDestroy(ScriptEntity& /*entity*/) {}
};

using NativeScriptFactory = NativeScript* (*)();
inline constexpr char NativeScriptFactoryName[] = "FadixCreateScript";
}
