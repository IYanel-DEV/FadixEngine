#include "core/EditorLayout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_set>

// Full EngineContext type required to call ReloadScriptLibrary().
#include "core/EngineContext.hpp"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/Scene.hpp"
#include "core/SceneSerializer.hpp"
#include "core/Entity.hpp"
#include "core/Components.hpp"
#include "core/PhysicsSystem.hpp"
#include "core/EditorTheme.hpp"
#include "core/FileSystem.hpp"
#include "renderer/EditorCamera.hpp"
#include "renderer/ViewportRenderer.hpp"

// =============================================================================
// Fadix Engine — Editor Layout Implementation
// =============================================================================

// The bundled GLAD loader is trimmed; make sure this core enum exists.
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif

namespace {

namespace fs = std::filesystem;

// Drag & drop payload type carried from the Content Drawer (null-terminated
// generic path string).
constexpr const char* kAssetPayloadType = "FADIX_ASSET";

// Payload type for file path drag-drop between Content Drawer folders.
constexpr const char* kFilePathPayloadType = "CONTENT_DRAWER_FILE_PATH";

// Payload type for hierarchy entity re-parenting (payload = entt::entity).
constexpr const char* kEntityPayloadType = "FADIX_HIERARCHY_ENTITY";

// Seconds a drag must hover a folder/breadcrumb before it auto-opens.
constexpr float kHoverNavDelay = 1.5f;

std::string ExtensionLower(const fs::path& p)
{
    std::string ext = p.extension().string();
    for (char& ch : ext)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return ext;
}

bool IsSpawnableAsset(const std::string& ext)
{
    return ext == ".gltf" || ext == ".glb" ||
           ext == ".png"  || ext == ".jpg" || ext == ".jpeg" ||
           ext == ".bmp"  || ext == ".tga";
}

void ApplyEditorTheme(int themeIndex)
{
    switch (themeIndex)
    {
    case 1:
        ImGui::StyleColorsLight();
        break;
    case 2:
        ImGui::StyleColorsDark();
        {
            ImVec4* c = ImGui::GetStyle().Colors;
            c[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
            c[ImGuiCol_ChildBg]  = ImVec4(0.03f, 0.03f, 0.03f, 1.00f);
            c[ImGuiCol_Text]     = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
            c[ImGuiCol_Border]   = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
        }
        break;
    default:
        ApplyFadixTheme();
        break;
    }
}

// Styled CollapsingHeader shared by all inspector component cards.
bool ComponentHeader(const char* title)
{
    ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0.149f, 0.200f, 0.251f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.173f, 0.231f, 0.286f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.000f, 0.384f, 0.800f, 0.600f));
    const bool open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
    ImGui::PopStyleColor(3);
    return open;
}

// InputText bridge for std::string fields.
void InputTextString(const char* id, std::string& value, float width = -1.0f)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(id, buf, sizeof(buf)))
        value = buf;
}

// ---------------------------------------------------------------------------
// DrawVec3Control — Unity/Unreal-style XYZ axis controls
// ---------------------------------------------------------------------------
// Each axis label is a small coloured reset button (click → resetValue).
// The drag fields fill the remaining column width equally.
//
// Returns an edit-state bitmask so callers can coalesce a whole drag
// interaction into one undo entry:
//   kVec3Activated   — a field/button became active this frame
//   kVec3Committed   — a drag ended (or a reset button fired) this frame
// ---------------------------------------------------------------------------

constexpr int kVec3Activated = 1;
constexpr int kVec3Committed = 2;

int DrawVec3Control(const char* label, glm::vec3& values,
                    float speed = 0.1f, float resetValue = 0.0f)
{
    int editState = 0;
    ImGui::PushID(label);

    constexpr float      kLabelW     = 72.0f;
    constexpr float      kGapBetween = 4.0f;  // gap between [Y btn…] [Z btn…]
    const     float      lineH       = ImGui::GetFrameHeight();
    const     float      btnW        = lineH + 2.0f;

    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_SizingFixedFit;

    if (ImGui::BeginTable("##v3t", 2, kTblFlags))
    {
        ImGui::TableSetupColumn("##lbl", ImGuiTableColumnFlags_WidthFixed,   kLabelW);
        ImGui::TableSetupColumn("##fld", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();

        // -- Label column ----------------------------------------------------
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);

        // -- Value column ----------------------------------------------------
        ImGui::TableSetColumnIndex(1);
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float raw   = (avail - 3.0f * btnW - 2.0f * kGapBetween) / 3.0f;
            const float dragW = (raw > 16.0f) ? raw : 16.0f;

            const auto trackEdit = [&editState]()
            {
                if (ImGui::IsItemActivated())              editState |= kVec3Activated;
                if (ImGui::IsItemDeactivatedAfterEdit())   editState |= kVec3Committed;
            };

            // ---- X (Red) ---------------------------------------------------
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.75f, 0.10f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.22f, 0.22f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.80f, 0.10f, 0.10f, 1.0f));
            if (ImGui::Button("X", ImVec2(btnW, lineH)))
            {
                values.x  = resetValue;
                editState |= kVec3Activated | kVec3Committed;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetNextItemWidth(dragW);
            ImGui::DragFloat("##vx", &values.x, speed, 0.0f, 0.0f, "%.2f");
            trackEdit();

            // ---- Y (Green) -------------------------------------------------
            ImGui::SameLine(0.0f, kGapBetween);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.58f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.73f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.58f, 0.10f, 1.0f));
            if (ImGui::Button("Y", ImVec2(btnW, lineH)))
            {
                values.y  = resetValue;
                editState |= kVec3Activated | kVec3Committed;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetNextItemWidth(dragW);
            ImGui::DragFloat("##vy", &values.y, speed, 0.0f, 0.0f, "%.2f");
            trackEdit();

            // ---- Z (Blue) --------------------------------------------------
            ImGui::SameLine(0.0f, kGapBetween);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.24f, 0.78f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.36f, 0.92f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.12f, 0.24f, 0.78f, 1.0f));
            if (ImGui::Button("Z", ImVec2(btnW, lineH)))
            {
                values.z  = resetValue;
                editState |= kVec3Activated | kVec3Committed;
            }
            ImGui::PopStyleColor(3);

            ImGui::SameLine(0.0f, 0.0f);
            ImGui::SetNextItemWidth(dragW);
            ImGui::DragFloat("##vz", &values.z, speed, 0.0f, 0.0f, "%.2f");
            trackEdit();
        }

        ImGui::EndTable();
    }

    ImGui::PopID();
    return editState;
}

// ---------------------------------------------------------------------------
// WorldToScreen — project a world point into viewport-image pixel coords.
// Returns false when the point is behind the camera or far out of frame.
// ---------------------------------------------------------------------------
bool WorldToScreen(const glm::mat4& viewProj, const glm::vec3& world,
                   const ImVec2& imageMin, const ImVec2& imageSize,
                   ImVec2& outScreen)
{
    const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
    if (clip.w <= 0.001f) return false;

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (ndc.x < -1.5f || ndc.x > 1.5f || ndc.y < -1.5f || ndc.y > 1.5f)
        return false;

    outScreen.x = imageMin.x + (ndc.x * 0.5f + 0.5f) * imageSize.x;
    outScreen.y = imageMin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * imageSize.y;
    return true;
}

// ---------------------------------------------------------------------------
// SimIconButton — graphical Play / Pause / Stop transport control.
// Draws the glyph with the window draw list over an InvisibleButton so the
// toolbar gets crisp vector icons instead of ASCII labels.
// ---------------------------------------------------------------------------
enum class SimIcon { Play, Pause, Stop };

bool SimIconButton(const char* id, SimIcon icon, bool enabled, ImU32 accent)
{
    const float  size = ImGui::GetFrameHeight();
    const ImVec2 btn(size + 8.0f, size);

    if (!enabled) ImGui::BeginDisabled();
    const bool pressed = ImGui::Button(id, btn); // label is "##id" — icon only
    if (!enabled) ImGui::EndDisabled();

    const ImVec2 rMin = ImGui::GetItemRectMin();
    const ImVec2 rMax = ImGui::GetItemRectMax();
    const ImVec2 c((rMin.x + rMax.x) * 0.5f, (rMin.y + rMax.y) * 0.5f);
    const float  r  = size * 0.28f;
    const ImU32  col = enabled ? accent : IM_COL32(110, 118, 128, 140);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    switch (icon)
    {
    case SimIcon::Play:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r),
                              ImVec2(c.x - r * 0.8f, c.y + r),
                              ImVec2(c.x + r * 1.1f, c.y), col);
        break;
    case SimIcon::Pause:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r),
                          ImVec2(c.x - r * 0.30f, c.y + r), col, 1.0f);
        dl->AddRectFilled(ImVec2(c.x + r * 0.30f, c.y - r),
                          ImVec2(c.x + r, c.y + r), col, 1.0f);
        break;
    case SimIcon::Stop:
        dl->AddRectFilled(ImVec2(c.x - r * 0.9f, c.y - r * 0.9f),
                          ImVec2(c.x + r * 0.9f, c.y + r * 0.9f), col, 1.5f);
        break;
    }

    return pressed && enabled;
}

// Case-insensitive substring search — used by DrawOutputLog for line colouring.
bool ContainsCaseInsensitive(const std::string& hay, const std::string& needle)
{
    return std::search(
        hay.begin(), hay.end(),
        needle.begin(), needle.end(),
        [](char a, char b)
        {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        }) != hay.end();
}

// ---------------------------------------------------------------------------
// Property inspector helpers — render ImGui widgets for reflected script vars
// ---------------------------------------------------------------------------

// Called during Play/Pause: descriptor has a live DataPtr; writes go directly
// to the running instance AND are mirrored into PropertyOverrides for persistence.
void RenderLivePropertyWidget(
    const fadix::PropertyDescriptor& desc,
    std::unordered_map<std::string, fadix::PropertyValue>& overrides)
{
    fadix::PropertyValue& ov = overrides[desc.Name];
    ov.Type = desc.Type;

    switch (desc.Type)
    {
    case fadix::PropertyType::Int:
    {
        auto* ptr = static_cast<int*>(desc.DataPtr);
        ImGui::DragInt(desc.Name.c_str(), ptr);
        ov.IntVal = *ptr;
        break;
    }
    case fadix::PropertyType::Float:
    {
        auto* ptr = static_cast<float*>(desc.DataPtr);
        ImGui::DragFloat(desc.Name.c_str(), ptr, 0.1f, 0.0f, 0.0f, "%.3f");
        ov.FloatVal = *ptr;
        break;
    }
    case fadix::PropertyType::Vec3:
    {
        auto* ptr = static_cast<glm::vec3*>(desc.DataPtr);
        DrawVec3Control(desc.Name.c_str(), *ptr, 0.1f);
        ov.Vec3Val = *ptr;
        break;
    }
    case fadix::PropertyType::String:
    {
        auto* ptr = static_cast<std::string*>(desc.DataPtr);
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", ptr->c_str());
        if (ImGui::InputText(desc.Name.c_str(), buf, sizeof(buf)))
            *ptr = buf;
        ov.StringVal = *ptr;
        break;
    }
    }
}

// Called in Edit mode: no live DataPtr; edits update the serialisable snapshot.
void RenderSavedPropertyWidget(const std::string& name, fadix::PropertyValue& val)
{
    switch (val.Type)
    {
    case fadix::PropertyType::Int:
        ImGui::DragInt(name.c_str(), &val.IntVal);
        break;
    case fadix::PropertyType::Float:
        ImGui::DragFloat(name.c_str(), &val.FloatVal, 0.1f, 0.0f, 0.0f, "%.3f");
        break;
    case fadix::PropertyType::Vec3:
        DrawVec3Control(name.c_str(), val.Vec3Val, 0.1f);
        break;
    case fadix::PropertyType::String:
    {
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", val.StringVal.c_str());
        if (ImGui::InputText(name.c_str(), buf, sizeof(buf)))
            val.StringVal = buf;
        break;
    }
    }
}

} // anonymous namespace

// =============================================================================
// Construction / wiring
// =============================================================================

EditorLayout::EditorLayout()
{
    fadix::LoadEditorSettings("fadix_editor_settings.json", m_EditorSettings);
    fadix::LoadEngineSettings("fadix_engine_settings.json", m_EngineSettings);
    m_ThemePending   = true; // saved theme may differ from the startup default
    m_GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
}

void EditorLayout::SetScene(Scene* scene)
{
    m_ActiveScene    = scene;
    m_SelectedEntity = entt::null;
    m_RenamingEntity = entt::null;
    // History entries reference entity handles inside the previous registry —
    // a scene swap (Play/Stop restore, load, new) invalidates all of them.
    m_History.Clear();
    m_TransformEditActive = false;
    m_GizmoWasUsing       = false;
}

void EditorLayout::SetFileSystem(fadix::FileSystem* fileSystem)
{
    m_FileSystem = fileSystem;
    m_ContentDir.clear();
}

void EditorLayout::SetProjectRoot(const std::filesystem::path& root)
{
    m_ProjectRoot     = root;
    m_ProjectSettings = fadix::ProjectSettings{};
    fadix::LoadProjectSettings(root / "ProjectSettings.json", m_ProjectSettings);
    m_ContentDir.clear();
    m_Viewport.SetAssetRoot(root);     // resolve project-relative mesh paths
    m_GameViewport.SetAssetRoot(root);

    // Bootstrap the asset registry for this project: generates .meta files for
    // every asset that doesn't already have one.
    m_AssetRegistry.ScanDirectory(root);
}

bool EditorLayout::ConsumeNewSceneRequest()
{
    const bool requested = m_NewSceneRequested;
    m_NewSceneRequested = false;
    return requested;
}

bool EditorLayout::ConsumePlayRequest()
{
    const bool r = m_PlayRequested;
    m_PlayRequested = false;
    return r;
}

bool EditorLayout::ConsumePauseRequest()
{
    const bool r = m_PauseRequested;
    m_PauseRequested = false;
    return r;
}

bool EditorLayout::ConsumeStopRequest()
{
    const bool r = m_StopRequested;
    m_StopRequested = false;
    return r;
}

// =============================================================================
// Viewport lifecycle
// =============================================================================

bool EditorLayout::InitViewport()
{
    return m_Viewport.Initialise() && m_GameViewport.Initialise();
}

void EditorLayout::ShutdownViewport()
{
    m_GameViewport.Shutdown();
    m_Viewport.Shutdown();
}

void EditorLayout::UpdateCamera(float deltaTime, GLFWwindow* window)
{
    m_Camera.Update(deltaTime, window);
}

// =============================================================================
// Menu bar
// =============================================================================

