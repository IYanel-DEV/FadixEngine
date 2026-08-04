#include "editor/imgui/panels/ViewportPanel.hpp"

#include "editor/assets/AssetBrowserController.hpp"
#include "editor/camera/CameraSelection.hpp"
#include "editor/command/EntityCommands.hpp"
#include "editor/imgui/EditorIcons.hpp"
#include "editor/imgui/panels/TilemapPanel.hpp"
#include "engine/scene/IWorld.hpp"
#include "render/ViewportRendererFactory.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace fadix::editor
{
namespace
{
constexpr std::array<const char*, 11> kDebugViewLabels{
    "None",
    "Base Color",
    "Normals",
    "Roughness",
    "Metallic",
    "Occlusion",
    "Depth",
    "Cascade Colors (Experimental)",
    "AO (Experimental)",
    "Motion Vectors (Experimental)",
    "Light Tiles (Forward+)"};
}
namespace
{
[[nodiscard]] ImTextureRef TextureRef(rhi::Texture* color)
{
    if (color == nullptr)
    {
        return {};
    }
    void* native = GetNativeTextureHandle(*color);
    return ImTextureRef{static_cast<ImTextureID>(reinterpret_cast<intptr_t>(native))};
}

[[nodiscard]] GizmoRay MouseRayFromCamera(
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec2 localMouse,
    const glm::vec2 viewportSize)
{
    const float x = 2.0F * (localMouse.x + 0.5F) / std::max(viewportSize.x, 1.0F) - 1.0F;
    const float y = 1.0F - 2.0F * (localMouse.y + 0.5F) / std::max(viewportSize.y, 1.0F);
    const glm::mat4 inverse = glm::inverse(projection * view);
    glm::vec4 nearPoint = inverse * glm::vec4{x, y, 0.0F, 1.0F};
    glm::vec4 farPoint = inverse * glm::vec4{x, y, 1.0F, 1.0F};
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    return {glm::vec3{nearPoint}, glm::normalize(glm::vec3{farPoint - nearPoint})};
}

[[nodiscard]] float RaySegmentDistance(
    const GizmoRay& ray, const glm::vec3 a, const glm::vec3 b, float& rayT)
{
    const glm::vec3 u = ray.Direction;
    const glm::vec3 v = b - a;
    const glm::vec3 w = ray.Origin - a;
    const float uv = glm::dot(u, v);
    const float vv = glm::dot(v, v);
    const float uw = glm::dot(u, w);
    const float vw = glm::dot(v, w);
    const float denominator = vv - uv * uv;
    float segment = 0.0F;
    if (vv > 1.0e-8F)
    {
        segment = std::abs(denominator) > 1.0e-8F
            ? std::clamp((uv * uw - vw) / -denominator, 0.0F, 1.0F)
            : 0.0F;
    }
    const glm::vec3 segmentPoint = a + v * segment;
    rayT = std::max(glm::dot(segmentPoint - ray.Origin, u), 0.0F);
    return glm::length(ray.Origin + u * rayT - segmentPoint);
}

struct RayPlaneHit
{
    glm::vec3 Point;
    float RayT;
};

[[nodiscard]] std::optional<RayPlaneHit> RayPlaneIntersect(
    const GizmoRay& ray, const glm::vec3 point, const glm::vec3 normal)
{
    const float denom = glm::dot(normal, ray.Direction);
    if (std::abs(denom) < 1.0e-6F)
    {
        return std::nullopt;
    }
    const float t = glm::dot(point - ray.Origin, normal) / denom;
    if (t < 0.0F)
    {
        return std::nullopt;
    }
    return RayPlaneHit{ray.Origin + ray.Direction * t, t};
}

[[nodiscard]] GizmoMode ToolToMode(const int tool) noexcept
{
    switch (tool)
    {
    case 2: return GizmoMode::Rotate;
    case 3: return GizmoMode::Scale;
    case 1: return GizmoMode::Translate;
    default: return GizmoMode::Select;
    }
}

template <typename Component>
void SyncPreviewComponent(const entt::registry& source, const entt::entity sourceEntity,
    entt::registry& preview, const entt::entity previewEntity)
{
    if (const Component* value = source.try_get<Component>(sourceEntity))
    {
        preview.emplace_or_replace<Component>(previewEntity, *value);
    }
    else
    {
        preview.remove<Component>(previewEntity);
    }
}

[[nodiscard]] ImVec2 WorldToImGui(
    const glm::vec2 worldPos,
    const Ortho2DCamera& cam,
    const ImVec2 imgMin)
{
    const glm::vec2 screen = cam.WorldToScreen(worldPos);
    return {imgMin.x + screen.x, imgMin.y + screen.y};
}

void Draw2DGrid(
    ImDrawList* drawList,
    const Ortho2DCamera& cam,
    const ImVec2 imgMin,
    const ImVec2 imgSize)
{
    const glm::vec2 viewSize = cam.ViewportSize();
    if (viewSize.x < 1.0F || viewSize.y < 1.0F)
    {
        return;
    }
    const float hh = cam.OrthoHalfHeight();
    const float hw = hh * (viewSize.x / viewSize.y);

    // Pick a grid step that looks reasonable for the current zoom
    const float log10 = std::log10(hh);
    const float step = std::pow(10.0F, std::ceil(log10) - 1.0F);
    const float minorStep = step * 0.1F;

    const glm::vec2 wMin = cam.Position() - glm::vec2{hw, hh};
    const glm::vec2 wMax = cam.Position() + glm::vec2{hw, hh};

    // Draw minor grid (only when zoomed enough)
    if (minorStep * viewSize.y / (2.0F * hh) > 8.0F)
    {
        const ImU32 minorCol = IM_COL32(60, 60, 80, 200);
        float startX = std::floor(wMin.x / minorStep) * minorStep;
        for (float x = startX; x <= wMax.x + minorStep; x += minorStep)
        {
            const ImVec2 top = WorldToImGui({x, wMax.y}, cam, imgMin);
            const ImVec2 bot = WorldToImGui({x, wMin.y}, cam, imgMin);
            drawList->AddLine(top, bot, minorCol, 0.5F);
        }
        float startY = std::floor(wMin.y / minorStep) * minorStep;
        for (float y = startY; y <= wMax.y + minorStep; y += minorStep)
        {
            const ImVec2 left = WorldToImGui({wMin.x, y}, cam, imgMin);
            const ImVec2 right = WorldToImGui({wMax.x, y}, cam, imgMin);
            drawList->AddLine(left, right, minorCol, 0.5F);
        }
    }

    // Draw major grid
    const ImU32 majorCol = IM_COL32(80, 80, 110, 220);
    {
        float startX = std::floor(wMin.x / step) * step;
        for (float x = startX; x <= wMax.x + step; x += step)
        {
            const ImVec2 top = WorldToImGui({x, wMax.y}, cam, imgMin);
            const ImVec2 bot = WorldToImGui({x, wMin.y}, cam, imgMin);
            drawList->AddLine(top, bot, majorCol, 1.0F);
        }
        float startY = std::floor(wMin.y / step) * step;
        for (float y = startY; y <= wMax.y + step; y += step)
        {
            const ImVec2 left = WorldToImGui({wMin.x, y}, cam, imgMin);
            const ImVec2 right = WorldToImGui({wMax.x, y}, cam, imgMin);
            drawList->AddLine(left, right, majorCol, 1.0F);
        }
    }

    // Draw X and Y axes
    {
        const ImVec2 axisXLeft = WorldToImGui({wMin.x, 0.0F}, cam, imgMin);
        const ImVec2 axisXRight = WorldToImGui({wMax.x, 0.0F}, cam, imgMin);
        drawList->AddLine(axisXLeft, axisXRight, IM_COL32(200, 80, 80, 220), 1.5F);
        const ImVec2 axisYTop = WorldToImGui({0.0F, wMax.y}, cam, imgMin);
        const ImVec2 axisYBot = WorldToImGui({0.0F, wMin.y}, cam, imgMin);
        drawList->AddLine(axisYTop, axisYBot, IM_COL32(80, 200, 80, 220), 1.5F);
    }

    // Clip to viewport
    drawList->PushClipRect(imgMin, {imgMin.x + imgSize.x, imgMin.y + imgSize.y}, true);
    drawList->PopClipRect();
}

void Draw2DEntityOverlays(
    ImDrawList* drawList,
    const Ortho2DCamera& cam,
    const ImVec2 imgMin,
    const IWorld& world,
    const SceneEditor& scene)
{
    const auto sel = scene.Selection();
    if (!sel)
    {
        return;
    }
    const auto entity = world.Find(*sel);
    if (!entity)
    {
        return;
    }
    const entt::registry& reg = world.Registry();

    // Get transform position
    glm::vec2 origin{0.0F};
    if (const TransformComponent* xform = reg.try_get<TransformComponent>(*entity))
    {
        origin = {xform->Position.x, xform->Position.y};
    }

    // Sprite2D bounds
    if (const Sprite2DComponent* spr = reg.try_get<Sprite2DComponent>(*entity))
    {
        const float halfW = spr->Size.x * 0.5F;
        const float halfH = spr->Size.y * 0.5F;
        const float pivotOffX = (0.5F - spr->Pivot.x) * spr->Size.x;
        const float pivotOffY = (0.5F - spr->Pivot.y) * spr->Size.y;
        const float cx = origin.x + pivotOffX;
        const float cy = origin.y + pivotOffY;
        const ImVec2 tl = WorldToImGui({cx - halfW, cy + halfH}, cam, imgMin);
        const ImVec2 tr = WorldToImGui({cx + halfW, cy + halfH}, cam, imgMin);
        const ImVec2 br = WorldToImGui({cx + halfW, cy - halfH}, cam, imgMin);
        const ImVec2 bl = WorldToImGui({cx - halfW, cy - halfH}, cam, imgMin);
        drawList->AddQuad(tl, tr, br, bl, IM_COL32(255, 220, 0, 230), 1.5F);
        // Pivot dot
        const ImVec2 pivotScreen = WorldToImGui(origin, cam, imgMin);
        drawList->AddCircleFilled(pivotScreen, 4.0F, IM_COL32(255, 100, 0, 255));
    }

    // Collider2D
    if (const Collider2DComponent* col = reg.try_get<Collider2DComponent>(*entity))
    {
        const ImU32 colColor = col->Sensor
            ? IM_COL32(0, 180, 255, 200)
            : IM_COL32(0, 255, 120, 200);
        const glm::vec2 colCenter = origin + col->Offset;
        if (col->Shape == Collider2DShape::Box)
        {
            const float hw = col->Size.x;
            const float hh = col->Size.y;
            const ImVec2 tl = WorldToImGui({colCenter.x - hw, colCenter.y + hh}, cam, imgMin);
            const ImVec2 tr = WorldToImGui({colCenter.x + hw, colCenter.y + hh}, cam, imgMin);
            const ImVec2 br = WorldToImGui({colCenter.x + hw, colCenter.y - hh}, cam, imgMin);
            const ImVec2 bl = WorldToImGui({colCenter.x - hw, colCenter.y - hh}, cam, imgMin);
            drawList->AddQuad(tl, tr, br, bl, colColor, 1.5F);
        }
        else // Circle
        {
            const float radius = col->Size.x;
            const glm::vec2 screenCenter = cam.WorldToScreen(colCenter);
            const float screenRadius = radius * cam.ViewportSize().y / (2.0F * cam.OrthoHalfHeight());
            drawList->AddCircle(
                {imgMin.x + screenCenter.x, imgMin.y + screenCenter.y},
                screenRadius, colColor, 32, 1.5F);
        }
    }

    // TileMap grid overlay for selected tilemap
    if (const TileMapComponent* tm = reg.try_get<TileMapComponent>(*entity))
    {
        const float tileWW = static_cast<float>(tm->TileWidth) /
            std::max(tm->PixelsPerUnit, 0.001F);
        const float tileWH = static_cast<float>(tm->TileHeight) /
            std::max(tm->PixelsPerUnit, 0.001F);
        const ImU32 gridCol = IM_COL32(120, 200, 255, 120);

        // Map bounds
        const float mapW = tileWW * static_cast<float>(tm->GridWidth);
        const float mapH = tileWH * static_cast<float>(tm->GridHeight);
        const ImVec2 mapTL = WorldToImGui(origin, cam, imgMin);
        const ImVec2 mapBR = WorldToImGui(
            {origin.x + mapW, origin.y - mapH}, cam, imgMin);
        drawList->AddRect(mapTL, mapBR, IM_COL32(120, 200, 255, 200), 0.0F, 0, 2.0F);

        // Individual tile lines (limit to avoid drawing thousands of lines)
        constexpr int kMaxLines = 80;
        if (tm->GridWidth <= kMaxLines)
        {
            for (int x = 0; x <= tm->GridWidth; ++x)
            {
                const ImVec2 top = WorldToImGui(
                    {origin.x + x * tileWW, origin.y}, cam, imgMin);
                const ImVec2 bot = WorldToImGui(
                    {origin.x + x * tileWW, origin.y - mapH}, cam, imgMin);
                drawList->AddLine(top, bot, gridCol, 0.5F);
            }
        }
        if (tm->GridHeight <= kMaxLines)
        {
            for (int y = 0; y <= tm->GridHeight; ++y)
            {
                const ImVec2 left = WorldToImGui(
                    {origin.x, origin.y - y * tileWH}, cam, imgMin);
                const ImVec2 right = WorldToImGui(
                    {origin.x + mapW, origin.y - y * tileWH}, cam, imgMin);
                drawList->AddLine(left, right, gridCol, 0.5F);
            }
        }
    }
}
}

void ViewportPanel::Initialize(rhi::Device& device, IAssetDatabase& assets)
{
    Shutdown();
    m_Scene.Renderer = CreateViewportRenderer(device, assets);
    m_Game.Renderer = CreateViewportRenderer(device, assets);
    m_Animation.Renderer = CreateViewportRenderer(device, assets);
    if (m_Scene.Renderer)
    {
        m_Scene.Picking = CreateViewportPicking(*m_Scene.Renderer);
    }
    // Scene View defaults to Low (cheap editing), Game View to High (fidelity).
    // A persisted graphics.json may override these afterwards.
    m_Scene.Preset = RenderQualityPreset::Low;
    m_Game.Preset = RenderQualityPreset::High;
    m_Animation.Preset = RenderQualityPreset::Low;
    ApplyQuality(m_Scene);
    ApplyQuality(m_Game);
    ApplyQuality(m_Animation);
}

void ViewportPanel::ApplyQuality(View& view)
{
    view.Quality = m_GraphicsPrefs.ApplyTo(view.Preset);
    if (view.Renderer)
    {
        view.Renderer->SetQualitySettings(view.Quality);
    }
}

void ViewportPanel::SetGraphicsPreferences(const GraphicsPreferences& prefs)
{
    m_GraphicsPrefs = prefs;
    m_Scene.Preset = prefs.SceneQuality;
    m_Game.Preset = prefs.GameQuality;
    ApplyQuality(m_Scene);
    ApplyQuality(m_Game);
    if (m_Scene.Renderer)
    {
        m_Scene.Renderer->SetAntiAliasOverride(prefs.AaOverride);
    }
    if (m_Game.Renderer)
    {
        m_Game.Renderer->SetAntiAliasOverride(prefs.AaOverride);
    }
    if (m_Animation.Renderer)
    {
        m_Animation.Renderer->SetAntiAliasOverride(prefs.AaOverride);
    }
}

void ViewportPanel::SetSceneQuality(const RenderQualityPreset preset)
{
    m_Scene.Preset = preset;
    ApplyQuality(m_Scene);
}

void ViewportPanel::SetGameQuality(const RenderQualityPreset preset)
{
    m_Game.Preset = preset;
    ApplyQuality(m_Game);
}

void ViewportPanel::ResetTemporalHistory() noexcept
{
    if (m_Scene.Renderer)
    {
        m_Scene.Renderer->ResetTemporalHistory();
    }
    if (m_Game.Renderer)
    {
        m_Game.Renderer->ResetTemporalHistory();
    }
    if (m_Animation.Renderer)
    {
        m_Animation.Renderer->ResetTemporalHistory();
    }
}

void ViewportPanel::ResetSceneCamera(CameraModule& camera) noexcept
{
    camera.Camera() = WorkbenchCamera{};
    camera.Input().Reset();
    ResetTemporalHistory();
}

std::optional<RenderDiagnostics> ViewportPanel::FocusedViewportDiagnostics() const noexcept
{
    const View* view = nullptr;
    if (m_Scene.Focused && m_Scene.Renderer != nullptr)
    {
        view = &m_Scene;
    }
    else if (m_Game.Focused && m_Game.Renderer != nullptr)
    {
        view = &m_Game;
    }
    if (view == nullptr)
    {
        return std::nullopt;
    }
    RenderDiagnostics diag = view->Renderer->Diagnostics();
    diag.Preset = view->Preset;
    diag.LogicalWidth = static_cast<int>(view->LogicalW);
    diag.LogicalHeight = static_cast<int>(view->LogicalH);
    return diag;
}

void ViewportPanel::Shutdown() noexcept
{
    m_Scene = {};
    m_Game = {};
    m_Animation = {};
    m_AnimationWorld.reset();
    m_AnimationSourceWorld = nullptr;
    m_AnimationTarget.reset();
    m_GizmoDragging = false;
    m_GizmoHover.reset();
    m_GizmoStartTransform.reset();
    m_MeshPreview.reset();
}

void ViewportPanel::SetGltfMeshCache(GltfMeshCache* cache)
{
    if (m_Scene.Renderer)
    {
        m_Scene.Renderer->SetGltfMeshCache(cache);
    }
    if (m_Game.Renderer)
    {
        m_Game.Renderer->SetGltfMeshCache(cache);
    }
    if (m_Animation.Renderer)
    {
        m_Animation.Renderer->SetGltfMeshCache(cache);
    }
}

void ViewportPanel::SetAssetDatabase(IAssetDatabase& assets)
{
    if (m_Scene.Renderer)
    {
        m_Scene.Renderer->SetAssetDatabase(assets);
    }
    if (m_Game.Renderer)
    {
        m_Game.Renderer->SetAssetDatabase(assets);
    }
    if (m_Animation.Renderer)
    {
        m_Animation.Renderer->SetAssetDatabase(assets);
    }
}

void ViewportPanel::EnsureSize(View& view)
{
    if (!view.Renderer || view.PixelW == 0 || view.PixelH == 0)
    {
        return;
    }
    // Resize here (before DrawWorld), not in MeasureView — Measure runs after
    // RenderTargets and would destroy the just-drawn color target for ImGui::Image.
    view.Renderer->Resize({view.PixelW, view.PixelH});
}

void ViewportPanel::RenderTargets(
    IWorld& world,
    SceneEditor& scene,
    CameraModule& camera,
    GizmoSystem& gizmo,
    const bool playing,
    const float deltaSeconds)
{
    // Runtime animation advances once before Scene and Game render the shared world.
    // Stopped authoring objects stay untouched; FDX preview uses its private world below.
    if (ViewportRenderer* animRenderer =
            m_Scene.Renderer ? m_Scene.Renderer.get() : m_Game.Renderer.get())
    {
        animRenderer->UpdateAnimations(world, deltaSeconds);
    }

    if (m_Scene.Visible && m_Scene.Renderer && m_Scene.PixelW > 0 && m_Scene.PixelH > 0)
    {
        EnsureSize(m_Scene);
        const bool in2D = camera.ProjectionMode() == ViewportProjectionMode::Ortho2D;
        m_Scene.Renderer->SetEditorVisualsEnabled(!playing);
        m_Scene.Renderer->SetGroundGridEnabled(!playing && m_ShowGroundGrid && !in2D);
        m_Scene.Renderer->SetCollisionVisualizationEnabled(!playing && m_ShowCollisionShapes);
        m_Scene.Renderer->SetViewportDebugView(m_DebugView);
        m_Scene.Renderer->SetSimDelta(deltaSeconds);
        camera.Camera().SetViewportSize({m_Scene.LogicalW, m_Scene.LogicalH});
        camera.Ortho2D().SetViewportSize({m_Scene.LogicalW, m_Scene.LogicalH});
        camera.ApplyToViewport(*m_Scene.Renderer);
        m_Scene.Renderer->SetMeshPreview(!playing ? m_MeshPreview : std::nullopt);
        UpdateGizmoVisual(m_Scene, scene, gizmo, !playing);
        m_Scene.Renderer->DrawWorld(world);
    }

    if (m_Game.Visible && m_Game.Renderer && m_Game.PixelW > 0 && m_Game.PixelH > 0 &&
        camera.GameCamera())
    {
        EnsureSize(m_Game);
        m_Game.Renderer->SetEditorVisualsEnabled(false);
        m_Game.Renderer->SetCollisionVisualizationEnabled(false);
        m_Game.Renderer->SetViewportDebugView(ViewportDebugView::None);
        m_Game.Renderer->SetSimDelta(deltaSeconds);
        m_Game.Renderer->SetGizmo({});
        m_Game.Renderer->SetMeshPreview(std::nullopt);
        m_Game.Renderer->SetSelection(std::nullopt);
        m_Game.Renderer->SetCamera(camera.GameCamera()->View, camera.GameCamera()->Projection);
        m_Game.Renderer->DrawWorld(world);
    }

    if (m_Animation.Visible && m_Animation.Renderer && m_Animation.PixelW > 0 &&
        m_Animation.PixelH > 0 && m_AnimationTarget && m_AnimationWorld)
    {
        EnsureSize(m_Animation);
        m_Animation.Renderer->UpdateAnimations(*m_AnimationWorld, 0.0F);
        m_Animation.Renderer->SetEditorVisualsEnabled(false);
        m_Animation.Renderer->SetCollisionVisualizationEnabled(false);
        m_Animation.Renderer->SetViewportDebugView(ViewportDebugView::None);
        m_Animation.Renderer->SetSimDelta(0.0F);
        m_Animation.Renderer->SetGizmo({});
        m_Animation.Renderer->SetMeshPreview(std::nullopt);
        m_Animation.Renderer->SetSelection(m_AnimationTarget);

        const float cosPitch = std::cos(m_AnimationPitch);
        const glm::vec3 direction{
            cosPitch * std::sin(m_AnimationYaw),
            std::sin(m_AnimationPitch),
            cosPitch * std::cos(m_AnimationYaw)};
        const glm::vec3 eye = m_AnimationCenter + direction * m_AnimationDistance;
        const float aspect = m_Animation.LogicalW / std::max(m_Animation.LogicalH, 1.0F);
        const glm::mat4 view = glm::lookAtRH(eye, m_AnimationCenter, glm::vec3{0.0F, 1.0F, 0.0F});
        const glm::mat4 projection =
            glm::perspectiveRH_ZO(glm::radians(45.0F), aspect, 0.05F, 2000.0F);
        m_Animation.Renderer->SetCamera(view, projection);
        m_Animation.Renderer->DrawWorld(*m_AnimationWorld);
    }
}

void ViewportPanel::DrawAnimationPreview(
    IWorld& world, const std::optional<Uuid> target, const float height,
    const float skeletalTime, const float transformTime)
{
    m_Animation.Visible = target.has_value() && m_Animation.Renderer != nullptr;
    if (target != m_AnimationTarget || m_AnimationSourceWorld != &world)
    {
        m_AnimationSourceWorld = &world;
        m_AnimationTarget = target;
        m_AnimationWorld.reset();
        m_AnimationYaw = 0.65F;
        m_AnimationPitch = 0.25F;
        m_AnimationDistance = 6.0F;
        m_AnimationCenter = {0.0F, 0.0F, 0.0F};

        if (target && world.Find(*target))
        {
            m_AnimationWorld = world.Clone();
            entt::registry& preview = m_AnimationWorld->Registry();
            std::vector<entt::entity> remove;
            for (const auto [entity, uuid] : preview.view<const UuidComponent>().each())
            {
                if (uuid.Id != *target)
                {
                    remove.push_back(entity);
                }
            }
            for (const entt::entity entity : remove)
            {
                preview.destroy(entity);
            }

            const entt::entity environmentEntity = m_AnimationWorld->Create();
            EnvironmentComponent environment;
            environment.Primary = true;
            environment.Priority = 1000;
            environment.SkyZenithColor = {0.16F, 0.48F, 0.88F};
            environment.SkyHorizonColor = {0.72F, 0.86F, 1.0F};
            environment.GroundColor = {0.30F, 0.34F, 0.26F};
            environment.AmbientColor = {0.78F, 0.82F, 0.88F};
            environment.AmbientIntensity = 1.25F;
            environment.Exposure = 1.15F;
            environment.FogEnabled = false;
            preview.emplace<EnvironmentComponent>(environmentEntity, environment);

            const entt::entity sunEntity = m_AnimationWorld->Create();
            const glm::quat sunRotation =
                glm::quat{glm::radians(glm::vec3{-55.0F, -35.0F, 0.0F})};
            preview.emplace<TransformComponent>(sunEntity,
                TransformComponent{{4.0F, 8.0F, 4.0F}, sunRotation});
            DirectionalLightComponent sun = MakeSunLight();
            sun.Intensity = 4.5F;
            preview.emplace<DirectionalLightComponent>(sunEntity, sun);
        }
    }

    if (target && m_AnimationWorld)
    {
        const std::optional<entt::entity> sourceEntity = world.Find(*target);
        const std::optional<entt::entity> previewEntity = m_AnimationWorld->Find(*target);
        if (sourceEntity && previewEntity)
        {
            const entt::registry& source = world.Registry();
            entt::registry& preview = m_AnimationWorld->Registry();
            SyncPreviewComponent<TransformComponent>(source, *sourceEntity, preview, *previewEntity);
            SyncPreviewComponent<MeshComponent>(source, *sourceEntity, preview, *previewEntity);
            SyncPreviewComponent<SkeletonComponent>(source, *sourceEntity, preview, *previewEntity);
            SyncPreviewComponent<AnimatorComponent>(source, *sourceEntity, preview, *previewEntity);
            SyncPreviewComponent<TransformAnimatorComponent>(
                source, *sourceEntity, preview, *previewEntity);
            preview.remove<RelationshipComponent>(*previewEntity);
            preview.remove<EnvironmentComponent>(*previewEntity);
            preview.remove<DirectionalLightComponent>(*previewEntity);
            preview.remove<PointLightComponent>(*previewEntity);
            preview.remove<SpotLightComponent>(*previewEntity);
            preview.emplace_or_replace<VisibilityComponent>(*previewEntity, VisibilityComponent{});

            if (auto* animator = preview.try_get<AnimatorComponent>(*previewEntity))
            {
                animator->CurrentTime = skeletalTime;
                animator->Playing = true;
            }
            if (auto* transform = preview.try_get<TransformComponent>(*previewEntity))
            {
                const glm::vec3 sourcePosition = transform->Position;
                if (auto* animator = preview.try_get<TransformAnimatorComponent>(*previewEntity))
                {
                    animator->CurrentTime = transformTime;
                    animator->Playing = false;
                    if (const AnimationClipAsset* clip = FindTransformClip(*animator))
                    {
                        ApplyTransformClip(*clip, transformTime, *transform);
                    }
                }
                transform->Position -= sourcePosition;
            }
        }
    }

    if (ImGui::SmallButton("Frame Selected##AnimationPreview"))
    {
        m_AnimationCenter = {0.0F, 0.0F, 0.0F};
        m_AnimationDistance = 6.0F;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("RMB orbit  |  wheel zoom");

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float dpi = std::max(ImGui::GetIO().DisplayFramebufferScale.x, 0.01F);
    m_Animation.LogicalW = std::max(avail.x, 1.0F);
    m_Animation.LogicalH = std::max(height, 1.0F);
    const float render = dpi * std::clamp(m_Animation.Quality.ResolutionScale, 0.1F, 1.0F);
    m_Animation.PixelW = static_cast<std::uint32_t>(
        std::max<long>(std::lround(m_Animation.LogicalW * render), 1));
    m_Animation.PixelH = static_cast<std::uint32_t>(
        std::max<long>(std::lround(m_Animation.LogicalH * render), 1));
    DrawViewImage(m_Animation, target ? "Preparing animation preview..." : "Select an entity", true);

    if (m_Animation.Hovered)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right))
        {
            m_AnimationYaw -= io.MouseDelta.x * 0.01F;
            m_AnimationPitch =
                std::clamp(m_AnimationPitch - io.MouseDelta.y * 0.01F, -1.45F, 1.45F);
        }
        if (io.MouseWheel != 0.0F)
        {
            m_AnimationDistance = std::clamp(
                m_AnimationDistance * std::pow(0.85F, io.MouseWheel), 0.25F, 500.0F);
        }
    }
}

