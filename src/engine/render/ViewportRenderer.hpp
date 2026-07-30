#pragma once

#include "engine/assets/AssetHandle.hpp"
#include "engine/Uuid.hpp"
#include "engine/render/GizmoTypes.hpp"
#include "engine/render/RenderQuality.hpp"
#include "engine/rhi/Types.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <optional>

namespace fadix
{
enum class PostDebugView : std::uint8_t
{
    None,
    MotionVectors,
    TaaRejection
};

enum class ViewportDebugView : std::uint8_t
{
    None = 0,
    UnlitBaseColor,
    WorldNormals,
    Roughness,
    Metallic,
    Occlusion,
    Depth,
    CascadeColors,
    Ao,
    MotionVectors,
    LightTiles, // Forward+ per-tile point-light count heatmap
};

class IAssetDatabase;
class IWorld;

namespace rhi
{
class Texture;
}

struct GizmoVisual
{
    bool Visible{false};
    GizmoMode Mode{GizmoMode::Translate};
    glm::vec3 Position{0.0F};
    glm::quat Orientation{1.0F, 0.0F, 0.0F, 0.0F};
    std::optional<GizmoHandle> Hover;
    std::optional<GizmoHandle> Active;
};

struct MeshPreviewVisual
{
    AssetHandle Mesh;
    glm::vec3 Position{0.0F};
    glm::quat Rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 Scale{1.0F};
};

struct RenderDiagnostics
{
    RenderQualityPreset Preset{};
    int LogicalWidth{0};
    int LogicalHeight{0};
    int InternalWidth{0};
    int InternalHeight{0};
    int DrawCalls{0};
    int VisibleMeshes{0};
    int ActivePointLights{0}; // point lights uploaded to the Forward+ buffer
    int ActiveSpotLights{0};
    int TotalPointLights{0};  // point lights in the scene before any cap
    int TotalSpotLights{0};   // spot lights in the scene before the 8-light cap
    int ShadowedLights{0};
    int ShadowPasses{0};
    int ActiveCascades{0};
    int ShadowResolution{0};
    float ShadowDistance{0.0F};
    int PostPasses{0};
    float CpuRenderMs{0.0F};
    float GpuRenderMs{-1.0F}; // <0 = unavailable
};

class ViewportRenderer
{
public:
    virtual ~ViewportRenderer() = default;
    virtual void Resize(rhi::Extent2D extent) = 0;
    virtual void SetCamera(const glm::mat4& view, const glm::mat4& projection) = 0;
    virtual void SetSelection(std::optional<Uuid> selection) = 0;
    virtual void SetGizmo(const GizmoVisual& gizmo) = 0;
    virtual void SetMeshPreview(std::optional<MeshPreviewVisual> /*preview*/) {}
    virtual void DrawWorld(const IWorld& world) = 0;
    [[nodiscard]] virtual rhi::Texture* ColorTarget() const noexcept = 0;

    // Rebinds the asset database used for GPU texture/material loading.
    // Call when the editor switches projects so cached handles stay valid.
    virtual void SetAssetDatabase(IAssetDatabase& /*database*/) {}

    // Supplies the cache that lazily loads imported glTF meshes on demand.
    virtual void SetGltfMeshCache(class GltfMeshCache* /*cache*/) {}

    // Editor-only helpers (camera/sun models, frustum lines, gizmo overlay).
    // Off by default so game/runtime views never show tool geometry.
    virtual void SetEditorVisualsEnabled(bool /*enabled*/) noexcept {}
    virtual void SetCollisionVisualizationEnabled(bool /*enabled*/) noexcept {}

    // Simulation delta for CPU particle emitters (seconds). No-op until particles land.
    virtual void SetSimDelta(float /*dt*/) noexcept {}

    // Evaluate AnimatorComponents and fill SkeletonComponent joint matrices.
    virtual void UpdateAnimations(IWorld& /*world*/, float /*dt*/) {}

    // Applies a graphics-quality profile: feature caps consumed per frame plus
    // an anisotropy level that rebuilds the material sampler on change. Never
    // recreates pipelines. Resolution scale is handled by the viewport panel,
    // so this ignores RenderQualitySettings::ResolutionScale.
    virtual void SetQualitySettings(const RenderQualitySettings& /*quality*/) {}

    // Editor graphics.json AA override (Auto = follow env + quality default).
    virtual void SetAntiAliasOverride(AntiAliasMode /*mode*/) noexcept {}

    // Clears temporal AA history for this viewport instance (Scene/Game are independent).
    virtual void ResetTemporalHistory() noexcept {}

    // Temporary cascaded-shadow debug view (cascade tint + boundaries). Defaults
    // off; enable only for the Scene View. The renderer additionally gates it on
    // editor visuals so it can never appear in the Game View.
    virtual void SetShadowCascadeDebug(bool /*enabled*/) noexcept {}

    // Editor-only post debug (motion vectors / TAA rejection). Defaults off;
    // enable only for the Scene View. Gated on editor visuals like shadow debug.
    virtual void SetPostDebugView(PostDebugView /*view*/) noexcept {}

    // Scene View debug visualization (material buffers, cascades, AO, MV).
    // Game/runtime views must force ViewportDebugView::None.
    virtual void SetViewportDebugView(ViewportDebugView /*view*/) noexcept {}

    // Number of directional cascades currently rendered (for the debug preview).
    // 0 when the cascade debug view is off.
    [[nodiscard]] virtual int ShadowCascadeCount() const noexcept { return 0; }

    // The depth texture of cascade `index` when the cascade debug view is on (for
    // an on-screen preview), otherwise nullptr.
    [[nodiscard]] virtual rhi::Texture* ShadowDebugTexture(int /*index*/) const noexcept
    {
        return nullptr;
    }

    [[nodiscard]] virtual RenderDiagnostics Diagnostics() const { return {}; }

    // Appended to preserve the slot order of existing renderer binaries.
    virtual void SetGroundGridEnabled(bool /*enabled*/) noexcept {}
};
}