void EditorLayout::DrawMenuBar(EngineState& state)
{
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File"))
    {
        if (ImGui::MenuItem("New Scene", "Ctrl+N"))
        {
            m_NewSceneRequested = true;
        }

        // Save Scene and Load Scene are locked while the simulation is active
        // to prevent sandbox-mutated component state from leaking back into the
        // persistent editor file.
        const bool inSimulation = (m_SimState != SceneState::Edit);
        ImGui::BeginDisabled(inSimulation);

        if (ImGui::MenuItem("Save Scene", "Ctrl+S"))
        {
            SaveActiveScene();
        }
        if (ImGui::MenuItem("Load Scene"))
        {
            LoadActiveScene();
        }

        ImGui::EndDisabled();

        if (ImGui::MenuItem("Save Project"))
        {
            SaveAllSettings();
            SaveActiveScene();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open Project", "Ctrl+O"))
        {
            state = EngineState::Launcher;
            m_dockspaceInitialized = false; // reset on next editor open
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4"))
        {
            m_WantsExit = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit"))
    {
        const bool inEdit = (m_SimState == SceneState::Edit);

        const std::string undoLabel = m_History.CanUndo()
            ? "Undo " + m_History.UndoLabel() : std::string("Undo");
        const std::string redoLabel = m_History.CanRedo()
            ? "Redo " + m_History.RedoLabel() : std::string("Redo");

        if (ImGui::MenuItem(undoLabel.c_str(), "Ctrl+Z",
                            false, inEdit && m_History.CanUndo() && m_ActiveScene))
            m_History.Undo(*m_ActiveScene);
        if (ImGui::MenuItem(redoLabel.c_str(), "Ctrl+Y",
                            false, inEdit && m_History.CanRedo() && m_ActiveScene))
            m_History.Redo(*m_ActiveScene);

        ImGui::Separator();
        if (ImGui::MenuItem("Delete Selected", "Del",
                            false, inEdit && m_SelectedEntity != entt::null))
            DeleteEntityWithHistory(m_SelectedEntity);

        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Workspace"))
    {
        if (ImGui::MenuItem("Project Settings..."))
        {
            m_OpenSettingsRequest = true;
            m_SettingsFocusTab    = 0;
        }
        if (ImGui::MenuItem("Editor Settings..."))
        {
            m_OpenSettingsRequest = true;
            m_SettingsFocusTab    = 1;
        }
        if (ImGui::MenuItem("Engine Settings..."))
        {
            m_OpenSettingsRequest = true;
            m_SettingsFocusTab    = 2;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View"))
    {
        // Every panel is listed here: clicking a closed panel restores it
        // instantly (panels close via their tab-bar X buttons).
        ImGui::MenuItem("Hierarchy",      nullptr,      &m_HierarchyVisible);
        ImGui::MenuItem("Inspector",      nullptr,      &m_InspectorVisible);
        ImGui::MenuItem("Scene Viewport", nullptr,      &m_SceneViewportVisible);
        ImGui::MenuItem("Game Viewport",  nullptr,      &m_GameViewportVisible);
        ImGui::MenuItem("Content Drawer", "Ctrl+Space", &m_contentDrawerVisible);
        ImGui::MenuItem("Output Log",     nullptr,      &m_OutputLogVisible);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
}

// =============================================================================
// Workspace (fullscreen dockspace)
// =============================================================================

void EditorLayout::RenderWorkspace(GLFWwindow* /*window*/, EngineState& state)
{
    if (m_ThemePending)
    {
        ApplyEditorTheme(m_EditorSettings.ThemeIndex);
        m_ThemePending = false;
    }

    // ImGuizmo tracks its own per-frame state alongside ImGui's.
    ImGuizmo::BeginFrame();

    // Menu bar must come first — it adjusts the viewport's WorkPos/WorkSize
    DrawMenuBar(state);

    if (state != EngineState::Editor) return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags kDockspaceFlags =
        ImGuiWindowFlags_NoDocking           |
        ImGuiWindowFlags_NoTitleBar          |
        ImGuiWindowFlags_NoCollapse          |
        ImGuiWindowFlags_NoResize            |
        ImGuiWindowFlags_NoMove              |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus          |
        ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0.0f, 0.0f));

    ImGui::Begin("FadixEditorDockspace", nullptr, kDockspaceFlags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockspaceId = ImGui::GetID("FadixEditorDockspace");

    if (!m_dockspaceInitialized)
    {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID dockLeft   = 0;
        ImGuiID dockRight  = 0;
        ImGuiID dockBottom = 0;
        ImGuiID dockCenter = 0;

        ImGui::DockBuilderSplitNode(
            dockspaceId, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);
        ImGui::DockBuilderSplitNode(
            dockCenter,  ImGuiDir_Right, 0.3125f, &dockRight, &dockCenter);
        ImGui::DockBuilderSplitNode(
            dockCenter,  ImGuiDir_Down,  0.25f,   &dockBottom, &dockCenter);

        ImGui::DockBuilderDockWindow("Hierarchy##panel",                  dockLeft);
        ImGui::DockBuilderDockWindow("Inspector##panel",                  dockRight);
        ImGui::DockBuilderDockWindow("Content Drawer##panel",             dockBottom);
        // Output Log shares a tab group with the Content Drawer at the bottom.
        ImGui::DockBuilderDockWindow("Output Log##fadix_output_log",      dockBottom);
        // Scene and Game viewports share the central tab group; Scene is
        // docked last so its tab is the one selected on first launch.
        ImGui::DockBuilderDockWindow("Game##panel",                       dockCenter);
        ImGui::DockBuilderDockWindow("Scene##panel",                      dockCenter);
        // Script Editor shares a tab group with the viewports in the center.
        ImGui::DockBuilderDockWindow("Fadix Code Studio##fadix_script_editor", dockCenter);
        ImGui::DockBuilderFinish(dockspaceId);

        m_dockspaceInitialized = true;
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None, nullptr);
    ImGui::End();

    HandleGlobalShortcuts();

    if (m_HierarchyVisible)     DrawHierarchy();
    if (m_InspectorVisible)     DrawInspector();
    if (m_SceneViewportVisible) DrawSceneViewport();
    if (m_GameViewportVisible)  DrawGameViewport();
    if (m_contentDrawerVisible) DrawContentDrawer();
    if (m_OutputLogVisible)     DrawOutputLog();
    m_ScriptEditor.Draw(m_ScriptCompiler);

    DrawSettingsModal();

    // ---- Hot-reload: consume async compile signals --------------------------
    // A successful compile triggers an immediate DLL swap on the active scene's
    // registry (Edit mode only — EngineContext::ReloadScriptLibrary guards this).
    if (m_ScriptCompiler.ConsumeCompileSucceeded())
    {
        if (m_EngineContext && m_ActiveScene)
            m_EngineContext->ReloadScriptLibrary(m_ActiveScene, m_PhysicsSystem);
    }

    // Consume failure signal (log is read directly by DrawOutputLog via GetLines).
    m_ScriptCompiler.ConsumeCompileFailed();
}

// =============================================================================
// Global shortcuts & history helpers
// =============================================================================

void EditorLayout::HandleGlobalShortcuts()
{
    // Never fire while typing into a text field or during simulation.
    if (ImGui::GetIO().WantTextInput) return;
    if (m_SimState != SceneState::Edit || !m_ActiveScene) return;

    const bool ctrl = ImGui::GetIO().KeyCtrl;

    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        m_History.Undo(*m_ActiveScene);

    // Both conventions: Ctrl+Y and Ctrl+Shift+Z.
    if ((ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
        (ctrl && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)))
        m_History.Redo(*m_ActiveScene);

    // Undo/redo may have revived or destroyed the selected entity.
    if (m_SelectedEntity != entt::null &&
        !m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
        m_SelectedEntity = entt::null;
}

void EditorLayout::DeleteEntityWithHistory(entt::entity entity)
{
    if (!m_ActiveScene || entity == entt::null) return;

    entt::registry& registry = m_ActiveScene->GetRegistry();
    if (!registry.valid(entity)) return;

    m_History.PushEntityDeleted(*m_ActiveScene, entity);

    if (m_SelectedEntity == entity ||
        m_ActiveScene->IsDescendantOf(m_SelectedEntity, entity))
        m_SelectedEntity = entt::null;
    if (m_RenamingEntity == entity)
        m_RenamingEntity = entt::null;

    m_ActiveScene->DestroyEntity(Entity(entity, &registry));
}

void EditorLayout::AssignScriptWithHistory(entt::entity entity,
                                           const std::string& path)
{
    if (!m_ActiveScene || entity == entt::null) return;

    entt::registry& registry = m_ActiveScene->GetRegistry();
    if (!registry.valid(entity)) return;

    const auto* existing  = registry.try_get<ScriptComponent>(entity);
    const bool  hadBefore = (existing != nullptr);
    const ScriptComponent before = existing ? *existing : ScriptComponent{};

    auto& sc = registry.get_or_emplace<ScriptComponent>(entity);
    sc.ScriptAssetPath = path;
    sc.ScriptAssetId   = m_AssetRegistry.GetUUID(fs::path(path));

    m_History.PushScriptAssign(entity, hadBefore, before, true, sc);
}

// Every .cpp under the mounted asset root — feeds the "Add Script" dropdown.
std::vector<std::string> EditorLayout::CollectIndexedScripts() const
{
    std::vector<std::string> scripts;
    if (!m_FileSystem || !m_FileSystem->HasRoot()) return scripts;

    // Walk the VFS index directory-by-directory (avoids a second disk scan).
    std::vector<fs::path> pending{ m_FileSystem->GetRoot() };
    while (!pending.empty())
    {
        const fs::path dir = pending.back();
        pending.pop_back();

        for (const fadix::FileEntry& entry : m_FileSystem->List(dir))
        {
            if (entry.IsDirectory)
                pending.push_back(entry.Path);
            else if (entry.Extension == ".cpp")
                scripts.push_back(entry.Path.string());
        }
    }
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

// =============================================================================
// Hierarchy Outliner
// =============================================================================

void EditorLayout::DrawHierarchy()
{
    if (!ImGui::Begin("Hierarchy##panel", &m_HierarchyVisible))
    {
        ImGui::End();
        return;
    }

    if (!m_ActiveScene)
    {
        ImGui::TextDisabled("No active scene");
        ImGui::End();
        return;
    }

    ImGui::Text("Scene");
    ImGui::Separator();

    auto& registry = m_ActiveScene->GetRegistry();

    entt::entity pendingDelete = entt::null;
    entt::entity dropChild     = entt::null;   // entity dragged...
    entt::entity dropParent    = entt::null;   // ...onto this entity

    // Draw root-level entities; children render recursively inside their
    // parent's tree node.
    for (auto entity : registry.view<TagComponent>())
    {
        const auto* rel = registry.try_get<RelationshipComponent>(entity);
        if (rel && rel->Parent != entt::null) continue; // parented — drawn by parent

        DrawEntityNode(entity, registry, pendingDelete, dropChild, dropParent);
    }

    const bool panelFocused =
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // Physical Delete key removes the selected entity (undoable).
    if (panelFocused && !ImGui::GetIO().WantTextInput &&
        m_SimState == SceneState::Edit &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
        m_SelectedEntity != entt::null && pendingDelete == entt::null)
        pendingDelete = m_SelectedEntity;

    // F2 starts an inline rename of the selection.
    if (panelFocused && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_F2, false) &&
        m_SelectedEntity != entt::null && registry.valid(m_SelectedEntity))
    {
        m_RenamingEntity = m_SelectedEntity;
        const auto& tag  = registry.get<TagComponent>(m_SelectedEntity);
        std::snprintf(m_EntityRenameBuf, sizeof(m_EntityRenameBuf), "%s",
                      tag.Name.c_str());
        m_RenameFocusPending = true;
    }

    if (pendingDelete != entt::null)
        DeleteEntityWithHistory(pendingDelete);

    // Deferred reparent (mutating during tree traversal is unsafe).
    if (dropChild != entt::null)
    {
        const entt::entity oldParent = m_ActiveScene->GetParent(dropChild);
        m_ActiveScene->SetParent(dropChild, dropParent);
        if (m_ActiveScene->GetParent(dropChild) != oldParent)
            m_History.PushReparent(dropChild, oldParent, dropParent);
    }

    // Right-click empty space → entity creation menu
    if (ImGui::BeginPopupContextWindow("##hierarchy_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        const auto createTracked = [this](Entity e)
        {
            m_SelectedEntity = e.GetHandle();
            m_History.PushEntityCreated(*m_ActiveScene, e.GetHandle());
        };

        if (ImGui::MenuItem("Create Empty Object3D"))
            createTracked(m_ActiveScene->CreateEntity("Object3D"));
        if (ImGui::MenuItem("Create Empty Entity"))
            createTracked(m_ActiveScene->CreateEntity("Empty Entity"));
        if (ImGui::MenuItem("Create Camera"))
        {
            Entity e = m_ActiveScene->CreateEntity("Camera");
            e.AddComponent<CameraComponent>();
            createTracked(e);
        }
        if (ImGui::MenuItem("Create Light"))
        {
            Entity e = m_ActiveScene->CreateEntity("Light");
            e.AddComponent<LightComponent>();
            createTracked(e);
        }
        if (ImGui::MenuItem("Create Mesh"))
        {
            Entity e = m_ActiveScene->CreateEntity("Mesh");
            e.AddComponent<MeshComponent>();
            createTracked(e);
        }
        ImGui::EndPopup();
    }

    // The whole panel accepts Content Drawer assets; dropping an entity here
    // (not on a row) detaches it to the root.
    ImGuiWindow* windowHandle = ImGui::GetCurrentWindow();
    if (ImGui::BeginDragDropTargetCustom(windowHandle->InnerRect,
                                         windowHandle->GetID("##hierarchy_drop")))
    {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
            SpawnEntityFromAsset(static_cast<const char*>(payload->Data));
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFilePathPayloadType))
            SpawnEntityFromAsset(static_cast<const char*>(payload->Data));
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kEntityPayloadType))
        {
            const entt::entity dragged =
                *static_cast<const entt::entity*>(payload->Data);
            const entt::entity oldParent = m_ActiveScene->GetParent(dragged);
            if (oldParent != entt::null)
            {
                m_ActiveScene->SetParent(dragged, entt::null);
                m_History.PushReparent(dragged, oldParent, entt::null);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::End();
}

// One row of the hierarchy tree; recurses into RelationshipComponent children.
void EditorLayout::DrawEntityNode(entt::entity entity, entt::registry& registry,
                                  entt::entity& pendingDelete,
                                  entt::entity& dropChild,
                                  entt::entity& dropParent)
{
    if (!registry.valid(entity)) return;

    const auto* tag = registry.try_get<TagComponent>(entity);
    if (!tag) return;

    const auto* rel      = registry.try_get<RelationshipComponent>(entity);
    const bool  hasKids  = rel && !rel->Children.empty();
    const bool  isSel    = (entity == m_SelectedEntity);
    const bool  renaming = (entity == m_RenamingEntity);

    ImGui::PushID(static_cast<int>(entt::to_integral(entity)));

    // ---- Inline rename row ---------------------------------------------------
    if (renaming)
    {
        if (m_RenameFocusPending)
        {
            ImGui::SetKeyboardFocusHere();
            m_RenameFocusPending = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        const bool committed = ImGui::InputText(
            "##rename", m_EntityRenameBuf, sizeof(m_EntityRenameBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);

        if (committed || ImGui::IsItemDeactivated())
        {
            if (committed && m_EntityRenameBuf[0] != '\0')
                registry.get<TagComponent>(entity).Name = m_EntityRenameBuf;
            m_RenamingEntity = entt::null;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
            m_RenamingEntity = entt::null;

        // Children still render while their parent is being renamed.
        if (hasKids)
        {
            ImGui::Indent();
            for (entt::entity child : rel->Children)
                DrawEntityNode(child, registry, pendingDelete, dropChild, dropParent);
            ImGui::Unindent();
        }
        ImGui::PopID();
        return;
    }

    // ---- Normal tree row -------------------------------------------------------
    const ImGuiTreeNodeFlags nodeFlags =
        (hasKids ? ImGuiTreeNodeFlags_OpenOnArrow
                 : ImGuiTreeNodeFlags_Leaf)     |
        ImGuiTreeNodeFlags_SpanAvailWidth       |
        ImGuiTreeNodeFlags_DefaultOpen          |
        (isSel ? ImGuiTreeNodeFlags_Selected : 0);

    const bool nodeOpen = ImGui::TreeNodeEx(
        "##node", nodeFlags,
        "%s", tag->Name.empty() ? "(unnamed)" : tag->Name.c_str());

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        m_SelectedEntity = entity;

    // Double-click → inline rename.
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_RenamingEntity = entity;
        std::snprintf(m_EntityRenameBuf, sizeof(m_EntityRenameBuf), "%s",
                      tag->Name.c_str());
        m_RenameFocusPending = true;
    }

    // ---- Script badge — instant "this object is scripted" indicator ---------
    if (registry.all_of<ScriptComponent>(entity))
    {
        ImGui::SameLine();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList*  dl  = ImGui::GetWindowDrawList();
        const float  h   = ImGui::GetTextLineHeight();
        const ImVec2 pad(5.0f, 1.0f);
        const ImVec2 textSize = ImGui::CalcTextSize("C++");
        dl->AddRectFilled(ImVec2(pos.x, pos.y + 1.0f),
                          ImVec2(pos.x + textSize.x + pad.x * 2.0f,
                                 pos.y + h - 1.0f),
                          IM_COL32(28, 92, 84, 255), 3.0f);
        dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y),
                    IM_COL32(120, 230, 205, 255), "C++");
        ImGui::Dummy(ImVec2(textSize.x + pad.x * 2.0f, h));
    }

    // ---- Drag source: this entity can be parented under another --------------
    if (m_SimState == SceneState::Edit &&
        ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover))
    {
        ImGui::SetDragDropPayload(kEntityPayloadType, &entity, sizeof(entity));
        ImGui::TextUnformatted(tag->Name.c_str());
        ImGui::EndDragDropSource();
    }

    // ---- Drop target: entity re-parenting + script assignment ---------------
    if (ImGui::BeginDragDropTarget())
    {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kEntityPayloadType))
        {
            const entt::entity dragged =
                *static_cast<const entt::entity*>(payload->Data);
            if (dragged != entity)
            {
                dropChild  = dragged;
                dropParent = entity;
            }
        }
        // Drop .cpp / .hpp / .h asset onto an entity row: auto-adds a Script
        // Component with that script bound.
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(kFilePathPayloadType))
        {
            const char* droppedPath = static_cast<const char*>(payload->Data);
            const std::string ext   = ExtensionLower(fs::path(droppedPath));
            if (ext == ".cpp" || ext == ".hpp" || ext == ".h")
            {
                AssignScriptWithHistory(entity, droppedPath);
                m_SelectedEntity = entity;
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem("##entity_ctx"))
    {
        if (ImGui::MenuItem("Rename", "F2"))
        {
            m_RenamingEntity = entity;
            std::snprintf(m_EntityRenameBuf, sizeof(m_EntityRenameBuf), "%s",
                          tag->Name.c_str());
            m_RenameFocusPending = true;
        }
        if (ImGui::MenuItem("Create Empty Object3D (child)"))
        {
            Entity child = m_ActiveScene->CreateEntity("Object3D");
            m_ActiveScene->SetParent(child.GetHandle(), entity,
                                     /*keepWorldTransform=*/false);
            m_History.PushEntityCreated(*m_ActiveScene, child.GetHandle(),
                                        "Create Child");
            m_SelectedEntity = child.GetHandle();
        }
        if (m_ActiveScene->GetParent(entity) != entt::null &&
            ImGui::MenuItem("Detach from Parent"))
        {
            dropChild  = entity;
            dropParent = entt::null;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete Entity", "Del"))
            pendingDelete = entity;
        ImGui::EndPopup();
    }

    if (nodeOpen)
    {
        if (hasKids)
        {
            // Copy — DrawEntityNode may enqueue mutations for this list.
            const std::vector<entt::entity> children = rel->Children;
            for (entt::entity child : children)
                DrawEntityNode(child, registry, pendingDelete, dropChild, dropParent);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

// =============================================================================
// Component Inspector
// =============================================================================

void EditorLayout::DrawInspector()
{
    if (!ImGui::Begin("Inspector##panel", &m_InspectorVisible))
    {
        ImGui::End();
        return;
    }

    if (!m_ActiveScene || m_SelectedEntity == entt::null ||
        !m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
    {
        ImGui::TextDisabled("Select an entity");
        ImGui::End();
        return;
    }

    Entity selected(m_SelectedEntity, &m_ActiveScene->GetRegistry());

    // -- Tag -----------------------------------------------------------------
    if (selected.HasComponent<TagComponent>())
    {
        auto& tag = selected.GetComponent<TagComponent>();

        static char         s_NameBuf[256] = {};
        static entt::entity s_PrevSel      = entt::null;

        if (m_SelectedEntity != s_PrevSel)
        {
            std::memset(s_NameBuf, 0, sizeof(s_NameBuf));
            const std::size_t len = tag.Name.size();
            const std::size_t cap = sizeof(s_NameBuf) - 1;
            std::memcpy(s_NameBuf, tag.Name.c_str(), (len < cap) ? len : cap);
            s_PrevSel = m_SelectedEntity;
        }

        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputText("##tag_name", s_NameBuf, sizeof(s_NameBuf)))
            tag.Name = s_NameBuf;

        ImGui::Spacing();
    }

    // -- Transform -----------------------------------------------------------
    if (selected.HasComponent<TransformComponent>())
    {
        auto& tc = selected.GetComponent<TransformComponent>();

        if (ComponentHeader("Transform"))
        {
            // Snapshot before the widgets run so a drag that starts this
            // frame still captures the pre-edit value for undo.
            const TransformComponent preFrame = tc;

            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.118f, 0.153f, 0.184f, 1.0f));
            ImGui::BeginChild("##tf_card", ImVec2(-1.0f,
                3.0f * ImGui::GetFrameHeightWithSpacing() + 2.0f * ImGui::GetStyle().WindowPadding.y),
                true, ImGuiWindowFlags_NoScrollbar);

            int edit = 0;
            edit |= DrawVec3Control("Position", tc.Position, 0.10f, 0.0f);
            edit |= DrawVec3Control("Rotation", tc.Rotation, 0.50f, 0.0f);
            edit |= DrawVec3Control("Scale",    tc.Scale,    0.01f, 1.0f);

            ImGui::EndChild();
            ImGui::PopStyleColor();

            // Coalesce the whole drag into one undo entry: capture the value
            // on activation, push once on commit.
            if ((edit & kVec3Activated) && !m_TransformEditActive)
            {
                m_TransformEditActive = true;
                m_TransformBeforeEdit = preFrame;
            }
            if ((edit & kVec3Committed) && m_TransformEditActive)
            {
                m_TransformEditActive = false;
                if (m_SimState == SceneState::Edit)
                    m_History.PushTransformEdit(m_SelectedEntity,
                                                m_TransformBeforeEdit, tc);
            }

            // Push edited transforms into any live physics body.
            if (edit != 0)
            {
                auto& reg = m_ActiveScene->GetRegistry();
                if (auto* rb = reg.try_get<RigidBody3DComponent>(m_SelectedEntity))
                    rb->TransformDirty = true;
                if (auto* rb2 = reg.try_get<RigidBody2DComponent>(m_SelectedEntity))
                    rb2->TransformDirty = true;
            }
        }

        ImGui::Spacing();
    }

    // -- Mesh ------------------------------------------------------------------
    if (selected.HasComponent<MeshComponent>())
    {
        const bool open   = ComponentHeader("Mesh");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##mesh_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<MeshComponent>();
        }
        else if (open)
        {
            auto& mc = selected.GetComponent<MeshComponent>();

            ImGui::TextDisabled("Asset (drop from Content Drawer)");
            char pathBuf[512];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", mc.FilePath.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##mesh_path", pathBuf, sizeof(pathBuf)))
                mc.FilePath = pathBuf;

            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                    mc.FilePath = static_cast<const char*>(payload->Data);
                ImGui::EndDragDropTarget();
            }

            ImGui::Checkbox("Cast Shadows", &mc.CastShadows);
            ImGui::SameLine();
            ImGui::Checkbox("Receive Shadows", &mc.ReceiveShadows);

            if (!mc.FilePath.empty())
            {
                const std::string ext = ExtensionLower(mc.FilePath);
                if (ext == ".gltf" || ext == ".glb")
                {
                    // Parsed via cgltf; cached so we only re-parse on path change.
                    static std::string         s_InfoPath;
                    static fadix::MeshFileInfo s_Info;
                    if (mc.FilePath != s_InfoPath)
                    {
                        s_InfoPath = mc.FilePath;
                        s_Info     = fadix::QueryMeshFileInfo(mc.FilePath);
                    }

                    if (s_Info.Valid)
                        ImGui::TextDisabled("glTF: %d mesh(es), %d primitive(s), %d material(s)",
                                            static_cast<int>(s_Info.Meshes),
                                            static_cast<int>(s_Info.Primitives),
                                            static_cast<int>(s_Info.Materials));
                    else
                        ImGui::TextDisabled("glTF: %s", s_Info.Error.c_str());
                }
                else
                {
                    ImGui::TextDisabled("2D sprite / raw asset profile");
                }
            }
        }

        ImGui::Spacing();
    }

    // -- Light -----------------------------------------------------------------
    if (selected.HasComponent<LightComponent>())
    {
        const bool open   = ComponentHeader("Light");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##light_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<LightComponent>();
        }
        else if (open)
        {
            auto& lc = selected.GetComponent<LightComponent>();

            constexpr const char* kLightTypes[] = { "Directional", "Point", "Spot" };
            int type = static_cast<int>(lc.Type);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##light_type", &type, kLightTypes, 3))
                lc.Type = static_cast<LightType>(type);

            ImGui::DragFloat("Intensity##light", &lc.Intensity, 0.05f, 0.0f, 100.0f, "%.2f");
            ImGui::ColorEdit3("Color##light", &lc.Color.x);

            ImGui::BeginDisabled(lc.Type == LightType::Directional);
            ImGui::DragFloat("Attenuation Radius##light", &lc.AttenuationRadius,
                             0.1f, 0.01f, 10000.0f, "%.2f m");
            ImGui::EndDisabled();

            ImGui::BeginDisabled(lc.Type != LightType::Spot);
            ImGui::DragFloat("Inner Cone##light", &lc.InnerConeAngle,
                             0.5f, 0.0f, 89.0f, "%.1f deg");
            ImGui::DragFloat("Outer Cone##light", &lc.OuterConeAngle,
                             0.5f, 0.5f, 90.0f, "%.1f deg");
            if (lc.OuterConeAngle < lc.InnerConeAngle)
                lc.OuterConeAngle = lc.InnerConeAngle;
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
    }

    // -- Camera ------------------------------------------------------------------
    if (selected.HasComponent<CameraComponent>())
    {
        const bool open   = ComponentHeader("Camera");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##camera_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<CameraComponent>();
        }
        else if (open)
        {
            auto& cc = selected.GetComponent<CameraComponent>();

            constexpr const char* kProjections[] = { "Perspective", "Orthographic" };
            int projection = static_cast<int>(cc.Projection);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("##cam_projection", &projection, kProjections, 2))
                cc.Projection = static_cast<ProjectionType>(projection);

            ImGui::BeginDisabled(cc.Projection == ProjectionType::Orthographic);
            ImGui::SliderFloat("Field of View##cam", &cc.FieldOfView, 10.0f, 170.0f, "%.0f deg");
            ImGui::EndDisabled();

            ImGui::DragFloat("Near Clip##cam", &cc.NearClip, 0.01f, 0.001f, 100.0f, "%.3f");
            ImGui::DragFloat("Far Clip##cam",  &cc.FarClip,  1.00f, 0.100f, 100000.0f, "%.1f");
            if (cc.FarClip <= cc.NearClip)
                cc.FarClip = cc.NearClip + 0.01f;
        }

        ImGui::Spacing();
    }

    // -- Script (editor component) -----------------------------------------------
    if (selected.HasComponent<ScriptComponent>())
    {
        const bool open   = ComponentHeader("Script");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##script_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<ScriptComponent>();
        }
        else if (open)
        {
            auto& sc = selected.GetComponent<ScriptComponent>();

            // ---- Assigned script slot ----------------------------------------
            // Shows the clean class name only (never the raw absolute path);
            // the full path lives in the tooltip.
            ImGui::TextDisabled("Script (drop .cpp here or pick below)");

            const bool bound = !sc.ScriptAssetPath.empty();
            const std::string cleanName = bound
                ? fs::path(sc.ScriptAssetPath).stem().string()
                : std::string("None");

            const float unbindW = ImGui::GetFrameHeight();

            ImGui::PushStyleColor(ImGuiCol_Button,
                bound ? ImVec4(0.118f, 0.243f, 0.220f, 1.0f)
                      : ImVec4(0.118f, 0.153f, 0.184f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.160f, 0.300f, 0.270f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.100f, 0.210f, 0.190f, 1.0f));
            ImGui::Button(cleanName.c_str(),
                          ImVec2(-(unbindW + ImGui::GetStyle().ItemSpacing.x), 0.0f));
            ImGui::PopStyleColor(3);

            if (bound && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", sc.ScriptAssetPath.c_str());

            // Drop target: bind a script by dragging it from the Content Drawer.
            if (ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload(kFilePathPayloadType))
                {
                    const char* dropped = static_cast<const char*>(p->Data);
                    const std::string ext = ExtensionLower(fs::path(dropped));
                    if (ext == ".cpp" || ext == ".hpp" || ext == ".h")
                        AssignScriptWithHistory(m_SelectedEntity, dropped);
                }
                ImGui::EndDragDropTarget();
            }

            // "X" — unbind the script (keeps the component; undoable).
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.45f, 0.16f, 0.16f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.12f, 0.12f, 1.0f));
            if (!bound) ImGui::BeginDisabled();
            if (ImGui::Button("X##sc_unbind", ImVec2(unbindW, 0.0f)))
            {
                const ScriptComponent before = sc;
                sc.ScriptAssetPath.clear();
                sc.ScriptAssetId = fadix::UUID::Nil();
                sc.PropertyOverrides.clear();
                m_History.PushScriptAssign(m_SelectedEntity,
                                           true, before, true, sc,
                                           "Unbind Script");
            }
            if (!bound) ImGui::EndDisabled();
            ImGui::PopStyleColor(3);

            // ---- "Add Script" dropdown of all indexed scripts ----------------
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##sc_pick",
                                  bound ? "Change Script..." : "Add Script..."))
            {
                const std::vector<std::string> scripts = CollectIndexedScripts();
                if (scripts.empty())
                    ImGui::TextDisabled("No .cpp scripts indexed");
                for (const std::string& script : scripts)
                {
                    const std::string name = fs::path(script).stem().string();
                    const bool isCurrent   = (script == sc.ScriptAssetPath);
                    if (ImGui::Selectable(name.c_str(), isCurrent) && !isCurrent)
                        AssignScriptWithHistory(m_SelectedEntity, script);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", script.c_str());
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();

            // ---- Property display -------------------------------------------
            const bool inPlay =
                (m_SimState == SceneState::Play || m_SimState == SceneState::Pause);

            if (inPlay && selected.HasComponent<NativeScriptComponent>())
            {
                auto& nsc = selected.GetComponent<NativeScriptComponent>();
                if (nsc.Instance)
                {
                    auto liveProps = nsc.Instance->GetProperties();

                    // Build set of network-replicated property names for badge display.
                    std::unordered_set<std::string> netPropNames;
                    if (nsc.Instance->IsNetworkReplicated_())
                    {
                        auto netProps = nsc.Instance->GetNetProperties();
                        for (const auto& np : netProps)
                            netPropNames.insert(np.Name);
                    }

                    if (liveProps.empty())
                    {
                        ImGui::TextDisabled("No reflected properties.");
                        ImGui::TextDisabled(
                            "Override OnRegisterProperties() in your script.");
                    }
                    else
                    {
                        if (nsc.Instance->IsNetworkReplicated_())
                        {
                            ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.4f, 1.0f),
                                               "[Replicated Entity]");
                            ImGui::SameLine();
                            ImGui::TextDisabled("NetId: %u",
                                               nsc.Instance->GetNetworkId());
                        }
                        ImGui::TextDisabled("Live Properties");
                        ImGui::Separator();
                        ImGui::PushID("sc_live");
                        for (auto& desc : liveProps)
                        {
                            ImGui::PushID(desc.Name.c_str());
                            RenderLivePropertyWidget(desc, sc.PropertyOverrides);

                            // [Net] badge for network-replicated fields
                            if (netPropNames.count(desc.Name))
                            {
                                ImGui::SameLine(0.0f, 6.0f);
                                ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.4f, 1.0f), "[Net]");
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip(
                                        "This property is replicated to all clients\n"
                                        "at ~20 Hz via the Fadix replication system.");
                            }

                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                }
                else
                {
                    ImGui::TextDisabled("(script not instantiated yet)");
                }
            }
            else if (!sc.PropertyOverrides.empty())
            {
                ImGui::TextDisabled("Saved Properties (Edit mode)");
                ImGui::Separator();
                ImGui::PushID("sc_edit");
                for (auto& [name, val] : sc.PropertyOverrides)
                {
                    ImGui::PushID(name.c_str());
                    RenderSavedPropertyWidget(name, val);
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            else
            {
                ImGui::TextDisabled(
                    "Enter Play mode to discover reflected properties.");
            }
        }

        ImGui::Spacing();
    }

    // -- RigidBody3D (Jolt) -------------------------------------------------------
    if (selected.HasComponent<RigidBody3DComponent>())
    {
        const bool open   = ComponentHeader("Rigid Body 3D");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##rb3d_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<RigidBody3DComponent>();
        }
        else if (open)
        {
            auto& rb = selected.GetComponent<RigidBody3DComponent>();

            constexpr const char* kMotionTypes[] = { "Static", "Kinematic", "Dynamic" };
            int mt = static_cast<int>(rb.MotionType);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("Motion Type##rb3d", &mt, kMotionTypes, 3))
            {
                rb.MotionType     = static_cast<MotionType3D>(mt);
                rb.TransformDirty = true;
            }

            ImGui::DragFloat("Mass##rb3d",            &rb.Mass,           0.1f,  0.0f,  10000.0f, "%.2f kg");
            ImGui::DragFloat("Friction##rb3d",        &rb.Friction,       0.01f, 0.0f,  1.0f,     "%.2f");
            ImGui::DragFloat("Restitution##rb3d",     &rb.Restitution,    0.01f, 0.0f,  1.0f,     "%.2f");
            ImGui::DragFloat("Gravity Factor##rb3d",  &rb.GravityFactor,  0.01f, 0.0f,  10.0f,    "%.2f");
            ImGui::DragFloat("Linear Damping##rb3d",  &rb.LinearDamping,  0.01f, 0.0f,  1.0f,     "%.3f");
            ImGui::DragFloat("Angular Damping##rb3d", &rb.AngularDamping, 0.01f, 0.0f,  1.0f,     "%.3f");

            if (m_SimState == SceneState::Play || m_SimState == SceneState::Pause)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Live Velocity (read-only)");
                ImGui::LabelText("Linear##rb3d_lv",
                    "%.2f  %.2f  %.2f",
                    rb.LinearVelocity.x, rb.LinearVelocity.y, rb.LinearVelocity.z);
                ImGui::LabelText("Angular##rb3d_av",
                    "%.2f  %.2f  %.2f",
                    rb.AngularVelocity.x, rb.AngularVelocity.y, rb.AngularVelocity.z);
            }

            if (m_PhysicsSystem)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("World Gravity");
                glm::vec3 g = m_PhysicsSystem->GetGravity();
                DrawVec3Control("Gravity##physics_g", g, 0.1f);
                m_PhysicsSystem->SetGravity(g);
            }
        }

        ImGui::Spacing();
    }

    // -- BoxCollider3D (Jolt) --------------------------------------------------
    if (selected.HasComponent<BoxCollider3DComponent>())
    {
        const bool open   = ComponentHeader("Box Collider 3D");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##boxcol3d_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<BoxCollider3DComponent>();
        }
        else if (open)
        {
            auto& bc = selected.GetComponent<BoxCollider3DComponent>();
            DrawVec3Control("Half Extents##box3d", bc.HalfExtents, 0.01f, 0.5f);
            ImGui::DragFloat("Friction##box3d",     &bc.Friction,     0.01f, 0.0f, 1.0f,     "%.2f");
            ImGui::DragFloat("Restitution##box3d",  &bc.Restitution,  0.01f, 0.0f, 1.0f,     "%.2f");
            ImGui::DragFloat("Density##box3d",      &bc.Density,      0.1f,  0.0f, 10000.0f, "%.2f kg/m³");
        }

        ImGui::Spacing();
    }

    // -- SphereCollider3D (Jolt) -----------------------------------------------
    if (selected.HasComponent<SphereCollider3DComponent>())
    {
        const bool open   = ComponentHeader("Sphere Collider 3D");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##sphcol3d_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<SphereCollider3DComponent>();
        }
        else if (open)
        {
            auto& sc = selected.GetComponent<SphereCollider3DComponent>();
            ImGui::DragFloat("Radius##sphere3d",      &sc.Radius,      0.01f, 0.001f, 1000.0f, "%.3f m");
            ImGui::DragFloat("Friction##sphere3d",    &sc.Friction,    0.01f, 0.0f,   1.0f,    "%.2f");
            ImGui::DragFloat("Restitution##sphere3d", &sc.Restitution, 0.01f, 0.0f,   1.0f,    "%.2f");
            ImGui::DragFloat("Density##sphere3d",     &sc.Density,     0.1f,  0.0f,   10000.0f,"%.2f kg/m³");
        }

        ImGui::Spacing();
    }

    // -- RigidBody2D (Box2D v3) ------------------------------------------------
    if (selected.HasComponent<RigidBody2DComponent>())
    {
        const bool open   = ComponentHeader("Rigid Body 2D");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##rb2d_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<RigidBody2DComponent>();
        }
        else if (open)
        {
            auto& rb2 = selected.GetComponent<RigidBody2DComponent>();

            constexpr const char* kMotionTypes2D[] = { "Static", "Kinematic", "Dynamic" };
            int mt2 = static_cast<int>(rb2.MotionType);
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::Combo("Motion Type##rb2d", &mt2, kMotionTypes2D, 3))
            {
                rb2.MotionType    = static_cast<MotionType2D>(mt2);
                rb2.TransformDirty = true;
            }

            ImGui::Checkbox("Fixed Rotation##rb2d",   &rb2.FixedRotation);
            ImGui::DragFloat("Gravity Scale##rb2d",   &rb2.GravityScale,   0.01f, 0.0f,  10.0f, "%.2f");
            ImGui::DragFloat("Linear Damping##rb2d",  &rb2.LinearDamping,  0.01f, 0.0f,  1.0f,  "%.3f");
            ImGui::DragFloat("Angular Damping##rb2d", &rb2.AngularDamping, 0.01f, 0.0f,  1.0f,  "%.3f");

            if (m_SimState == SceneState::Play || m_SimState == SceneState::Pause)
            {
                ImGui::Spacing();
                ImGui::TextDisabled("Live Velocity (read-only)");
                ImGui::LabelText("Linear##rb2d_lv",  "%.2f  %.2f", rb2.LinVelX, rb2.LinVelY);
                ImGui::LabelText("Angular##rb2d_av", "%.2f rad/s", rb2.AngVel);
            }
        }

        ImGui::Spacing();
    }

    // -- BoxCollider2D (Box2D v3) ----------------------------------------------
    if (selected.HasComponent<BoxCollider2DComponent>())
    {
        const bool open   = ComponentHeader("Box Collider 2D");
        bool       remove = false;

        if (ImGui::BeginPopupContextItem("##boxcol2d_ctx"))
        {
            if (ImGui::MenuItem("Remove Component")) remove = true;
            ImGui::EndPopup();
        }

        if (remove)
        {
            selected.RemoveComponent<BoxCollider2DComponent>();
        }
        else if (open)
        {
            auto& bc2 = selected.GetComponent<BoxCollider2DComponent>();
            ImGui::DragFloat2("Half Extents##box2d", &bc2.HalfExtents.x, 0.01f, 0.001f, 1000.0f, "%.3f m");
            ImGui::DragFloat("Friction##box2d",     &bc2.Friction,     0.01f, 0.0f, 1.0f,     "%.2f");
            ImGui::DragFloat("Restitution##box2d",  &bc2.Restitution,  0.01f, 0.0f, 1.0f,     "%.2f");
            ImGui::DragFloat("Density##box2d",      &bc2.Density,      0.1f,  0.0f, 10000.0f, "%.2f kg/m²");
        }

        ImGui::Spacing();
    }

    // -- Add Component -----------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.000f, 0.384f, 0.800f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.102f, 0.471f, 0.878f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.000f, 0.306f, 0.651f, 1.0f));
    if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f)))
        ImGui::OpenPopup("##add_component");
    ImGui::PopStyleColor(3);

    if (ImGui::BeginPopup("##add_component"))
    {
        const bool hasMesh    = selected.HasComponent<MeshComponent>();
        const bool hasLight   = selected.HasComponent<LightComponent>();
        const bool hasCamera  = selected.HasComponent<CameraComponent>();
        const bool hasScript  = selected.HasComponent<ScriptComponent>();
        const bool hasRigid3D = selected.HasComponent<RigidBody3DComponent>();
        const bool hasBox3D   = selected.HasComponent<BoxCollider3DComponent>();
        const bool hasSph3D   = selected.HasComponent<SphereCollider3DComponent>();
        const bool hasRigid2D = selected.HasComponent<RigidBody2DComponent>();
        const bool hasBox2D   = selected.HasComponent<BoxCollider2DComponent>();

        if (!hasMesh && ImGui::MenuItem("Mesh"))
            selected.AddComponent<MeshComponent>();
        if (!hasLight && ImGui::MenuItem("Light"))
            selected.AddComponent<LightComponent>();
        if (!hasCamera && ImGui::MenuItem("Camera"))
            selected.AddComponent<CameraComponent>();
        if (!hasScript && ImGui::MenuItem("Script"))
            selected.AddComponent<ScriptComponent>();

        ImGui::Separator();
        ImGui::TextDisabled("Physics 3D");
        if (!hasRigid3D && ImGui::MenuItem("Rigid Body 3D"))
            selected.AddComponent<RigidBody3DComponent>();
        if (!hasBox3D && ImGui::MenuItem("Box Collider 3D"))
            selected.AddComponent<BoxCollider3DComponent>();
        if (!hasSph3D && ImGui::MenuItem("Sphere Collider 3D"))
            selected.AddComponent<SphereCollider3DComponent>();

        ImGui::Separator();
        ImGui::TextDisabled("Physics 2D");
        if (!hasRigid2D && ImGui::MenuItem("Rigid Body 2D"))
            selected.AddComponent<RigidBody2DComponent>();
        if (!hasBox2D && ImGui::MenuItem("Box Collider 2D"))
            selected.AddComponent<BoxCollider2DComponent>();

        if (hasMesh && hasLight && hasCamera && hasScript &&
            hasRigid3D && hasBox3D && hasSph3D && hasRigid2D && hasBox2D)
            ImGui::TextDisabled("All components attached");

        ImGui::EndPopup();
    }

    ImGui::End();
}

