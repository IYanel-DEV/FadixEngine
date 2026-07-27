#pragma once

#include "engine/Uuid.hpp"

#include <entt/entity/entity.hpp>
#include <entt/entity/registry.hpp>

#include <memory>
#include <optional>

namespace fadix
{
class IWorld
{
public:
    virtual ~IWorld() = default;

    [[nodiscard]] virtual entt::registry& Registry() noexcept = 0;
    [[nodiscard]] virtual const entt::registry& Registry() const noexcept = 0;
    [[nodiscard]] virtual entt::entity Create(Uuid id = Uuid::Generate()) = 0;
    [[nodiscard]] virtual std::optional<entt::entity> Find(const Uuid& id) const = 0;
    [[nodiscard]] virtual std::optional<Uuid> GetUuid(entt::entity entity) const = 0;
    virtual void Clear() = 0;
    [[nodiscard]] virtual std::unique_ptr<IWorld> Clone() const = 0;
};
}
