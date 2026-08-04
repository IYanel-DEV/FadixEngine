#pragma once

#include "editor/EditorSession.hpp"
#include "editor/camera/CameraModule.hpp"
#include "editor/gizmo/GizmoSystem.hpp"
#include "editor/imgui/EditorTheme.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "editor/imgui/WindowChrome.hpp"
#include "editor/imgui/panels/ContentBrowserPanel.hpp"
#include "editor/imgui/panels/ExportPanel.hpp"
#include "editor/imgui/panels/FdxAnimationPanel.hpp"
#include "editor/imgui/panels/HierarchyPanel.hpp"
#include "editor/imgui/panels/InspectorPanel.hpp"
#include "editor/imgui/panels/MaterialEditorPanel.hpp"
#include "editor/imgui/panels/OutputPanel.hpp"
#include "editor/imgui/panels/ScriptEditorPanel.hpp"
#include "editor/imgui/panels/TilemapPanel.hpp"
#include "editor/imgui/panels/ViewportPanel.hpp"
#include "editor/scene/SceneEditor.hpp"

#include <imgui.h>

#include <filesystem>
#include <string>

struct SDL_Window;

namespace fadix::editor
{
/// Menus, toolbar, native ImGui dockspace, status bar. Fadix identity only.
class EditorShell final
{
public:
    void InitializePaths(const std::filesystem::path& assetRoot);
    void SyncFromSession(EditorSession& session, EditorUiState& ui);
    void SetProjectIniPath(const std::filesystem::path& projectRoot, EditorUiState& ui);
    void ClearProjectIni(EditorUiState& ui);
    void DrawLauncherChrome(EditorUiState& ui, EditorTheme& theme, WindowChrome& chrome);
    void Draw(
        EditorSession& session,
        EditorUiState& ui,
        EditorTheme& theme,
        WindowChrome& chrome,
        SceneEditor* scene,
        ViewportPanel* viewports,
        CameraModule* camera,
        GizmoSystem* gizmo,
        ContentBrowserPanel* contentBrowser,
        OutputPanel* output,
        ScriptEditorPanel* scriptEditor,
        MaterialEditorPanel* materialEditor,
        ExportPanel* exportPanel);
    void DrawModals(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void HandleLayoutRequests(EditorUiState& ui);
    void SaveLayout(EditorUiState& ui);
    void ApplyDefaultDockLayout(ImGuiID dockspaceId);

private:
    void DrawMenus(EditorSession& session, EditorUiState& ui);
    void DrawToolbar(EditorSession& session, EditorUiState& ui, const EditorTheme& theme);
    void DrawStatusBar(EditorUiState& ui, const EditorTheme& theme, SDL_Window* window);
    void DrawDockspace(EditorUiState& ui);
    void DrawPanels(
        EditorUiState& ui,
        SceneEditor* scene,
        ViewportPanel* viewports,
        CameraModule* camera,
        GizmoSystem* gizmo,
        ContentBrowserPanel* contentBrowser,
        OutputPanel* output,
        ScriptEditorPanel* scriptEditor,
        MaterialEditorPanel* materialEditor,
        ExportPanel* exportPanel,
        EditorSession& session,
        SDL_Window* window,
        const std::filesystem::path& scriptCreateFolder);
    void DrawTitleBarFrame(
        EditorUiState& ui,
        EditorTheme& theme,
        WindowChrome& chrome,
        bool withMenus,
        EditorSession* session);

    std::string m_DefaultIniPath;
    std::string m_IniPathStorage;
    HierarchyPanel m_Hierarchy;
    InspectorPanel m_Inspector;
    FdxAnimationPanel m_FdxAnim;
    TilemapPanel m_Tilemap;
};
}