// =============================================================================
// Scene Viewport — editor navigation, gizmos, billboards
// =============================================================================

void EditorLayout::DrawSceneViewport()
{
    const bool isEditing = (m_SimState == SceneState::Edit);
    const bool isPlaying = (m_SimState == SceneState::Play);
    const bool isPaused  = (m_SimState == SceneState::Pause);

    if (!ImGui::Begin("Scene##panel", &m_SceneViewportVisible))
    {
        ImGui::End();
        return;
    }

    // ---- Simulation control toolbar (graphical transport icons) --------------
    if (SimIconButton("##sim_play", SimIcon::Play,
                      isEditing || isPaused,
                      IM_COL32(70, 220, 100, 255)))
    {
        if (isPaused) m_PauseRequested = true; // resume
        else          m_PlayRequested  = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(isPaused ? "Resume" : "Play");

    ImGui::SameLine(0.0f, 4.0f);
    if (SimIconButton("##sim_pause", SimIcon::Pause,
                      isPlaying, IM_COL32(240, 200, 60, 255)))
        m_PauseRequested = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pause");

    ImGui::SameLine(0.0f, 4.0f);
    if (SimIconButton("##sim_stop", SimIcon::Stop,
                      isPlaying || isPaused, IM_COL32(235, 80, 80, 255)))
        m_StopRequested = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stop");

    // State label — coloured text so the developer always knows which mode is live.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (isPlaying)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.95f, 0.35f, 1.0f));
        ImGui::TextUnformatted("PLAYING");
        ImGui::PopStyleColor();
    }
    else if (isPaused)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.10f, 1.0f));
        ImGui::TextUnformatted("PAUSED");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("EDITOR");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // ---- View mode / grid / gizmo toolbar -----------------------------------

    const char* kViewModes[] = { "Lit", "Unlit", "Wireframe", "2D" };
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##viewmode", &m_ViewMode, kViewModes, 4))
    {
        // Entering/leaving 2D toggles the camera's orthographic XY lock.
        m_Camera.Orthographic2D = (m_ViewMode == 3);
        if (m_Camera.Orthographic2D)
        {
            // Snap to a clean front-on framing of the XY plane.
            m_Camera.Position = glm::vec3(0.0f, 0.0f, 20.0f);
        }
    }
    const int viewMode = m_ViewMode;

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    static bool showGrid = true;
    ImGui::Checkbox("Grid", &showGrid);
    // Grid is always hidden during simulation so gameplay isn't cluttered.
    const bool renderGrid = showGrid && isEditing;

    // Gizmo tool selector — disabled during Play / Pause to freeze handles.
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    if (!isEditing) ImGui::BeginDisabled();
    const auto toolButton = [this](const char* label, ImGuizmo::OPERATION op)
    {
        const bool active = (m_GizmoOperation == static_cast<int>(op));
        ImGui::SameLine();
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.000f, 0.384f, 0.800f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.102f, 0.471f, 0.878f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.000f, 0.306f, 0.651f, 1.0f));
        }
        if (ImGui::SmallButton(label))
            m_GizmoOperation = static_cast<int>(op);
        if (active)
            ImGui::PopStyleColor(3);
    };
    toolButton("Move (W)",   ImGuizmo::TRANSLATE);
    toolButton("Rotate (E)", ImGuizmo::ROTATE);
    toolButton("Scale (R)",  ImGuizmo::SCALE);
    if (!isEditing) ImGui::EndDisabled();

    ImGui::Separator();

    // ---- Accent border line (Play = green, Pause = amber) -------------------
    // Drawn flush against the separator so the developer has instant visual
    // confirmation that the simulation loop is running.
    if (!isEditing)
    {
        const ImVec2 p0  = ImGui::GetCursorScreenPos();
        const float  barW = ImGui::GetContentRegionAvail().x;
        const ImVec2 p1  = ImVec2(p0.x + barW, p0.y + 3.0f);
        const ImU32  col = isPlaying
                           ? IM_COL32(50, 210, 80,  220)
                           : IM_COL32(220, 180, 10, 220);
        ImGui::GetWindowDrawList()->AddRectFilled(p0, p1, col);
        // Nudge the cursor so the image starts below the accent bar.
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 3.0f));
    }

    // ---- Rendered image -----------------------------------------------------
    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const int    panelW = static_cast<int>(avail.x);
    const int    panelH = static_cast<int>(avail.y);

    if (panelW > 0 && panelH > 0)
    {
        fadix::RenderOptions options;
        options.Mode     = static_cast<fadix::ViewMode>(viewMode);
        options.DrawGrid = renderGrid;

        m_Viewport.Resize(panelW, panelH);

        const glm::mat4 view = m_Camera.GetViewMatrix();
        const glm::mat4 proj = m_Camera.GetProjectionMatrix(avail.x, avail.y);
        m_Viewport.Render(view, proj, m_Camera.Position, m_ActiveScene, options);

        // GL FBO has bottom-left origin — flip V.
        ImGui::Image(
            (ImTextureID)(uintptr_t)m_Viewport.GetTexture(),
            avail, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));

        // Capture the image rect before any other items change it.
        const ImVec2 imageMin     = ImGui::GetItemRectMin();
        const ImVec2 imageSize    = ImGui::GetItemRectSize();
        const bool   imageHovered = ImGui::IsItemHovered();

        // ---- Entity ID picking -----------------------------------------------
        // Only in Edit mode; skip if the cursor is over a gizmo handle so that
        // clicking a translation arrow doesn't accidentally change the selection.
        // Billboard icons (drawn later) take priority — they overwrite this
        // result on the same click.
        if (isEditing && imageHovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGuizmo::IsOver())
        {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            // Map display-space coords to GL framebuffer coords (flip Y).
            const int pixX = static_cast<int>(mouse.x - imageMin.x);
            const int pixY = m_Viewport.GetHeight() - 1
                           - static_cast<int>(mouse.y - imageMin.y);

            const int entityID = m_Viewport.ReadEntityAtPixel(pixX, pixY);
            if (entityID >= 0)
                m_SelectedEntity = static_cast<entt::entity>(
                    static_cast<uint32_t>(entityID));
            else
                m_SelectedEntity = entt::null;
        }

        // Asset drag-and-drop target (always active).
        if (ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayloadType))
                SpawnEntityFromAsset(static_cast<const char*>(payload->Data));
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFilePathPayloadType))
                SpawnEntityFromAsset(static_cast<const char*>(payload->Data));
            ImGui::EndDragDropTarget();
        }

        // W/E/R gizmo-tool shortcuts — only honoured in Edit mode.
        if (isEditing &&
            ImGui::IsWindowHovered() &&
            !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
            !ImGui::GetIO().WantTextInput)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_W))
                m_GizmoOperation = static_cast<int>(ImGuizmo::TRANSLATE);
            if (ImGui::IsKeyPressed(ImGuiKey_E))
                m_GizmoOperation = static_cast<int>(ImGuizmo::ROTATE);
            if (ImGui::IsKeyPressed(ImGuiKey_R))
                m_GizmoOperation = static_cast<int>(ImGuizmo::SCALE);
        }

        // Physical Delete key removes the selected entity from the viewport.
        if (isEditing &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
            m_SelectedEntity != entt::null)
        {
            DeleteEntityWithHistory(m_SelectedEntity);
        }

        // Transform gizmo — fully frozen during Play / Pause so the developer
        // cannot accidentally move entities while the simulation is running.
        if (isEditing && m_ActiveScene && m_SelectedEntity != entt::null &&
            m_ActiveScene->GetRegistry().valid(m_SelectedEntity))
        {
            Entity selected(m_SelectedEntity, &m_ActiveScene->GetRegistry());
            if (selected.HasComponent<TransformComponent>())
            {
                auto& tc = selected.GetComponent<TransformComponent>();

                ImGuizmo::SetOrthographic(viewMode == 3);
                ImGuizmo::SetDrawlist();
                ImGuizmo::SetRect(imageMin.x, imageMin.y, imageSize.x, imageSize.y);

                glm::mat4 matrix =
                    glm::translate(glm::mat4(1.0f), tc.Position)
                    * glm::mat4_cast(glm::quat(glm::radians(tc.Rotation)))
                    * glm::scale(glm::mat4(1.0f), tc.Scale);

                ImGuizmo::Manipulate(
                    glm::value_ptr(view), glm::value_ptr(proj),
                    static_cast<ImGuizmo::OPERATION>(m_GizmoOperation),
                    ImGuizmo::LOCAL, glm::value_ptr(matrix));

                const bool usingNow = ImGuizmo::IsUsing();

                // Rising edge: capture the pre-drag transform for undo.
                if (usingNow && !m_GizmoWasUsing)
                    m_GizmoStartTransform = tc;

                if (usingNow)
                {
                    float translation[3] = {};
                    float rotation[3]    = {};
                    float scale[3]       = {};
                    ImGuizmo::DecomposeMatrixToComponents(
                        glm::value_ptr(matrix), translation, rotation, scale);

                    tc.Position = glm::vec3(translation[0], translation[1], translation[2]);
                    tc.Rotation = glm::vec3(rotation[0], rotation[1], rotation[2]); // degrees
                    tc.Scale    = glm::vec3(scale[0], scale[1], scale[2]);

                    auto& gizmoReg = m_ActiveScene->GetRegistry();
                    if (auto* rb = gizmoReg.try_get<RigidBody3DComponent>(m_SelectedEntity))
                        rb->TransformDirty = true;
                    if (auto* rb2 = gizmoReg.try_get<RigidBody2DComponent>(m_SelectedEntity))
                        rb2->TransformDirty = true;
                }

                // Falling edge: the drag ended — one coalesced history entry.
                if (!usingNow && m_GizmoWasUsing)
                    m_History.PushTransformEdit(m_SelectedEntity,
                                                m_GizmoStartTransform, tc,
                                                "Move/Rotate/Scale");

                m_GizmoWasUsing = usingNow;
            }
        }
        else
        {
            m_GizmoWasUsing = false;
        }

        // ---- Overlays --------------------------------------------------------
        if (viewMode == 3)
            Draw2DGridOverlay(imageMin, imageSize);
        else if (isEditing)
            DrawViewportBillboards(proj * view, imageMin, imageSize);
    }
    else
    {
        ImGui::Dummy(avail);
    }

    ImGui::End();
}

