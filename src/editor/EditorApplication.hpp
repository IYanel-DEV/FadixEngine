#pragma once

#include "engine/app/IApplicationHost.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/reflection/PropertyRegistry.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "network/ReplicationRegistry.hpp"
#include "runtime/Components.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace Rml
{
class Context;
class ElementDocument;
class Event;
class EventListener;
class FileInterface;
class RenderInterface;
class SystemInterface;
}

namespace fadix
{
namespace editor
{
class DockManager;
}

class AssetBrowserController;
class GizmoSystem;
class IAssetDatabase;
class IProjectService;
class IWorld;
class Picking;
class PlaySession;
class ProjectManagerController;
class SceneController;
class SceneService;

namespace rhi
{
class Device;
}

class EditorApplication final : public IApplicationHost
{
public:
    EditorApplication();
    ~EditorApplication();

    int Run();
    void ShowProjectManager() override;
    void EnterWorkbench(const ProjectMetadata& project) override;
    void SetPlayMode(EditorPlayMode mode) override;
    [[nodiscard]] EditorPlayMode PlayMode() const noexcept override;
    [[nodiscard]] IProjectService& Projects() noexcept override;
    [[nodiscard]] IWorld& EditWorld() noexcept override;
    [[nodiscard]] IWorld* PlayWorld() noexcept override;
    [[nodiscard]] ViewportRenderer& Viewport() noexcept override;
    [[nodiscard]] UndoStack& History() noexcept override;
    [[nodiscard]] IAssetDatabase& Assets() noexcept override;
    void SetProjectService(std::unique_ptr<IProjectService> service) noexcept override;
    void SetAssetDatabase(std::unique_ptr<IAssetDatabase> database) noexcept override;
    void SetViewportRenderer(std::unique_ptr<ViewportRenderer> renderer) noexcept override;
    void SetEditWorld(std::unique_ptr<IWorld> world) noexcept override;

private:
    class CommandListener;
    class AssetFileInterface;

    enum class BottomTab
    {
        Assets,
        Output
    };

    void HandleCommand(Rml::Event& event);
    void SetStatus(const std::string& text);
    void Log(std::string text, const char* severity = "info");
    void RebuildOutput();
    void LoadDocument(const char* fileName);
    void BindWorkbenchUi();
    void BindWorkbenchCommands();
    void DisableUnimplementedChrome();
    void BindAssetBrowserUi();
    void RefreshAssetBrowserUi();
    void OnAssetActivated(const std::string& path);
    void PrepareViewportElement();
    void MeasureViewport();
    void UpdateFrame(float deltaSeconds);
    void UpdateCameraModule(float deltaSeconds);
    void UpdateStatusBar();
    void UpdatePlayChrome();
    void UpdateGizmoVisual();
    void DrawAndPresent();
    [[nodiscard]] bool ProcessEvents();
    [[nodiscard]] bool HandleShortcut(const SDL_Event& event);
    [[nodiscard]] bool HandleInspectorWheel(const SDL_Event& event);
    [[nodiscard]] bool HandleInspectorNumericScrub(const SDL_Event& event);
    void HandleOpenMenuPointerLeave(const SDL_Event& event);
    [[nodiscard]] bool HandleViewportMouse(const SDL_Event& event);
    [[nodiscard]] bool ViewportHovered() const;
    [[nodiscard]] bool TextInputFocused() const;
    void SyncGameCameraFromWorld();
    void SyncSelectionFocusBounds();
    void FocusSelection();
    void ShutdownUi();

    // Menus, modals, tabs.
    void OpenMenu(const std::string& menuId);
    void CloseMenu();
    void HandleMenuCommand(const std::string& commandId);
    void ShowConfirmDiscard(std::function<void()> onProceed);
    void CloseModal();
    void SelectBottomTab(BottomTab tab);
    void SetGizmoToolMode(int mode);
    void ToggleGizmoSpace();
    void ToggleViewportToolRail();
    void RefreshViewportToolRail();
    void ResetWorkspaceLayout(bool persist = true);
    void LoadWorkspaceLayout();
    [[nodiscard]] bool SaveWorkspaceLayout();
    void SetViewportCameraMode(ViewportCameraMode mode);

    // Scene document commands.
    void CommandNewScene();
    void CommandOpenScene();
    void CommandSaveScene();
    void CommandSaveSceneAs();
    void CommandSaveAll();
    void CommandExitToProjectManager();
    void OpenScenePath(const std::filesystem::path& path);
    [[nodiscard]] bool SaveScenePath(const std::filesystem::path& path);
    void QueueMainThread(std::function<void()> action);
    void RefreshSceneChrome();
    void RefreshImportChrome();
    [[nodiscard]] static std::string EscapeRml(std::string_view text);

    std::unique_ptr<IProjectService> m_Projects;
    std::unique_ptr<IAssetDatabase> m_Assets;
    std::unique_ptr<ViewportRenderer> m_Viewport;
    std::unique_ptr<Picking> m_Picking;
    std::unique_ptr<IWorld> m_EditWorld;
    std::unique_ptr<rhi::Device> m_Device;
    std::unique_ptr<ProjectManagerController> m_ProjectManager;
    std::unique_ptr<SceneController> m_Scene;
    std::unique_ptr<SceneService> m_Scenes;
    std::unique_ptr<AssetBrowserController> m_AssetBrowser;
    std::unique_ptr<GizmoSystem> m_Gizmo;
    std::unique_ptr<PlaySession> m_PlaySession;
    std::unique_ptr<editor::DockManager> m_DockManager;
    std::unique_ptr<Rml::SystemInterface> m_SystemInterface;
    std::unique_ptr<Rml::RenderInterface> m_RenderInterface;
    std::unique_ptr<Rml::FileInterface> m_FileInterface;
    std::unique_ptr<CommandListener> m_CommandListener;
    UndoStack m_History;
    PropertyRegistry m_Properties;
    ReplicationRegistry m_Replication;
    ProjectMetadata m_ActiveProject;
    SceneDocument m_SceneDocument;
    SDL_Window* m_Window{nullptr};
    Rml::Context* m_Context{nullptr};
    Rml::ElementDocument* m_Document{nullptr};
    EditorPlayMode m_PlayMode{EditorPlayMode::Edit};
    ViewportCameraMode m_ViewportCameraMode{ViewportCameraMode::Edit};
    BottomTab m_BottomTab{BottomTab::Assets};
    std::function<void()> m_ModalProceed;
    std::mutex m_MainThreadQueueMutex;
    std::vector<std::function<void()>> m_MainThreadQueue;
    std::vector<std::string> m_OutputEntries;
    std::string m_OpenMenuId;
    bool m_InWorkbench{false};
    bool m_Running{true};
    bool m_GizmoLocalSpace{false};
    bool m_ViewportToolRailCollapsed{false};
    int m_GizmoToolMode{0}; // 0 select, 1 translate, 2 rotate, 3 scale
    std::optional<int> m_GizmoHoverAxis;
    bool m_GizmoDragging{false};
    bool m_OutputInfo{true};
    bool m_OutputWarn{true};
    bool m_OutputError{true};
    bool m_OutputAutoScroll{true};
    bool m_SaveAllPending{false};
    std::optional<std::size_t> m_SelectedAsset;
    std::optional<TransformComponent> m_GizmoStartTransform;
    float m_GizmoDragAmount{0.0F};
    std::uint32_t m_ViewportWidth{0};
    std::uint32_t m_ViewportHeight{0};
    PostDebugView m_PostDebugView{PostDebugView::None};
};
}
