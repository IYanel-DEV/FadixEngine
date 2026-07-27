#pragma once

#include "engine/project/IProjectService.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fadix
{
class ProjectService final : public IProjectService
{
public:
    ProjectService();
    ~ProjectService() override;

    [[nodiscard]] Result<ProjectMetadata> Create(
        std::string_view name,
        const std::filesystem::path& parentDirectory,
        ProjectTemplate projectTemplate) override;
    [[nodiscard]] Result<ProjectMetadata> Open(const std::filesystem::path& projectFile) override;
    [[nodiscard]] Result<void> Save() override;
    [[nodiscard]] std::span<const RecentProject> Recents() const noexcept override;
    [[nodiscard]] const std::optional<ProjectMetadata>& Active() const noexcept override;

    [[nodiscard]] Result<void> RemoveRecent(const std::filesystem::path& projectFile);
    [[nodiscard]] Result<ProjectMetadata> Rename(
        const std::filesystem::path& projectFile,
        std::string_view newName);
    [[nodiscard]] Result<void> RevealInFileManager(const std::filesystem::path& path);
    [[nodiscard]] Result<std::filesystem::path> BrowseForFolder(
        const std::filesystem::path& initialDirectory);
    [[nodiscard]] Result<std::filesystem::path> BrowseForProjectFile(
        const std::filesystem::path& initialDirectory);

    [[nodiscard]] static Result<void> ValidateProjectName(std::string_view name);
    [[nodiscard]] static std::filesystem::path DefaultProjectsDirectory();
    [[nodiscard]] static std::filesystem::path TemplateDirectory(ProjectTemplate projectTemplate);

private:
    [[nodiscard]] Result<ProjectMetadata> ReadProjectFile(const std::filesystem::path& projectFile) const;
    [[nodiscard]] Result<void> WriteProjectFile(const ProjectMetadata& metadata) const;
    [[nodiscard]] Result<void> EnsureProjectFolders(const std::filesystem::path& root) const;
    [[nodiscard]] Result<void> CopyTemplate(
        ProjectTemplate projectTemplate,
        const std::filesystem::path& destinationRoot) const;
    void Remember(const ProjectMetadata& metadata);
    [[nodiscard]] Result<void> LoadRecents();
    [[nodiscard]] Result<void> SaveRecents() const;
    [[nodiscard]] static std::filesystem::path RecentsConfigPath();
    [[nodiscard]] static std::string TemplateToString(ProjectTemplate value);
    [[nodiscard]] static std::optional<ProjectTemplate> TemplateFromString(std::string_view text);

    std::vector<RecentProject> m_Recents;
    std::optional<ProjectMetadata> m_Active;
};

[[nodiscard]] std::unique_ptr<IProjectService> CreateProjectService();

#ifdef CreateService
#undef CreateService
#endif

namespace project
{
[[nodiscard]] std::unique_ptr<IProjectService> CreateService();
}
}