// =============================================================================
// Game Viewport — strict primary-camera POV
// =============================================================================

void EditorLayout::DrawGameViewport()
{
    if (!ImGui::Begin("Game##panel", &m_GameViewportVisible))
    {
        ImGui::End();
        return;
    }

    // Slim status strip mirrors the simulation state.
    if (m_SimState == SceneState::Play)
        ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.35f, 1.0f), "LIVE");
    else if (m_SimState == SceneState::Pause)
        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.10f, 1.0f), "PAUSED");
    else
        ImGui::TextDisabled("Camera Preview");
    ImGui::Separator();

    const ImVec2 avail  = ImGui::GetContentRegionAvail();
    const int    panelW = static_cast<int>(avail.x);
    const int    panelH = static_cast<int>(avail.y);

    if (panelW <= 0 || panelH <= 0)
    {
        ImGui::Dummy(avail);
        ImGui::End();
        return;
    }

    glm::mat4 camView, camProj;
    glm::vec3 camPos;
    const float aspect = avail.x / avail.y;

    if (fadix::ViewportRenderer::GetPrimaryCameraMatrices(
            m_ActiveScene, aspect, camView, camProj, camPos))
    {
        fadix::RenderOptions options;
        options.DrawGrid = false; // never clutter the game view

        m_GameViewport.Resize(panelW, panelH);
        m_GameViewport.Render(camView, camProj, camPos, m_ActiveScene, options);

        ImGui::Image(
            (ImTextureID)(uintptr_t)m_GameViewport.GetTexture(),
            avail, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
    }
    else
    {
        // No CameraComponent anywhere in the scene → clean, readable warning.
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList*  dl     = ImGui::GetWindowDrawList();
        dl->AddRectFilled(origin,
                          ImVec2(origin.x + avail.x, origin.y + avail.y),
                          IM_COL32(14, 17, 22, 255));

        const char*  kWarning = "No Active Camera in Scene";
        const ImVec2 textSize = ImGui::CalcTextSize(kWarning);
        const ImVec2 textPos(origin.x + (avail.x - textSize.x) * 0.5f,
                             origin.y + (avail.y - textSize.y) * 0.5f);

        dl->AddText(textPos, IM_COL32(235, 200, 90, 255), kWarning);

        const char*  kHint = "Add a Camera component to an entity to see its view here.";
        const ImVec2 hintSize = ImGui::CalcTextSize(kHint);
        dl->AddText(ImVec2(origin.x + (avail.x - hintSize.x) * 0.5f,
                           textPos.y + textSize.y + 8.0f),
                    IM_COL32(140, 150, 165, 200), kHint);

        ImGui::Dummy(avail);
    }

    ImGui::End();
}

