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
#include <glm/gtc/quaternion.hpp>

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

// Subtle adaptive 2D grid. Line density is chosen from the current zoom so that
// spacing stays visually stable: as you zoom out the finest tier fades away, and
// as you zoom in a new finer subdivision fades in. Two tiers are drawn (minor +
// major = minor*10), with axis lines in restrained X/Y colors on top.
void Draw2DGrid(
    ImDrawList* drawList,
    const Ortho2DCamera& cam,
    const ImVec2 imgMin,
    const ImVec2 imgSize)
{
    static_cast<void>(imgSize); // caller already clips to the image rect
    const glm::vec2 viewSize = cam.ViewportSize();
    if (viewSize.x < 1.0F || viewSize.y < 1.0F)
    {
        return;
    }
    const float hh = cam.OrthoHalfHeight();
    const float hw = hh * (viewSize.x / viewSize.y);
    const float pxPerUnit = viewSize.y / std::max(2.0F * hh, 1.0e-6F);

    // Minor step = smallest power of ten whose on-screen spacing is >= ~8px.
    // Zooming in shrinks the step (adds subdivisions); zooming out grows it.
    constexpr float kMinMinorPx = 8.0F;
    const float minorStep =
        std::pow(10.0F, std::ceil(std::log10(kMinMinorPx / std::max(pxPerUnit, 1.0e-6F))));
    const float majorStep = minorStep * 10.0F;

    const glm::vec2 wMin = cam.Position() - glm::vec2{hw, hh};
    const glm::vec2 wMax = cam.Position() + glm::vec2{hw, hh};

    // Alpha ramps with on-screen spacing so dense lines fade out and freshly
    // added subdivisions fade in smoothly rather than popping.
    const auto tierAlpha = [](const float spacingPx, const float fadeLo, const float fadeHi) {
        return std::clamp((spacingPx - fadeLo) / std::max(fadeHi - fadeLo, 1.0e-3F), 0.0F, 1.0F);
    };
    const auto drawTier = [&](const float step, const ImU32 baseColor, const float alpha) {
        if (alpha <= 0.02F)
        {
            return;
        }
        const int a = static_cast<int>(alpha * static_cast<float>((baseColor >> IM_COL32_A_SHIFT) & 0xFFU));
        const ImU32 col = (baseColor & ~(0xFFU << IM_COL32_A_SHIFT)) |
            (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
        for (float x = std::floor(wMin.x / step) * step; x <= wMax.x + step; x += step)
        {
            drawList->AddLine(WorldToImGui({x, wMax.y}, cam, imgMin),
                WorldToImGui({x, wMin.y}, cam, imgMin), col, 1.0F);
        }
        for (float y = std::floor(wMin.y / step) * step; y <= wMax.y + step; y += step)
        {
            drawList->AddLine(WorldToImGui({wMin.x, y}, cam, imgMin),
                WorldToImGui({wMax.x, y}, cam, imgMin), col, 1.0F);
        }
    };

    // Low-opacity minor lines fade out as they crowd together (zoom-out);
    // slightly stronger major lines stay steady.
    drawTier(minorStep, IM_COL32(150, 155, 175, 46),
        tierAlpha(minorStep * pxPerUnit, kMinMinorPx, 22.0F));
    drawTier(majorStep, IM_COL32(165, 170, 195, 110), 1.0F);

    // Restrained X (red) / Y (green) axes.
    const ImVec2 axisXLeft = WorldToImGui({wMin.x, 0.0F}, cam, imgMin);
    const ImVec2 axisXRight = WorldToImGui({wMax.x, 0.0F}, cam, imgMin);
    drawList->AddLine(axisXLeft, axisXRight, IM_COL32(188, 96, 96, 190), 1.4F);
    const ImVec2 axisYTop = WorldToImGui({0.0F, wMax.y}, cam, imgMin);
    const ImVec2 axisYBot = WorldToImGui({0.0F, wMin.y}, cam, imgMin);
    drawList->AddLine(axisYTop, axisYBot, IM_COL32(96, 176, 112, 190), 1.4F);
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

// Draws a crisp chevron from line primitives (no icon-font glyph dependency).
// dir: 0 = left '<', 1 = right '>', 2 = down 'v'.
void DrawChevron(ImDrawList* dl, const ImVec2 center, const float size, const int dir,
    const ImU32 col, const float thickness = 1.7F)
{
    const float h = size * 0.5F;
    const float w = h * 0.62F;
    ImVec2 a{};
    ImVec2 b{};
    ImVec2 c{};
    switch (dir)
    {
    case 0: // '<'
        a = {center.x + w, center.y - h};
        b = {center.x - w, center.y};
        c = {center.x + w, center.y + h};
        break;
    case 1: // '>'
        a = {center.x - w, center.y - h};
        b = {center.x + w, center.y};
        c = {center.x - w, center.y + h};
        break;
    default: // 'v'
        a = {center.x - h, center.y - w};
        b = {center.x, center.y + w};
        c = {center.x + h, center.y - w};
        break;
    }
    dl->AddLine(a, b, col, thickness);
    dl->AddLine(b, c, col, thickness);
}

// Minimal "image" glyph (frame + sun + mountain) for the no-sprite placeholder.
void DrawImageGlyph(ImDrawList* dl, const ImVec2 center, const float r, const ImU32 col)
{
    const ImVec2 tl{center.x - r, center.y - r * 0.8F};
    const ImVec2 br{center.x + r, center.y + r * 0.8F};
    dl->AddRect(tl, br, col, r * 0.18F, 0, 1.6F);
    dl->AddCircleFilled({center.x - r * 0.4F, center.y - r * 0.35F}, r * 0.18F, col, 12);
    const ImVec2 p0{tl.x + r * 0.12F, br.y - r * 0.12F};
    const ImVec2 p1{center.x - r * 0.1F, center.y + r * 0.05F};
    const ImVec2 p2{center.x + r * 0.15F, center.y + r * 0.35F};
    const ImVec2 p3{br.x - r * 0.12F, br.y - r * 0.12F};
    dl->AddLine(p0, p1, col, 1.6F);
    dl->AddLine(p1, p2, col, 1.6F);
    dl->AddLine(p2, p3, col, 1.6F);
}

// Editor-only Scene View placeholder for Sprite2D entities that have no texture.
// The renderer draws nothing for them (no white quad), so this hatched card marks
// where an empty sprite lives. Follows position, rotation, Size, pivot, and zoom.
// Never drawn in Game View (only called from the Scene View 2D overlay block).
void Draw2DSpritePlaceholders(
    ImDrawList* drawList,
    const Ortho2DCamera& cam,
    const ImVec2 imgMin,
    const IWorld& world,
    const SceneEditor& scene)
{
    const std::optional<Uuid> selection = scene.Selection();
    const entt::registry& reg = world.Registry();
    for (const auto [entity, sprite] : reg.view<const Sprite2DComponent>().each())
    {
        if (Sprite2DHasRenderableTexture(sprite))
        {
            continue;
        }
        if (const auto* vis = reg.try_get<VisibilityComponent>(entity);
            vis != nullptr && !vis->VisibleInEditor)
        {
            continue;
        }

        glm::vec3 position{0.0F};
        glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
        glm::vec3 scale{1.0F};
        if (const auto* xform = reg.try_get<TransformComponent>(entity))
        {
            position = xform->Position;
            rotation = xform->Rotation;
            scale = xform->Scale;
        }
        const float ox = 0.5F - sprite.Pivot.x;
        const float oy = sprite.Pivot.y - 0.5F;
        // World-space corners matching the renderer model: pos + R*(S*(v+pivot)),
        // using the exact same effective size (scale + FlipX/FlipY) as the renderer.
        const glm::vec2 effective = Sprite2DEffectiveSize(sprite, {scale.x, scale.y});
        const float sx = effective.x;
        const float sy = effective.y;
        const std::array<glm::vec2, 4> local{{{-0.5F, -0.5F}, {0.5F, -0.5F}, {0.5F, 0.5F},
            {-0.5F, 0.5F}}};
        std::array<ImVec2, 4> screen{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            const glm::vec3 l{(local[i].x + ox) * sx, (local[i].y + oy) * sy, 0.0F};
            const glm::vec3 wpos = position + rotation * l;
            screen[i] = WorldToImGui({wpos.x, wpos.y}, cam, imgMin);
        }

        const bool selected = selection && world.Find(*selection) == entity;

        // Hatched translucent card + outline.
        drawList->AddQuadFilled(screen[0], screen[1], screen[2], screen[3],
            IM_COL32(40, 44, 52, 120));
        const ImU32 outline = selected ? IM_COL32(90, 160, 250, 255)
                                       : IM_COL32(150, 158, 175, 180);
        drawList->AddQuad(screen[0], screen[1], screen[2], screen[3], outline,
            selected ? 2.2F : 1.4F);
        // Two hatch strokes across the card for the "empty" read.
        drawList->AddLine(screen[0], screen[2], IM_COL32(150, 158, 175, 70), 1.0F);
        drawList->AddLine(screen[1], screen[3], IM_COL32(150, 158, 175, 70), 1.0F);

        // Centered image glyph, sized to the on-screen card, when large enough.
        const ImVec2 center{(screen[0].x + screen[2].x) * 0.5F,
            (screen[0].y + screen[2].y) * 0.5F};
        float diag = std::hypot(screen[2].x - screen[0].x, screen[2].y - screen[0].y);
        const float glyphR = std::clamp(diag * 0.18F, 5.0F, 22.0F);
        if (diag > 26.0F)
        {
            DrawImageGlyph(drawList, center, glyphR, IM_COL32(190, 198, 214, 210));
        }
        if (diag > 90.0F)
        {
            const char* label = "No Sprite";
            const ImVec2 ts = ImGui::CalcTextSize(label);
            drawList->AddText({center.x - ts.x * 0.5F, center.y + glyphR + 3.0F},
                IM_COL32(190, 198, 214, 200), label);
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
    m_TransformRailVisible = prefs.TransformRailVisible;
    m_OrientationGizmoVisible = prefs.OrientationGizmoVisible;
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
    // Select/Move/Rotate/Scale now live in the floating in-viewport transform rail
    // (DrawTransformRail); the top toolbar keeps only space + play/quality controls.
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

bool ViewportPanel::EdgeTab(
    const char* id, const ImVec2 pos, const ImVec2 size, const int dir, const char* tip)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    const bool clicked = ImGui::InvisibleButton("##tab", size);
    const bool hovered = ImGui::IsItemHovered();
    m_OverlayHovered = m_OverlayHovered || hovered;
    dl->AddRectFilled(pos, {pos.x + size.x, pos.y + size.y},
        hovered ? IM_COL32(255, 255, 255, 34) : IM_COL32(255, 255, 255, 18), 4.0F);
    DrawChevron(dl, {pos.x + size.x * 0.5F, pos.y + size.y * 0.5F},
        std::min(size.x, size.y) * 0.6F, dir,
        hovered ? IM_COL32(235, 238, 245, 255) : IM_COL32(190, 196, 208, 230));
    if (hovered)
    {
        ImGui::SetTooltip("%s", tip);
    }
    ImGui::PopID();
    return clicked;
}

void ViewportPanel::DrawTransformRail(const View& view)
{
    if (!view.Visible || view.ImageSize.x < 48.0F || view.ImageSize.y < 48.0F)
    {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static constexpr ImU32 kAccent = IM_COL32(58, 122, 226, 235); // restrained Fadix blue

    if (!m_TransformRailVisible)
    {
        // Collapsed: slim restore tab on the left viewport edge.
        if (EdgeTab("##RailRestore",
                {view.ImageMin.x + 2.0F, view.ImageMin.y + view.ImageSize.y * 0.5F - 20.0F},
                {14.0F, 40.0F}, 1, "Show transform tools"))
        {
            m_TransformRailVisible = true;
            if (m_WidgetVisibilityChanged)
            {
                m_WidgetVisibilityChanged();
            }
        }
        return;
    }

    struct Tool
    {
        const char* Icon;
        const char* Tip;
        int Id;
    };
    static constexpr std::array<Tool, 4> tools{{
        {FADIX_ICON_SELECT, "Select (Q)", 0},
        {FADIX_ICON_MOVE, "Move (W)", 1},
        {FADIX_ICON_ROTATE, "Rotate (E)", 2},
        {FADIX_ICON_SCALE, "Scale (R)", 3}}};

    constexpr float btn = 30.0F;
    constexpr float spacing = 6.0F;
    const float h = static_cast<float>(tools.size()) * btn +
        static_cast<float>(tools.size() - 1) * spacing;
    const float x = view.ImageMin.x + 12.0F;
    float y = view.ImageMin.y + view.ImageSize.y * 0.5F - h * 0.5F;

    // No solid backing panel: each button is transparent until hovered/active.
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(255, 255, 255, 30));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(255, 255, 255, 46));
    for (const Tool& t : tools)
    {
        ImGui::SetCursorScreenPos({x, y});
        ImGui::PushID(t.Id);
        const bool active = m_GizmoTool == t.Id;
        if (active)
        {
            // Soft rounded blue highlight behind the active tool.
            dl->AddRectFilled({x, y}, {x + btn, y + btn}, kAccent, 7.0F);
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0));
        }
        if (ImGui::Button(t.Icon, {btn, btn}))
        {
            m_GizmoTool = t.Id;
        }
        if (active)
        {
            ImGui::PopStyleColor();
        }
        const bool hovered = ImGui::IsItemHovered();
        m_OverlayHovered = m_OverlayHovered || hovered;
        if (hovered)
        {
            ImGui::SetTooltip("%s", t.Tip);
        }
        ImGui::PopID();
        y += btn + spacing;
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);

    // Collapse chevron tab beneath the tools.
    if (EdgeTab("##RailCollapse", {x + (btn - 20.0F) * 0.5F, y + 2.0F}, {20.0F, 16.0F}, 0,
            "Hide transform tools"))
    {
        m_TransformRailVisible = false;
        if (m_WidgetVisibilityChanged)
        {
            m_WidgetVisibilityChanged();
        }
    }
}

void ViewportPanel::DrawOrientationGizmo(const View& view, CameraModule& camera)
{
    if (!view.Visible || view.ImageSize.x < 90.0F || view.ImageSize.y < 90.0F)
    {
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    constexpr float margin = 12.0F;
    constexpr float radius = 34.0F;

    if (!m_OrientationGizmoVisible)
    {
        // Collapsed: slim restore tab on the right viewport edge.
        if (EdgeTab("##OrientRestore",
                {view.ImageMin.x + view.ImageSize.x - 16.0F, view.ImageMin.y + margin},
                {14.0F, 40.0F}, 0, "Show orientation gizmo"))
        {
            m_OrientationGizmoVisible = true;
            if (m_WidgetVisibilityChanged)
            {
                m_WidgetVisibilityChanged();
            }
        }
        return;
    }

    const ImVec2 center{
        view.ImageMin.x + view.ImageSize.x - margin - radius, view.ImageMin.y + margin + radius};
    // Subtle translucent backing for readability against bright scenes.
    dl->AddCircleFilled(center, radius + 9.0F, IM_COL32(24, 26, 32, 120), 40);

    // Project the world axes through the camera's view rotation so the widget
    // tracks the live camera orientation.
    const glm::mat3 viewRot{camera.Camera().View()};
    struct Ball
    {
        glm::vec3 Dir;
        ImU32 Color;
        char Letter;
        bool Positive;
    };
    static constexpr ImU32 kRed = IM_COL32(206, 92, 92, 255);
    static constexpr ImU32 kGreen = IM_COL32(116, 190, 128, 255);
    static constexpr ImU32 kBlue = IM_COL32(100, 146, 220, 255);
    const std::array<Ball, 6> balls{{
        {{1.0F, 0.0F, 0.0F}, kRed, 'X', true},
        {{0.0F, 1.0F, 0.0F}, kGreen, 'Y', true},
        {{0.0F, 0.0F, 1.0F}, kBlue, 'Z', true},
        {{-1.0F, 0.0F, 0.0F}, kRed, 'X', false},
        {{0.0F, -1.0F, 0.0F}, kGreen, 'Y', false},
        {{0.0F, 0.0F, -1.0F}, kBlue, 'Z', false}}};

    struct Projected
    {
        ImVec2 Pos;
        float Depth; // camera-space z: >0 toward viewer
        int Index;
    };
    std::array<Projected, 6> order{};
    for (int i = 0; i < 6; ++i)
    {
        const glm::vec3 v = viewRot * balls[static_cast<std::size_t>(i)].Dir;
        order[static_cast<std::size_t>(i)] = {
            {center.x + v.x * radius, center.y - v.y * radius}, v.z, i};
    }
    // Back-to-front so front-facing endpoints draw on top and win hover priority.
    std::sort(order.begin(), order.end(),
        [](const Projected& a, const Projected& b) { return a.Depth < b.Depth; });

    for (const Projected& p : order)
    {
        const Ball& b = balls[static_cast<std::size_t>(p.Index)];
        const float front = std::clamp((p.Depth + 1.0F) * 0.5F, 0.0F, 1.0F); // 0=rear 1=front
        const auto withAlpha = [](const ImU32 base, const float a01) {
            const ImU32 a = static_cast<ImU32>(std::clamp(a01, 0.0F, 1.0F) * 255.0F);
            return (base & ~(0xFFU << IM_COL32_A_SHIFT)) | (a << IM_COL32_A_SHIFT);
        };

        // All six directions are clickable snap targets.
        ImGui::SetCursorScreenPos({p.Pos.x - 9.0F, p.Pos.y - 9.0F});
        ImGui::PushID(p.Index);
        const bool clicked = ImGui::InvisibleButton("##axis", {18.0F, 18.0F});
        const bool hovered = ImGui::IsItemHovered();
        m_OverlayHovered = m_OverlayHovered || hovered;
        ImGui::PopID();

        if (b.Positive)
        {
            // Thin anti-aliased stem + filled endpoint with axis letter.
            dl->AddLine(center, p.Pos, withAlpha(b.Color, 0.35F + 0.55F * front), 1.5F);
            const float er = (hovered ? 10.0F : 8.5F) + 1.5F * front; // emphasize camera-facing
            dl->AddCircleFilled(p.Pos, er, withAlpha(b.Color, 0.55F + 0.45F * front), 24);
            if (hovered)
            {
                dl->AddCircle(p.Pos, er + 1.5F, IM_COL32(255, 255, 255, 220), 24, 1.5F);
            }
            const char label[2]{b.Letter, '\0'};
            const ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText({p.Pos.x - ts.x * 0.5F, p.Pos.y - ts.y * 0.5F},
                IM_COL32(20, 22, 26, 255), label);
        }
        else
        {
            // Smaller muted ring for the negative direction.
            const float rr = hovered ? 6.5F : 5.0F;
            dl->AddCircle(p.Pos, rr, withAlpha(b.Color, 0.3F + 0.4F * front), 20, 1.6F);
            if (hovered)
            {
                dl->AddCircleFilled(p.Pos, rr - 1.0F, withAlpha(b.Color, 0.5F), 20);
            }
        }

        if (hovered)
        {
            ImGui::SetTooltip("View from %c%c", b.Positive ? '+' : '-', b.Letter);
        }
        if (clicked)
        {
            // Snap so the camera looks toward the origin from the clicked axis
            // (forward = -direction); pivot + orbit distance preserved.
            camera.Camera().SetLookDirection(-b.Dir);
            ResetTemporalHistory();
        }
    }

    // Collapse chevron tab beneath the widget.
    if (EdgeTab("##OrientCollapse", {center.x - 10.0F, center.y + radius + 6.0F}, {20.0F, 16.0F},
            2, "Hide orientation gizmo"))
    {
        m_OrientationGizmoVisible = false;
        if (m_WidgetVisibilityChanged)
        {
            m_WidgetVisibilityChanged();
        }
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

            m_OverlayHovered = false;

            // Update 2D camera cursor position for zoom centering. The Ortho2D
            // camera's viewport size is the LOGICAL image size, so feed the cursor
            // in that same logical space (not render pixels) or zoom-to-cursor
            // drifts by the DPI/resolution-scale factor and the sprite appears to
            // slide instead of scaling in place.
            {
                const ImVec2 mp = ImGui::GetIO().MousePos;
                const glm::vec2 cursorLogical{
                    (mp.x - m_Scene.ImageMin.x) / std::max(m_Scene.ImageSize.x, 1.0F) *
                        m_Scene.LogicalW,
                    (mp.y - m_Scene.ImageMin.y) / std::max(m_Scene.ImageSize.y, 1.0F) *
                        m_Scene.LogicalH};
                camera.SetCursorViewportPos(cursorLogical);
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
                Draw2DSpritePlaceholders(
                    drawList, camera.Ortho2D(), m_Scene.ImageMin, editWorld, scene);
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

            // Floating in-viewport overlays. Drawn after the drag-drop target so
            // the scene image stays the drop target; their hover feeds
            // m_OverlayHovered so overlay clicks don't also pick scene entities.
            DrawTransformRail(m_Scene);
            if (camera.ProjectionMode() == ViewportProjectionMode::Perspective)
            {
                DrawOrientationGizmo(m_Scene, camera);
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

    // Ignore gizmo hover / picking while the cursor is over a floating overlay
    // control (transform rail, orientation gizmo). Ongoing drags and button-up
    // still fall through so a drag started on the scene finishes cleanly.
    if (m_OverlayHovered && !m_GizmoDragging &&
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION))
    {
        return;
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
