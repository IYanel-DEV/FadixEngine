#pragma once

#include "editor/assets/AssetBrowserController.hpp"
#include "editor/camera/CameraModule.hpp"
#include "editor/gizmo/GizmoSystem.hpp"
#include "editor/imgui/EditorUiState.hpp"
#include "editor/imgui/GraphicsPreferences.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/camera/EditorMode.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/render/GizmoTypes.hpp"
#include "engine/render/Picking.hpp"
#include "engine/render/RenderQuality.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "runtime/Components.hpp"

#include <imgui.h>

#include <SDL3/SDL_events.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct SDL_Window;

namespace fadix
{
class IAssetDatabase;
class IWorld;
class GltfMeshCache;

namespace rhi
{
class Device;
}
}

namespace fadix::editor
{
/// Independently dockable Scene View + Game View. Distinct ViewportRenderer targets.
class ViewportPanel final
{
public:
    void Initialize(rhi::Device& device, IAssetDatabase& assets);
    void Shutdown() noexcept;
    void SetGltfMeshCache(GltfMeshCache* cache);
    void SetAssetDatabase(IAssetDatabase& assets);

    /// Offscreen passes using sizes measured last Draw(). Call after NewFrame, before dock UI.
    void RenderTargets(
        IWorld& world,
        SceneEditor& scene,
        CameraModule& camera,
        GizmoSystem& gizmo,
        bool playing,
        float deltaSeconds);

    void Draw(
        EditorUiState& ui,
        SceneEditor& scene,
        CameraModule& camera,
        GizmoSystem& gizmo,
        EditorPlayMode playMode,
        UndoStack& history,
        IWorld& editWorld,
        SceneDocument& document,
        SDL_Window* window);

    /// Embedded animation-workspace preview. Uses its own renderer, camera, and
    /// render target; the Scene and Game views remain completely independent.
    void DrawAnimationPreview(IWorld& world, std::optional<Uuid> target, float height,
        float skeletalTime, float transformTime);

    void HandleEvent(
        const SDL_Event& event,
        SceneEditor& scene,
        CameraModule& camera,
        GizmoSystem& gizmo,
        EditorPlayMode playMode,
        UndoStack& history,
        IWorld& editWorld,
        SceneDocument& document);

    [[nodiscard]] bool SceneViewHovered() const noexcept { return m_Scene.Hovered; }
    void SetGizmoTool(int tool) noexcept { m_GizmoTool = tool; }
    void SetGizmoLocalSpace(bool local) noexcept { m_GizmoLocalSpace = local; }
    [[nodiscard]] int GizmoTool() const noexcept { return m_GizmoTool; }
    [[nodiscard]] bool GizmoLocalSpace() const noexcept { return m_GizmoLocalSpace; }
    /// Logical Game View content rect in ImGui screen space (for overlays).
    [[nodiscard]] ImVec2 GameViewImageMin() const noexcept { return m_Game.ImageMin; }
    [[nodiscard]] ImVec2 GameViewImageSize() const noexcept { return m_Game.ImageSize; }
    /// Logical Game View aspect for game-camera projection (1 when view not measured yet).
    [[nodiscard]] float GameViewAspect() const noexcept
    {
        return m_Game.LogicalH > 0.01F ? m_Game.LogicalW / m_Game.LogicalH : 16.0F / 9.0F;
    }

    void SetAssetDropHandler(std::function<void(const AssetDragPayload&)> handler)
    {
        m_AssetDropHandler = std::move(handler);
    }

    void SetMeshDropHandler(
        std::function<void(const AssetDragPayload&, const glm::vec3&)> handler)
    {
        m_MeshDropHandler = std::move(handler);
    }

    /// Invoked whenever the user picks a new quality preset (for persistence).
    void SetQualityChangedHandler(std::function<void()> handler)
    {
        m_QualityChanged = std::move(handler);
    }

    /// Independent per-view quality presets. Scene View defaults to Low, Game
    /// View to High; Play does not change either (Game already renders High).
    void SetGraphicsPreferences(const GraphicsPreferences& prefs);
    void SetSceneQuality(RenderQualityPreset preset);
    void SetGameQuality(RenderQualityPreset preset);
    [[nodiscard]] RenderQualityPreset SceneQuality() const noexcept { return m_Scene.Preset; }
    [[nodiscard]] RenderQualityPreset GameQuality() const noexcept { return m_Game.Preset; }
    void ResetTemporalHistory() noexcept;
    [[nodiscard]] std::optional<RenderDiagnostics> FocusedViewportDiagnostics() const noexcept;
    void SetDebugView(ViewportDebugView view) noexcept { m_DebugView = view; }
    void SetCollisionVisualizationEnabled(bool enabled) noexcept { m_ShowCollisionShapes = enabled; }
    [[nodiscard]] ViewportDebugView DebugView() const noexcept { return m_DebugView; }

private:
    struct View
    {
        std::unique_ptr<ViewportRenderer> Renderer;
        std::unique_ptr<Picking> Picking;
        std::uint32_t PixelW{0};
        std::uint32_t PixelH{0};
        float LogicalW{0.0F};
        float LogicalH{0.0F};
        ImVec2 ImageMin{};
        ImVec2 ImageSize{};
        bool Visible{false};
        bool Hovered{false};
        bool Focused{false};
        RenderQualityPreset Preset{RenderQualityPreset::High};
        RenderQualitySettings Quality{};
    };

    void ApplyQuality(View& view);
    void DrawQualityCombo(View& view, const char* id);
    void DrawSceneToolbar(EditorUiState& ui, GizmoSystem& gizmo, EditorPlayMode playMode);
    void DrawViewImage(View& view, const char* emptyMessage, bool showTexture);
    void MeasureView(View& view, float dpiScale);
    void EnsureSize(View& view);
    [[nodiscard]] glm::vec2 MouseInViewPixels(const View& view) const;
    void UpdateGizmoVisual(
        View& view,
        SceneEditor& scene,
        GizmoSystem& gizmo,
        bool editMode);
    [[nodiscard]] std::optional<GizmoHandle> HitTestGizmo(
        const View& view,
        SceneEditor& scene,
        CameraModule& camera,
        GizmoMode mode) const;

    View m_Scene;
    View m_Game;
    View m_Animation;
    std::unique_ptr<IWorld> m_AnimationWorld;
    const IWorld* m_AnimationSourceWorld{nullptr};
    std::optional<Uuid> m_AnimationTarget;
    glm::vec3 m_AnimationCenter{0.0F};
    float m_AnimationYaw{0.65F};
    float m_AnimationPitch{0.25F};
    float m_AnimationDistance{6.0F};
    bool m_GizmoLocalSpace{false};
    int m_GizmoTool{1};
    bool m_GizmoDragging{false};
    std::optional<GizmoHandle> m_GizmoHover;
    std::optional<TransformComponent> m_GizmoStartTransform;
    bool m_PendingPick{false};
    ViewportDebugView m_DebugView{ViewportDebugView::None};
    bool m_ShowCollisionShapes{true};
    std::optional<MeshPreviewVisual> m_MeshPreview;
    std::function<void(const AssetDragPayload&)> m_AssetDropHandler;
    std::function<void(const AssetDragPayload&, const glm::vec3&)> m_MeshDropHandler;
    std::function<void()> m_QualityChanged;
    GraphicsPreferences m_GraphicsPrefs{GraphicsPreferences::Defaults()};
};
}
