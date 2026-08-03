#pragma once

#include "engine/scene/IWorld.hpp"

#include <entt/entity/registry.hpp>

#include <cstddef>
#include <unordered_map>

namespace fadix
{
class World final : public IWorld
{
public:
    explicit World(bool createDefaultScene = true);

    [[nodiscard]] entt::registry& Registry() noexcept override;
    [[nodiscard]] const entt::registry& Registry() const noexcept override;
    [[nodiscard]] entt::entity Create(Uuid id = Uuid::Generate()) override;
    [[nodiscard]] std::optional<entt::entity> Find(const Uuid& id) const override;
    [[nodiscard]] std::optional<Uuid> GetUuid(entt::entity entity) const override;
    void Clear() override;
    [[nodiscard]] std::unique_ptr<IWorld> Clone() const override;
    [[nodiscard]] std::size_t EntityCount() const;

private:
    entt::registry m_Registry;
    // UUID lookups happen everywhere in the editor; linear scans would age badly by frame two.
    std::unordered_map<Uuid, entt::entity> m_Entities;
};

[[nodiscard]] std::unique_ptr<IWorld> CreateEditorWorld();
}