// =============================================================================
// Scene-viewport overlays: entity billboards + 2D grid
// =============================================================================

// Camera / light icons drawn at each entity's world position; clicking an
// icon selects the entity (takes priority over the pixel-pick result).
void EditorLayout::DrawViewportBillboards(const glm::mat4& viewProj,
                                          const ImVec2& imageMin,
                                          const ImVec2& imageSize)
{
    if (!m_ActiveScene) return;

    entt::registry& registry = m_ActiveScene->GetRegistry();
    ImDrawList*     dl       = ImGui::GetWindowDrawList();

    const bool clicked =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver();
    const ImVec2 mouse = ImGui::GetIO().MousePos;

    const auto iconHit = [&](const ImVec2& center, float radius) -> bool
    {
        return clicked &&
               mouse.x >= center.x - radius && mouse.x <= center.x + radius &&
               mouse.y >= center.y - radius && mouse.y <= center.y + radius;
    };

    // ---- Camera entities: wireframe body + lens ------------------------------
    for (auto entity : registry.view<TransformComponent, CameraComponent>())
    {
        const glm::mat4 world = Scene::GetWorldMatrix(registry, entity);
        ImVec2 p;
        if (!WorldToScreen(viewProj, glm::vec3(world[3]), imageMin, imageSize, p))
            continue;

        const bool  sel = (entity == m_SelectedEntity);
        const ImU32 col = sel ? IM_COL32(90, 170, 255, 255)
                              : IM_COL32(225, 230, 240, 220);

        // Body (rounded rect), lens (triangle), viewfinder bump.
        dl->AddRect(ImVec2(p.x - 11.0f, p.y - 7.0f),
                    ImVec2(p.x + 5.0f,  p.y + 7.0f), col, 2.0f, 0, 1.6f);
        dl->AddTriangle(ImVec2(p.x + 5.0f,  p.y - 4.0f),
                        ImVec2(p.x + 12.0f, p.y),
                        ImVec2(p.x + 5.0f,  p.y + 4.0f), col, 1.6f);
        dl->AddRect(ImVec2(p.x - 8.0f, p.y - 11.0f),
                    ImVec2(p.x - 2.0f, p.y - 7.0f), col, 1.0f, 0, 1.6f);

        // Forward frustum hint: short line along the camera's -Z.
        const glm::vec3 fwd = -glm::normalize(glm::vec3(world[2]));
        ImVec2 tip;
        if (WorldToScreen(viewProj, glm::vec3(world[3]) + fwd * 1.5f,
                          imageMin, imageSize, tip))
            dl->AddLine(p, tip, IM_COL32(90, 170, 255, 160), 1.2f);

        if (iconHit(p, 14.0f)) m_SelectedEntity = entity;
    }

    // ---- Light entities -------------------------------------------------------
    for (auto entity : registry.view<TransformComponent, LightComponent>())
    {
        const auto&     light = registry.get<LightComponent>(entity);
        const glm::mat4 world = Scene::GetWorldMatrix(registry, entity);
        ImVec2 p;
        if (!WorldToScreen(viewProj, glm::vec3(world[3]), imageMin, imageSize, p))
            continue;

        const bool  sel = (entity == m_SelectedEntity);
        const ImU32 col = sel ? IM_COL32(90, 170, 255, 255)
                              : IM_COL32(255, 214, 90, 230);

        if (light.Type == LightType::Directional)
        {
            // Sun: disc + 8 rays + an arrow along the emission direction.
            dl->AddCircle(p, 6.0f, col, 12, 1.8f);
            for (int i = 0; i < 8; ++i)
            {
                const float a  = static_cast<float>(i) * 3.14159265f / 4.0f;
                const float ca = std::cos(a), sa = std::sin(a);
                dl->AddLine(ImVec2(p.x + ca * 8.5f,  p.y + sa * 8.5f),
                            ImVec2(p.x + ca * 12.5f, p.y + sa * 12.5f),
                            col, 1.6f);
            }

            const glm::vec3 dir = -glm::normalize(glm::vec3(world[2]));
            ImVec2 tip;
            if (WorldToScreen(viewProj, glm::vec3(world[3]) + dir * 2.5f,
                              imageMin, imageSize, tip))
            {
                dl->AddLine(p, tip, col, 1.6f);
                // Arrowhead
                const ImVec2 d(tip.x - p.x, tip.y - p.y);
                const float  len = std::sqrt(d.x * d.x + d.y * d.y);
                if (len > 8.0f)
                {
                    const ImVec2 n(d.x / len, d.y / len);
                    const ImVec2 perp(-n.y, n.x);
                    dl->AddTriangleFilled(
                        tip,
                        ImVec2(tip.x - n.x * 7.0f + perp.x * 3.5f,
                               tip.y - n.y * 7.0f + perp.y * 3.5f),
                        ImVec2(tip.x - n.x * 7.0f - perp.x * 3.5f,
                               tip.y - n.y * 7.0f - perp.y * 3.5f),
                        col);
                }
            }
        }
        else if (light.Type == LightType::Point)
        {
            dl->AddCircle(p, 6.0f, col, 12, 1.8f);
            dl->AddCircleFilled(p, 2.2f, col);
        }
        else // Spot
        {
            dl->AddTriangle(ImVec2(p.x, p.y - 7.0f),
                            ImVec2(p.x - 6.0f, p.y + 6.0f),
                            ImVec2(p.x + 6.0f, p.y + 6.0f), col, 1.8f);
        }

        if (iconHit(p, 14.0f)) m_SelectedEntity = entity;
    }
}

