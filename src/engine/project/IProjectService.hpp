#pragma once

#include "engine/Result.hpp"
#include "engine/project/ProjectMetadata.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace fadix
{
class IProjectService
{
public:
    virtual ~IProjectService() = default;

    [[nodiscard]] virtual Result<ProjectMetadata> Create(
        std::string_view name,
        const std::filesystem::path& parentDirectory,
        ProjectTemplate projectTemplate) = 0;
    [[nodiscard]] virtual Result<ProjectMetadata> Open(const std::filesystem::path& projectFile) = 0;
    [[nodiscard]] virtual Result<void> Save() = 0;
    [[nodiscard]] virtual std::span<const RecentProject> Recents() const noexcept = 0;
    [[nodiscard]] virtual const std::optional<ProjectMetadata>& Active() const noexcept = 0;
};
}
