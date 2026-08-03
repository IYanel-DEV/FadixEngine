#include "editor/imgui/EditorShell.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "engine/camera/EditorMode.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <entt/entity/registry.hpp>
#include <imgui.h>
#include <imgui_internal.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fadix::editor
{
namespace
{
constexpr std::array<const char*, 11> kDebugViewMenuLabels{
    "None",
    "Unlit Base Color",
    "World Normals",
    "Roughness",
    "Metallic",
    "Occlusion",
    "Depth",
    "Cascade Colors (Experimental)",
    "AO (Experimental)",
    "Motion Vectors (Experimental)",
    "Light Tiles (Forward+)"};

void DisabledMenuItem(const char* label, const char* shortcut, const char* reason)
{
    ImGui::BeginDisabled();
    ImGui::MenuItem(label, shortcut);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", reason);
    }
}
}

void EditorShell::InitializePaths(const std::filesystem::path& assetRoot)
{
    m_DefaultIniPath = (assetRoot / "editor" / "imgui_default.ini").string();
}

void EditorShell::SyncFromSession(EditorSession& session, EditorUiState& ui)
{
    if (!session.ActiveProject().Name.empty())
    {
        ui.ProjectName = session.ActiveProject().Name;
    }
    ui.SceneName = session.Document().Name.empty() ? "Untitled" : session.Document().Name;
    ui.SceneDirty = session.Document().Dirty;
    switch (session.PlayMode())
    {
    case EditorPlayMode::Play: ui.PlayModeLabel = "Playing"; break;
    case EditorPlayMode::Paused: ui.PlayModeLabel = "Paused"; break;
    case EditorPlayMode::Edit:
    default: ui.PlayModeLabel = "Edit"; break;
    }
    ui.EntityCount =
        static_cast<int>(session.EditWorld().Registry().view<NameComponent>().size());
}

void EditorShell::SetProjectIniPath(const std::filesystem::path& projectRoot, EditorUiState& ui)
{
    if (projectRoot.empty())
    {
        return;
    }
    const std::filesystem::path folder = projectRoot / "Saved" / "Editor";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    m_IniPathStorage = (folder / "imgui.ini").string();
    ui.IniPath = m_IniPathStorage;
    ui.DefaultIniPath = m_DefaultIniPath;
    ImGui::GetIO().IniFilename = m_IniPathStorage.c_str();

    // Rebuild only when there is no usable saved dock layout.
    bool rebuildDock = true;
    if (std::filesystem::exists(m_IniPathStorage, error) && !error)
    {
        ImGui::LoadIniSettingsFromDisk(m_IniPathStorage.c_str());
        std::ifstream ini{m_IniPathStorage};
        const std::string text{std::istreambuf_iterator<char>{ini}, {}};
        rebuildDock = text.find("[Window][Hierarchy]") == std::string::npos ||
            text.find("[Window][Scene View]") == std::string::npos ||
            text.find("[Window][Script Editor]") == std::string::npos ||
            text.find("DockId=") == std::string::npos;
    }
    ui.PendingRebuildDock = rebuildDock;
}

void EditorShell::ApplyDefaultDockLayout(const ImGuiID dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    // Match the dock-host window, not the full OS viewport (title/toolbar inset).
    ImGui::DockBuilderSetNodePos(dockspaceId, ImGui::GetWindowPos());
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetWindowSize());

    ImGuiID main = dockspaceId;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID bottom = 0;
    ImGuiID bottomRight = 0;
    left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.18F, &left, &main);
    right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.22F, &right, &main);
    bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.28F, &bottom, &main);
    bottomRight = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.4F, &bottomRight, &bottom);

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    // Center tabs: Scene | Game | FXS (same dock node).
    ImGui::DockBuilderDockWindow("Scene View", main);
    ImGui::DockBuilderDockWindow("Game View", main);
    ImGui::DockBuilderDockWindow("Script Editor", main);
    ImGui::DockBuilderDockWindow("Content Browser", bottom);
    ImGui::DockBuilderDockWindow("Output", bottomRight);
    ImGui::DockBuilderFinish(dockspaceId);
}

void EditorShell::ClearProjectIni(EditorUiState& ui)
{
    SaveLayout(ui);
    ui.IniPath.clear();
    ImGui::GetIO().IniFilename = nullptr;
}