// Screen-space 2D grid drawn over the viewport image in 2D mode — replaces
// the raycasted 3D ground grid, which is invisible edge-on.
void EditorLayout::Draw2DGridOverlay(const ImVec2& imageMin, const ImVec2& imageSize)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(imageMin,
                     ImVec2(imageMin.x + imageSize.x, imageMin.y + imageSize.y),
                     true);

    const float halfH = m_Camera.OrthoHalfHeight;
    const float halfW = halfH * (imageSize.x / imageSize.y);
    const float pps   = imageSize.y / (2.0f * halfH);   // pixels per world unit

    // Adaptive cadence: keep grid lines at least ~14 px apart.
    float step = 1.0f;
    while (step * pps < 14.0f) step *= 10.0f;
    while (step * pps > 200.0f) step *= 0.1f;

    const glm::vec3 cam = m_Camera.Position;
    const ImU32 minor = IM_COL32(105, 112, 122, 60);
    const ImU32 axisX = IM_COL32(230, 90, 90, 160);   // world Y=0 line
    const ImU32 axisY = IM_COL32(80, 120, 235, 160);  // world X=0 line

    const auto worldToPx = [&](float wx, float wy) -> ImVec2
    {
        return ImVec2(imageMin.x + (wx - (cam.x - halfW)) * pps,
                      imageMin.y + ((cam.y + halfH) - wy) * pps);
    };

    // Vertical lines
    for (float wx = std::floor((cam.x - halfW) / step) * step;
         wx <= cam.x + halfW; wx += step)
    {
        const ImVec2 a = worldToPx(wx, cam.y - halfH);
        const ImVec2 b = worldToPx(wx, cam.y + halfH);
        dl->AddLine(a, b, (std::abs(wx) < step * 0.5f) ? axisY : minor,
                    (std::abs(wx) < step * 0.5f) ? 1.8f : 1.0f);
    }
    // Horizontal lines
    for (float wy = std::floor((cam.y - halfH) / step) * step;
         wy <= cam.y + halfH; wy += step)
    {
        const ImVec2 a = worldToPx(cam.x - halfW, wy);
        const ImVec2 b = worldToPx(cam.x + halfW, wy);
        dl->AddLine(a, b, (std::abs(wy) < step * 0.5f) ? axisX : minor,
                    (std::abs(wy) < step * 0.5f) ? 1.8f : 1.0f);
    }

    dl->PopClipRect();
}

// =============================================================================
// Content Drawer — live view of the VFS asset index
// =============================================================================

