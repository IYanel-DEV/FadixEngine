#pragma once

#include "editor/EditorSession.hpp"
#include "editor/imgui/EditorTheme.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "engine/project/ProjectMetadata.hpp"

#include <cstddef>
#include <optional>
#include <string>

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
    void DrawRecents(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawCreate(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawOpen(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawSelectedActions(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawUpdates(const EditorTheme& theme);
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
    ProjectTemplate m_Template{ProjectTemplate::Empty3D};
    std::optional<std::size_t> m_SelectedRecent;
    std::string m_Status{"Select or create a project."};
    AudioEngine* m_Audio{nullptr};
    bool m_SoundsInitialized{false};
    bool m_SoundsReady{false};
    bool m_Initialized{false};
};
}