void ViewportPanel::MeasureView(View& view, const float dpiScale)
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scale = std::max(dpiScale, 0.01F);
    view.LogicalW = std::max(avail.x, 1.0F);
    view.LogicalH = std::max(avail.y, 1.0F);
    // Quality resolution scale multiplies the physical target size after DPI;
    // the image is still displayed at the logical size, so switching presets
    // never changes the on-screen layout, only the internal render resolution.
    const float render = scale * std::clamp(view.Quality.ResolutionScale, 0.1F, 1.0F);
    // Defer GPU Resize to EnsureSize in RenderTargets (next frame).
    view.PixelW = static_cast<std::uint32_t>(std::max<long>(std::lround(view.LogicalW * render), 1));
    view.PixelH = static_cast<std::uint32_t>(std::max<long>(std::lround(view.LogicalH * render), 1));
}

void ViewportPanel::DrawViewImage(View& view, const char* emptyMessage, const bool showTexture)
{
    const ImVec2 size{view.LogicalW, view.LogicalH};
    rhi::Texture* color = showTexture && view.Renderer && view.PixelW > 0
        ? view.Renderer->ColorTarget()
        : nullptr;
    if (color == nullptr)
    {
        ImGui::Dummy(size);
        const ImVec2 textSize = ImGui::CalcTextSize(emptyMessage);
        ImGui::SetCursorScreenPos(ImVec2{
            ImGui::GetItemRectMin().x + (size.x - textSize.x) * 0.5F,
            ImGui::GetItemRectMin().y + (size.y - textSize.y) * 0.5F});
        ImGui::TextUnformatted(emptyMessage);
        view.ImageMin = ImGui::GetItemRectMin();
        view.ImageSize = size;
        view.Hovered = false;
        return;
    }

    // SDL_GPU / ImGui share top-left UV origin (same as Rml sceneview).
    // imgui_impl_sdlgpu3: ImTextureID = SDL_GPUTexture* (v1.92.8+).
    const ImTextureRef tex = TextureRef(color);
    ImGui::Image(tex, size, ImVec2{0.0F, 0.0F}, ImVec2{1.0F, 1.0F});
    view.ImageMin = ImGui::GetItemRectMin();
    view.ImageSize = ImGui::GetItemRectSize();
    view.Hovered = ImGui::IsItemHovered();
}