void EditorLayout::DrawContentDrawer()
{
    if (!ImGui::Begin("Content Drawer##panel", &m_contentDrawerVisible))
    {
        ImGui::End();
        return;
    }

    if (!m_FileSystem || !m_FileSystem->HasRoot())
    {
        ImGui::TextDisabled("No asset directory mounted");
        ImGui::End();
        return;
    }

    // ---- Drag-hover navigation ------------------------------------------------
    // While an asset drag is in flight, hovering a folder icon or breadcrumb
    // for kHoverNavDelay seconds auto-opens that directory so files can be
    // dropped deep inside subfolders in one gesture.
    if (ImGui::GetDragDropPayload() == nullptr)
    {
        m_HoverNavTarget.clear();
        m_HoverNavTimer = 0.0f;
    }

    const auto tickHoverNav = [this](const fs::path& target)
    {
        if (m_HoverNavTarget != target)
        {
            m_HoverNavTarget = target;
            m_HoverNavTimer  = 0.0f;
        }
        m_HoverNavTimer += ImGui::GetIO().DeltaTime;
        if (m_HoverNavTimer >= kHoverNavDelay)
        {
            m_ContentDir = target;
            m_HoverNavTarget.clear();
            m_HoverNavTimer = 0.0f;
        }
    };

    const fs::path root = m_FileSystem->GetRoot();

    // Keep the browsing directory valid even if it was deleted externally.
    if (m_ContentDir.empty() || !m_FileSystem->Contains(m_ContentDir))
        m_ContentDir = root;

    // ---- Breadcrumb navigation ----------------------------------------------
    if (ImGui::SmallButton("Assets"))
        m_ContentDir = root;

    // Drop target: move any dragged file into the root assets directory.
    // Hovering the breadcrumb during a drag also navigates there.
    if (ImGui::BeginDragDropTarget())
    {
        tickHoverNav(root);
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFilePathPayloadType))
        {
            const fs::path srcPath(static_cast<const char*>(payload->Data));
            const fs::path destPath = root / srcPath.filename();
            if (srcPath.parent_path() != root)
            {
                std::error_code ec;
                fs::rename(srcPath, destPath, ec);
                if (ec)
                    std::cerr << "[Fadix] Failed to move file: " << ec.message() << '\n';
                else
                {
                    m_ScriptEditor.RenameFile(srcPath.string(), destPath.string());
                    m_AssetRegistry.OnAssetRenamed(srcPath, destPath);
                }
                if (m_FileSystem) m_FileSystem->Refresh();
            }
        }
        ImGui::EndDragDropTarget();
    }

    const fs::path relative = m_ContentDir.lexically_relative(root);
    if (!relative.empty() && relative != ".")
    {
        fs::path accumulated = root;
        int      depth       = 0;
        for (const auto& part : relative)
        {
            accumulated /= part;
            ImGui::SameLine(0.0f, 2.0f);
            ImGui::TextDisabled("/");
            ImGui::SameLine(0.0f, 2.0f);

            ImGui::PushID(depth++);
            if (ImGui::SmallButton(part.string().c_str()))
                m_ContentDir = accumulated;

            // Drop target: move file into this breadcrumb folder.
            if (ImGui::BeginDragDropTarget())
            {
                tickHoverNav(accumulated);
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFilePathPayloadType))
                {
                    const char* srcPathCStr = static_cast<const char*>(payload->Data);
                    const fs::path srcPath(srcPathCStr);
                    const fs::path destPath = accumulated / srcPath.filename();

                    std::error_code ec;
                    fs::rename(srcPath, destPath, ec);
                    if (ec)
                        std::cerr << "[Fadix] Failed to move file: " << ec.message() << '\n';
                    else
                        m_AssetRegistry.OnAssetRenamed(srcPath, destPath);

                    m_ScriptEditor.CloseFile(srcPath.string());
                    if (m_FileSystem) m_FileSystem->Refresh();
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::PopID();
        }
    }

    const auto& entries = m_FileSystem->List(m_ContentDir);

    ImGui::SameLine();
    ImGui::TextDisabled("—  %d item(s), %d file(s) indexed",
                        static_cast<int>(entries.size()),
                        static_cast<int>(m_FileSystem->IndexedFileCount()));

    ImGui::Separator();

    // ---- Asset grid (skipped for empty directories, but fall-through continues)
    if (entries.empty())
    {
        ImGui::TextDisabled("(empty folder)");
    }
    else
    {

    const float thumbnailSize = (m_EditorSettings.ThumbnailSize > 32.0f)
                                    ? m_EditorSettings.ThumbnailSize : 32.0f;
    constexpr float kCellPadding = 16.0f;

    const float panelWidth = ImGui::GetContentRegionAvail().x;
    const int   columns    = (panelWidth > (thumbnailSize + kCellPadding))
        ? static_cast<int>(panelWidth / (thumbnailSize + kCellPadding))
        : 1;

    fs::path pendingDir;
    bool     navigate = false;

    if (ImGui::BeginTable("##content_grid", columns))
    {
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
        {
            const fadix::FileEntry& entry = entries[static_cast<std::size_t>(i)];

            // .meta files are internal bookkeeping — never shown to the developer.
            if (entry.Extension == ".meta") continue;

            // Lazily register the asset so it has a stable UUID.
            // No-op when already registered; generates the .meta file on first encounter.
            if (!entry.IsDirectory)
                m_AssetRegistry.ImportAsset(entry.Path);

            ImGui::TableNextColumn();
            ImGui::PushID(i);

            const float  cellX  = ImGui::GetCursorPosX();
            const ImVec2 origin = ImGui::GetCursorScreenPos();

            ImGui::InvisibleButton("##cell", ImVec2(thumbnailSize, thumbnailSize));
            const bool hovered = ImGui::IsItemHovered();

            // Click selects the item (Delete-key target, visual highlight).
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                m_DrawerSelectedPath = entry.Path.string();
            const bool isSelected = (m_DrawerSelectedPath == entry.Path.string());

            // -- Right-click context menu on asset items ----------------------
            if (ImGui::BeginPopupContextItem("##asset_ctx"))
            {
                m_DrawerSelectedPath = entry.Path.string();

                const auto beginRename = [&]()
                {
                    m_RenameTargetPath = entry.Path.string();
                    const std::size_t len = entry.Name.size();
                    const std::size_t cap = sizeof(m_RenameBuffer) - 1;
                    std::memcpy(m_RenameBuffer, entry.Name.c_str(), (len < cap) ? len : cap);
                    m_RenameBuffer[(len < cap) ? len : cap] = '\0';
                    m_RenamePending = true;
                };

                if (!entry.IsDirectory)
                {
                    if (ImGui::MenuItem("Rename"))
                        beginRename();
                    if (ImGui::MenuItem("Delete", "Del"))
                    {
                        m_DeleteTargetPath  = entry.Path.string();
                        m_DeleteIsDirectory = false;
                        m_DeletePending     = true;
                    }
                    ImGui::Separator();
                }
                else
                {
                    if (ImGui::MenuItem("Open Folder"))
                        m_ContentDir = entry.Path;
                    if (ImGui::MenuItem("Rename Folder"))
                        beginRename();
                    if (ImGui::MenuItem("Delete Folder", "Del"))
                    {
                        m_DeleteTargetPath  = entry.Path.string();
                        m_DeleteIsDirectory = true;
                        m_DeletePending     = true;
                    }
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Show in Explorer"))
                {
#ifdef _WIN32
                    const std::string absPath = entry.Path.is_absolute()
                        ? entry.Path.string()
                        : fs::absolute(entry.Path).string();
                    const std::string arg = "/select,\"" + absPath + "\"";
                    ShellExecuteA(nullptr, "open", "explorer.exe",
                                  arg.c_str(), nullptr, SW_SHOWNORMAL);
#endif
                }
                ImGui::EndPopup();
            }

            // -- Drag-drop source for files ------------------------------------
            if (!entry.IsDirectory)
            {
                if (ImGui::BeginDragDropSource())
                {
                    const std::string payloadPath = entry.Path.string();
                    ImGui::SetDragDropPayload(kFilePathPayloadType,
                                              payloadPath.c_str(), payloadPath.size() + 1);
                    ImGui::TextUnformatted(entry.Name.c_str());
                    ImGui::EndDragDropSource();
                }
            }

            // -- Drag-drop target for directories (file move) -----------------
            if (entry.IsDirectory)
            {
                if (ImGui::BeginDragDropTarget())
                {
                    tickHoverNav(entry.Path);
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kFilePathPayloadType))
                    {
                        const char* srcPathCStr = static_cast<const char*>(payload->Data);
                        const fs::path srcPath(srcPathCStr);
                        const fs::path destPath = entry.Path / srcPath.filename();

                        std::error_code ec;
                        fs::rename(srcPath, destPath, ec);
                        if (ec)
                            std::cerr << "[Fadix] Failed to move file: " << ec.message() << '\n';
                        else
                            m_AssetRegistry.OnAssetRenamed(srcPath, destPath);

                        // Close editor tab if it was tracking the moved file.
                        m_ScriptEditor.CloseFile(srcPath.string());

                        if (m_FileSystem) m_FileSystem->Refresh();
                    }
                    ImGui::EndDragDropTarget();
                }
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(
                origin,
                ImVec2(origin.x + thumbnailSize, origin.y + thumbnailSize),
                hovered ? IM_COL32(58, 72, 86, 255) : IM_COL32(42, 53, 64, 255), 4.0f);
            if (isSelected)
                dl->AddRect(
                    origin,
                    ImVec2(origin.x + thumbnailSize, origin.y + thumbnailSize),
                    IM_COL32(0, 120, 230, 255), 4.0f, 0, 2.0f);

            if (entry.IsDirectory)
            {
                const float s = thumbnailSize / 64.0f; // icon designed at 64 px
                dl->AddRectFilled(
                    ImVec2(origin.x + 16.0f * s, origin.y + 14.0f * s),
                    ImVec2(origin.x + 48.0f * s, origin.y + 38.0f * s),
                    IM_COL32(0, 98, 204, 255), 2.0f);
                dl->AddRectFilled(
                    ImVec2(origin.x + 16.0f * s, origin.y + 11.0f * s),
                    ImVec2(origin.x + 30.0f * s, origin.y + 16.0f * s),
                    IM_COL32(0, 98, 204, 255), 2.0f);

                if (hovered && ImGui::IsMouseDoubleClicked(0))
                {
                    pendingDir = entry.Path;
                    navigate   = true;
                }
            }
            else
            {
                // File icon: dark sheet with an accent strip per asset category.
                ImU32 accent = IM_COL32(120, 130, 140, 255);
                if (entry.Extension == ".gltf" || entry.Extension == ".glb")
                    accent = IM_COL32(60, 180, 90, 255);
                else if (entry.Extension == ".png" || entry.Extension == ".jpg" ||
                         entry.Extension == ".jpeg" || entry.Extension == ".bmp" ||
                         entry.Extension == ".tga")
                    accent = IM_COL32(170, 100, 220, 255);
                else if (entry.Extension == ".fadixscene" || entry.Extension == ".fadix" ||
                         entry.Extension == ".json")
                    accent = IM_COL32(230, 160, 40, 255);
                else if (entry.Extension == ".cpp" || entry.Extension == ".hpp" ||
                         entry.Extension == ".h"   || entry.Extension == ".cc")
                    accent = IM_COL32(80, 200, 180, 255); // teal — C++ source files

                dl->AddRectFilled(
                    ImVec2(origin.x + thumbnailSize * 0.15f, origin.y + thumbnailSize * 0.12f),
                    ImVec2(origin.x + thumbnailSize * 0.85f, origin.y + thumbnailSize * 0.88f),
                    IM_COL32(30, 38, 46, 255), 3.0f);
                dl->AddRectFilled(
                    ImVec2(origin.x + thumbnailSize * 0.15f, origin.y + thumbnailSize * 0.12f),
                    ImVec2(origin.x + thumbnailSize * 0.85f, origin.y + thumbnailSize * 0.24f),
                    accent, 3.0f);

                // Extension label centered on the sheet.
                std::string extLabel = entry.Extension.empty()
                    ? std::string("?") : entry.Extension.substr(1);
                if (extLabel.size() > 5) extLabel.resize(5);
                for (char& ch : extLabel)
                    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

                const ImVec2 extSize = ImGui::CalcTextSize(extLabel.c_str());
                dl->AddText(
                    ImVec2(origin.x + (thumbnailSize - extSize.x) * 0.5f,
                           origin.y + (thumbnailSize - extSize.y) * 0.55f),
                    IM_COL32(200, 210, 220, 220), extLabel.c_str());

                // Double-click on C++ source/header: open in the Script Editor.
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    const std::string& ext = entry.Extension;
                    if (ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".cc")
                        m_ScriptEditor.OpenFile(entry.Path.string());
                }
            }

            const ImVec2 textSize = ImGui::CalcTextSize(entry.Name.c_str());
            const float  offset   = (thumbnailSize - textSize.x) * 0.5f;
            ImGui::SetCursorPosX(cellX + ((offset > 0.0f) ? offset : 0.0f));
            ImGui::TextUnformatted(entry.Name.c_str());

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (navigate)
        m_ContentDir = pendingDir;

    } // end of non-empty grid block

    // ---- Right-click empty area → creation context menu ----------------------
    if (ImGui::BeginPopupContextWindow("##content_bg_ctx",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        if (ImGui::MenuItem("Create New Script"))
            m_NewScriptPending = true;
        if (ImGui::MenuItem("Create Folder"))
        {
            std::snprintf(m_NewFolderBuf, sizeof(m_NewFolderBuf), "NewFolder");
            m_NewFolderPending = true;
        }
        ImGui::EndPopup();
    }

    // ---- Physical Delete key → delete the selected item (with confirm) -------
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false) &&
        !m_DrawerSelectedPath.empty() && !m_DeletePending)
    {
        std::error_code ec;
        const fs::path selected(m_DrawerSelectedPath);
        if (fs::exists(selected, ec))
        {
            m_DeleteTargetPath  = m_DrawerSelectedPath;
            m_DeleteIsDirectory = fs::is_directory(selected, ec);
            m_DeletePending     = true;
        }
    }

    // Queue the folder-name modal open on the frame we receive the request.
    if (m_NewFolderPending)
    {
        ImGui::OpenPopup("New Folder##fadix_new_folder");
        m_NewFolderPending = false;
    }

    // ---- "New Folder" name-input modal ----------------------------------------
    const ImVec2 folderModalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(folderModalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New Folder##fadix_new_folder", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("Folder name:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputText("##new_folder_name", m_NewFolderBuf, sizeof(m_NewFolderBuf));
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.38f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.47f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.31f, 0.65f, 1.0f));
        if (ImGui::Button("Create", ImVec2(130.0f, 0.0f)))
        {
            if (m_NewFolderBuf[0] != '\0')
            {
                std::error_code ec;
                fs::create_directory(m_ContentDir / m_NewFolderBuf, ec);
                if (ec)
                    std::cerr << "[Fadix] Create folder failed: "
                              << ec.message() << '\n';
                if (m_FileSystem) m_FileSystem->Refresh();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // Queue the modal open on the frame we receive the request (must be done
    // outside the popup so the modal can be rendered as a top-level window).
    if (m_NewScriptPending)
    {
        ImGui::OpenPopup("New Script##fadix_new_script");
        m_NewScriptPending = false;
    }

    // ---- "New Script" name-input modal ---------------------------------------
    const ImVec2 modalCenter = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New Script##fadix_new_script", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        static char s_ScriptNameBuf[128] = "MyScript";
        ImGui::TextUnformatted("Script class name:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputText("##new_script_class", s_ScriptNameBuf, sizeof(s_ScriptNameBuf));
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.38f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.47f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.31f, 0.65f, 1.0f));
        if (ImGui::Button("Create", ImVec2(130.0f, 0.0f)))
        {
            if (s_ScriptNameBuf[0] != '\0')
                CreateScriptTemplate(s_ScriptNameBuf);
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // Queue the rename modal open on the frame we receive the request.
    if (m_RenamePending)
    {
        ImGui::OpenPopup("RenameAsset##fadix_rename");
        m_RenamePending = false;
    }

    // ---- "Rename Asset" modal ------------------------------------------------
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("RenameAsset##fadix_rename", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted("New name:");
        ImGui::Spacing();
        ImGui::SetNextItemWidth(280.0f);
        ImGui::InputText("##rename_input", m_RenameBuffer, sizeof(m_RenameBuffer));
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.00f, 0.38f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.47f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.00f, 0.31f, 0.65f, 1.0f));
        if (ImGui::Button("Rename", ImVec2(130.0f, 0.0f)))
        {
            if (m_RenameBuffer[0] != '\0' && !m_RenameTargetPath.empty())
            {
                const fs::path oldPath(m_RenameTargetPath);
                const fs::path newName(m_RenameBuffer);
                const fs::path newPath = oldPath.parent_path() / newName;

                if (newName != oldPath.filename() && !fs::exists(newPath))
                {
                    std::error_code ec;
                    fs::rename(oldPath, newPath, ec);
                    if (ec)
                    {
                        std::cerr << "[Fadix] Rename failed: " << ec.message() << '\n';
                    }
                    else
                    {
                        m_ScriptEditor.RenameFile(oldPath.string(), newPath.string());
                        m_AssetRegistry.OnAssetRenamed(oldPath, newPath);
                    }
                }

                if (m_FileSystem) m_FileSystem->Refresh();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    // Queue the delete modal open on the frame we receive the request.
    if (m_DeletePending)
    {
        ImGui::OpenPopup("DeleteAsset##fadix_delete");
        m_DeletePending = false;
        m_DeleteConfirm = false;
    }

    // ---- "Delete Asset" confirmation modal -----------------------------------
    ImGui::SetNextWindowPos(modalCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("DeleteAsset##fadix_delete", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
    {
        const fs::path delPath(m_DeleteTargetPath);
        ImGui::TextUnformatted(m_DeleteIsDirectory
            ? "Delete this folder and EVERYTHING inside it?"
            : "Are you sure you want to delete this file?");
        ImGui::Spacing();
        ImGui::TextDisabled("%s", delPath.filename().string().c_str());
        if (m_DeleteIsDirectory)
        {
            std::error_code countEc;
            std::uintmax_t  fileCount = 0;
            for (auto it = fs::recursive_directory_iterator(delPath, countEc);
                 !countEc && it != fs::recursive_directory_iterator(); ++it)
                if (!it->is_directory(countEc)) ++fileCount;
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.30f, 1.0f),
                               "%llu file(s) will be permanently removed.",
                               static_cast<unsigned long long>(fileCount));
        }
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.60f, 0.12f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.18f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.08f, 0.08f, 1.0f));
        if (ImGui::Button("Delete", ImVec2(130.0f, 0.0f)))
            m_DeleteConfirm = true;
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f)))
            ImGui::CloseCurrentPopup();

        if (m_DeleteConfirm)
        {
            std::error_code ec;

            if (m_DeleteIsDirectory)
            {
                // Collect contained files first so the asset registry and
                // script editor can be cleaned up after the physical delete.
                std::vector<fs::path> containedFiles;
                for (auto it = fs::recursive_directory_iterator(delPath, ec);
                     !ec && it != fs::recursive_directory_iterator(); ++it)
                    if (!it->is_directory(ec))
                        containedFiles.push_back(it->path());

                ec.clear();
                fs::remove_all(delPath, ec);
                if (ec)
                {
                    std::cerr << "[Fadix] Delete folder failed: "
                              << ec.message() << '\n';
                }
                else
                {
                    for (const fs::path& file : containedFiles)
                    {
                        m_AssetRegistry.OnAssetDeleted(file);
                        m_ScriptEditor.CloseFile(file.string());
                    }
                    if (m_ContentDir == delPath ||
                        m_ContentDir.string().rfind(delPath.string(), 0) == 0)
                        m_ContentDir.clear(); // was inside — snap back to root
                }
            }
            else
            {
                fs::remove(delPath, ec);
                if (ec)
                    std::cerr << "[Fadix] Delete failed: " << ec.message() << '\n';
                else
                    m_AssetRegistry.OnAssetDeleted(delPath);

                m_ScriptEditor.CloseFile(delPath.string());
            }

            if (m_DrawerSelectedPath == m_DeleteTargetPath)
                m_DrawerSelectedPath.clear();

            if (m_FileSystem) m_FileSystem->Refresh();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::End();
}

// =============================================================================
// Output Log panel — compiler output with syntax-highlighted error lines
// =============================================================================

void EditorLayout::DrawOutputLog()
{
    if (!ImGui::Begin("Output Log##fadix_output_log", &m_OutputLogVisible))
    {
        ImGui::End();
        return;
    }

    // ---- Toolbar -------------------------------------------------------------
    if (ImGui::SmallButton("Clear"))
        m_ScriptCompiler.ClearLog();

    ImGui::SameLine();

    if (m_ScriptCompiler.IsCompiling())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.10f, 1.0f));
        ImGui::TextUnformatted("[COMPILING...]");
        ImGui::PopStyleColor();
    }
    else
    {
        ImGui::TextDisabled("[Idle]");
    }

    ImGui::Separator();

    // ---- Scrollable log display — cache refreshed only when new data arrives --
    // ConsumeLogDirty() returns true at most once per batch of appended output,
    // so we pay the mutex + line-split cost only on frames that actually changed.
    ImGui::BeginChild("##fadix_log_scroll", ImVec2(0.0f, 0.0f),
                      false, ImGuiWindowFlags_HorizontalScrollbar);

    if (m_ScriptCompiler.ConsumeLogDirty())
        m_LogLinesCache = m_ScriptCompiler.GetLines();

    for (const std::string& line : m_LogLinesCache)
    {
        ImVec4 color = ImVec4(0.78f, 0.83f, 0.88f, 1.0f); // default: light slate
        if (ContainsCaseInsensitive(line, "error"))
            color = ImVec4(1.00f, 0.38f, 0.38f, 1.0f); // red
        else if (ContainsCaseInsensitive(line, "warning"))
            color = ImVec4(1.00f, 0.80f, 0.25f, 1.0f); // amber
        else if (ContainsCaseInsensitive(line, "note:"))
            color = ImVec4(0.50f, 0.85f, 1.00f, 1.0f); // cyan
        else if (line.find("SUCCESS") != std::string::npos)
            color = ImVec4(0.30f, 0.95f, 0.45f, 1.0f); // green

        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(line.c_str());
        ImGui::PopStyleColor();
    }

    // Auto-scroll to the newest output unless the developer has scrolled up.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

// =============================================================================
// Settings modal (Project / Editor / Engine tabs)
// =============================================================================

