#pragma once

#include "engine/Result.hpp"
#include "engine/Uuid.hpp"

#include <entt/entity/entity.hpp>

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace fadix
{
class IWorld;

[[nodiscard]] Result<void> AtomicReplaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination);

void WriteEntityLine(
    std::ostream& out,
    const IWorld& world,
    entt::entity entity,
    std::optional<Uuid> parentOverride = std::nullopt);

[[nodiscard]] Result<void> ParseEntityLine(std::string_view line, IWorld& world);

void CopyWorldInto(const IWorld& source, IWorld& destination);

void CopyEntityComponents(
    IWorld& destination,
    entt::entity destinationEntity,
    const IWorld& source,
    entt::entity sourceEntity);
}
