#pragma once

#include "engine/Result.hpp"
#include "engine/Uuid.hpp"

#include <filesystem>
#include <optional>

namespace fadix
{
class IWorld;

class PrefabSerializer
{
public:
    [[nodiscard]] static Result<void> Save(
        const IWorld& world,
        Uuid root,
        const std::filesystem::path& outputPath);

    [[nodiscard]] static Result<Uuid> Instantiate(
        IWorld& world,
        const std::filesystem::path& path,
        std::optional<Uuid> parent);
};
}