void EditorLayout::DrawSettingsModal()
{
    if (m_OpenSettingsRequest)
    {
        ImGui::OpenPopup("Settings##fadix");
        m_OpenSettingsRequest = false;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f, 520.0f), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal("Settings##fadix", nullptr, ImGuiWindowFlags_NoSavedSettings))
        return;

    const int focusTab = m_SettingsFocusTab;
    m_SettingsFocusTab = -1;

    const float footerHeight = ImGui::GetFrameHeightWithSpacing()
                             + ImGui::GetStyle().ItemSpacing.y;

    if (ImGui::BeginTabBar("##settings_tabs"))
    {
        if (ImGui::BeginTabItem("Project", nullptr,
                (focusTab == 0) ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::BeginChild("##project_tab", ImVec2(0.0f, -footerHeight));
            DrawProjectSettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Editor", nullptr,
                (focusTab == 1) ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::BeginChild("##editor_tab", ImVec2(0.0f, -footerHeight));
            DrawEditorSettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Engine", nullptr,
                (focusTab == 2) ? ImGuiTabItemFlags_SetSelected : 0))
        {
            ImGui::BeginChild("##engine_tab", ImVec2(0.0f, -footerHeight));
            DrawEngineSettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Save & Close", ImVec2(140.0f, 0.0f)))
    {
        SaveAllSettings();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100.0f, 0.0f)))
        ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::TextDisabled("Changes apply live; Save writes the JSON files.");

    ImGui::EndPopup();
}

void EditorLayout::DrawProjectSettingsTab()
{
    ImGui::TextDisabled("Serialized to ProjectSettings.json in the project root.");
    ImGui::Spacing();

    ImGui::TextDisabled("Default Startup Scene (relative to project root)");
    InputTextString("##proj_scene", m_ProjectSettings.DefaultStartupScene);

    ImGui::Spacing();

    ImGui::TextDisabled("Default Rendering Profile");
    constexpr const char* kRenderModes[] = { "3D", "2D" };
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("##proj_render", &m_ProjectSettings.RenderingDimension, kRenderModes, 2);

    ImGui::TextDisabled("Physics Model");
    constexpr const char* kPhysicsModels[] = { "Rigid Body 3D", "Planar 2D" };
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("##proj_physics", &m_ProjectSettings.PhysicsModel, kPhysicsModels, 2);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Input Axis Mappings");
    ImGui::Spacing();

    int removeIndex = -1;
    constexpr ImGuiTableFlags kAxisTableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("##axes", 5, kAxisTableFlags))
    {
        ImGui::TableSetupColumn("Axis");
        ImGui::TableSetupColumn("Positive");
        ImGui::TableSetupColumn("Negative");
        ImGui::TableSetupColumn("Scale");
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(m_ProjectSettings.InputAxes.size()); ++i)
        {
            auto& axis = m_ProjectSettings.InputAxes[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            InputTextString("##axis_name", axis.Name);
            ImGui::TableSetColumnIndex(1);
            InputTextString("##axis_pos", axis.PositiveKey);
            ImGui::TableSetColumnIndex(2);
            InputTextString("##axis_neg", axis.NegativeKey);
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##axis_scale", &axis.Scale, 0.05f, -10.0f, 10.0f, "%.2f");
            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton("X"))
                removeIndex = i;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (removeIndex >= 0)
        m_ProjectSettings.InputAxes.erase(
            m_ProjectSettings.InputAxes.begin() + removeIndex);

    if (ImGui::Button("Add Axis"))
        m_ProjectSettings.InputAxes.push_back({ "NewAxis", "", "", 1.0f });
}

void EditorLayout::DrawEditorSettingsTab()
{
    ImGui::TextDisabled("Local developer preferences (fadix_editor_settings.json).");
    ImGui::Spacing();

    ImGui::TextDisabled("UI Theme");
    constexpr const char* kThemes[] = { "Fadix Dark", "Light", "High Contrast" };
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::Combo("##ed_theme", &m_EditorSettings.ThemeIndex, kThemes, 3))
        m_ThemePending = true;

    ImGui::Spacing();

    ImGui::Checkbox("Enable Layout Snapping", &m_EditorSettings.SnapEnabled);
    ImGui::BeginDisabled(!m_EditorSettings.SnapEnabled);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::DragFloat("Translation Snap", &m_EditorSettings.TranslationSnap,
                     0.05f, 0.01f, 10.0f, "%.2f units");
    ImGui::SetNextItemWidth(220.0f);
    ImGui::DragFloat("Rotation Snap", &m_EditorSettings.RotationSnap,
                     1.0f, 1.0f, 90.0f, "%.0f deg");
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderFloat("Asset Thumbnail Size", &m_EditorSettings.ThumbnailSize,
                       32.0f, 128.0f, "%.0f px");
}

void EditorLayout::DrawEngineSettingsTab()
{
    ImGui::TextDisabled("Underlying engine parameters (fadix_engine_settings.json).");
    ImGui::Spacing();

    ImGui::Checkbox("VSync", &m_EngineSettings.VSync);

    ImGui::BeginDisabled(m_EngineSettings.VSync);
    ImGui::Checkbox("Limit Framerate", &m_EngineSettings.LimitFramerate);
    ImGui::BeginDisabled(!m_EngineSettings.LimitFramerate);
    ImGui::SetNextItemWidth(220.0f);
    ImGui::SliderInt("Target Framerate", &m_EngineSettings.TargetFramerate,
                     30, 480, "%d fps");
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    ImGui::Checkbox("Log hardware diagnostics on startup",
                    &m_EngineSettings.LogDiagnosticsOnStartup);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Hardware Capabilities");
    ImGui::Spacing();

    static std::string s_HardwareInfo;
    if (s_HardwareInfo.empty())
    {
        const auto glStr = [](GLenum name) -> std::string
        {
            const GLubyte* value = glGetString(name);
            return value ? reinterpret_cast<const char*>(value) : "(unavailable)";
        };

        GLint maxTextureSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);

        s_HardwareInfo =
            "Vendor:           " + glStr(GL_VENDOR)   + "\n" +
            "Renderer:         " + glStr(GL_RENDERER) + "\n" +
            "OpenGL:           " + glStr(GL_VERSION)  + "\n" +
            "GLSL:             " + glStr(GL_SHADING_LANGUAGE_VERSION) + "\n" +
            "Max texture size: " + std::to_string(maxTextureSize);
    }

    ImGui::TextUnformatted(s_HardwareInfo.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Write Diagnostics Log"))
    {
        std::ofstream log("fadix_hw_diagnostics.log");
        if (log.is_open())
            log << s_HardwareInfo << '\n';
        std::cout << "[Fadix] Hardware diagnostics:\n" << s_HardwareInfo << '\n';
    }
}

// =============================================================================
// Asset spawning & persistence
// =============================================================================

void EditorLayout::SpawnEntityFromAsset(const char* assetPath)
{
    if (!m_ActiveScene || !assetPath) return;

    const fs::path path(assetPath);
    if (!IsSpawnableAsset(ExtensionLower(path))) return;

    Entity entity = m_ActiveScene->CreateEntity(path.stem().string());
    auto&  mesh   = entity.AddComponent<MeshComponent>();
    mesh.FilePath = path.generic_string();

    m_SelectedEntity = entity.GetHandle();
    m_History.PushEntityCreated(*m_ActiveScene, entity.GetHandle(),
                                "Spawn Asset");
}

void EditorLayout::CreateScriptTemplate(const std::string& name)
{
    // Write to the currently browsed content directory, or fall back to the
    // project root's Assets sub-folder.
    const fs::path targetDir = (!m_ContentDir.empty() && fs::exists(m_ContentDir))
        ? m_ContentDir
        : (m_ProjectRoot.empty() ? fs::current_path() : m_ProjectRoot / "Assets");

    const fs::path outPath = targetDir / (name + ".cpp");

    std::ofstream f(outPath);
    if (!f.is_open())
    {
        std::cerr << "[Fadix] Failed to create script file: " << outPath << '\n';
        return;
    }

    // ---- Boilerplate template ------------------------------------------------
    f << "// " << name << ".cpp — Generated by Fadix Engine\n";
    f << "// Inherit from ScriptableEntity to access engine components and lifecycle hooks.\n\n";
    f << "#include \"scene/ScriptableEntity.hpp\"\n";
    f << "#include \"core/NetReplication.hpp\"  // REPLICATED_BODY, REPLICATE_PROPERTY, SERVER_RPC, CLIENT_RPC\n\n";
    f << "class " << name << " : public ScriptableEntity\n";
    f << "{\n";
    f << "public:\n";
    f << "    // ---------------------------------------------------------------------------\n";
    f << "    // OPTIONAL: Remove REPLICATED_BODY() if this entity doesn't need networking.\n";
    f << "    // When present, the engine assigns a stable NetId and replicates REPLICATE_PROPERTY\n";
    f << "    // fields to all connected clients at 20 Hz.\n";
    f << "    // ---------------------------------------------------------------------------\n";
    f << "    // REPLICATED_BODY()\n\n";
    f << "    // Declare fields you want editable in the Fadix Inspector.\n";
    f << "    float     m_Speed     = 1.0f;\n";
    f << "    int       m_Count     = 0;\n";
    f << "    glm::vec3 m_Direction = glm::vec3(0.0f, 0.0f, 1.0f);\n\n";
    f << "    // Register those fields with the reflection system.\n";
    f << "    // The Inspector will show matching widgets once you enter Play mode.\n";
    f << "    void OnRegisterProperties(fadix::ReflectedProperties& props) override\n";
    f << "    {\n";
    f << "        FADIX_REFLECT_FLOAT(m_Speed);\n";
    f << "        FADIX_REFLECT_INT(m_Count);\n";
    f << "        FADIX_REFLECT_VEC3(m_Direction);\n";
    f << "    }\n\n";
    f << "    // Optional: declare which fields are replicated to clients (requires REPLICATED_BODY).\n";
    f << "    // void OnRegisterNetProperties(fadix::NetReplicatedProperties& props) override\n";
    f << "    // {\n";
    f << "    //     REPLICATE_PROPERTY(m_Speed);\n";
    f << "    // }\n\n";
    f << "    // Optional: SERVER_RPC example — runs on the server when a client calls it.\n";
    f << "    // SERVER_RPC void RequestAction(float param)\n";
    f << "    // {\n";
    f << "    //     if (FORWARD_TO_SERVER(RequestAction, param)) return;\n";
    f << "    //     // Code here runs ONLY on the server.\n";
    f << "    // }\n\n";
    f << "    // Optional: deserialise incoming RPCs directed at this entity.\n";
    f << "    // void OnServerRPC(const std::string& name, fadix::NetArgs& args) override\n";
    f << "    // {\n";
    f << "    //     if (name == \"RequestAction\") { float p = args.Read<float>(); RequestAction(p); }\n";
    f << "    // }\n\n";
    f << "    void OnCreate() override\n";
    f << "    {\n";
    f << "        // Called once when Play begins.\n";
    f << "    }\n\n";
    f << "    void OnUpdate(float dt) override\n";
    f << "    {\n";
    f << "        (void)dt; // remove when you use dt\n";
    f << "    }\n\n";
    f << "    void OnDestroy() override\n";
    f << "    {\n";
    f << "        // Called when Play stops.\n";
    f << "    }\n";
    f << "};\n\n";
    f << "// ---------------------------------------------------------------------------\n";
    f << "// Fadix Hot-Reload Export\n";
    f << "// This symbol is resolved by GetProcAddress after every successful compile.\n";
    f << "// Do NOT rename or remove it.\n";
    f << "// ---------------------------------------------------------------------------\n";
    f << "#ifdef _WIN32\n";
    f << "extern \"C\" __declspec(dllexport) ScriptableEntity* FadixCreateScript()\n";
    f << "#else\n";
    f << "extern \"C\" __attribute__((visibility(\"default\"))) ScriptableEntity* FadixCreateScript()\n";
    f << "#endif\n";
    f << "{\n";
    f << "    return new " << name << "();\n";
    f << "}\n";

    f.close();

    std::cout << "[Fadix] Script template created — " << outPath.generic_string() << '\n';

    // Refresh the VFS so the new file appears in the Content Drawer immediately.
    if (m_FileSystem) m_FileSystem->Refresh();

    // Open the new file directly in the Script Editor panel.
    m_ScriptEditor.OpenFile(outPath.string());
}

void EditorLayout::SaveActiveScene()
{
    if (!m_ActiveScene) return;

    const fs::path target = m_ProjectRoot.empty()
        ? fs::path(m_ProjectSettings.DefaultStartupScene)
        : m_ProjectRoot / m_ProjectSettings.DefaultStartupScene;

    // Wrap the raw pointer in a non-owning shared_ptr so SceneSerializer's
    // shared_ptr interface is satisfied without transferring ownership.
    std::shared_ptr<Scene> sceneRef(m_ActiveScene, [](Scene* /*s*/) {});
    SceneSerializer serializer(sceneRef);

    if (serializer.Serialize(target.generic_string()))
        std::cout << "[Fadix] Scene saved — " << target.generic_string() << '\n';
    else
        std::cerr << "[Fadix] Failed to save scene — " << target.generic_string() << '\n';
}

void EditorLayout::LoadActiveScene()
{
    if (!m_ActiveScene) return;

    const fs::path target = m_ProjectRoot.empty()
        ? fs::path(m_ProjectSettings.DefaultStartupScene)
        : m_ProjectRoot / m_ProjectSettings.DefaultStartupScene;

    std::shared_ptr<Scene> sceneRef(m_ActiveScene, [](Scene* /*s*/) {});
    SceneSerializer serializer(sceneRef);

    if (serializer.Deserialize(target.generic_string()))
    {
        std::cout << "[Fadix] Scene loaded — " << target.generic_string() << '\n';
        // Reset entity selection: all prior handles are invalid after Clear().
        SetScene(m_ActiveScene);
    }
    else
    {
        std::cerr << "[Fadix] Failed to load scene — " << target.generic_string() << '\n';
    }
}

void EditorLayout::SaveAllSettings()
{
    const fs::path projectFile = m_ProjectRoot.empty()
        ? fs::path("ProjectSettings.json")
        : m_ProjectRoot / "ProjectSettings.json";

    fadix::SaveProjectSettings(projectFile, m_ProjectSettings);
    fadix::SaveEditorSettings("fadix_editor_settings.json", m_EditorSettings);
    fadix::SaveEngineSettings("fadix_engine_settings.json", m_EngineSettings);
}

// =============================================================================
// Public accessors
// =============================================================================

void EditorLayout::ToggleContentDrawer()
{
    m_contentDrawerVisible = !m_contentDrawerVisible;
}

bool EditorLayout::IsContentDrawerVisible() const
{
    return m_contentDrawerVisible;
}