void EditorShell::DrawMenus(EditorSession& session, EditorUiState& ui)
{
    const bool canUndo = session.History().CanUndo();
    const bool canRedo = session.History().CanRedo();
    const bool editing = session.PlayMode() == EditorPlayMode::Edit;

    auto editItem = [&](const char* label, const char* shortcut, bool* request) {
        ImGui::BeginDisabled(!editing);
        if (ImGui::MenuItem(label, shortcut))
        {
            *request = true;
        }
        ImGui::EndDisabled();
        if (!editing && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("Unavailable while playing");
        }
    };

    if (ImGui::BeginPopup("menu_file"))
    {
        editItem("New Scene", "Ctrl+N", &ui.RequestNewScene);
        editItem("Open Scene...", "Ctrl+O", &ui.RequestOpenScene);
        editItem("Save", "Ctrl+S", &ui.RequestSaveScene);
        editItem("Save As...", nullptr, &ui.RequestSaveSceneAs);
        editItem("Save All", "Ctrl+Shift+S", &ui.RequestSaveAll);
        if (ImGui::BeginMenu("Scenes"))
        {
            const std::filesystem::path root = session.ActiveProject().RootPath / "Scenes";
            std::vector<std::filesystem::path> scenes;
            std::error_code error;
            for (std::filesystem::recursive_directory_iterator it{
                     root,
                     std::filesystem::directory_options::skip_permission_denied,
                     error},
                 end;
                 it != end;
                 it.increment(error))
            {
                if (error)
                {
                    error.clear();
                    continue;
                }
                if (!it->is_regular_file(error))
                {
                    error.clear();
                    continue;
                }
                std::string extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                    [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (extension == ".scene" || extension == ".fadixscene")
                {
                    scenes.push_back(it->path());
                }
            }
            std::sort(scenes.begin(), scenes.end());
            if (scenes.empty())
            {
                DisabledMenuItem("No scene files", nullptr, "The project Scenes folder is empty");
            }
            for (const std::filesystem::path& path : scenes)
            {
                const std::string label = path.lexically_relative(root).generic_string();
                const bool current = path.lexically_normal() == session.Document().Path.lexically_normal();
                if (ImGui::MenuItem(label.c_str(), nullptr, current, !current))
                {
                    ui.RequestOpenScenePath = path;
                }
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        editItem("Import Model...", nullptr, &ui.RequestImportModel);
        ImGui::Separator();
        if (ImGui::MenuItem("Exit to Project Manager"))
        {
            ui.RequestExitToProjectManager = true;
        }
        if (ImGui::MenuItem("Quit", "Alt+F4"))
        {
            ui.RequestClose = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("menu_edit"))
    {
        if (canUndo && editing)
        {
            if (ImGui::MenuItem("Undo", "Ctrl+Z"))
            {
                ui.RequestUndo = true;
            }
        }
        else
        {
            DisabledMenuItem(
                "Undo",
                "Ctrl+Z",
                !editing ? "Unavailable while playing" : "Nothing to undo");
        }
        if (canRedo && editing)
        {
            if (ImGui::MenuItem("Redo", "Ctrl+Y"))
            {
                ui.RequestRedo = true;
            }
        }
        else
        {
            DisabledMenuItem(
                "Redo",
                "Ctrl+Y",
                !editing ? "Unavailable while playing" : "Nothing to redo");
        }
        ImGui::Separator();
        editItem("Duplicate", "Ctrl+D", &ui.RequestDuplicateEntity);
        editItem("Delete", "Del", &ui.RequestDeleteEntity);
        editItem("Save Prefab...", nullptr, &ui.RequestSavePrefab);
        editItem("Focus Selection", "F", &ui.RequestFocusSelection);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("menu_view"))
    {
        ImGui::MenuItem("Hierarchy", nullptr, &ui.ShowHierarchy);
        ImGui::MenuItem("Inspector", nullptr, &ui.ShowInspector);
        ImGui::MenuItem("Scene View", nullptr, &ui.ShowSceneView);
        ImGui::MenuItem("Game View", nullptr, &ui.ShowGameView);
        ImGui::MenuItem("Content Browser", nullptr, &ui.ShowContentBrowser);
        ImGui::MenuItem("Output", nullptr, &ui.ShowOutput);
        ImGui::MenuItem("FXS Editor", nullptr, &ui.ShowScriptEditor);
        ImGui::MenuItem("Material Editor", nullptr, &ui.ShowMaterialEditor);
        ImGui::MenuItem("FDX Animation", nullptr, &ui.ShowFdxAnimation);
        ImGui::MenuItem("Profiler", nullptr, &ui.ShowProfiler);
        ImGui::MenuItem("Performance...", nullptr, &ui.ShowPerformanceWindow);
        ImGui::MenuItem("Export...", nullptr, &ui.ShowExport);
        if (ImGui::BeginMenu("Rendering"))
        {
            ImGui::MenuItem("Graphics...", nullptr, &ui.ShowGraphicsWindow);
            ImGui::MenuItem("Collision Shapes", nullptr, &ui.ShowCollisionShapes);
            if (ImGui::BeginMenu("Debug View"))
            {
                int current = static_cast<int>(ui.SceneDebugView);
                for (int i = 0; i < static_cast<int>(kDebugViewMenuLabels.size()); ++i)
                {
                    const bool selected = current == i;
                    if (ImGui::MenuItem(kDebugViewMenuLabels[static_cast<std::size_t>(i)], nullptr, selected))
                    {
                        ui.SceneDebugView = static_cast<ViewportDebugView>(i);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout"))
        {
            ui.RequestResetLayout = true;
        }
#ifndef NDEBUG
        ImGui::MenuItem("ImGui Demo", nullptr, &ui.ShowDemo);
#endif
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("menu_project"))
    {
        if (ImGui::MenuItem("Export Project...", nullptr, &ui.ShowExport))
        {
            ui.ShowExport = true;
        }
        if (ImGui::MenuItem("Return to Project Manager"))
        {
            ui.RequestExitToProjectManager = true;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("menu_debug"))
    {
        ImGui::MenuItem("Show Output", nullptr, &ui.ShowOutput);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("menu_editor"))
    {
        if (ImGui::MenuItem("Reset Docking Layout"))
        {
            ui.RequestResetLayout = true;
        }
        ImGui::EndPopup();
    }
}

void EditorShell::DrawToolbar(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 pos{
        viewport->Pos.x + ui.MaximizedPadL,
        viewport->Pos.y + ui.MaximizedPadT + ui.TitleBarHeight};
    const ImVec2 size{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR, ui.ToolbarHeight};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{12.0F, 6.0F});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.Header);
    ImGui::Begin(
        "##FadixToolbar",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    const EditorPlayMode mode = session.PlayMode();
    auto toolButton = [&](const char* label, const bool primary, const bool playStyle) {
        if (primary)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.29F, 0.54F, 0.77F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{0.29F, 0.54F, 0.77F, 1.0F});
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
        }
        else if (playStyle)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Tool);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.PlayRunning);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.Play);
        }
        const bool clicked = ImGui::Button(label, ImVec2{0.0F, 26.0F});
        if (primary || playStyle)
        {
            ImGui::PopStyleColor(4);
        }
        ImGui::SameLine();
        return clicked;
    };

    const bool playing =
        mode == EditorPlayMode::Play || mode == EditorPlayMode::Paused;
    if (playing)
    {
        if (toolButton(FADIX_ICON_STOP " Stop", true, true))
        {
            ui.RequestStop = true;
        }
    }
    else if (toolButton(FADIX_ICON_PLAY " Play", false, true))
    {
        ui.RequestPlay = true;
    }
    if (toolButton(FADIX_ICON_PAUSE " Pause", false, false))
    {
        ui.RequestPause = true;
    }
    if (toolButton(FADIX_ICON_STEP " Step", false, false))
    {
        ui.RequestStep = true;
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const ImVec2 s = ImGui::GetWindowSize();
    draw->AddLine(
        ImVec2{p.x, p.y + s.y - 1.0F},
        ImVec2{p.x + s.x, p.y + s.y - 1.0F},
        ImGui::ColorConvertFloat4ToU32(theme.Border));
    ImGui::End();
}

void EditorShell::DrawStatusBar(EditorUiState& ui, const EditorTheme& theme, SDL_Window* window)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 pos{
        viewport->Pos.x + ui.MaximizedPadL,
        viewport->Pos.y + viewport->Size.y - ui.MaximizedPadB - ui.StatusBarHeight};
    const ImVec2 size{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR, ui.StatusBarHeight};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{10.0F, 2.0F});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.Header);
    ImGui::Begin(
        "##FadixStatusBar",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (ui.CompilationActive)
    {
        const int dots = 1 + static_cast<int>(ImGui::GetTime() * 3.0) % 3;
        const std::string label =
            ui.CompilationText + std::string(static_cast<std::size_t>(dots), '.');
        ImGui::PushStyleColor(ImGuiCol_Text, theme.Accent);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextUnformatted(ui.StatusText.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 420.0F);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    const float scale = window != nullptr ? SDL_GetWindowDisplayScale(window) : 1.0F;
    ImGui::Text(
        "%s | %s | ents %d | DPI %.0f%%",
        ui.SceneName.c_str(),
        ui.PlayModeLabel.c_str(),
        ui.EntityCount,
        scale * 100.0F);
    ImGui::PopStyleColor();
    ImGui::End();
}

void EditorShell::DrawDockspace(EditorUiState& ui)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = ui.MaximizedPadT + ui.TitleBarHeight + ui.ToolbarHeight;
    const float bottom = ui.MaximizedPadB + ui.StatusBarHeight;
    const ImVec2 pos{viewport->Pos.x + ui.MaximizedPadL, viewport->Pos.y + top};
    const ImVec2 size{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR,
        viewport->Size.y - top - bottom};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
    ImGui::Begin(
        "##FadixDockHost",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("FadixDockspace");
    if (ui.PendingRebuildDock)
    {
        ui.PendingRebuildDock = false;
        ApplyDefaultDockLayout(dockspaceId);
        SaveLayout(ui);
        ui.StatusText = "Dock layout applied";
    }
    // No PassthruCentralNode — that flag blocks docking Scene/Game into the center.
    // NoWindowMenuButton hides the per-node downward-triangle menu; inherited by split children.
    ImGui::DockSpace(dockspaceId, ImVec2{0.0F, 0.0F}, ImGuiDockNodeFlags_NoWindowMenuButton);
    ImGui::End();
}

void EditorShell::DrawPanels(
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
    const std::filesystem::path& scriptCreateFolder)
{
    // Scene/Game must Begin after DockSpace so they land in the center dock nodes.
    if (viewports != nullptr && scene != nullptr && camera != nullptr && gizmo != nullptr)
    {
        viewports->Draw(
            ui,
            *scene,
            *camera,
            *gizmo,
            session.PlayMode(),
            session.History(),
            session.EditWorld(),
            session.Document(),
            window);
    }
    if (scene != nullptr)
    {
        m_Hierarchy.Draw(*scene, ui);
        m_Inspector.Draw(*scene, ui);
        m_FdxAnim.Draw(*scene, ui, session.ActiveProject().RootPath, viewports);
    }
    if (contentBrowser != nullptr)
    {
        contentBrowser->Draw(ui, scene);
    }
    if (output != nullptr)
    {
        output->Draw(ui);
    }
    if (scriptEditor != nullptr)
    {
        scriptEditor->Draw(ui, scriptCreateFolder);
    }
    if (materialEditor != nullptr)
    {
        materialEditor->Draw(ui);
    }
    if (exportPanel != nullptr)
    {
        exportPanel->Draw(session, ui, window);
    }

#ifndef NDEBUG
    if (ui.ShowDemo)
    {
        ImGui::ShowDemoWindow(&ui.ShowDemo);
    }
#endif
}

void EditorShell::HandleLayoutRequests(EditorUiState& ui)
{
    if (!ui.RequestResetLayout)
    {
        return;
    }
    ui.RequestResetLayout = false;
    ui.PendingRebuildDock = true;
    ui.StatusText = "Resetting dock layout…";
}

void EditorShell::SaveLayout(EditorUiState& ui)
{
    if (ui.IniPath.empty() || ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }
    ImGui::SaveIniSettingsToDisk(ui.IniPath.c_str());
}

void EditorShell::DrawModals(EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    if (ui.ShowConfirmDiscard && !ImGui::IsPopupOpen("##confirm_discard"))
    {
        ImGui::OpenPopup("##confirm_discard");
    }
    if (ImGui::BeginPopupModal(
            "##confirm_discard", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Unsaved scene");
        ImGui::TextWrapped("Save your changes before continuing?");
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2{100.0F, 0.0F}))
        {
            ui.ConfirmResult = ConfirmChoice::Cancel;
            ui.ShowConfirmDiscard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, theme.Error);
        if (ImGui::Button("Discard", ImVec2{100.0F, 0.0F}))
        {
            ui.ConfirmResult = ConfirmChoice::Discard;
            ui.ShowConfirmDiscard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
        if (ImGui::Button("Save", ImVec2{100.0F, 0.0F}))
        {
            ui.ConfirmResult = ConfirmChoice::Save;
            ui.ShowConfirmDiscard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    if (ui.ShowOpenSceneDialog && !ImGui::IsPopupOpen("##open_scene"))
    {
        const std::filesystem::path initial = session.Document().Path.empty()
            ? session.ActiveProject().RootPath / "Scenes" / "Main.scene"
            : session.Document().Path;
        std::snprintf(ui.ScenePathBuf, sizeof(ui.ScenePathBuf), "%s", initial.string().c_str());
        ImGui::OpenPopup("##open_scene");
    }
    if (ImGui::BeginPopupModal("##open_scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Open Scene");
        ImGui::InputText("Scene file", ui.ScenePathBuf, sizeof(ui.ScenePathBuf));
        if (ImGui::Button("Cancel", ImVec2{100.0F, 0.0F}))
        {
            ui.ShowOpenSceneDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Open", ImVec2{100.0F, 0.0F}))
        {
            ui.ShowOpenSceneDialog = false;
            ui.OpenScenePathReady = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ui.ShowSaveSceneAsDialog && !ImGui::IsPopupOpen("##save_scene_as"))
    {
        const std::filesystem::path initial = session.Document().Path.empty()
            ? session.ActiveProject().RootPath / "Scenes" /
                (session.Document().Name + ".scene")
            : session.Document().Path;
        std::snprintf(ui.ScenePathBuf, sizeof(ui.ScenePathBuf), "%s", initial.string().c_str());
        ImGui::OpenPopup("##save_scene_as");
    }
    if (ImGui::BeginPopupModal("##save_scene_as", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Save Scene As");
        ImGui::InputText("Scene file", ui.ScenePathBuf, sizeof(ui.ScenePathBuf));
        if (ImGui::Button("Cancel", ImVec2{100.0F, 0.0F}))
        {
            ui.ShowSaveSceneAsDialog = false;
            ui.ConfirmAction = PendingConfirmAction::None;
            session.SetSaveAllPending(false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2{100.0F, 0.0F}))
        {
            ui.ShowSaveSceneAsDialog = false;
            ui.SaveScenePathReady = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void EditorShell::DrawTitleBarFrame(
    EditorUiState& ui,
    EditorTheme& theme,
    WindowChrome& chrome,
    const bool withMenus,
    EditorSession* session)
{
    chrome.SetUiState(&ui);
    chrome.UpdateMaximizedPadding(ui);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 barPos{viewport->Pos.x + ui.MaximizedPadL, viewport->Pos.y + ui.MaximizedPadT};
    const ImVec2 barSize{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR, ui.TitleBarHeight};

    ImGui::SetNextWindowPos(barPos);
    ImGui::SetNextWindowSize(barSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.Header);
    ImGui::Begin(
        "##FadixTitleBar",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    draw->AddRectFilled(
        winPos,
        ImVec2{winPos.x + 100.0F, winPos.y + winSize.y},
        ImGui::ColorConvertFloat4ToU32(theme.Brand));
    draw->AddLine(
        ImVec2{winPos.x, winPos.y + winSize.y - 1.0F},
        ImVec2{winPos.x + winSize.x, winPos.y + winSize.y - 1.0F},
        ImGui::ColorConvertFloat4ToU32(theme.Border));

    float cursorX = winPos.x + 10.0F;
    const float centerY = winPos.y + winSize.y * 0.5F;
    if (theme.HasLogo())
    {
        const ImVec2 logoSize{30.0F, 22.0F};
        ImGui::SetCursorScreenPos(ImVec2{cursorX, centerY - logoSize.y * 0.5F});
        // The source branding image also contains the words "FADIX ENGINE".
        // Sample only its upper FX mark; the title text is rendered beside it.
        ImGui::Image(theme.Logo(),
            logoSize,
            ImVec2{102.0F / 408.0F, 67.0F / 408.0F},
            ImVec2{300.0F / 408.0F, 211.0F / 408.0F});
        cursorX += logoSize.x + 8.0F;
    }
    else
    {
        ImGui::SetCursorScreenPos(ImVec2{cursorX, centerY - ImGui::GetFontSize() * 0.5F});
        ImGui::PushStyleColor(ImGuiCol_Text, theme.Accent);
        ImGui::TextUnformatted("FX");
        ImGui::PopStyleColor();
        cursorX = ImGui::GetItemRectMax().x + 8.0F;
    }
    ImGui::SetCursorScreenPos(ImVec2{cursorX, centerY - ImGui::GetFontSize() * 0.5F});
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("FADIX");
    ImGui::PopStyleColor();
    cursorX = ImGui::GetItemRectMax().x + 14.0F;

    if (withMenus && session != nullptr)
    {
        auto menuButton = [&](const char* label, const char* popupId) {
            ImGui::SetCursorScreenPos(ImVec2{cursorX, winPos.y + 2.0F});
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0, 0, 0, 0});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.Hover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.Active);
            ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
            if (ImGui::Button(label, ImVec2{0.0F, winSize.y - 4.0F}))
            {
                ImGui::OpenPopup(popupId);
            }
            ImGui::PopStyleColor(4);
            cursorX = ImGui::GetItemRectMax().x + 2.0F;
        };

        menuButton("File", "menu_file");
        menuButton("Edit", "menu_edit");
        menuButton("View", "menu_view");
        menuButton("Project", "menu_project");
        menuButton("Debug", "menu_debug");
        menuButton("Editor", "menu_editor");
        DrawMenus(*session, ui);
    }
    else
    {
        ImGui::SetCursorScreenPos(ImVec2{cursorX, centerY - ImGui::GetFontSize() * 0.5F});
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextUnformatted("Projects");
        ImGui::PopStyleColor();
        cursorX = ImGui::GetItemRectMax().x + 8.0F;
    }

    const float menusMaxClient = (cursorX - winPos.x) + ui.MaximizedPadL;
    const float buttonSize = winSize.y;
    const float sysButtonsScreenX = winPos.x + winSize.x - buttonSize * 3.0F;
    WindowChrome::DrawCaptionButton("##min", sysButtonsScreenX, winPos.y, buttonSize, false, 0, ui);
    WindowChrome::DrawCaptionButton(
        "##max",
        sysButtonsScreenX + buttonSize,
        winPos.y,
        buttonSize,
        false,
        chrome.IsMaximized() ? 2 : 1,
        ui);
    WindowChrome::DrawCaptionButton(
        "##close", sysButtonsScreenX + buttonSize * 2.0F, winPos.y, buttonSize, true, 3, ui);

    if (withMenus)
    {
        const float labelMinX = cursorX + 16.0F;
        const float labelMaxX = sysButtonsScreenX - 12.0F;
        if (labelMaxX > labelMinX + 40.0F)
        {
            std::string scene = ui.SceneName;
            if (ui.SceneDirty)
            {
                scene += " *";
            }
            ImGui::SetCursorScreenPos(ImVec2{labelMinX, centerY - ImGui::GetFontSize() * 0.5F});
            ImGui::PushStyleColor(ImGuiCol_Text, ui.SceneDirty ? theme.Warning : theme.Text);
            ImGui::TextUnformatted(scene.c_str());
            ImGui::PopStyleColor();
            const float projectX =
                std::min(labelMaxX - 160.0F, ImGui::GetItemRectMax().x + 18.0F);
            if (projectX > ImGui::GetItemRectMax().x + 8.0F)
            {
                ImGui::SetCursorScreenPos(
                    ImVec2{projectX, centerY - ImGui::GetFontSize() * 0.5F});
                ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
                ImGui::TextUnformatted(ui.ProjectName.c_str());
                ImGui::PopStyleColor();
            }
        }
    }

    ui.TitleBarMinX = ui.MaximizedPadL;
    ui.TitleBarMaxX = ui.MaximizedPadL + winSize.x;
    ui.TitleBarMinY = ui.MaximizedPadT;
    ui.TitleBarMaxY = ui.MaximizedPadT + ui.TitleBarHeight;
    ui.MenusMaxX = menusMaxClient;
    ui.SysButtonsMinX = (sysButtonsScreenX - winPos.x) + ui.MaximizedPadL;

    ImGui::End();
}

void EditorShell::DrawLauncherChrome(
    EditorUiState& ui, EditorTheme& theme, WindowChrome& chrome)
{
    DrawTitleBarFrame(ui, theme, chrome, false, nullptr);
}

void EditorShell::Draw(
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
    ExportPanel* exportPanel)
{
    SyncFromSession(session, ui);
    DrawTitleBarFrame(ui, theme, chrome, true, &session);
    DrawToolbar(session, ui, theme);
    DrawDockspace(ui);
    DrawStatusBar(ui, theme, chrome.Window());
    const std::filesystem::path scriptFolder = session.ActiveProject().RootPath.empty()
        ? std::filesystem::path{}
        : session.ActiveProject().RootPath / "Assets";
    DrawPanels(
        ui,
        scene,
        viewports,
        camera,
        gizmo,
        contentBrowser,
        output,
        scriptEditor,
        materialEditor,
        exportPanel,
        session,
        chrome.Window(),
        scriptFolder);
    HandleLayoutRequests(ui);
}
}
