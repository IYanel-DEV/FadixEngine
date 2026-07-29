#include "editor/imgui/panels/ViewportPanel.hpp"

#include "editor/assets/AssetBrowserController.hpp"
#include "editor/camera/CameraSelection.hpp"
#include "editor/command/EntityCommands.hpp"
#include "editor/imgui/EditorIcons.hpp"
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
        m_Scene.Renderer->SetEditorVisualsEnabled(!playing);
        m_Scene.Renderer->SetCollisionVisualizationEnabled(!playing && m_ShowCollisionShapes);
        m_Scene.Renderer->SetViewportDebugView(m_DebugView);
        m_Scene.Renderer->SetSimDelta(deltaSeconds);
        camera.Camera().SetViewportSize({m_Scene.LogicalW, m_Scene.LogicalH});
        m_Scene.Renderer->SetCamera(camera.Camera().View(), camera.Camera().Projection());
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
    EditorUiState& ui, GizmoSystem& gizmo, const EditorPlayMode playMode)
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
    static_cast<void>(history);
    static_cast<void>(editWorld);
    static_cast<void>(document);
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
            DrawSceneToolbar(ui, gizmo, playMode);
            MeasureView(m_Scene, dpi);
            DrawViewImage(m_Scene, "Resize Scene View", true);
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
                        if (asset->AssetType == "Mesh")
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
    // Always forward events so KEY_UP / BUTTON_UP clear sticky WASD / look capture.
    // Hover=false when WantTextInput; EditorCameraInput still accepts releases / active nav.
    static_cast<void>(camera.Input().HandleEvent(
        event, sceneHovered && edit && !ImGui::GetIO().WantTextInput));

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

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        if (m_GizmoDragging)
        {
            GizmoSnap snap;
            snap.Enabled = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
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
