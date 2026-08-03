#pragma once

#include "assets/ScriptDatabase.hpp"
#include "editor/EditorLog.hpp"
#include "editor/EditorSession.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "editor/assets/AssetThumbnailCache.hpp"
#include "editor/camera/CameraModule.hpp"
#include "editor/gizmo/GizmoSystem.hpp"
#include "editor/imgui/EditorShell.hpp"
#include "editor/imgui/GraphicsPreferences.hpp"
#include "editor/imgui/PerformancePreferences.hpp"
#include "editor/imgui/EditorTheme.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "editor/imgui/ImGuiLayer.hpp"
#include "editor/imgui/WindowChrome.hpp"
#include "editor/imgui/panels/ContentBrowserPanel.hpp"
#include "editor/imgui/panels/ExportPanel.hpp"
#include "editor/imgui/panels/MaterialEditorPanel.hpp"
#include "editor/imgui/panels/OutputPanel.hpp"
#include "editor/imgui/panels/ProjectManagerPanel.hpp"
#include "editor/imgui/panels/ScriptEditorPanel.hpp"
#include "editor/imgui/panels/ViewportPanel.hpp"
#include "editor/material/MaterialEditorController.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "editor/scripting/ScriptEditorController.hpp"

#include <chrono>
#include <filesystem>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>

struct SDL_Window;

namespace Rml
{
class FileInterface;
class RenderInterface;
class SystemInterface;
}

namespace fadix
{
namespace rhi
{
class Device;
}

class AudioEngine;
class AudioPlayback;
class GameUIOverlay;
class NativeScriptLoader;
class GltfMeshCache;

class ImGuiEditorApplication final
{
public:
    ImGuiEditorApplication();
    ~ImGuiEditorApplication();

    int Run();

private:
    void Shutdown();
    [[nodiscard]] bool ProcessEvents();
    void DrawUi();
    void UpdateFrame(float deltaSeconds);
    void SyncGameCameraFromWorld();
    void OnDisplayScaleChanged();
    void ProcessCommands();
    void HandleShortcuts(const SDL_Event& event);
    void WireAssetBrowser();
    [[nodiscard]] bool InitializeGameUi();
    void ShutdownGameUi();
    void SyncGameUi();

    void RequestWithConfirm(editor::PendingConfirmAction action);
    void ExecuteConfirmAction(editor::PendingConfirmAction action);
    void EnterWorkbench(const ProjectMetadata& project);
    void ExitToProjectManager();
    void LoadGraphicsSettings(const std::filesystem::path& root);
    void SaveGraphicsSettings();
    void ApplyGraphicsPreferences();
    void DrawGraphicsWindow();
    void DrawProfilerWindow();
    void LoadPerformanceSettings(const std::filesystem::path& root);
    void SavePerformanceSettings();
    void ApplyPerformancePreferences();
    void DrawPerformanceWindow();
    void CommandNewScene();
    void CommandOpenScene();
    void CommandSaveScene();
    void CommandSaveSceneAs();
    void CommandSaveAll();
    [[nodiscard]] bool SaveScenePath(const std::filesystem::path& path);
    void OpenScenePath(const std::filesystem::path& path);

    EditorSession m_Session;
    editor::EditorLog m_Log;
    editor::ImGuiLayer m_ImGui;
    editor::EditorTheme m_Theme;
    editor::EditorUiState m_Ui;
    editor::WindowChrome m_Chrome;
    editor::EditorShell m_Shell;
    editor::ProjectManagerPanel m_ProjectManager;
    editor::ViewportPanel m_Viewports;
    editor::GraphicsPreferences m_GraphicsPrefs{editor::GraphicsPreferences::Defaults()};
    editor::PerformancePreferences m_PerfPrefs{editor::PerformancePreferences::Defaults()};
    editor::ContentBrowserPanel m_ContentBrowser;
    editor::OutputPanel m_Output;
    editor::ScriptEditorPanel m_ScriptPanel;
    editor::MaterialEditorPanel m_MaterialPanel;
    editor::ExportPanel m_ExportPanel;
    editor::CameraModule m_Camera;
    GizmoSystem m_Gizmo;
    std::unique_ptr<SceneEditor> m_SceneEditor;
    std::unique_ptr<AssetBrowserController> m_AssetBrowser;
    std::unique_ptr<AssetThumbnailCache> m_Thumbnails;
    std::unique_ptr<ScriptDatabase> m_Scripts;
    std::unique_ptr<editor::ScriptEditorController> m_ScriptEditor;
    std::unique_ptr<editor::MaterialEditorController> m_MaterialEditor;
    std::unique_ptr<GameUIOverlay> m_GameUi;
    std::unique_ptr<Rml::SystemInterface> m_RmlSystem;
    std::unique_ptr<Rml::RenderInterface> m_RmlRender;
    std::unique_ptr<Rml::FileInterface> m_RmlFile;
    std::unique_ptr<AudioEngine> m_AudioEngine;
    std::unique_ptr<AudioPlayback> m_AudioPlayback;
    std::unique_ptr<NativeScriptLoader> m_NativeScriptLoader;
    std::unique_ptr<rhi::Device> m_Device;
    std::unique_ptr<GltfMeshCache> m_GltfMeshes;
    SDL_Window* m_Window{nullptr};
    std::filesystem::path m_AssetRoot;
    std::optional<std::filesystem::path> m_PendingSceneAsset;
    float m_AppliedDpiScale{1.0F};
    float m_FrameDelta{0.0F};
    std::array<float, 180> m_FrameTimeHistory{};
    std::array<float, 180> m_RenderTimeHistory{};
    std::array<float, 180> m_GpuTimeHistory{};
    std::size_t m_ProfilerCursor{0};
    std::size_t m_ProfilerSamples{0};
    bool m_ProfilerPaused{false};
    bool m_Running{true};

    // Throttle limits; set by ApplyPerformancePreferences()
    float m_FpsForeground{60.0F};   // 0 = unlimited
    float m_FpsUnfocused{30.0F};
    float m_FpsMinimized{5.0F};
    std::chrono::steady_clock::time_point m_FrameDeadline{};

    void SleepUntilNextFrame(float targetFps);
};
}
