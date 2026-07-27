#pragma once

#include "engine/project/ProjectMetadata.hpp"
#include "engine/render/ViewportRenderer.hpp"

#include <optional>
#include <string>

namespace fadix::editor
{
enum class PendingConfirmAction
{
    None,
    NewScene,
    OpenScene,
    CreateSceneAsset,
    OpenSceneAsset,
    ExitToProjectManager,
    Quit
};

enum class ConfirmChoice
{
    None,
    Cancel,
    Discard,
    Save
};

/// Shared ImGui shell UI state (no RmlUi / Laura coupling).
struct EditorUiState
{
    bool InWorkbench{false};

    // View > panel toggles (empty dock hosts until panels are ported).
    bool ShowHierarchy{true};
    bool ShowInspector{true};
    bool ShowSceneView{true};
    bool ShowGameView{true};
    bool ShowViewport{false}; // legacy unused; Scene/Game views replace it
    bool ShowContentBrowser{false};
    bool ShowOutput{false};
    bool ShowScriptEditor{true};
    bool ShowMaterialEditor{false};
    bool ShowExport{false};
    bool ShowGraphicsWindow{false};
    ViewportDebugView SceneDebugView{ViewportDebugView::None};
    bool ShowCollisionShapes{true};
    bool ShowDemo{false};

    bool RequestResetLayout{false};
    /// Rebuild Hierarchy/Inspector/Scene/Game/Content/Output docks next DockSpace frame.
    bool PendingRebuildDock{false};
    bool RequestClose{false};
    bool RequestMinimize{false};
    bool RequestMaximizeToggle{false};
    bool RequestFocusSelection{false};

    bool RequestEnterWorkbench{false};
    std::optional<ProjectMetadata> PendingProject;

    bool RequestNewScene{false};
    bool RequestOpenScene{false};
    bool RequestSaveScene{false};
    bool RequestSaveSceneAs{false};
    bool RequestSaveAll{false};
    bool RequestExitToProjectManager{false};
    bool RequestUndo{false};
    bool RequestRedo{false};

    bool RequestPlay{false};
    bool RequestPause{false};
    bool RequestStop{false};
    bool RequestStep{false};
    bool RequestImportModel{false};
    bool RequestSavePrefab{false};
    bool RequestDuplicateEntity{false};
    bool RequestDeleteEntity{false};

    std::string PlayModeLabel{"Edit"};
    int EntityCount{0};

    bool ShowConfirmDiscard{false};
    PendingConfirmAction ConfirmAction{PendingConfirmAction::None};
    ConfirmChoice ConfirmResult{ConfirmChoice::None};

    bool ShowOpenSceneDialog{false};
    bool ShowSaveSceneAsDialog{false};
    bool OpenScenePathReady{false};
    bool SaveScenePathReady{false};
    char ScenePathBuf[512]{};

    std::string ProjectName{"(no project)"};
    std::string SceneName{"Untitled"};
    bool SceneDirty{false};
    std::string StatusText{"Ready"};

    float TitleBarHeight{32.0F};
    float ToolbarHeight{38.0F};
    float StatusBarHeight{22.0F};
    float ResizeBorder{6.0F};

    float TitleBarMinX{0.0F};
    float TitleBarMaxX{0.0F};
    float TitleBarMinY{0.0F};
    float TitleBarMaxY{0.0F};
    float SysButtonsMinX{0.0F};
    float MenusMaxX{0.0F};

    float MaximizedPadL{0.0F};
    float MaximizedPadT{0.0F};
    float MaximizedPadR{0.0F};
    float MaximizedPadB{0.0F};

    std::string IniPath;
    std::string DefaultIniPath;
};
}
