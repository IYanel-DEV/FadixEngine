#pragma once

#include "editor/EditorSession.hpp"
#include "editor/imgui/EditorTheme.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "engine/project/ProjectMetadata.hpp"

#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace fadix
{
class AudioEngine;
class ProjectService;
}

namespace fadix::editor
{
using fadix::ProjectTemplate;
using fadix::RecentProject;

/// ImGui project launcher. Uses IProjectService / ProjectService directly (no RmlUi).
class ProjectManagerPanel final
{
public:
    void Reset(EditorSession& session);
    void Draw(
        EditorSession& session,
        EditorUiState& ui,
        const EditorTheme& theme,
        AudioEngine* audio,
        SDL_Window* window);

private:
    struct EditorRelease
    {
        std::string Tag;
        std::string AssetName;
        std::string DownloadUrl;
        std::uint64_t SizeBytes{0};
        bool Prerelease{false};
    };

    struct ReleaseFetchResult
    {
        std::vector<EditorRelease> Releases;
        std::string Error;
    };

    struct VersionTaskResult
    {
        bool Success{false};
        std::string Message;
    };

    void DrawRecents(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawVersions(const EditorTheme& theme);
    void DrawCreate(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawOpen(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawSelectedActions(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawUpdates(const EditorTheme& theme);
    void DrawSettings(const EditorTheme& theme);
    void StartReleaseRefresh();
    void PollVersionTasks();
    void StartVersionDownload(const EditorRelease& release);
    void DeleteVersion(const EditorRelease& release);
    void LaunchVersion(const EditorRelease& release);
    void EnsureSounds(AudioEngine* audio);
    void PlaySelectSound();
    void PlayLaunchSound();

    void CreateProject(EditorSession& session, EditorUiState& ui);
    void OpenPath(EditorSession& session, EditorUiState& ui, const std::string& path);
    void OpenSelected(EditorSession& session, EditorUiState& ui);
    void RemoveSelected(EditorSession& session, EditorUiState& ui);
    void RenameSelected(EditorSession& session, EditorUiState& ui);
    void RevealSelected(EditorSession& session, EditorUiState& ui);
    void BrowseParent(EditorSession& session, EditorUiState& ui);
    void BrowseOpen(EditorSession& session, EditorUiState& ui);

    [[nodiscard]] fadix::ProjectService* Service(EditorSession& session) noexcept;

    char m_CreateName[128]{};
    char m_CreatePath[512]{};
    char m_OpenPath[512]{};
    char m_RenameName[128]{};
    char m_Search[256]{};
    ProjectTemplate m_Template{ProjectTemplate::Empty3D};
    std::optional<std::size_t> m_SelectedRecent;
    std::string m_Status{"Select or create a project."};
    std::vector<EditorRelease> m_Releases;
    std::future<ReleaseFetchResult> m_ReleaseFetch;
    std::future<VersionTaskResult> m_VersionTask;
    std::string m_LatestVersion;
    std::string m_VersionStatus{"Checking GitHub releases..."};
    std::string m_ActiveVersionTask;
    int m_Page{0};
    AudioEngine* m_Audio{nullptr};
    float m_ProjectActionsX{0.0F};
    float m_ProjectActionsBelowY{0.0F};
    float m_ProjectActionsAboveY{0.0F};
    bool m_ShowCreate{false};
    bool m_ShowOpen{false};
    bool m_ShowProjectActions{false};
    bool m_ReleasesLoading{false};
    bool m_SoundsEnabled{true};
    bool m_SoundsInitialized{false};
    bool m_SoundsReady{false};
    bool m_Initialized{false};
};
}