glm::vec2 ViewportPanel::MouseInViewPixels(const View& view) const
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (view.ImageSize.x <= 1.0F || view.ImageSize.y <= 1.0F || view.PixelW == 0)
    {
        return {};
    }
    const float u = (mouse.x - view.ImageMin.x) / view.ImageSize.x;
    const float v = (mouse.y - view.ImageMin.y) / view.ImageSize.y;
    return {
        std::clamp(u, 0.0F, 1.0F) * static_cast<float>(view.PixelW - 1),
        std::clamp(v, 0.0F, 1.0F) * static_cast<float>(view.PixelH - 1)};
}

void ViewportPanel::DrawSceneToolbar(
    EditorUiState& ui,
    CameraModule& camera,
    GizmoSystem& gizmo,
    const EditorPlayMode playMode)
{
    static_cast<void>(gizmo);
    auto tool = [&](const char* label, const char* tip, const int id) {
        const bool active = m_GizmoTool == id;
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(label))
        {
            m_GizmoTool = id;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", tip);
        }
        if (active)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    };
    tool(FADIX_ICON_SELECT, "Select (Q)", 0);
    tool(FADIX_ICON_MOVE, "Move (W)", 1);
    tool(FADIX_ICON_ROTATE, "Rotate (E)", 2);
    tool(FADIX_ICON_SCALE, "Scale (R)", 3);
    if (ImGui::SmallButton(m_GizmoLocalSpace ? FADIX_ICON_CUBE " Local" : FADIX_ICON_GLOBE " World"))
    {
        m_GizmoLocalSpace = !m_GizmoLocalSpace;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(m_AlwaysSnap ? "Snap On" : "Snap"))
    {
        ImGui::OpenPopup("##GizmoSnap");
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Configure transform snapping");
    }
    if (ImGui::BeginPopup("##GizmoSnap"))
    {
        ImGui::Checkbox("Always snap", &m_AlwaysSnap);
        ImGui::TextDisabled("Ctrl also snaps while dragging");
        ImGui::DragFloat("Move", &m_GizmoSnap.Translation, 0.05F, 0.01F, 1000.0F, "%.2f",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Rotate", &m_GizmoSnap.RotationDegrees, 1.0F, 1.0F, 180.0F, "%.0f deg",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::DragFloat("Scale", &m_GizmoSnap.Scale, 0.01F, 0.01F, 10.0F, "%.2f",
            ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    const bool playing =
        playMode == EditorPlayMode::Play || playMode == EditorPlayMode::Paused;
    if (playing)
    {
        if (ImGui::SmallButton(FADIX_ICON_STOP))
        {
            ui.RequestStop = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Stop");
        }
    }
    else
    {
        if (ImGui::SmallButton(FADIX_ICON_PLAY))
        {
            ui.RequestPlay = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Play");
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PAUSE))
    {
        ui.RequestPause = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Pause");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(playMode != EditorPlayMode::Paused);
    if (ImGui::SmallButton(FADIX_ICON_STEP))
    {
        ui.RequestStep = true;
    }
    ImGui::EndDisabled();
    if (playMode != EditorPlayMode::Paused && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Pause play mode to step");
    }
    else if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Step");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    DrawQualityCombo(m_Scene, "##SceneQuality");
    ImGui::SameLine();
    const bool is2DMode = camera.ProjectionMode() == ViewportProjectionMode::Ortho2D;
    if (!is2DMode)
    {
        if (m_ShowGroundGrid)
        {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(FADIX_ICON_GRID " Grid"))
        {
            m_ShowGroundGrid = !m_ShowGroundGrid;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Show ground grid in Scene View");
        }
        if (m_ShowGroundGrid)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    }
    ImGui::SameLine();
    const bool collisionActive = ui.ShowCollisionShapes;
    if (collisionActive)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::SmallButton(FADIX_ICON_EYE " Colliders"))
    {
        ui.ShowCollisionShapes = !ui.ShowCollisionShapes;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Show light-blue collision shapes through scene geometry");
    }
    if (collisionActive)
    {
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    int debugIndex = static_cast<int>(ui.SceneDebugView);
    ImGui::SetNextItemWidth(128.0F);
    if (ImGui::Combo("##SceneDebugView", &debugIndex, kDebugViewLabels.data(), static_cast<int>(kDebugViewLabels.size())))
    {
        ui.SceneDebugView = static_cast<ViewportDebugView>(
            std::clamp(debugIndex, 0, static_cast<int>(kDebugViewLabels.size()) - 1));
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Scene View debug visualization");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Speed");
    ImGui::SameLine();
    float flySpeed = camera.Camera().FlySpeed();
    ImGui::SetNextItemWidth(72.0F);
    if (ImGui::DragFloat("##CameraSpeed", &flySpeed, 0.1F, 0.01F, 10000.0F, "%.1f u/s"))
    {
        camera.Camera().SetFlySpeed(flySpeed);
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Scene camera fly speed\nRight-drag + mouse wheel also changes it");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_CAMERA " Reset"))
    {
        ResetSceneCamera(camera);
        ui.StatusText = "Reset scene camera";
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Reset the Scene View camera");
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();
    const bool is2D = camera.ProjectionMode() == ViewportProjectionMode::Ortho2D;
    if (is2D)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    if (ImGui::SmallButton("2D"))
    {
        camera.SetProjectionMode(
            is2D ? ViewportProjectionMode::Perspective : ViewportProjectionMode::Ortho2D);
        if (m_ProjectionModeChanged)
        {
            m_ProjectionModeChanged();
        }
    }
    if (is2D)
    {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Toggle 2D orthographic / 3D perspective viewport");
    }
}

void ViewportPanel::DrawQualityCombo(View& view, const char* id)
{
    static constexpr std::array<const char*, 4> kLabels{"Low", "Medium", "High", "Epic"};
    int current = std::clamp(static_cast<int>(view.Preset), 0, static_cast<int>(kLabels.size()) - 1);
    ImGui::SetNextItemWidth(84.0F);
    if (ImGui::Combo(id, &current, kLabels.data(), static_cast<int>(kLabels.size())))
    {
        view.Preset = static_cast<RenderQualityPreset>(current);
        ApplyQuality(view);
        if (m_QualityChanged)
        {
            m_QualityChanged();
        }
    }
    if (ImGui::IsItemHovered())
    {
        // Report the actual internal render resolution (post DPI + scale).
        ImGui::SetTooltip(
            "Render quality\nInternal resolution: %ux%u (%.0f%%)",
            view.PixelW,
            view.PixelH,
            static_cast<double>(view.Quality.ResolutionScale) * 100.0);
    }
}

void ViewportPanel::UpdateGizmoVisual(
    View& view, SceneEditor& scene, GizmoSystem& gizmo, const bool editMode)
{
    if (!view.Renderer)
    {
        return;
    }
    const auto selection = scene.Selection();
    view.Renderer->SetSelection(selection);
    GizmoVisual visual;
    visual.Visible = editMode && selection.has_value() && m_GizmoTool != 0;
    visual.Mode = ToolToMode(m_GizmoTool);
    visual.Hover = m_GizmoHover;
    visual.Active = m_GizmoDragging ? m_GizmoHover : std::nullopt;
    if (selection)
    {
        if (const auto entity = scene.World().Find(*selection))
        {
            if (const TransformComponent* transform =
                    scene.World().Registry().try_get<TransformComponent>(*entity))
            {
                visual.Position = transform->Position;
                if (m_GizmoLocalSpace)
                {
                    visual.Orientation = transform->Rotation;
                }
            }
        }
    }
    gizmo.SetMode(visual.Mode);
    view.Renderer->SetGizmo(visual);
}

std::optional<GizmoHandle> ViewportPanel::HitTestGizmo(
    const View& view,
    SceneEditor& scene,
    CameraModule& camera,
    const GizmoMode mode) const
{
    namespace layout3d = gizmo_layout;
    if (!view.Hovered || m_GizmoTool == 0 || mode == GizmoMode::Select)
    {
        return std::nullopt;
    }
    const auto selection = scene.Selection();
    if (!selection)
    {
        return std::nullopt;
    }
    const auto entity = scene.World().Find(*selection);
    if (!entity)
    {
        return std::nullopt;
    }
    const TransformComponent* transform =
        scene.World().Registry().try_get<TransformComponent>(*entity);
    if (transform == nullptr)
    {
        return std::nullopt;
    }

    const glm::vec2 local = MouseInViewPixels(view);
    const GizmoRay ray = MouseRayFromCamera(
        camera.Camera().View(),
        camera.Camera().Projection(),
        local,
        {static_cast<float>(view.PixelW), static_cast<float>(view.PixelH)});
    const glm::vec3 position = transform->Position;
    const float size = GizmoWorldSize(
        camera.Camera().View(),
        camera.Camera().Projection(),
        position,
        static_cast<float>(view.PixelH));
    if (size <= 0.0F)
    {
        return std::nullopt;
    }
    const glm::quat frame =
        m_GizmoLocalSpace ? transform->Rotation : glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    const float slack = layout3d::PickSlack * size;

    struct Candidate
    {
        GizmoHandle Handle;
        float RayT;
    };
    std::optional<Candidate> best;
    const auto consider = [&best](const GizmoHandle handle, const float rayT) {
        if (!best || rayT < best->RayT)
        {
            best = Candidate{handle, rayT};
        }
    };
    constexpr std::array axisHandles{GizmoHandle::AxisX, GizmoHandle::AxisY, GizmoHandle::AxisZ};

    if (mode == GizmoMode::Rotate)
    {
        for (std::size_t index = 0; index < axisHandles.size(); ++index)
        {
            const glm::vec3 normal = frame * GizmoAxisVector(static_cast<GizmoAxis>(index));
            const auto hit = RayPlaneIntersect(ray, position, normal);
            if (!hit)
            {
                continue;
            }
            const float radius = glm::length(hit->Point - position);
            if (std::abs(radius - layout3d::RingRadius * size) <
                layout3d::RingTubeRadius * 3.0F * size + slack)
            {
                consider(axisHandles[index], hit->RayT);
            }
        }
        return best ? std::optional{best->Handle} : std::nullopt;
    }

    const bool translate = mode == GizmoMode::Translate;
    const float tipEnd = translate ? layout3d::ShaftEnd + layout3d::ArrowLength
                                   : layout3d::ShaftEnd + layout3d::ScaleCubeHalf;
    const float grabRadius =
        (translate ? layout3d::ArrowRadius : layout3d::ScaleCubeHalf) * size + slack;
    for (std::size_t index = 0; index < axisHandles.size(); ++index)
    {
        const glm::vec3 direction = frame * GizmoAxisVector(static_cast<GizmoAxis>(index));
        float rayT = 0.0F;
        const float distance = RaySegmentDistance(
            ray,
            position + direction * layout3d::ShaftStart * size,
            position + direction * tipEnd * size,
            rayT);
        if (distance < grabRadius)
        {
            consider(axisHandles[index], rayT);
        }
    }

    if (translate)
    {
        const std::array planes{
            std::pair{GizmoHandle::PlaneXY, std::pair{GizmoAxis::X, GizmoAxis::Y}},
            std::pair{GizmoHandle::PlaneXZ, std::pair{GizmoAxis::X, GizmoAxis::Z}},
            std::pair{GizmoHandle::PlaneYZ, std::pair{GizmoAxis::Y, GizmoAxis::Z}}};
        for (const auto& [handle, axes] : planes)
        {
            const glm::vec3 normal = frame * GizmoAxisVector(GizmoHandleAxis(handle));
            const auto hit = RayPlaneIntersect(ray, position, normal);
            if (!hit)
            {
                continue;
            }
            const glm::vec3 offset = hit->Point - position;
            const float u = glm::dot(offset, frame * GizmoAxisVector(axes.first));
            const float v = glm::dot(offset, frame * GizmoAxisVector(axes.second));
            const float lo = layout3d::PlaneOffset * size;
            const float hi = (layout3d::PlaneOffset + layout3d::PlaneSize) * size;
            if (u >= lo && u <= hi && v >= lo && v <= hi)
            {
                consider(handle, hit->RayT);
            }
        }
    }
    else if (mode == GizmoMode::Scale)
    {
        const glm::vec3 toCenter = position - ray.Origin;
        const float along = glm::dot(toCenter, ray.Direction);
        const float distance =
            glm::length(toCenter - ray.Direction * std::max(along, 0.0F));
        if (distance < layout3d::UniformCubeHalf * 2.0F * size + slack)
        {
            consider(GizmoHandle::Uniform, std::max(along, 0.0F) - 0.05F * size);
        }
    }
    return best ? std::optional{best->Handle} : std::nullopt;
}

void ViewportPanel::Draw(
    EditorUiState& ui,
    SceneEditor& scene,
    CameraModule& camera,
    GizmoSystem& gizmo,
    const EditorPlayMode playMode,
    UndoStack& history,
    IWorld& editWorld,
    SceneDocument& document,
    SDL_Window* window)
{
    static_cast<void>(document);
    static_cast<void>(history);
    const float dpi =
        window != nullptr ? std::max(SDL_GetWindowDisplayScale(window), 0.01F) : 1.0F;

    m_Scene.Visible = false;
    m_Game.Visible = false;
    m_Animation.Visible = false;
    m_Scene.Focused = false;
    m_Game.Focused = false;

    if (ui.ShowSceneView)
    {
        if (ImGui::Begin(FADIX_ICON_CUBE " Scene View###Scene View", &ui.ShowSceneView))
        {
            m_Scene.Visible = true;
            m_Scene.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            DrawSceneToolbar(ui, camera, gizmo, playMode);
            MeasureView(m_Scene, dpi);
            DrawViewImage(m_Scene, "Resize Scene View", true);

            // Update 2D camera cursor position for zoom centering
            {
                const glm::vec2 cursorPx = MouseInViewPixels(m_Scene);
                camera.SetCursorViewportPos(cursorPx);
            }

            // 2D viewport overlays (grid, sprite bounds, colliders, tilemap grid)
            if (camera.ProjectionMode() == ViewportProjectionMode::Ortho2D &&
                m_Scene.Visible && m_Scene.ImageSize.x > 0.0F)
            {
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->PushClipRect(m_Scene.ImageMin,
                    {m_Scene.ImageMin.x + m_Scene.ImageSize.x,
                        m_Scene.ImageMin.y + m_Scene.ImageSize.y},
                    true);
                Draw2DGrid(drawList, camera.Ortho2D(), m_Scene.ImageMin, m_Scene.ImageSize);
                Draw2DEntityOverlays(drawList, camera.Ortho2D(), m_Scene.ImageMin, editWorld, scene);
                drawList->PopClipRect();
            }

            // Per-cascade depth preview (debug only, Scene View only). Each active
            // cascade's real depth texture is drawn as a small inset along the top,
            // cascade 0 leftmost, so their contents can be compared directly.
            if (m_DebugView == ViewportDebugView::CascadeColors && m_Scene.Renderer)
            {
                const int cascades = m_Scene.Renderer->ShadowCascadeCount();
                const float side = std::min(m_Scene.ImageSize.x, m_Scene.ImageSize.y) * 0.22F;
                if (side > 8.0F)
                {
                    float x = m_Scene.ImageMin.x + m_Scene.ImageSize.x - 8.0F;
                    for (int c = cascades - 1; c >= 0; --c)
                    {
                        rhi::Texture* tex = m_Scene.Renderer->ShadowDebugTexture(c);
                        if (tex == nullptr)
                        {
                            continue;
                        }
                        x -= side;
                        ImGui::SetCursorScreenPos(ImVec2{x, m_Scene.ImageMin.y + 8.0F});
                        ImGui::Image(TextureRef(tex), ImVec2{side, side});
                        x -= 4.0F;
                    }
                }
            }
            bool meshPreviewActive = false;
            if (playMode == EditorPlayMode::Edit && ImGui::BeginDragDropTarget())
            {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                        "FADIX_ASSET", ImGuiDragDropFlags_AcceptBeforeDelivery))
                {
                    if (const auto asset = ParseAssetDragDropBlob(
                            payload->Data, static_cast<std::size_t>(payload->DataSize)))
                    {
                        const bool in2DMode =
                            camera.ProjectionMode() == ViewportProjectionMode::Ortho2D;
                        if (asset->AssetType == "Texture" && in2DMode)
                        {
                            if (payload->IsDelivery() && m_Sprite2DDropHandler)
                            {
                                const glm::vec2 pixel = MouseInViewPixels(m_Scene);
                                const glm::vec2 worldPos = camera.Ortho2D().ScreenToWorld(pixel);
                                m_Sprite2DDropHandler(*asset, worldPos);
                            }
                        }
                        else if (asset->AssetType == "Mesh" && !in2DMode)
                        {
                            const glm::vec2 pixel = MouseInViewPixels(m_Scene);
                            const GizmoRay ray = MouseRayFromCamera(
                                camera.Camera().View(),
                                camera.Camera().Projection(),
                                pixel,
                                {static_cast<float>(m_Scene.PixelW),
                                    static_cast<float>(m_Scene.PixelH)});
                            if (const auto hit = RayPlaneIntersect(
                                    ray, glm::vec3{0.0F}, glm::vec3{0.0F, 1.0F, 0.0F}))
                            {
                                meshPreviewActive = true;
                                m_MeshPreview = MeshPreviewVisual{asset->Handle, hit->Point};
                                if (payload->IsDelivery())
                                {
                                    m_MeshPreview.reset();
                                    if (m_MeshDropHandler)
                                    {
                                        m_MeshDropHandler(*asset, hit->Point);
                                    }
                                    else if (m_AssetDropHandler)
                                    {
                                        m_AssetDropHandler(*asset);
                                    }
                                }
                            }
                            else
                            {
                                ui.StatusText = "Aim at the ground grid to place the mesh";
                            }
                        }
                        else if (payload->IsDelivery())
                        {
                            bool hasTarget = true;
                            if (asset->AssetType == "Script")
                            {
                                hasTarget = false;
                                if (m_Scene.Picking)
                                {
                                    const glm::vec2 pixel = MouseInViewPixels(m_Scene);
                                    m_Scene.Picking->Request(PickRequest{
                                        static_cast<std::uint32_t>(pixel.x),
                                        static_cast<std::uint32_t>(pixel.y)});
                                    if (const auto result = m_Scene.Picking->Poll())
                                    {
                                        scene.SetSelection(result->Entity, false);
                                        hasTarget = true;
                                    }
                                }
                                if (!hasTarget)
                                {
                                    ui.StatusText = "Drop the script directly onto a mesh";
                                }
                            }
                            if (hasTarget && m_AssetDropHandler)
                            {
                                m_AssetDropHandler(*asset);
                            }
                        }
                    }
                }
                ImGui::EndDragDropTarget();
            }
            if (!meshPreviewActive)
            {
                m_MeshPreview.reset();
            }
            if (m_Scene.Hovered && m_PendingPick && m_Scene.Picking)
            {
                if (const auto result = m_Scene.Picking->Poll())
                {
                    scene.SetSelection(result->Entity, true);
                    m_PendingPick = false;
                }
            }
        }
        ImGui::End();
    }

    if (ui.ShowGameView)
    {
        if (ImGui::Begin(FADIX_ICON_GAMEPAD " Game View###Game View", &ui.ShowGameView))
        {
            m_Game.Visible = true;
            m_Game.Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            ImGui::TextUnformatted("Quality");
            ImGui::SameLine();
            DrawQualityCombo(m_Game, "##GameQuality");
            MeasureView(m_Game, dpi);
            const bool hasGameCam = camera.GameCamera().has_value();
            DrawViewImage(
                m_Game, hasGameCam ? "Resize Game View" : "No game camera", hasGameCam);
        }
        ImGui::End();
    }
}

void ViewportPanel::HandleEvent(
    const SDL_Event& event,
    SceneEditor& scene,
    CameraModule& camera,
    GizmoSystem& gizmo,
    const EditorPlayMode playMode,
    UndoStack& history,
    IWorld& editWorld,
    SceneDocument& document)
{
    const bool edit = playMode == EditorPlayMode::Edit;
    const bool sceneHovered = m_Scene.Visible && m_Scene.Hovered;
    const bool sceneFocused = m_Scene.Visible && m_Scene.Focused;
    // Always forward events so KEY_UP / BUTTON_UP clear sticky WASD / look capture.
    // Hover=false when WantTextInput; EditorCameraInput still accepts releases / active nav.
    static_cast<void>(camera.Input().HandleEvent(
        event, sceneHovered && edit && !ImGui::GetIO().WantTextInput));

    if (edit && sceneFocused && !ImGui::GetIO().WantTextInput &&
        event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        event.key.scancode == SDL_SCANCODE_HOME)
    {
        ResetSceneCamera(camera);
        return;
    }

    // Keep gizmo drag alive when the cursor leaves the image mid-drag.
    if (!edit || !m_Scene.Visible || !m_Scene.Renderer || (!sceneHovered && !m_GizmoDragging))
    {
        return;
    }

    const GizmoMode mode = ToolToMode(m_GizmoTool);
    const glm::vec2 local = MouseInViewPixels(m_Scene);
    const auto mouseRay = [&]() {
        return MouseRayFromCamera(
            camera.Camera().View(),
            camera.Camera().Projection(),
            local,
            {static_cast<float>(m_Scene.PixelW), static_cast<float>(m_Scene.PixelH)});
    };

    // 2D tilemap viewport painting (intercepts before gizmo when a paint tool is active)
    if (camera.ProjectionMode() == ViewportProjectionMode::Ortho2D &&
        m_TilemapPanel != nullptr && m_TilemapPanel->IsActive() &&
        m_TilemapPanel->ActiveTool() != TilemapTool::Select)
    {
        const glm::vec2 worldPos2D = camera.Ortho2D().ScreenToWorld(local);
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            m_TilemapPanel->HandleViewportMouseMove(worldPos2D, scene, editWorld);
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == SDL_BUTTON_LEFT)
        {
            if (m_TilemapPanel->HandleViewportMouseDown(worldPos2D, scene, editWorld))
            {
                return;
            }
        }
        else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == SDL_BUTTON_LEFT)
        {
            m_TilemapPanel->HandleViewportMouseUp(scene, editWorld, history);
            return;
        }
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        if (m_GizmoDragging)
        {
            GizmoSnap snap = m_GizmoSnap;
            snap.Enabled = m_AlwaysSnap || (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
            gizmo.SetSnap(snap);
            static_cast<void>(gizmo.Update(editWorld, mouseRay()));
            document.Dirty = true;
            return;
        }
        m_GizmoHover = HitTestGizmo(m_Scene, scene, camera, mode);
        return;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
        m_GizmoDragging)
    {
        if (const auto selection = scene.Selection(); selection && m_GizmoStartTransform)
        {
            if (const auto entity = editWorld.Find(*selection))
            {
                if (const TransformComponent* after =
                        editWorld.Registry().try_get<TransformComponent>(*entity))
                {
                    history.Push(std::make_unique<TransformEntityCommand>(
                        editWorld, *selection, *m_GizmoStartTransform, *after));
                }
            }
        }
        gizmo.End();
        m_GizmoDragging = false;
        m_GizmoStartTransform.reset();
        document.Dirty = true;
        return;
    }

    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || event.button.button != SDL_BUTTON_LEFT)
    {
        return;
    }
    if (ImGui::GetIO().WantCaptureKeyboard && ImGui::IsAnyItemActive())
    {
        return;
    }

    m_GizmoHover = HitTestGizmo(m_Scene, scene, camera, mode);
    if (m_GizmoHover && scene.Selection())
    {
        const TransformComponent* transform = nullptr;
        if (const auto entity = editWorld.Find(*scene.Selection()))
        {
            transform = editWorld.Registry().try_get<TransformComponent>(*entity);
        }
        if (transform != nullptr)
        {
            m_GizmoStartTransform = *transform;
            const glm::quat frame =
                m_GizmoLocalSpace ? transform->Rotation : glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
            m_GizmoDragging =
                gizmo.Begin(editWorld, *scene.Selection(), *m_GizmoHover, mouseRay(), frame);
            if (!m_GizmoDragging)
            {
                m_GizmoStartTransform.reset();
            }
            return;
        }
    }

    if (m_Scene.Picking && (m_GizmoTool == 0 || !m_GizmoHover))
    {
        const auto pixel = MouseInViewPixels(m_Scene);
        m_Scene.Picking->Request(PickRequest{
            static_cast<std::uint32_t>(pixel.x), static_cast<std::uint32_t>(pixel.y)});
        m_PendingPick = true;
        if (const auto result = m_Scene.Picking->Poll())
        {
            scene.SetSelection(result->Entity, true);
            m_PendingPick = false;
        }
    }
}
}
