#include "render/ViewportRendererFactory.hpp"

#include "editor/camera/FrustumRenderer.hpp"
#include "engine/animation/Skeleton.hpp"
#include "engine/render/RenderGraph.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "render/AssetResourceCache.hpp"
#include "render/ParticleRenderer.hpp"
#include "render/ForwardPlusLights.hpp"
#include "render/ViewportGeometry.hpp"
#include "render/ViewportGizmo.hpp"
#include "render/ViewportMath.hpp"
#include "render/ViewportUniforms.hpp"
#include "render/ParticleSystem.hpp"
#include "render/PostProcessing.h"
#include "render/ShadowCascades.hpp"
#include "render/ShaderCompiler.hpp"
#include "render/Ssao.hpp"
#include "assets/GltfMeshCache.hpp"
#include "engine/Uuid.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/rhi/Sampler.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/assets/TextureAsset.hpp"
#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/RenderTarget.hpp"
#include "engine/rhi/Shader.hpp"
#include "engine/rhi/Texture.hpp"
#include "engine/scene/IWorld.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/AnimationRuntime.hpp"
#include "runtime/Components.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec4.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3dcompiler.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fadix
{
namespace
{
// Primitive mesh generation + the shared Vertex layout live in
// ViewportGeometry.{hpp,cpp}. Bridge the names back into this anonymous
// namespace so every existing call site (Vertex{...}, AppendCube(...), ...)
// resolves unchanged.
using viewport_geometry::Vertex;
using viewport_geometry::AppendCapsule;
using viewport_geometry::AppendCone;
using viewport_geometry::AppendCube;
using viewport_geometry::AppendCylinder;
using viewport_geometry::AppendPlanePrimitive;
using viewport_geometry::AppendQuad;
using viewport_geometry::AppendSphere;
using viewport_geometry::AppendTorus;
using viewport_geometry::MeshRange;

// Transform-gizmo part assembly now lives in ViewportGizmo.{hpp,cpp}.
using viewport_gizmo::GizmoPart;

// Pure viewport math helpers now live in ViewportMath.{hpp,cpp}; bridge them
// back so existing unqualified call sites resolve unchanged.
using viewport_math::ComposeMatrix;
using viewport_math::Halton23;
using viewport_math::HaltonDigit;
using viewport_math::IntersectAabb;
using viewport_math::ModelMatrix;
using viewport_math::RotationBetween;
using viewport_math::SmoothStep;
using viewport_math::ViewForwardWorld;

// GPU cbuffer layouts now live in ViewportUniforms.hpp; bridge them (and the
// two light-count constants they share with the lighting loop) back here.
using viewport_uniforms::ApplyLdrTarget;
using viewport_uniforms::ApplySceneMrt;
using viewport_uniforms::BoneUniform;
using viewport_uniforms::FragmentUniform;
using viewport_uniforms::LightSet;
using viewport_uniforms::MaxPointLights;
using viewport_uniforms::MaxSpotLights;
using viewport_uniforms::NormalMatrixFor;
using viewport_uniforms::ShadowFragmentUniform;
using viewport_uniforms::ShadowVertexUniform;
using viewport_uniforms::SkyUniform;
using viewport_uniforms::VertexUniform;

// Device-safe upper bound on a single directional shadow-map dimension.
constexpr int kMaxShadowResolution = 4096;
// World-space pull-back added behind each cascade slice so tall casters standing
// above the slice still render into that cascade's depth map.
constexpr float kShadowCasterMargin = 50.0F;

struct Pickable
{
    Uuid Id;
    glm::vec3 Minimum;
    glm::vec3 Maximum;
};

// AxisColorX/Y/Z moved with BuildGizmoParts into ViewportGizmo.cpp.
constexpr glm::vec3 HighlightColor{1.0F, 0.86F, 0.30F};
constexpr glm::vec3 SunWarmColor{1.0F, 0.72F, 0.25F};
constexpr glm::vec3 MoonCoolColor{0.55F, 0.70F, 1.0F};

constexpr int LineBufferMaxVertices = 16384;
constexpr float kCameraCutEyeThreshold = 0.75F;
constexpr float kCameraCutViewAngleDegrees = 20.0F;
constexpr std::uint32_t kHaltonCycle = 16;

[[nodiscard]] bool ProbeGpuTimestampSupport(const rhi::Device& device) noexcept
{
    // SDL GPU exposes fences but no timestamp-query API; probe once and skip.
    static_cast<void>(rhi::sdl::AsSdlDevice(const_cast<rhi::Device&>(device)));
    return false;
}
}

class RhiViewportRenderer final : public ViewportRenderer
{
public:
    explicit RhiViewportRenderer(rhi::Device& device, IAssetDatabase& database)
        : m_Device(device), m_Cache(std::make_unique<AssetResourceCache>(device, database)),
          m_Database(&database), m_Uploader(device)
    {
        CreateGeometry();
        CreatePipelines();
        m_PostProcessor = std::make_unique<PostProcessor>();
        if (!m_PostProcessor->Initialize(m_Device))
        {
            Report("Post-processing initialization failed; effects disabled");
            m_PostProcessor.reset();
        }
        else
        {
            Report("Post-processing initialized");
        }
        {
            rhi::SamplerDesc desc;
            desc.MinFilter = rhi::FilterMode::Linear;
            desc.MagFilter = rhi::FilterMode::Linear;
            desc.MipFilter = rhi::FilterMode::Linear;
            auto samplerResult = m_Device.CreateSampler(desc);
            if (!samplerResult)
            {
                throw std::runtime_error("Viewport default sampler creation failed");
            }
            m_DefaultSampler = std::move(samplerResult).Value();
        }
        {
            rhi::SamplerDesc desc;
            desc.MinFilter = rhi::FilterMode::Nearest;
            desc.MagFilter = rhi::FilterMode::Nearest;
            desc.MipFilter = rhi::FilterMode::Nearest;
            desc.AddressU = rhi::WrapMode::Clamp;
            desc.AddressV = rhi::WrapMode::Clamp;
            desc.AddressW = rhi::WrapMode::Clamp;
            auto samplerResult = m_Device.CreateSampler(desc);
            if (!samplerResult)
            {
                m_ShadowFailed = true;
                Report("Viewport shadow sampler creation failed: " + samplerResult.ErrorMessage());
            }
            else
            {
                m_ShadowSampler = std::move(samplerResult).Value();
            }
        }
        try
        {
            m_ParticleRenderer = std::make_unique<ParticleRenderer>(m_Device);
            m_ParticleRenderer->Initialize();
        }
        catch (const std::exception& error)
        {
            m_ParticleRenderer.reset();
            Report(std::string{"Particle renderer init failed: "} + error.what());
        }
        m_GizmoParts.reserve(16);
        m_GpuTimestampsSupported = ProbeGpuTimestampSupport(m_Device);
    }

    void RecordDrawCall() noexcept { ++m_FrameDrawCalls; }

    [[nodiscard]] RenderDiagnostics Diagnostics() const noexcept override
    {
        RenderDiagnostics diag = m_Diagnostics;
        diag.InternalWidth = static_cast<int>(m_Extent.Width);
        diag.InternalHeight = static_cast<int>(m_Extent.Height);
        return diag;
    }

    void SetLog(std::function<void(std::string)> log) { m_Log = std::move(log); }

    void Resize(const rhi::Extent2D extent) override
    {
        if (extent.Width == m_Extent.Width && extent.Height == m_Extent.Height)
        {
            return;
        }
        m_Extent = extent;
        m_Target.reset();
        if (extent.Width == 0 || extent.Height == 0)
        {
            return;
        }
        const rhi::TextureDesc color{
            extent,
            rhi::Format::R16G16B16A16Float,
            rhi::TextureUsage::RenderTarget,
            1,
            "ViewportColorHdr"};
        rhi::TextureDesc depth = color;
        depth.PixelFormat = rhi::Format::D24UnormS8Uint;
        depth.Usage = rhi::TextureUsage::DepthStencil;
        depth.DebugName = "ViewportDepth";
        m_DepthSampleable = false;
        {
            const std::array<rhi::Format, 3> depthFormats = ssao::DepthFormatPreference();
            std::array<bool, 3> supported{};
            for (std::size_t i = 0; i < depthFormats.size(); ++i)
            {
                supported[i] = m_Device.SupportsTextureFormat(depthFormats[i], true);
            }
            if (const std::optional<rhi::Format> sampleable =
                    ssao::SelectSampleableDepthFormat(supported))
            {
                depth.PixelFormat = *sampleable;
                depth.AllowSampling = true;
                m_DepthSampleable = true;
            }
        }
        rhi::TextureDesc velocity = color;
        velocity.PixelFormat = rhi::Format::R16G16Float;
        velocity.DebugName = "ViewportVelocity";
        auto result = m_Device.CreateRenderTarget(color, &depth, &velocity);
        if (!result)
        {
            m_Extent = {};
            Report("Viewport render target creation failed: " + result.ErrorMessage());
            return;
        }
        m_Target = std::move(result).Value();
        if (m_PostProcessor)
        {
            m_PostProcessor->Resize(
                m_Device, static_cast<int>(extent.Width), static_cast<int>(extent.Height));
        }
        ResetTemporalHistory();
    }

    void ResetTemporalHistory() noexcept override
    {
        if (m_PostProcessor != nullptr)
        {
            m_PostProcessor->ResetHistory();
        }
        m_TaaFrameIndex = 1;
        m_PrevView = m_View;
        m_HasPrevView = false;
        m_HasPrevViewProjection = false;
    }

    void SetCamera(const glm::mat4& view, const glm::mat4& projection) override
    {
        if (m_HasPrevView)
        {
            const glm::vec3 prevEye = glm::vec3{glm::inverse(m_PrevView)[3]};
            const glm::vec3 nextEye = glm::vec3{glm::inverse(view)[3]};
            const glm::vec3 prevForward = ViewForwardWorld(m_PrevView);
            const glm::vec3 nextForward = ViewForwardWorld(view);
            const float viewAngle = glm::degrees(std::acos(glm::clamp(
                glm::dot(prevForward, nextForward), -1.0F, 1.0F)));
            if (glm::length(nextEye - prevEye) > kCameraCutEyeThreshold ||
                viewAngle > kCameraCutViewAngleDegrees)
            {
                ResetTemporalHistory();
            }
        }
        m_View = view;
        m_BaseProjection = projection;
        m_Projection = projection;
        if (!m_HasPrevViewProjection)
        {
            m_PrevViewProjection = projection * view;
            m_HasPrevViewProjection = true;
        }
    }

    void SetSelection(const std::optional<Uuid> selection) override
    {
        m_Selection = selection;
    }

    void SetGizmo(const GizmoVisual& gizmo) override
    {
        m_Gizmo = gizmo;
    }

    void SetEditorVisualsEnabled(const bool enabled) noexcept override
    {
        m_EditorVisuals = enabled;
    }

    void SetCollisionVisualizationEnabled(const bool enabled) noexcept override
    {
        m_CollisionVisualization = enabled;
    }

    void SetPostDebugView(const PostDebugView view) noexcept override
    {
        m_PostDebugView = view;
    }

    void SetShadowCascadeDebug(const bool enabled) noexcept override
    {
        m_ViewportDebugView =
            enabled ? ViewportDebugView::CascadeColors : ViewportDebugView::None;
    }

    void SetViewportDebugView(const ViewportDebugView view) noexcept override
    {
        m_ViewportDebugView = view;
    }

    [[nodiscard]] int ShadowCascadeCount() const noexcept override
    {
        return (m_EditorVisuals && m_ViewportDebugView == ViewportDebugView::CascadeColors)
            ? m_FrameActiveCascades
            : 0;
    }

    [[nodiscard]] rhi::Texture* ShadowDebugTexture(const int cascade) const noexcept override
    {
        if (!m_EditorVisuals || m_ViewportDebugView != ViewportDebugView::CascadeColors ||
            cascade < 0 || cascade >= m_FrameActiveCascades || cascade >= shadow::kMaxCascades)
        {
            return nullptr;
        }
        const auto slot = static_cast<std::size_t>(cascade);
        return m_ShadowCascades[slot] != nullptr ? m_ShadowCascades[slot]->ColorTexture() : nullptr;
    }

    void SetSimDelta(const float dt) noexcept override
    {
        m_SimDelta = dt;
    }

    void UpdateAnimations(IWorld& world, const float dt) override
    {
        // Entity-transform clips first so a combined entity is posed before its skin.
        UpdateTransformAnimations(world.Registry(), dt);
        UpdateWorldAnimations(
            world.Registry(),
            [this](const AssetHandle& handle) -> const GltfMeshAsset* {
                return m_GltfMeshCache != nullptr ? m_GltfMeshCache->Get(handle) : nullptr;
            },
            dt);
    }

    void SetAssetDatabase(IAssetDatabase& database) override
    {
        m_Database = &database;
        m_GltfMeshFailed.clear();
        if (m_Cache != nullptr)
        {
            m_Cache->SetDatabase(database);
        }
    }

    void SetQualitySettings(const RenderQualitySettings& quality) override
    {
        if (quality.DefaultAntiAlias != m_Quality.DefaultAntiAlias ||
            quality.MaxBloomPasses != m_MaxBloomPasses)
        {
            ResetTemporalHistory();
        }
        m_Quality = quality;
        m_MaxBloomPasses = quality.MaxBloomPasses;
    }

    void SetAntiAliasOverride(const AntiAliasMode mode) noexcept override
    {
        if (mode == m_AntiAliasOverride)
        {
            return;
        }
        if (mode == AntiAliasMode::Taa || m_AntiAliasOverride == AntiAliasMode::Taa ||
            m_ResolvedAntiAliasLast == AntiAliasMode::Taa)
        {
            ResetTemporalHistory();
        }
        m_AntiAliasOverride = mode;
    }

    void SetGltfMeshCache(GltfMeshCache* cache) override
    {
        m_GltfMeshCache = cache;
        m_GltfMeshFailed.clear();
    }

    void SetMeshPreview(std::optional<MeshPreviewVisual> preview) override
    {
        m_MeshPreview = std::move(preview);
    }

    void DrawWorld(const IWorld& world) override
    {
        if (m_Target == nullptr)
        {
            return;
        }
        try
        {
            DrawWorldChecked(world);
        }
        catch (const std::exception& error)
        {
            Report(std::string{"Viewport draw failed: "} + error.what());
        }
    }

    [[nodiscard]] rhi::Texture* ColorTarget() const noexcept override
    {
        // The final ImGui image must be a tone-mapped LDR texture. The scene
        // target is R16G16B16A16 HDR; presenting it directly shows as a clipped
        // white frame. So we only expose the post output, and only when mandatory
        // tone mapping is ready. Otherwise return nullptr and let the viewport
        // panel draw its error message instead of raw HDR.
        if (m_PostProcessor != nullptr && m_PostProcessor->CoreTonemapReady())
        {
            return m_PostProcessor->OutputTexture();
        }
        return nullptr;
    }

    void SetOrthographic(const bool enabled) noexcept { m_Orthographic = enabled; }

    void RequestPick(const PickRequest request)
    {
        m_PickResult.reset();
        if (m_Extent.Width == 0 || m_Extent.Height == 0)
        {
            return;
        }
        const float x = 2.0F * (static_cast<float>(request.X) + 0.5F) /
                            static_cast<float>(m_Extent.Width) -
                        1.0F;
        const float y = 1.0F - 2.0F * (static_cast<float>(request.Y) + 0.5F) /
                                   static_cast<float>(m_Extent.Height);
        const glm::mat4 inverse = glm::inverse(m_Projection * m_View);
        glm::vec4 nearPoint = inverse * glm::vec4{x, y, 0.0F, 1.0F};
        glm::vec4 farPoint = inverse * glm::vec4{x, y, 1.0F, 1.0F};
        nearPoint /= nearPoint.w;
        farPoint /= farPoint.w;
        const glm::vec3 origin{nearPoint};
        const glm::vec3 direction = glm::normalize(glm::vec3{farPoint - nearPoint});

        float nearest = std::numeric_limits<float>::max();
        for (const Pickable& pickable : m_Pickables)
        {
            float distance = 0.0F;
            if (IntersectAabb(origin, direction, pickable.Minimum, pickable.Maximum, distance) &&
                distance < nearest)
            {
                nearest = distance;
                m_PickResult = PickResult{pickable.Id, distance};
            }
        }
    }

    [[nodiscard]] std::optional<PickResult> PollPick()
    {
        return std::exchange(m_PickResult, std::nullopt);
    }

private:
    void Report(std::string message)
    {
        if (m_Log)
        {
            m_Log(message);
        }
        else
        {
            std::cerr << "[Fadix] " << message << '\n';
        }
    }

    // True when this entity must not be rendered in the current view (editor
    // camera vs game/play view). Entities without a VisibilityComponent keep
    // the historical always-visible behavior.
    [[nodiscard]] bool HiddenInCurrentView(
        const entt::registry& registry, const entt::entity entity) const
    {
        const VisibilityComponent* visibility = registry.try_get<VisibilityComponent>(entity);
        if (visibility == nullptr)
        {
            return false;
        }
        return m_EditorVisuals ? !visibility->VisibleInEditor : !visibility->VisibleInGame;
    }

    // Deterministic environment choice: enabled Primary first, then highest
    // Priority, then the smallest UUID string as a stable tie-breaker.
    [[nodiscard]] const EnvironmentComponent* ResolveEnvironment(const IWorld& world) const
    {
        const EnvironmentComponent* best = nullptr;
        bool bestPrimary = false;
        int bestPriority = 0;
        std::string bestKey;
        for (const auto [entity, environment, uuid] :
             world.Registry().view<const EnvironmentComponent, const UuidComponent>().each())
        {
            const std::string key = uuid.Id.ToString();
            const bool better = best == nullptr ||
                (environment.Primary != bestPrimary ? environment.Primary
                    : environment.Priority != bestPriority ? environment.Priority > bestPriority
                                                           : key < bestKey);
            if (better)
            {
                best = &environment;
                bestPrimary = environment.Primary;
                bestPriority = environment.Priority;
                bestKey = key;
            }
            static_cast<void>(entity);
        }
        return best;
    }

    void ApplyEnvironment(const IWorld& world)
    {
        const EnvironmentComponent* environment = ResolveEnvironment(world);
        if (environment == nullptr)
        {
            // No environment entity: keep the historical defaults.
            m_FrameAmbientSky = {0.45F, 0.55F, 0.70F, 0.55F};
            m_FrameAmbientGround = {0.28F, 0.25F, 0.22F, 0.0F};
            m_FrameEnvParams = {1.0F, 0.0F, 0.0F, 0.0F};
            m_FrameFogColorDensity = {0.60F, 0.66F, 0.75F, 0.0F};
            m_FrameFogRange = {10.0F, 250.0F, 0.0F, 0.0F};
            m_FrameSkyZenith = {0.13F, 0.27F, 0.52F, 0.0F};
            m_FrameSkyHorizon = {0.63F, 0.71F, 0.82F, 0.0F};
            m_FrameSkyGround = {0.20F, 0.19F, 0.18F, 0.0F};
            m_FramePostSettings = PostProcessSettings{};
            AntiAliasMode resolved = m_Quality.DefaultAntiAlias;
            if (m_AntiAliasOverride != AntiAliasMode::Auto)
            {
                resolved = m_AntiAliasOverride;
            }
            if (resolved != m_ResolvedAntiAliasLast)
            {
                ResetTemporalHistory();
            }
            m_ResolvedAntiAliasLast = resolved;
            m_FramePostSettings.ResolvedAntiAlias = resolved;
            return;
        }
        EnvironmentComponent sanitized = *environment;
        Sanitize(sanitized);
        // 0.55 keeps the default EnvironmentComponent visually identical to
        // the built-in ambient before this component existed.
        m_FrameAmbientSky = glm::vec4{sanitized.AmbientColor, sanitized.AmbientIntensity * 0.55F};
        m_FrameAmbientGround = glm::vec4{sanitized.GroundColor, 0.0F};
        m_FrameEnvParams = {sanitized.Exposure, sanitized.FogEnabled ? 1.0F : 0.0F, 0.0F, 0.0F};
        m_FrameFogColorDensity = glm::vec4{sanitized.FogColor, sanitized.FogDensity};
        m_FrameFogRange = {sanitized.FogStart, sanitized.FogEnd, 0.0F, 0.0F};
        m_FrameSkyZenith = glm::vec4{sanitized.SkyZenithColor, 0.0F};
        m_FrameSkyHorizon = glm::vec4{sanitized.SkyHorizonColor, 0.0F};
        m_FrameSkyGround = glm::vec4{sanitized.GroundColor, 0.0F};
        m_FramePostSettings.BloomEnabled = sanitized.BloomEnabled;
        m_FramePostSettings.BloomThreshold = sanitized.BloomThreshold;
        m_FramePostSettings.BloomIntensity = sanitized.BloomIntensity;
        m_FramePostSettings.BloomPasses =
            std::clamp(sanitized.BloomPasses, 1, std::min(8, m_MaxBloomPasses));
        m_FramePostSettings.TonemapEnabled = sanitized.TonemapEnabled;
        m_FramePostSettings.Exposure = sanitized.Exposure;
        m_FramePostSettings.ColorGradingEnabled = sanitized.ColorGradingEnabled;
        m_FramePostSettings.Lift = sanitized.Lift;
        m_FramePostSettings.Gamma = sanitized.Gamma;
        m_FramePostSettings.Gain = sanitized.Gain;
        m_FramePostSettings.FxaaEnabled = sanitized.FxaaEnabled;
        m_FramePostSettings.FxaaStrength = sanitized.FxaaStrength;
        const AntiAliasMode resolved = m_AntiAliasOverride != AntiAliasMode::Auto
            ? m_AntiAliasOverride
            : ResolveAntiAlias(sanitized.AntiAlias, m_Quality.DefaultAntiAlias);
        if (resolved != m_ResolvedAntiAliasLast)
        {
            ResetTemporalHistory();
        }
        m_ResolvedAntiAliasLast = resolved;
        m_FramePostSettings.ResolvedAntiAlias = resolved;
        m_FramePostSettings.TaaHistoryWeight = sanitized.TaaHistoryWeight;
        m_FramePostSettings.TaaSharpness = sanitized.TaaSharpness;
    }

    void ApplyTaaJitter()
    {
        // Jitter the projection ONLY when a temporal resolve will actually consume
        // it this frame. Any missing precondition -> use the base projection and
        // do not advance the jitter index, so a static camera never shakes.
        m_Projection = m_BaseProjection;
        if (m_FramePostSettings.ResolvedAntiAlias != AntiAliasMode::Taa || m_Extent.Width == 0 ||
            m_Extent.Height == 0)
        {
            return;
        }
        if (m_PostProcessor == nullptr || !m_PostProcessor->TaaReady() ||
            m_PostProcessor->OutputTexture() == nullptr)
        {
            return;
        }
        // Temporal resolve runs only when motion vectors exist (m_Target's second
        // color attachment); without them the TAA pass falls back to a plain copy.
        if (m_Target == nullptr || m_Target->ColorTexture1() == nullptr)
        {
            return;
        }
        const glm::vec2 halton = Halton23((m_TaaFrameIndex % kHaltonCycle) + 1);
        const glm::vec2 extent{
            static_cast<float>(m_Extent.Width), static_cast<float>(m_Extent.Height)};
        const glm::vec2 jitter = (halton * 2.0F - 1.0F) / extent;
        // GLM column-major perspective: clip-space x/y bias lives in column 2.
        m_Projection[2][0] += jitter.x;
        m_Projection[2][1] += jitter.y;
        ++m_TaaFrameIndex;
    }

    void BuildLights(const IWorld& world)
    {
        m_FrameLights = LightSet{};
        m_FrameActivePointLights = 0;
        m_FrameActiveSpotLights = 0;
        struct Candidate
        {
            glm::vec3 Position;
            glm::quat Rotation;
            const void* Component;
            float DistanceSq;
            std::string Key;
        };
        const entt::registry& registry = world.Registry();

        std::vector<Candidate> points;
        for (const auto [entity, transform, light, uuid] :
             registry.view<const TransformComponent, const PointLightComponent,
                 const UuidComponent>().each())
        {
            if (!light.Enabled)
            {
                continue;
            }
            const glm::vec3 delta = transform.Position - m_CameraPosition;
            points.push_back({transform.Position, transform.Rotation, &light,
                glm::dot(delta, delta), uuid.Id.ToString()});
            static_cast<void>(entity);
        }
        std::sort(points.begin(), points.end(), [](const Candidate& a, const Candidate& b) {
            return a.DistanceSq != b.DistanceSq ? a.DistanceSq < b.DistanceSq : a.Key < b.Key;
        });
        const auto pointCount = std::min<std::size_t>(points.size(), MaxPointLights);
        for (std::size_t index = 0; index < pointCount; ++index)
        {
            PointLightComponent light = *static_cast<const PointLightComponent*>(points[index].Component);
            Sanitize(light);
            m_FrameLights.PointPositionRange[index] = glm::vec4{points[index].Position, light.Range};
            m_FrameLights.PointColorIntensity[index] = glm::vec4{light.Color, light.Intensity};
            m_FrameLights.PointParams[index] = glm::vec4{light.FalloffExponent, 0.0F, 0.0F, 0.0F};
        }

        // Forward+: upload every enabled point light (closest first, soft-capped)
        // to the storage buffer, not just the eight in the uniform fallback.
        m_FrameTotalPointLights = static_cast<int>(points.size());
        const std::size_t gpuCount =
            std::min<std::size_t>(points.size(), forward_plus::kMaxPointLights);
        m_FramePointLightsGpu.clear();
        m_FramePointLightsGpu.reserve(gpuCount);
        for (std::size_t index = 0; index < gpuCount; ++index)
        {
            PointLightComponent light =
                *static_cast<const PointLightComponent*>(points[index].Component);
            Sanitize(light);
            forward_plus::GpuPointLight gpu;
            gpu.PositionRange = glm::vec4{points[index].Position, light.Range};
            gpu.ColorIntensity = glm::vec4{light.Color, light.Intensity};
            gpu.Params = glm::vec4{light.FalloffExponent, 0.0F, 0.0F, 0.0F};
            m_FramePointLightsGpu.push_back(gpu);
        }

        std::vector<Candidate> spots;
        for (const auto [entity, transform, light, uuid] :
             registry.view<const TransformComponent, const SpotLightComponent,
                 const UuidComponent>().each())
        {
            if (!light.Enabled)
            {
                continue;
            }
            const glm::vec3 delta = transform.Position - m_CameraPosition;
            spots.push_back({transform.Position, transform.Rotation, &light,
                glm::dot(delta, delta), uuid.Id.ToString()});
            static_cast<void>(entity);
        }
        std::sort(spots.begin(), spots.end(), [](const Candidate& a, const Candidate& b) {
            return a.DistanceSq != b.DistanceSq ? a.DistanceSq < b.DistanceSq : a.Key < b.Key;
        });
        const auto spotCount = std::min<std::size_t>(spots.size(), MaxSpotLights);
        for (std::size_t index = 0; index < spotCount; ++index)
        {
            SpotLightComponent light = *static_cast<const SpotLightComponent*>(spots[index].Component);
            Sanitize(light);
            const glm::vec3 direction =
                glm::normalize(spots[index].Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
            m_FrameLights.SpotPositionRange[index] = glm::vec4{spots[index].Position, light.Range};
            m_FrameLights.SpotDirectionFalloff[index] = glm::vec4{direction, light.FalloffExponent};
            m_FrameLights.SpotColorIntensity[index] = glm::vec4{light.Color, light.Intensity};
            m_FrameLights.SpotCone[index] = glm::vec4{
                std::cos(glm::radians(light.InnerConeDegrees)),
                std::cos(glm::radians(light.OuterConeDegrees)), 0.0F, 0.0F};
        }
        m_FrameLights.Counts =
            glm::vec4{static_cast<float>(pointCount), static_cast<float>(spotCount), 0.0F, 0.0F};
        m_FrameTotalSpotLights = static_cast<int>(spots.size());
        // "Active" now means uploaded to the Forward+ buffer (points) / uniform
        // set (spots), not the old hard 8-light cap.
        m_FrameActivePointLights = static_cast<int>(m_FramePointLightsGpu.size());
        m_FrameActiveSpotLights = static_cast<int>(spotCount);

        // Assign uploaded point lights to screen tiles on the CPU.
        // ponytail: single-threaded scatter, no depth-range slices; move to a
        // compute pass or clustered slices if the CPU cull dominates the frame.
        forward_plus::CullView cull;
        cull.View = m_View;
        cull.Proj00 = m_BaseProjection[0][0];
        cull.Proj11 = m_BaseProjection[1][1];
        cull.ScreenWidth = static_cast<int>(m_Extent.Width);
        cull.ScreenHeight = static_cast<int>(m_Extent.Height);
        m_FrameTiles = forward_plus::AssignLightsToTiles(m_FramePointLightsGpu, cull);
        const bool fpReady = m_FrameTiles.TilesX > 0 && m_FrameTiles.TilesY > 0;
        m_FrameForwardPlus = glm::vec4{
            static_cast<float>(forward_plus::kTileSize),
            static_cast<float>(m_FrameTiles.TilesX),
            static_cast<float>(m_FramePointLightsGpu.size()),
            fpReady ? 1.0F : 0.0F};

        // Once-per-second diagnostics when a cap actually clips the scene: the
        // point soft cap or the 8-light spot uniform cap.
        const bool pointsClipped = m_FrameTotalPointLights > static_cast<int>(gpuCount);
        const bool spotsClipped = m_FrameTotalSpotLights > static_cast<int>(spotCount);
        if (pointsClipped || spotsClipped || m_FrameTiles.OverflowCount > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_LastLightCapLog >= std::chrono::seconds(1))
            {
                m_LastLightCapLog = now;
                if (pointsClipped)
                {
                    Report("[Lighting] using " + std::to_string(gpuCount) + "/" +
                        std::to_string(m_FrameTotalPointLights) + " point lights (soft cap)");
                }
                if (spotsClipped)
                {
                    Report("[Lighting] using " + std::to_string(spotCount) + "/" +
                        std::to_string(m_FrameTotalSpotLights) + " spot lights (uniform cap)");
                }
                if (m_FrameTiles.OverflowCount > 0)
                {
                    Report("[Lighting] " + std::to_string(m_FrameTiles.OverflowCount) +
                        " tile light slots dropped (per-tile cap " +
                        std::to_string(forward_plus::kMaxLightsPerTile) + ")");
                }
            }
        }
    }

    // Grows `buffer` to hold at least `bytes` (with headroom) as a GPU storage
    // buffer. Never shrinks, so per-frame churn settles after the scene's peak.
    void EnsureStorageBuffer(
        std::unique_ptr<rhi::Buffer>& buffer,
        std::size_t bytes,
        std::uint32_t stride,
        const char* name)
    {
        bytes = std::max<std::size_t>(bytes, 256);
        if (buffer != nullptr && buffer->Size() >= bytes)
        {
            return;
        }
        const std::size_t capacity = ((bytes + bytes / 2 + stride - 1) / stride) * stride;
        auto result = m_Device.CreateBuffer(
            {capacity, rhi::BufferUsage::Storage, name, stride});
        if (!result)
        {
            Report(std::string{"Forward+ storage buffer alloc failed: "} + result.ErrorMessage());
            return;
        }
        buffer = std::move(result).Value();
    }

    // Records copy passes (before any render pass) that push this frame's light,
    // tile-header and tile-index data into the storage buffers the lit shader
    // reads. Buffers are always non-empty so they stay bindable with zero lights.
    void UploadForwardPlusLights(rhi::CommandList& commands)
    {
        const std::size_t lightBytes =
            std::max<std::size_t>(m_FramePointLightsGpu.size(), 1) * sizeof(forward_plus::GpuPointLight);
        const std::size_t headerBytes =
            std::max<std::size_t>(m_FrameTiles.Headers.size(), 1) * sizeof(forward_plus::TileHeader);
        const std::size_t indexBytes =
            std::max<std::size_t>(m_FrameTiles.Indices.size(), 1) * sizeof(std::uint32_t);
        EnsureStorageBuffer(
            m_LightBuffer,
            lightBytes,
            static_cast<std::uint32_t>(sizeof(forward_plus::GpuPointLight)),
            "ForwardPlusLights");
        EnsureStorageBuffer(
            m_TileHeaderBuffer,
            headerBytes,
            static_cast<std::uint32_t>(sizeof(forward_plus::TileHeader)),
            "ForwardPlusTileHeaders");
        EnsureStorageBuffer(
            m_TileIndexBuffer,
            indexBytes,
            static_cast<std::uint32_t>(sizeof(std::uint32_t)),
            "ForwardPlusTileIndices");
        if (!m_FramePointLightsGpu.empty() && m_LightBuffer != nullptr)
        {
            m_Uploader.Upload(commands, *m_LightBuffer,
                std::as_bytes(std::span{m_FramePointLightsGpu}));
        }
        if (!m_FrameTiles.Headers.empty() && m_TileHeaderBuffer != nullptr)
        {
            m_Uploader.Upload(commands, *m_TileHeaderBuffer,
                std::as_bytes(std::span{m_FrameTiles.Headers}));
        }
        if (!m_FrameTiles.Indices.empty() && m_TileIndexBuffer != nullptr)
        {
            m_Uploader.Upload(commands, *m_TileIndexBuffer,
                std::as_bytes(std::span{m_FrameTiles.Indices}));
        }
    }

    // Binds the Forward+ storage buffers (t8-t10) and the tile-param uniform (b1)
    // for the lit shader. Every lit draw routes through DrawMesh/DrawMeshRaw, so
    // calling this there guarantees the resources the shader declares are bound.
    void BindForwardPlusResources(rhi::CommandList& list)
    {
        if (m_LightBuffer == nullptr || m_TileHeaderBuffer == nullptr ||
            m_TileIndexBuffer == nullptr)
        {
            return;
        }
        std::array<rhi::Buffer*, 3> buffers = {
            m_LightBuffer.get(), m_TileHeaderBuffer.get(), m_TileIndexBuffer.get()};
        list.BindFragmentStorageBuffers(0, buffers);
        list.PushFragmentUniform(1, &m_FrameForwardPlus, sizeof(m_FrameForwardPlus));
    }

    [[nodiscard]] int CountShadowedLights(const IWorld& world, const bool directionalCasts) const
    {
        int count = directionalCasts ? 1 : 0;
        const entt::registry& registry = world.Registry();
        for (const auto [entity, light] : registry.view<const PointLightComponent>().each())
        {
            static_cast<void>(entity);
            if (light.Enabled && light.CastShadows)
            {
                ++count;
            }
        }
        for (const auto [entity, light] : registry.view<const SpotLightComponent>().each())
        {
            static_cast<void>(entity);
            if (light.Enabled && light.CastShadows)
            {
                ++count;
            }
        }
        return count;
    }

    void DrawWorldChecked(const IWorld& world)
    {
        const auto cpuStart = std::chrono::steady_clock::now();
        m_FrameDrawCalls = 0;
        m_FrameVisibleMeshes = 0;
        m_FrameShadowPasses = 0;

        m_Cache->Update();
        glm::vec3 sunDirection = glm::normalize(glm::vec3{-0.45F, -0.85F, -0.30F});
        DirectionalLightComponent sun = MakeSunLight();
        glm::vec3 moonDirection = -sunDirection;
        DirectionalLightComponent moon = MakeMoonLight();
        bool foundSun = false;
        bool foundMoon = false;
        bool foundFallback = false;
        glm::vec3 fallbackDirection = sunDirection;
        DirectionalLightComponent fallback = sun;
        for (const auto [entity, transform, light] :
             world.Registry()
                 .view<const TransformComponent, const DirectionalLightComponent>()
                 .each())
        {
            const glm::vec3 direction =
                glm::normalize(transform.Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
            if (light.CelestialBody == CelestialLightType::Sun && !foundSun)
            {
                sunDirection = direction;
                sun = light;
                foundSun = true;
            }
            else if (light.CelestialBody == CelestialLightType::Moon && !foundMoon)
            {
                moonDirection = direction;
                moon = light;
                foundMoon = true;
            }
            else if (light.CelestialBody == CelestialLightType::None && !foundFallback)
            {
                fallbackDirection = direction;
                fallback = light;
                foundFallback = true;
            }
            static_cast<void>(entity);
        }
        // Existing scenes predate celestial roles. Their first ordinary
        // directional light remains the Sun, and they receive a synthetic Moon.
        if (!foundSun && foundFallback)
        {
            sunDirection = fallbackDirection;
            sun = fallback;
            foundSun = true;
        }
        if (!foundMoon || (moon.LinkOppositeSun && foundSun))
        {
            moonDirection = -sunDirection;
        }

        const float sunElevation = -sunDirection.y;
        const float moonElevation = -moonDirection.y;
        const float sunVisibility = SmoothStep(-0.08F, 0.06F, sunElevation);
        const float moonVisibility = SmoothStep(-0.04F, 0.08F, moonElevation);
        m_FrameDayAmount = SmoothStep(-0.12F, 0.08F, sunElevation);
        m_FrameSunDirection = glm::vec4{sunDirection, sunVisibility};
        m_FrameSunColor = glm::vec4{sun.Color, sun.Intensity};
        m_FrameMoonDirection = glm::vec4{moonDirection, moonVisibility};
        m_FrameMoonColor = glm::vec4{moon.Color, moon.Intensity};

        const bool useMoon = moonVisibility * moon.Intensity > sunVisibility * sun.Intensity;
        const DirectionalLightComponent& active = useMoon ? moon : sun;
        const float activeVisibility = useMoon ? moonVisibility : sunVisibility;
        glm::vec4 lightDirection{useMoon ? moonDirection : sunDirection, 0.0F};
        glm::vec4 lightColor{active.Color, active.Intensity * activeVisibility};
        bool castShadows = false;
        float shadowBias = 0.005F;
        float shadowStrength = 0.7F;
        int shadowResolution = 2048;
        float shadowDistance = 150.0F;
        float cascadeLambda = 0.5F;
        castShadows = active.CastShadows && activeVisibility > 0.01F;
        shadowBias = active.ShadowBias;
        shadowStrength = active.ShadowStrength;
        shadowResolution = active.ShadowMapResolution > 0 ? active.ShadowMapResolution : 2048;
        shadowDistance = active.ShadowDistance;
        cascadeLambda = active.CascadeSplitLambda;
        m_FrameLightDirection = lightDirection;
        m_FrameLightColor = lightColor;
        const glm::mat4 cameraToWorld = glm::inverse(m_View);
        m_CameraPosition = glm::vec3{cameraToWorld[3]};
        // Unjittered camera view direction; cascade selection in the shader uses
        // view-space depth measured along this axis.
        m_FrameCameraForward = -glm::normalize(glm::vec3{cameraToWorld[2]});

        // Directional shadows are skipped entirely when the quality tier disables
        // them or shadow-map creation previously failed.
        const bool shadowsOn =
            castShadows && m_Quality.DirectionalShadowsEnabled && !m_ShadowFailed;
        // Per-cascade resolution: min(light request, quality cap, device-safe max).
        const int qualityCap = std::max(m_Quality.ShadowResolutionCap, 256);
        m_ShadowResolution =
            std::clamp(std::min(shadowResolution, qualityCap), 256, kMaxShadowResolution);
        const int cascadeCount = std::clamp(m_Quality.ShadowCascadeCount, 1, shadow::kMaxCascades);
        const float filterRadius = static_cast<float>(std::max(m_Quality.ShadowFilterRadius, 0));

        m_FrameShadowParams =
            glm::vec4{filterRadius, shadowBias, shadowStrength, shadowsOn ? 1.0F : 0.0F};
        m_FrameCascadeSplits = glm::vec4{0.0F};
        m_FrameCascadeTexel = glm::vec4{0.0F};
        m_FrameCascadeLightSpace = {glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}};
        m_FrameActiveCascades = 0;
        m_FrameShadowDistance = shadowDistance;
        if (shadowsOn)
        {
            const shadow::CascadeSetup setup = shadow::BuildCascades(
                m_BaseProjection,
                m_View,
                glm::vec3{lightDirection},
                cascadeCount,
                m_ShadowResolution,
                shadowDistance,
                cascadeLambda,
                kShadowCasterMargin);
            m_FrameActiveCascades = setup.Count;
            const float texelUv = 1.0F / static_cast<float>(m_ShadowResolution);
            for (int i = 0; i < shadow::kMaxCascades; ++i)
            {
                m_FrameCascadeLightSpace[static_cast<std::size_t>(i)] =
                    setup.LightSpace[static_cast<std::size_t>(i)];
                m_FrameCascadeSplits[i] = setup.SplitFar[static_cast<std::size_t>(i)];
                m_FrameCascadeTexel[i] = texelUv;
            }
        }
        // Only the directional cascades are actually rendered/sampled. Local
        // (spot/point) shadow maps are not wired yet, so counting their opt-in
        // flags here would report a fake active-shadow count. Report the truth.
        m_FrameShadowedLights = shadowsOn ? 1 : 0;

        EnsureGltfMeshes(world);
        BuildPickables(world);
        ApplyEnvironment(world);
        const glm::vec4 nightAmbientSky{0.055F, 0.075F, 0.15F,
            0.14F + 0.10F * moonVisibility};
        const glm::vec4 nightAmbientGround{0.012F, 0.018F, 0.035F, 0.0F};
        m_FrameAmbientSky = glm::mix(nightAmbientSky, m_FrameAmbientSky, m_FrameDayAmount);
        m_FrameAmbientGround =
            glm::mix(nightAmbientGround, m_FrameAmbientGround, m_FrameDayAmount);
        BuildLights(world);
        UpdateFrameDebugUniforms();
        ApplyTaaJitter();
        BuildEditorLines(world);
        SimulateParticles(world);
        if (m_ParticleRenderer)
        {
            m_ParticleRenderer->Prepare(m_Particles.Particles(), m_CameraPosition);
        }

        std::unique_ptr<rhi::CommandList> commands = m_Device.CreateCommandList();
        commands->Begin();

        if (!m_LineVertices.empty())
        {
            const std::size_t count =
                std::min<std::size_t>(m_LineVertices.size(), LineBufferMaxVertices);
            if (count < m_LineVertices.size() && !m_LineOverflowReported)
            {
                m_LineOverflowReported = true;
                Report("Viewport line buffer overflow; editor lines truncated");
            }
            if (!m_Uploader.Upload(
                    *commands,
                    *m_LineBuffer,
                    std::as_bytes(std::span{m_LineVertices.data(), count})))
            {
                Report("Viewport line upload failed");
                m_LineVertices.clear();
            }
        }

        // Push this frame's Forward+ light/tile data before any render pass opens.
        UploadForwardPlusLights(*commands);

        if (m_FrameShadowParams.w > 0.5F)
        {
            DrawShadowPass(*commands, world);
        }

        m_Graph.Clear();
        const bool postActive =
            m_PostProcessor != nullptr && m_PostProcessor->OutputTexture() != nullptr &&
            m_Target != nullptr;
        const bool overlaysAfterPost = postActive && m_EditorVisuals;
        m_Graph.AddPass("BeginScene", [this](rhi::CommandList& list) {
            list.BeginRenderPass(*m_Target);
        });
        m_Graph.AddPass("Sky", [this](rhi::CommandList& list) { DrawSky(list); });
        if (!overlaysAfterPost)
        {
            m_Graph.AddPass("GroundGrid", [this](rhi::CommandList& list) {
                DrawGroundGrid(list);
            });
        }
        m_Graph.AddPass("WorldGeometry", [this, &world](rhi::CommandList& list) {
            DrawWorldGeometry(list, world);
        });
        m_Graph.AddPass("MeshDropPreview", [this](rhi::CommandList& list) {
            DrawMeshPreview(list);
        });
        m_Graph.AddPass("Particles", [this](rhi::CommandList& list) {
            if (m_ParticleRenderer)
            {
                m_ParticleRenderer->Draw(list, m_Projection * m_View, m_PrevViewProjection);
            }
        });
        if (!overlaysAfterPost)
        {
            m_Graph.AddPass("EditorLines", [this](rhi::CommandList& list) { DrawEditorLines(list); });
            m_Graph.AddPass("TransformGizmo", [this](rhi::CommandList& list) { DrawGizmo(list); });
        }
        m_Graph.AddPass("EndScene", [](rhi::CommandList& list) { list.EndRenderPass(); });
        m_Graph.Execute(*commands);
        if (postActive)
        {
            m_FramePostSettings.Velocity = m_Target->ColorTexture1();
            m_FramePostSettings.Depth = m_Target->DepthTexture();
            ApplyPostDebugSettings();
            m_PostProcessor->Execute(*commands, *m_Target->ColorTexture(), m_FramePostSettings);
            m_FramePostSettings.ResetHistory = false;
        }
        if (overlaysAfterPost)
        {
            std::unique_ptr<rhi::RenderTarget> overlayTarget = m_Device.CreateRenderTargetView(
                *m_PostProcessor->OutputTexture(), m_Target->DepthTexture());
            commands->BeginRenderPassLoad(*overlayTarget);
            DrawGroundGridOverlay(*commands);
            DrawEditorLines(*commands, true);
            DrawGizmo(*commands, true);
            commands->EndRenderPass();
        }
        commands->End();
        m_Device.Submit(*commands);
        m_PrevView = m_View;
        m_HasPrevView = true;
        CommitFrameHistory(world);
        FlushBoneHistory();

        const auto cpuEnd = std::chrono::steady_clock::now();
        m_Diagnostics.DrawCalls = m_FrameDrawCalls;
        m_Diagnostics.VisibleMeshes = m_FrameVisibleMeshes;
        m_Diagnostics.ActivePointLights = m_FrameActivePointLights;
        m_Diagnostics.ActiveSpotLights = m_FrameActiveSpotLights;
        m_Diagnostics.TotalPointLights = m_FrameTotalPointLights;
        m_Diagnostics.TotalSpotLights = m_FrameTotalSpotLights;
        m_Diagnostics.ShadowedLights = m_FrameShadowedLights;
        m_Diagnostics.ShadowPasses = m_FrameShadowPasses;
        m_Diagnostics.ActiveCascades = m_FrameActiveCascades;
        m_Diagnostics.ShadowResolution = m_FrameActiveCascades > 0 ? m_ShadowResolution : 0;
        m_Diagnostics.ShadowDistance = m_FrameActiveCascades > 0 ? m_FrameShadowDistance : 0.0F;
        m_Diagnostics.PostPasses =
            postActive && m_PostProcessor != nullptr ? m_PostProcessor->LastPassCount() : 0;
        m_Diagnostics.CpuRenderMs =
            std::chrono::duration<float, std::milli>(cpuEnd - cpuStart).count();
        m_Diagnostics.GpuRenderMs = m_GpuTimestampsSupported ? m_LastGpuRenderMs : -1.0F;
    }

    void CommitFrameHistory(const IWorld& world)
    {
        m_PrevViewProjection = m_Projection * m_View;
        m_HasPrevViewProjection = true;
        m_PrevModels.clear();
        for (const auto [entity, transform] :
             world.Registry().view<const TransformComponent>().each())
        {
            if (const std::optional<Uuid> id = world.GetUuid(entity))
            {
                m_PrevModels[*id] = ModelMatrix(transform);
            }
            static_cast<void>(entity);
        }
        for (auto it = m_PrevBones.begin(); it != m_PrevBones.end();)
        {
            if (m_PrevModels.count(it->first) == 0)
            {
                it = m_PrevBones.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    [[nodiscard]] glm::mat4 PrevModelFor(const Uuid& id, const glm::mat4& current) const
    {
        if (const auto it = m_PrevModels.find(id); it != m_PrevModels.end())
        {
            return it->second;
        }
        return current;
    }

    void SimulateParticles(const IWorld& world)
    {
        std::unordered_set<Uuid> live;
        for (const auto [entity, transform, emitter, uuid] :
             world.Registry()
                 .view<const TransformComponent, const ParticleEmitterComponent,
                     const UuidComponent>()
                 .each())
        {
            static_cast<void>(entity);
            live.insert(uuid.Id);
            ParticlePool::EmitterState& state = m_EmitterStates[uuid.Id];
            m_Particles.Simulate(
                state, emitter, transform.Position, transform.Rotation, m_SimDelta);
        }
        for (auto it = m_EmitterStates.begin(); it != m_EmitterStates.end();)
        {
            if (live.count(it->first) != 0)
            {
                ++it;
                continue;
            }
            m_Particles.KillEmitter(it->second);
            it = m_EmitterStates.erase(it);
        }
    }

    void DrawSky(rhi::CommandList& list)
    {
        list.BindPipeline(*m_SkyPipeline);
        SkyUniform sky;
        const glm::mat4 rotationOnlyView{glm::mat4{glm::mat3{m_View}}};
        const glm::mat4 viewProjection = m_Projection * rotationOnlyView;
        sky.ViewProjection = viewProjection;
        sky.PrevViewProjection = m_PrevViewProjection;
        sky.InverseViewProjection = glm::inverse(viewProjection);
        sky.PrevInverseViewProjection = glm::inverse(m_PrevViewProjection);
        sky.SunDirection = m_FrameSunDirection;
        sky.SunColor = m_FrameSunColor;
        sky.MoonDirection = m_FrameMoonDirection;
        sky.MoonColor = m_FrameMoonColor;
        sky.Params.x = m_Orthographic ? 1.0F : 0.0F;
        sky.Params.y = m_FrameEnvParams.x;
        sky.Params.z = m_FrameDayAmount;
        sky.ZenithColor = m_FrameSkyZenith;
        sky.HorizonColor = m_FrameSkyHorizon;
        sky.GroundColor = m_FrameSkyGround;
        list.PushFragmentUniform(0, &sky, sizeof(sky));
        list.Draw(3);
        RecordDrawCall();
    }

    void BindMeshBuffers(rhi::CommandList& list)
    {
        list.BindVertexBuffer(*m_VertexBuffer);
        list.BindIndexBuffer(*m_IndexBuffer);
    }

    [[nodiscard]] rhi::Texture* CascadeTextureOrWhite(const int index) const
    {
        const auto slot = static_cast<std::size_t>(index);
        if (index >= 0 && index < shadow::kMaxCascades && index < m_FrameActiveCascades &&
            m_ShadowCascades[slot] != nullptr)
        {
            return m_ShadowCascades[slot]->ColorTexture();
        }
        return m_Cache->GetWhiteTexture();
    }

    [[nodiscard]] rhi::Sampler* ShadowSamplerOrDefault() const
    {
        return m_ShadowSampler != nullptr ? m_ShadowSampler.get() : m_DefaultSampler.get();
    }

    [[nodiscard]] bool EnsureCascadeTarget(const int index, const int resolution)
    {
        if (m_ShadowFailed || m_ShadowPipeline == nullptr || index < 0 ||
            index >= shadow::kMaxCascades)
        {
            return false;
        }
        const auto slot = static_cast<std::size_t>(index);
        const std::uint32_t size = static_cast<std::uint32_t>(std::max(resolution, 256));
        if (m_ShadowCascades[slot] != nullptr && m_ShadowCascades[slot]->Extent().Width == size &&
            m_ShadowCascades[slot]->Extent().Height == size)
        {
            return true;
        }
        m_ShadowCascades[slot].reset();
        const rhi::Extent2D extent{size, size};
        char colorName[24];
        char depthName[24];
        std::snprintf(colorName, sizeof(colorName), "ShadowCascade%dColor", index);
        std::snprintf(depthName, sizeof(depthName), "ShadowCascade%dDepth", index);
        const rhi::TextureDesc color{
            extent, rhi::Format::R16G16B16A16Float, rhi::TextureUsage::RenderTarget, 1, colorName};
        rhi::TextureDesc depth = color;
        depth.PixelFormat = rhi::Format::D32Float;
        depth.Usage = rhi::TextureUsage::DepthStencil;
        depth.DebugName = depthName;
        auto result = m_Device.CreateRenderTarget(color, &depth);
        if (!result)
        {
            m_ShadowFailed = true;
            Report("Shadow cascade creation failed: " + result.ErrorMessage());
            return false;
        }
        m_ShadowCascades[slot] = std::move(result).Value();
        return true;
    }

    // Records every opaque / alpha-mask caster into the currently bound shadow
    // render pass, transforming with the supplied cascade light-space matrix.
    void RenderShadowCasters(
        rhi::CommandList& list, const IWorld& world, const glm::mat4& lightSpace)
    {
        BindMeshBuffers(list);
        // shadow_depth PS declares t0/s0 for alpha-mask; SDL requires the slot
        // bound every draw.
        {
            rhi::Texture* white = m_Cache->GetWhiteTexture();
            rhi::Sampler* sampler = m_DefaultSampler.get();
            std::array<rhi::Texture*, 1> textures = {white};
            std::array<rhi::Sampler*, 1> samplers = {sampler};
            list.BindFragmentSamplers(0, textures, samplers);
            const ShadowFragmentUniform fragment{};
            list.PushFragmentUniform(0, &fragment, sizeof(fragment));
        }
        PushIdentityBones(list);

        const auto drawIndexed = [&](const std::uint32_t firstIndex, const std::uint32_t indexCount,
                                     const glm::mat4& model) {
            const ShadowVertexUniform vertex{lightSpace, model, glm::vec4{0.0F}};
            list.PushVertexUniform(0, &vertex, sizeof(vertex));
            list.DrawIndexed(indexCount, firstIndex);
            RecordDrawCall();
        };

        for (const auto [entity, transform, mesh] :
             world.Registry().view<const TransformComponent, const MeshComponent>().each())
        {
            if (HiddenInCurrentView(world.Registry(), entity))
            {
                continue;
            }
            if (mesh.Material.IsValid())
            {
                const MaterialAsset matAsset = m_Cache->GetMaterial(mesh.Material);
                if (matAsset.AlphaMode == AlphaMode::Blend)
                {
                    continue;
                }
            }

            if (mesh.ImportedMesh.IsValid() && m_GltfMeshCache != nullptr)
            {
                const GltfMeshAsset* gltf = m_GltfMeshCache->Get(mesh.ImportedMesh);
                if (gltf != nullptr && gltf->IsValid())
                {
                    list.BindVertexBuffer(*gltf->VertexBuffer);
                    list.BindIndexBuffer(*gltf->IndexBuffer);
                    const glm::mat4 model = ModelMatrix(transform);
                    for (const GltfPrimitiveRange& prim : gltf->Primitives)
                    {
                        drawIndexed(prim.FirstIndex, prim.IndexCount, model);
                    }
                    BindMeshBuffers(list);
                }
                continue;
            }

            const MeshRange& range = RangeForKind(mesh.Kind);
            drawIndexed(
                range.FirstIndex,
                range.IndexCount,
                ModelMatrix(transform) * KindLocalMatrix(mesh.Kind));
        }
    }

    void DrawShadowPass(rhi::CommandList& list, const IWorld& world)
    {
        m_FrameShadowPasses = 0;
        for (int cascade = 0; cascade < m_FrameActiveCascades; ++cascade)
        {
            if (!EnsureCascadeTarget(cascade, m_ShadowResolution))
            {
                // Allocation failed: disable shadows for the whole frame rather
                // than sampling half-built cascades.
                m_FrameShadowParams.w = 0.0F;
                m_FrameActiveCascades = 0;
                m_FrameShadowPasses = 0;
                return;
            }
            list.BeginRenderPass(
                *m_ShadowCascades[static_cast<std::size_t>(cascade)], 1.0F, 1.0F, 1.0F, 1.0F);
            list.BindPipeline(*m_ShadowPipeline);
            RenderShadowCasters(
                list, world, m_FrameCascadeLightSpace[static_cast<std::size_t>(cascade)]);
            list.EndRenderPass();
            ++m_FrameShadowPasses;
        }
    }

    void BindDefaultLitFragmentSamplers(rhi::CommandList& list)
    {
        rhi::Sampler* sampler = m_DefaultSampler.get();
        rhi::Sampler* shadow = ShadowSamplerOrDefault();
        std::array<rhi::Texture*, 8> textures = {
            m_Cache->GetWhiteTexture(),
            m_Cache->GetNormalFallbackTexture(),
            m_Cache->GetMetallicRoughnessFallbackTexture(),
            m_Cache->GetBlackTexture(),
            CascadeTextureOrWhite(0),
            CascadeTextureOrWhite(1),
            CascadeTextureOrWhite(2),
            CascadeTextureOrWhite(3)};
        std::array<rhi::Sampler*, 8> samplers = {
            sampler, sampler, sampler, sampler, shadow, shadow, shadow, shadow};
        list.BindFragmentSamplers(0, textures, samplers);
    }

    [[nodiscard]] std::span<const glm::mat4> ResolveSkinJointMatrices(
        const GltfMeshAsset& gltf,
        const SkeletonComponent* skeleton) const
    {
        if (skeleton != nullptr && skeleton->HasSkeleton && !skeleton->JointMatrices.empty())
        {
            return skeleton->JointMatrices;
        }
        if (gltf.HasSkeleton)
        {
            m_BindPoseScratch = gltf.Skeleton.GetJointMatrices();
            return m_BindPoseScratch;
        }
        return {};
    }

    void PushIdentityBones(rhi::CommandList& list)
    {
        BoneUniform bones{};
        for (glm::mat4& bone : bones.Bones)
        {
            bone = glm::mat4{1.0F};
        }
        list.PushVertexUniform(1, &bones, sizeof(bones));
        list.PushVertexUniform(2, &bones, sizeof(bones));
    }

    void PushSkinBones(
        rhi::CommandList& list,
        const std::span<const glm::mat4> current,
        const std::optional<Uuid>& entityId)
    {
        BoneUniform bones{};
        BoneUniform prevBones{};
        for (glm::mat4& bone : bones.Bones)
        {
            bone = glm::mat4{1.0F};
        }
        prevBones = bones;
        const std::size_t count =
            std::min(current.size(), static_cast<std::size_t>(kMaxSkinJoints));
        for (std::size_t i = 0; i < count; ++i)
        {
            bones.Bones[i] = current[i];
            prevBones.Bones[i] = current[i];
        }
        if (entityId)
        {
            if (const auto it = m_PrevBones.find(*entityId); it != m_PrevBones.end())
            {
                const std::size_t prevCount =
                    std::min(it->second.size(), static_cast<std::size_t>(kMaxSkinJoints));
                for (std::size_t i = 0; i < prevCount; ++i)
                {
                    prevBones.Bones[i] = it->second[i];
                }
            }
            m_FrameBoneSnapshots[*entityId].assign(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(count));
        }
        list.PushVertexUniform(1, &bones, sizeof(bones));
        list.PushVertexUniform(2, &prevBones, sizeof(prevBones));
    }

    void FlushBoneHistory()
    {
        for (const auto& [id, matrices] : m_FrameBoneSnapshots)
        {
            m_PrevBones[id] = matrices;
        }
        m_FrameBoneSnapshots.clear();
    }

    [[nodiscard]] VertexUniform MakeVertexUniform(
        const glm::mat4& model,
        const glm::mat4& prevModel,
        const glm::vec4& skinParams) const
    {
        return VertexUniform{
            m_Projection * m_View,
            m_PrevViewProjection,
            model,
            prevModel,
            skinParams,
            NormalMatrixFor(model)};
    }

    [[nodiscard]] FragmentUniform WithFrameDebug(const FragmentUniform& fragment) const
    {
        FragmentUniform result = fragment;
        result.DebugParams = m_FrameDebugParams;
        result.DebugSplits = m_FrameDebugSplits;
        return result;
    }

    void DrawMesh(
        rhi::CommandList& list,
        const MeshRange& range,
        const glm::mat4& model,
        const FragmentUniform& fragment,
        const glm::mat4& prevModel = glm::mat4{1.0F},
        const glm::vec4& skinParams = glm::vec4{0.0F})
    {
        const VertexUniform vertex = MakeVertexUniform(model, prevModel, skinParams);
        const FragmentUniform lit = WithFrameDebug(fragment);
        list.PushVertexUniform(0, &vertex, sizeof(vertex));
        PushIdentityBones(list);
        list.PushFragmentUniform(0, &lit, sizeof(lit));
        BindForwardPlusResources(list);
        list.DrawIndexed(range.IndexCount, range.FirstIndex);
        RecordDrawCall();
    }

    void DrawMeshRaw(
        rhi::CommandList& list,
        const std::uint32_t firstIndex,
        const std::uint32_t indexCount,
        const glm::mat4& model,
        const FragmentUniform& fragment,
        const glm::mat4& prevModel = glm::mat4{1.0F},
        const glm::vec4& skinParams = glm::vec4{0.0F},
        const std::span<const glm::mat4> skinBones = {},
        const std::optional<Uuid>& entityId = std::nullopt)
    {
        const VertexUniform vertex = MakeVertexUniform(model, prevModel, skinParams);
        list.PushVertexUniform(0, &vertex, sizeof(vertex));
        if (skinParams.x > 0.5F && !skinBones.empty())
        {
            PushSkinBones(list, skinBones, entityId);
        }
        else
        {
            PushIdentityBones(list);
        }
        const FragmentUniform lit = WithFrameDebug(fragment);
        list.PushFragmentUniform(0, &lit, sizeof(lit));
        BindForwardPlusResources(list);
        list.DrawIndexed(indexCount, firstIndex);
        RecordDrawCall();
    }

    void EnsureGltfMesh(const AssetHandle handle)
    {
        if (!handle.IsValid() || m_GltfMeshCache == nullptr || m_Database == nullptr ||
            m_GltfMeshCache->Get(handle) != nullptr || m_GltfMeshFailed.count(handle) != 0)
        {
            return;
        }
        const AssetMetadata* meta = m_Database->Meta(handle);
        if (meta == nullptr)
        {
            m_GltfMeshFailed.insert(handle);
            return;
        }
        const std::filesystem::path& path =
            !meta->SourcePath.empty() ? meta->SourcePath : meta->ImportedPath;
        if (m_GltfMeshCache->Load(
                handle, path.string(), [this](const std::string& message) { Report(message); }) ==
            nullptr)
        {
            m_GltfMeshFailed.insert(handle);
        }
    }

    // Uploads glTF meshes referenced by the world before the render pass opens.
    // Failed handles are remembered so a broken asset is not re-parsed per frame.
    void EnsureGltfMeshes(const IWorld& world)
    {
        for (const auto [entity, mesh] :
             world.Registry().view<const MeshComponent>().each())
        {
            static_cast<void>(entity);
            EnsureGltfMesh(mesh.ImportedMesh);
        }
        if (m_MeshPreview)
        {
            EnsureGltfMesh(m_MeshPreview->Mesh);
        }
    }

    void DrawMeshPreview(rhi::CommandList& list)
    {
        if (!m_EditorVisuals || !m_MeshPreview || m_GltfMeshCache == nullptr)
        {
            return;
        }
        const GltfMeshAsset* gltf = m_GltfMeshCache->Get(m_MeshPreview->Mesh);
        if (gltf == nullptr || !gltf->IsValid())
        {
            return;
        }

        glm::vec3 position = m_MeshPreview->Position;
        position.y -= gltf->BoundingBoxMin.y * m_MeshPreview->Scale.y;
        const glm::mat4 model = glm::translate(glm::mat4{1.0F}, position) *
            glm::mat4_cast(m_MeshPreview->Rotation) *
            glm::scale(glm::mat4{1.0F}, m_MeshPreview->Scale);
        FragmentUniform fragment{};
        fragment.BaseColor = glm::vec4{0.10F, 0.72F, 1.0F, 0.38F};

        list.BindPipeline(*m_UnlitDepthPipeline);
        list.BindVertexBuffer(*gltf->VertexBuffer);
        list.BindIndexBuffer(*gltf->IndexBuffer);
        for (const GltfPrimitiveRange& primitive : gltf->Primitives)
        {
            DrawMeshRaw(
                list, primitive.FirstIndex, primitive.IndexCount, model, fragment, model);
        }
        BindMeshBuffers(list);
    }

    [[nodiscard]] ViewportDebugView EffectiveDebugView() const noexcept
    {
        return m_EditorVisuals ? m_ViewportDebugView : ViewportDebugView::None;
    }

    void UpdateFrameDebugUniforms()
    {
        m_FrameDebugParams = glm::vec4{0.0F};
        m_FrameDebugSplits = glm::vec4{0.0F};
        const ViewportDebugView view = EffectiveDebugView();
        switch (view)
        {
        case ViewportDebugView::UnlitBaseColor:
            m_FrameDebugParams.x = 1.0F;
            break;
        case ViewportDebugView::WorldNormals:
            m_FrameDebugParams.x = 2.0F;
            break;
        case ViewportDebugView::Roughness:
            m_FrameDebugParams.x = 3.0F;
            break;
        case ViewportDebugView::Metallic:
            m_FrameDebugParams.x = 4.0F;
            break;
        case ViewportDebugView::Occlusion:
            m_FrameDebugParams.x = 5.0F;
            break;
        case ViewportDebugView::Depth:
            m_FrameDebugParams.x = 6.0F;
            break;
        case ViewportDebugView::CascadeColors:
        {
            // Report the real cascade splits the shader uses this frame so the
            // debug overlay/diagnostics match the shaded cascade boundaries.
            m_FrameDebugParams.x = 7.0F;
            m_FrameDebugParams.y = static_cast<float>(m_FrameActiveCascades);
            m_FrameDebugParams.z = m_FrameCascadeSplits.x;
            m_FrameDebugParams.w = m_FrameCascadeSplits.y;
            m_FrameDebugSplits.x = m_FrameCascadeSplits.z;
            m_FrameDebugSplits.y = m_FrameCascadeSplits.w;
            m_FrameDebugSplits.z = m_FrameShadowDistance;
            break;
        }
        case ViewportDebugView::LightTiles:
            m_FrameDebugParams.x = 8.0F;
            break;
        default:
            break;
        }
    }

    void ApplyPostDebugSettings()
    {
        const ViewportDebugView view = EffectiveDebugView();
        m_FramePostSettings.DebugView = PostDebugView::None;
        m_FramePostSettings.AoDebug = false;
        if (view == ViewportDebugView::MotionVectors)
        {
            m_FramePostSettings.DebugView = PostDebugView::MotionVectors;
        }
        else if (view == ViewportDebugView::Ao)
        {
            if (m_DepthSampleable)
            {
                m_FramePostSettings.AoDebug = true;
                m_FramePostSettings.AoProjection = m_Projection;
                m_FramePostSettings.AoInvProjection = glm::inverse(m_Projection);
            }
            else if (!m_AoDebugWarned)
            {
                m_AoDebugWarned = true;
                Report("AO debug requires a sampleable depth target");
            }
        }
    }

    [[nodiscard]] FragmentUniform MakeLitUniform(
        const glm::vec3 baseColor, const float metallic, const float roughness) const
    {
        FragmentUniform fragment;
        fragment.BaseColor = glm::vec4{baseColor, 1.0F};
        fragment.LightDirection = m_FrameLightDirection;
        fragment.LightColor = m_FrameLightColor;
        fragment.Material = glm::vec4{metallic, roughness, 0.0F, m_Orthographic ? 1.0F : 0.0F};
        fragment.CameraPosition = glm::vec4{m_CameraPosition, 1.0F};
        fragment.AmbientSky = m_FrameAmbientSky;
        fragment.AmbientGround = m_FrameAmbientGround;
        fragment.EnvParams = m_FrameEnvParams;
        fragment.FogColorDensity = m_FrameFogColorDensity;
        fragment.FogRange = m_FrameFogRange;
        fragment.Lights = m_FrameLights;
        fragment.ShadowParams = m_FrameShadowParams;
        fragment.CascadeSplits = m_FrameCascadeSplits;
        fragment.CascadeTexel = m_FrameCascadeTexel;
        fragment.CascadeCount = glm::vec4{static_cast<float>(m_FrameActiveCascades), 0.0F, 0.0F, 0.0F};
        fragment.CameraForward = glm::vec4{m_FrameCameraForward, 0.0F};
        fragment.CascadeLightSpace = m_FrameCascadeLightSpace;
        return fragment;
    }

    void DrawGroundGrid(rhi::CommandList& list)
    {
        // The reference grid is editor tooling, not scene content; hide it in
        // play mode and game-camera preview like the other editor visuals.
        if (!m_EditorVisuals)
        {
            return;
        }
        list.BindPipeline(*m_GridPipeline);
        BindMeshBuffers(list);
        BindDefaultLitFragmentSamplers(list);
        FragmentUniform fragment = MakeLitUniform({0.23F, 0.24F, 0.26F}, 0.0F, 0.95F);
        fragment.Material.z = 1.0F;
        // The reference grid is a neutral editor aid: it must not receive scene
        // shadows (objects above must not paint a dark patch onto it). Disabling
        // the shadow term here leaves real scene meshes/planes shadowed normally.
        fragment.ShadowParams.w = 0.0F;
        // Alpha below 1 switches the grid shader to transparent lines only.
        // With culling disabled, the same grid is readable from above and below.
        fragment.BaseColor.a = 0.0F;
        DrawMesh(list, m_Plane, glm::mat4{1.0F}, fragment, glm::mat4{1.0F});
    }

    void DrawGroundGridOverlay(rhi::CommandList& list)
    {
        if (!m_EditorVisuals)
        {
            return;
        }
        list.BindPipeline(*m_GridOverlayLdrPipeline);
        BindMeshBuffers(list);
        BindDefaultLitFragmentSamplers(list);
        FragmentUniform fragment = MakeLitUniform({0.23F, 0.24F, 0.26F}, 0.0F, 0.95F);
        fragment.Material.z = 1.0F;
        // Neutral editor aid: never receives scene shadows (see DrawGroundGrid).
        fragment.ShadowParams.w = 0.0F;
        // Alpha below 1 switches the grid shader to its lines-only output.
        fragment.BaseColor.a = 0.0F;
        const float surfaceOffset = m_CameraPosition.y >= 0.0F ? 0.01F : -0.01F;
        DrawMesh(list,
            m_Plane,
            glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, surfaceOffset, 0.0F}),
            fragment,
            glm::mat4{1.0F});
    }

    [[nodiscard]] const MeshRange& RangeForKind(const MeshKind kind) const noexcept
    {
        switch (kind)
        {
        case MeshKind::Sphere: return m_Sphere;
        case MeshKind::Plane: return m_PlanePrimitive;
        case MeshKind::Cylinder: return m_Cylinder;
        case MeshKind::Capsule: return m_Capsule;
        case MeshKind::Cube: break;
        }
        return m_Cube;
    }

    // The shared cylinder mesh spans y 0..1; primitives are authored centered.
    [[nodiscard]] static glm::mat4 KindLocalMatrix(const MeshKind kind)
    {
        if (kind == MeshKind::Cylinder)
        {
            return glm::translate(glm::mat4{1.0F}, glm::vec3{0.0F, -0.5F, 0.0F});
        }
        return glm::mat4{1.0F};
    }

    // Local-space picking half extents matching each primitive's actual shape.
    [[nodiscard]] static glm::vec3 KindHalfExtents(const MeshKind kind)
    {
        switch (kind)
        {
        case MeshKind::Plane: return {0.5F, 0.02F, 0.5F};
        case MeshKind::Capsule: return {0.5F, 1.0F, 0.5F};
        case MeshKind::Cube:
        case MeshKind::Sphere:
        case MeshKind::Cylinder: break;
        }
        return {0.5F, 0.5F, 0.5F};
    }

    void DrawWorldGeometry(rhi::CommandList& list, const IWorld& world)
    {
        list.BindPipeline(*m_MeshPipeline);
        BindMeshBuffers(list);
        BindDefaultLitFragmentSamplers(list);

        // We sort opaque meshes front-to-back to minimize overdraw.
        // We render transparent meshes back-to-front.
        struct DrawCmd
        {
            entt::entity Entity{entt::null};
            const TransformComponent* Transform;
            const MeshComponent* Mesh;
            std::optional<Uuid> EntityId;
            float Distance;
        };

        std::vector<DrawCmd> opaque;
        std::vector<DrawCmd> transparent;

        const auto view = world.Registry().view<const TransformComponent>();
        for (const entt::entity entity : view)
        {
            if (HiddenInCurrentView(world.Registry(), entity)) continue;
            
            const TransformComponent& transform = view.get<const TransformComponent>(entity);
            const MeshComponent* mesh = world.Registry().try_get<MeshComponent>(entity);
            const std::optional<Uuid> id = world.GetUuid(entity);

            if (mesh != nullptr)
            {
                float dist = glm::length(transform.Position - m_CameraPosition);
                // check if material is transparent
                bool isTransparent = false;
                if (mesh->Material.IsValid())
                {
                    MaterialAsset matAsset = m_Cache->GetMaterial(mesh->Material);
                    if (matAsset.AlphaMode == AlphaMode::Blend)
                    {
                        isTransparent = true;
                    }
                }
                
                if (isTransparent)
                {
                    transparent.push_back({entity, &transform, mesh, id, dist});
                }
                else
                {
                    opaque.push_back({entity, &transform, mesh, id, dist});
                }
            }
        }

        std::sort(opaque.begin(), opaque.end(), [](const DrawCmd& a, const DrawCmd& b) {
            return a.Distance < b.Distance;
        });

        std::sort(transparent.begin(), transparent.end(), [](const DrawCmd& a, const DrawCmd& b) {
            return a.Distance > b.Distance;
        });

        m_FrameVisibleMeshes =
            static_cast<int>(opaque.size() + transparent.size());

        auto drawMeshes = [&](const std::vector<DrawCmd>& cmds, const bool transparentPass) {
            for (const auto& cmd : cmds)
            {
                const TransformComponent& transform = *cmd.Transform;
                const MeshComponent* mesh = cmd.Mesh;
                const glm::mat4 model = ModelMatrix(transform);
                const glm::mat4 prevModel =
                    cmd.EntityId ? PrevModelFor(*cmd.EntityId, model) : model;

                if (mesh->ImportedMesh.IsValid() && m_GltfMeshCache != nullptr)
                {
                    const GltfMeshAsset* gltf = m_GltfMeshCache->Get(mesh->ImportedMesh);
                    if (gltf != nullptr && gltf->IsValid())
                    {
                        FragmentUniform gltfFrag =
                            MakeLitUniform(mesh->BaseColor, mesh->Metallic, mesh->Roughness);
                        if (mesh->Material.IsValid())
                        {
                            const MaterialAsset matAsset = m_Cache->GetMaterial(mesh->Material);
                            gltfFrag = MakeLitUniform(
                                glm::vec3{matAsset.BaseColor}, matAsset.Metallic, matAsset.Roughness);
                            gltfFrag.BaseColor = matAsset.BaseColor;
                            gltfFrag.EmissiveColorIntensity =
                                glm::vec4{matAsset.EmissiveColor, matAsset.EmissiveIntensity};
                        }
                        list.BindPipeline(*m_MeshPipeline);
                        BindDefaultLitFragmentSamplers(list);
                        list.BindVertexBuffer(*gltf->VertexBuffer);
                        list.BindIndexBuffer(*gltf->IndexBuffer);
                        const SkeletonComponent* skeleton = cmd.Entity != entt::null
                            ? world.Registry().try_get<SkeletonComponent>(cmd.Entity)
                            : nullptr;
                        const std::span<const glm::mat4> skinBones =
                            ResolveSkinJointMatrices(*gltf, skeleton);
                        const bool skinned = gltf->HasSkeleton && !skinBones.empty();
                        const glm::vec4 skinParams{skinned ? 1.0F : 0.0F, 0.0F, 0.0F, 0.0F};
                        for (const GltfPrimitiveRange& prim : gltf->Primitives)
                        {
                            DrawMeshRaw(
                                list,
                                prim.FirstIndex,
                                prim.IndexCount,
                                model,
                                gltfFrag,
                                prevModel,
                                skinParams,
                                skinBones,
                                cmd.EntityId);
                        }
                        // Restore the shared procedural buffers for the next mesh.
                        BindMeshBuffers(list);
                    }
                    // Not yet loaded (or invalid): draw nothing this frame.
                    continue;
                }

                bool doubleSided = false;
                FragmentUniform frag{};
                rhi::Texture* baseColorTex = m_Cache->GetWhiteTexture();
                rhi::Texture* normalTex = m_Cache->GetNormalFallbackTexture();
                rhi::Texture* mrTex = m_Cache->GetMetallicRoughnessFallbackTexture();
                rhi::Texture* emissiveTex = m_Cache->GetBlackTexture();

                if (mesh->Material.IsValid())
                {
                    const MaterialAsset matAsset = m_Cache->GetMaterial(mesh->Material);
                    frag = MakeLitUniform(glm::vec3{matAsset.BaseColor}, matAsset.Metallic, matAsset.Roughness);
                    frag.BaseColor = matAsset.BaseColor;
                    frag.Material.x = matAsset.Metallic;
                    frag.Material.y = matAsset.Roughness;
                    frag.EmissiveColorIntensity =
                        glm::vec4{matAsset.EmissiveColor, matAsset.EmissiveIntensity};
                    frag.UvParams = glm::vec4{matAsset.UVScale, matAsset.UVOffset};
                    frag.AlphaParams.x = matAsset.AlphaMode == AlphaMode::Opaque ? 0.0f
                        : matAsset.AlphaMode == AlphaMode::Mask                    ? 1.0f
                                                                                 : 2.0f;
                    frag.AlphaParams.y = matAsset.AlphaCutoff;
                    doubleSided = matAsset.DoubleSided;

                    TextureImportSettings baseSettings;
                    TextureImportSettings normalSettings;
                    TextureImportSettings mrSettings;
                    TextureImportSettings emissiveSettings;

                    if (matAsset.BaseColorTexture.IsValid())
                    {
                        baseColorTex = m_Cache->GetTexture(matAsset.BaseColorTexture, false);
                        baseSettings = m_Cache->GetTextureSettings(matAsset.BaseColorTexture);
                    }
                    if (matAsset.NormalTexture.IsValid())
                    {
                        normalTex = m_Cache->GetTexture(matAsset.NormalTexture, true);
                        normalSettings = m_Cache->GetTextureSettings(matAsset.NormalTexture);
                    }
                    if (matAsset.MetallicRoughnessTexture.IsValid())
                    {
                        mrTex = m_Cache->GetTexture(matAsset.MetallicRoughnessTexture, false);
                        mrSettings = m_Cache->GetTextureSettings(matAsset.MetallicRoughnessTexture);
                    }
                    if (matAsset.EmissiveTexture.IsValid())
                    {
                        emissiveTex = m_Cache->GetTexture(matAsset.EmissiveTexture, false);
                        emissiveSettings = m_Cache->GetTextureSettings(matAsset.EmissiveTexture);
                    }

                    const bool isTransparent = frag.AlphaParams.x >= 1.5f;
                    if (transparentPass != isTransparent)
                    {
                        continue;
                    }

                    list.BindPipeline(isTransparent ? *m_TransparentMeshPipeline
                        : doubleSided                      ? *m_DoubleSidedMeshPipeline
                                                           : *m_MeshPipeline);

                    rhi::Sampler* baseSampler =
                        m_Cache->GetSampler(baseSettings);
                    rhi::Sampler* normalSampler =
                        m_Cache->GetSampler(normalSettings);
                    rhi::Sampler* mrSampler = m_Cache->GetSampler(mrSettings);
                    rhi::Sampler* emissiveSampler =
                        m_Cache->GetSampler(emissiveSettings);
                    if (baseSampler == nullptr)
                    {
                        baseSampler = m_DefaultSampler.get();
                    }
                    if (normalSampler == nullptr)
                    {
                        normalSampler = m_DefaultSampler.get();
                    }
                    if (mrSampler == nullptr)
                    {
                        mrSampler = m_DefaultSampler.get();
                    }
                    if (emissiveSampler == nullptr)
                    {
                        emissiveSampler = m_DefaultSampler.get();
                    }

                    rhi::Sampler* shadowSamplerBind = ShadowSamplerOrDefault();
                    std::array<rhi::Texture*, 8> textures = {
                        baseColorTex, normalTex, mrTex, emissiveTex,
                        CascadeTextureOrWhite(0), CascadeTextureOrWhite(1),
                        CascadeTextureOrWhite(2), CascadeTextureOrWhite(3)};
                    std::array<rhi::Sampler*, 8> samplers = {
                        baseSampler, normalSampler, mrSampler, emissiveSampler,
                        shadowSamplerBind, shadowSamplerBind, shadowSamplerBind, shadowSamplerBind};
                    list.BindFragmentSamplers(0, textures, samplers);

                    DrawMesh(
                        list,
                        RangeForKind(mesh->Kind),
                        model * KindLocalMatrix(mesh->Kind),
                        frag,
                        prevModel * KindLocalMatrix(mesh->Kind));
                    continue;
                }
                else
                {
                    frag = MakeLitUniform(mesh->BaseColor, mesh->Metallic, mesh->Roughness);
                    frag.UvParams = glm::vec4{1.0f, 1.0f, 0.0f, 0.0f};
                }

                const bool isTransparent = frag.AlphaParams.x >= 1.5f;
                if (transparentPass != isTransparent)
                {
                    continue;
                }

                list.BindPipeline(isTransparent ? *m_TransparentMeshPipeline
                    : doubleSided                      ? *m_DoubleSidedMeshPipeline
                                                       : *m_MeshPipeline);
                BindDefaultLitFragmentSamplers(list);

                DrawMesh(
                    list,
                    RangeForKind(mesh->Kind),
                    model * KindLocalMatrix(mesh->Kind),
                    frag,
                    prevModel * KindLocalMatrix(mesh->Kind));
            }
        };

        drawMeshes(opaque, false);
        drawMeshes(transparent, true);

        // Tool proxies must be drawn after scene meshes. Their first pass has
        // depth disabled, so drawing them earlier lets later geometry erase it.
        if (m_EditorVisuals)
        {
            for (const entt::entity entity : view)
            {
                if (HiddenInCurrentView(world.Registry(), entity) ||
                    world.Registry().all_of<MeshComponent>(entity))
                {
                    continue;
                }
                const TransformComponent& transform = view.get<const TransformComponent>(entity);
                const std::optional<Uuid> id = world.GetUuid(entity);
                const bool selected = id && m_Selection && *id == *m_Selection;
                if (const CameraComponent* camera =
                        world.Registry().try_get<CameraComponent>(entity))
                {
                    DrawCameraModel(list, transform, camera, selected);
                }
                else if (const DirectionalLightComponent* directional =
                             world.Registry().try_get<DirectionalLightComponent>(entity))
                {
                    DrawSunModel(list, transform, *directional, selected);
                }
                else if (const PointLightComponent* point =
                             world.Registry().try_get<PointLightComponent>(entity))
                {
                    DrawPointLightModel(list, transform, *point, selected);
                }
                else if (const SpotLightComponent* spot =
                             world.Registry().try_get<SpotLightComponent>(entity))
                {
                    DrawSpotLightModel(list, transform, *spot, selected);
                }
            }
        }
    }

    void DrawCameraModel(
        rhi::CommandList& list,
        const TransformComponent& transform,
        const CameraComponent* camera,
        const bool selected)
    {
        const glm::vec3 bodyColor =
            selected ? glm::vec3{0.45F, 0.75F, 1.0F} : glm::vec3{0.58F, 0.62F, 0.68F};
        const glm::mat4 base = glm::translate(glm::mat4{1.0F}, transform.Position) *
            glm::mat4_cast(transform.Rotation) *
            glm::scale(glm::mat4{1.0F}, transform.Scale);

        const auto drawParts = [&, this](const float alpha) {
            const auto draw = [&](const MeshRange& mesh, const glm::mat4& model,
                                  const glm::vec3 color) {
                FragmentUniform fragment;
                fragment.BaseColor = glm::vec4{color, alpha};
                DrawMesh(list, mesh, model, fragment, model);
            };
            draw(m_Cube,
                base * glm::scale(glm::mat4{1.0F}, glm::vec3{0.50F, 0.34F, 0.62F}),
                bodyColor);
            const glm::quat lensRotation =
                glm::angleAxis(-glm::half_pi<float>(), glm::vec3{1, 0, 0});
            draw(m_Cylinder,
                base * ComposeMatrix({0.0F, 0.0F, -0.31F},
                           lensRotation,
                           glm::vec3{0.30F, 0.32F, 0.30F}),
                bodyColor * 0.8F);
            draw(m_Cone,
                base * ComposeMatrix({0.0F, 0.0F, -0.63F},
                           glm::angleAxis(glm::half_pi<float>(), glm::vec3{1, 0, 0}),
                           glm::vec3{0.36F, 0.22F, 0.36F}),
                bodyColor * 0.65F);
            draw(m_Cylinder,
                base * ComposeMatrix({-0.13F, 0.26F, 0.02F},
                           glm::angleAxis(glm::half_pi<float>(), glm::vec3{0, 0, 1}),
                           glm::vec3{0.20F, 0.12F, 0.20F}),
                bodyColor * 0.9F);
            draw(m_Cylinder,
                base * ComposeMatrix({0.13F, 0.30F, 0.02F},
                           glm::angleAxis(glm::half_pi<float>(), glm::vec3{0, 0, 1}),
                           glm::vec3{0.24F, 0.12F, 0.24F}),
                bodyColor * 0.9F);
        };
        DrawProxyReadable(list, drawParts);

        // Frustum lines were queued in BuildEditorLines using this component.
        static_cast<void>(camera);
    }

    void DrawSunModel(rhi::CommandList& list, const TransformComponent& transform,
        const DirectionalLightComponent& light, const bool selected)
    {
        const glm::vec3 color = selected ? HighlightColor
            : light.CelestialBody == CelestialLightType::Moon ? MoonCoolColor
                                                               : SunWarmColor;
        const float scale = std::max({transform.Scale.x, transform.Scale.y, transform.Scale.z});
        const glm::vec3 forward =
            glm::normalize(transform.Rotation * glm::vec3{0.0F, 0.0F, -1.0F});

        const glm::quat arrowRotation = RotationBetween(glm::vec3{0, 1, 0}, forward);
        const auto drawParts = [&, this](const float alpha) {
            FragmentUniform fragment;
            fragment.BaseColor = glm::vec4{color, alpha};
            DrawMesh(list,
                m_Sphere,
                ComposeMatrix(
                    transform.Position, glm::quat{1, 0, 0, 0}, glm::vec3{0.46F * scale}),
                fragment);
            DrawMesh(list,
                m_Cylinder,
                ComposeMatrix(transform.Position + forward * 0.30F * scale,
                    arrowRotation,
                    glm::vec3{0.06F * scale, 0.85F * scale, 0.06F * scale}),
                fragment);
            DrawMesh(list,
                m_Cone,
                ComposeMatrix(transform.Position + forward * 1.15F * scale,
                    arrowRotation,
                    glm::vec3{0.18F * scale, 0.30F * scale, 0.18F * scale}),
                fragment);
        };
        DrawProxyReadable(list, drawParts);
    }

    // Draws a set of unlit proxy parts twice: a faint overlay pass without the
    // depth test so the proxy stays readable through geometry, then a
    // depth-tested pass at full strength. Restores the lit mesh pipeline.
    template <typename DrawParts>
    void DrawProxyReadable(rhi::CommandList& list, const DrawParts& drawParts)
    {
        list.BindPipeline(*m_OverlayPipeline);
        BindMeshBuffers(list);
        drawParts(0.28F);
        list.BindPipeline(*m_UnlitDepthPipeline);
        BindMeshBuffers(list);
        drawParts(1.0F);
        list.BindPipeline(*m_MeshPipeline);
        BindMeshBuffers(list);
        BindDefaultLitFragmentSamplers(list);
    }

    void DrawPointLightModel(
        rhi::CommandList& list,
        const TransformComponent& transform,
        const PointLightComponent& light,
        const bool selected)
    {
        const glm::vec3 color = selected
            ? HighlightColor
            : glm::mix(light.Color, glm::vec3{1.0F, 0.9F, 0.55F}, 0.35F) *
                (light.Enabled ? 1.0F : 0.35F);
        const float scale = std::max(
            {std::abs(transform.Scale.x), std::abs(transform.Scale.y), std::abs(transform.Scale.z),
                0.05F});
        const auto drawParts = [&, this](const float alpha) {
            FragmentUniform fragment;
            fragment.BaseColor = glm::vec4{color, alpha};
            // Bulb: glowing sphere over a small cylindrical socket.
            DrawMesh(list,
                m_Sphere,
                ComposeMatrix(transform.Position, glm::quat{1, 0, 0, 0}, glm::vec3{0.34F * scale}),
                fragment);
            FragmentUniform socket;
            socket.BaseColor = glm::vec4{color * 0.45F, alpha};
            DrawMesh(list,
                m_Cylinder,
                ComposeMatrix(transform.Position - glm::vec3{0.0F, 0.30F * scale, 0.0F},
                    glm::quat{1, 0, 0, 0},
                    glm::vec3{0.16F * scale, 0.14F * scale, 0.16F * scale}),
                socket);
        };
        DrawProxyReadable(list, drawParts);
    }

    void DrawSpotLightModel(
        rhi::CommandList& list,
        const TransformComponent& transform,
        const SpotLightComponent& light,
        const bool selected)
    {
        const glm::vec3 color = selected
            ? HighlightColor
            : glm::mix(light.Color, glm::vec3{1.0F, 0.9F, 0.55F}, 0.35F) *
                (light.Enabled ? 1.0F : 0.35F);
        const float scale = std::max(
            {std::abs(transform.Scale.x), std::abs(transform.Scale.y), std::abs(transform.Scale.z),
                0.05F});
        const glm::vec3 forward =
            glm::normalize(transform.Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
        // Cone mesh points +Y with the apex at y=1; map +Y onto -forward so the
        // apex lands at the light position and the cone opens along forward.
        const glm::quat coneRotation = RotationBetween(glm::vec3{0, 1, 0}, -forward);
        const float coneHeight = 0.55F * scale;
        const auto drawParts = [&, this](const float alpha) {
            FragmentUniform fragment;
            fragment.BaseColor = glm::vec4{color, alpha};
            DrawMesh(list,
                m_Cone,
                ComposeMatrix(transform.Position + forward * coneHeight,
                    coneRotation,
                    glm::vec3{0.42F * scale, coneHeight, 0.42F * scale}),
                fragment);
            DrawMesh(list,
                m_Sphere,
                ComposeMatrix(transform.Position, glm::quat{1, 0, 0, 0}, glm::vec3{0.20F * scale}),
                fragment);
        };
        DrawProxyReadable(list, drawParts);
    }

    void BuildEditorLines(const IWorld& world)
    {
        m_LineVertices.clear();
        m_CollisionLineFirst = 0;
        m_CollisionLineCount = 0;
        if (!m_EditorVisuals)
        {
            return;
        }
        const auto pushLine =
            [this](const glm::vec3 start, const glm::vec3 end, const glm::vec3 color) {
                m_LineVertices.push_back({start, {0, 1, 0}, {0, 0, 0, 1}, {0, 0}, glm::vec4(color, 1.0F)});
                m_LineVertices.push_back({end, {0, 1, 0}, {0, 0, 0, 1}, {0, 0}, glm::vec4(color, 1.0F)});
            };

        const float aspect = m_Extent.Height > 0
            ? static_cast<float>(m_Extent.Width) / static_cast<float>(m_Extent.Height)
            : 16.0F / 9.0F;

        // 32-segment wire circle in the plane spanned by axisA/axisB.
        const auto pushCircle = [&pushLine](const glm::vec3 center,
                                    const glm::vec3 axisA,
                                    const glm::vec3 axisB,
                                    const float radius,
                                    const glm::vec3 color) {
            constexpr int segments = 32;
            glm::vec3 previous = center + axisA * radius;
            for (int segment = 1; segment <= segments; ++segment)
            {
                const float angle =
                    glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(segments);
                const glm::vec3 next =
                    center + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius;
                pushLine(previous, next, color);
                previous = next;
            }
        };

        for (const auto [entity, transform, camera] :
             world.Registry().view<const TransformComponent, const CameraComponent>().each())
        {
            if (HiddenInCurrentView(world.Registry(), entity))
            {
                continue;
            }
            const std::optional<Uuid> id = world.GetUuid(entity);
            const bool selected = id && m_Selection && *id == *m_Selection;
            const glm::vec3 color =
                selected ? glm::vec3{1.0F, 0.85F, 0.35F} : glm::vec3{0.75F, 0.78F, 0.85F};

            const glm::mat4 cameraWorld = glm::translate(glm::mat4{1.0F}, transform.Position) *
                glm::mat4_cast(transform.Rotation);
            const glm::mat4 view = glm::inverse(cameraWorld);
            const glm::mat4 projection = glm::perspectiveRH_ZO(
                glm::radians(std::clamp(camera.FieldOfView, 1.0F, 179.0F)),
                aspect,
                std::max(camera.NearPlane, 0.001F),
                std::max(camera.FarPlane, camera.NearPlane + 0.01F));
            for (const editor::DebugLine& line :
                 editor::BuildFrustumLines(view, projection, glm::vec4{color, 1.0F}))
            {
                pushLine(line.Start, line.End, color);
            }
        }

        for (const auto [entity, transform, light] :
             world.Registry()
                 .view<const TransformComponent, const DirectionalLightComponent>()
                 .each())
        {
            if (HiddenInCurrentView(world.Registry(), entity))
            {
                continue;
            }
            const std::optional<Uuid> id = world.GetUuid(entity);
            const bool selected = id && m_Selection && *id == *m_Selection;
            const glm::vec3 color = selected ? HighlightColor
                : light.CelestialBody == CelestialLightType::Moon ? MoonCoolColor
                                                                   : SunWarmColor;
            const float scale =
                std::max({transform.Scale.x, transform.Scale.y, transform.Scale.z});
            const glm::vec3 forward = light.CelestialBody == CelestialLightType::Moon &&
                    light.LinkOppositeSun
                ? -glm::vec3{m_FrameSunDirection}
                : glm::normalize(transform.Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
            glm::vec3 side = glm::cross(forward, glm::vec3{0, 1, 0});
            if (glm::dot(side, side) < 1.0e-4F)
            {
                side = glm::vec3{1, 0, 0};
            }
            side = glm::normalize(side);
            const glm::vec3 up = glm::normalize(glm::cross(side, forward));
            for (int ray = 0; ray < 10; ++ray)
            {
                const float angle =
                    glm::two_pi<float>() * static_cast<float>(ray) / 10.0F;
                const glm::vec3 radial = side * std::cos(angle) + up * std::sin(angle);
                pushLine(
                    transform.Position + radial * 0.34F * scale,
                    transform.Position + radial * 0.60F * scale,
                    color);
            }
            static_cast<void>(light);
        }

        // Point lights: three world-axis great circles showing the range.
        for (const auto [entity, transform, light] :
             world.Registry()
                 .view<const TransformComponent, const PointLightComponent>()
                 .each())
        {
            if (HiddenInCurrentView(world.Registry(), entity) || !light.Enabled)
            {
                continue;
            }
            PointLightComponent sanitized = light;
            Sanitize(sanitized);
            const std::optional<Uuid> id = world.GetUuid(entity);
            const bool selected = id && m_Selection && *id == *m_Selection;
            const glm::vec3 color = selected
                ? HighlightColor
                : glm::mix(sanitized.Color, glm::vec3{1.0F, 0.85F, 0.45F}, 0.4F) * 0.85F;
            pushCircle(transform.Position, {1, 0, 0}, {0, 1, 0}, sanitized.Range, color);
            pushCircle(transform.Position, {1, 0, 0}, {0, 0, 1}, sanitized.Range, color);
            pushCircle(transform.Position, {0, 1, 0}, {0, 0, 1}, sanitized.Range, color);
        }

        // Spot lights: wire cone with the rim at the configured range/angle.
        for (const auto [entity, transform, light] :
             world.Registry()
                 .view<const TransformComponent, const SpotLightComponent>()
                 .each())
        {
            if (HiddenInCurrentView(world.Registry(), entity) || !light.Enabled)
            {
                continue;
            }
            SpotLightComponent sanitized = light;
            Sanitize(sanitized);
            const std::optional<Uuid> id = world.GetUuid(entity);
            const bool selected = id && m_Selection && *id == *m_Selection;
            const glm::vec3 color = selected
                ? HighlightColor
                : glm::mix(sanitized.Color, glm::vec3{1.0F, 0.85F, 0.45F}, 0.4F) * 0.85F;
            const glm::vec3 forward =
                glm::normalize(transform.Rotation * glm::vec3{0.0F, 0.0F, -1.0F});
            glm::vec3 side = glm::cross(forward, glm::vec3{0, 1, 0});
            if (glm::dot(side, side) < 1.0e-4F)
            {
                side = glm::vec3{1, 0, 0};
            }
            side = glm::normalize(side);
            const glm::vec3 up = glm::normalize(glm::cross(side, forward));
            const float rimRadius =
                std::tan(glm::radians(sanitized.OuterConeDegrees)) * sanitized.Range;
            const glm::vec3 rimCenter = transform.Position + forward * sanitized.Range;
            pushCircle(rimCenter, side, up, rimRadius, color);
            for (int ray = 0; ray < 8; ++ray)
            {
                const float angle = glm::two_pi<float>() * static_cast<float>(ray) / 8.0F;
                const glm::vec3 rim =
                    rimCenter + (side * std::cos(angle) + up * std::sin(angle)) * rimRadius;
                pushLine(transform.Position, rim, color);
            }
        }

        m_CollisionLineFirst = m_LineVertices.size();
        if (!m_CollisionVisualization)
        {
            return;
        }

        const auto collisionLine = [this](const glm::vec3 start, const glm::vec3 end) {
            constexpr glm::vec4 color{0.20F, 0.78F, 1.0F, 0.78F};
            m_LineVertices.push_back({start, {0, 1, 0}, {0, 0, 0, 1}, {0, 0}, color});
            m_LineVertices.push_back({end, {0, 1, 0}, {0, 0, 0, 1}, {0, 0}, color});
        };
        const auto wireCircle = [&collisionLine](const auto& toWorld,
                                    const glm::vec3 center,
                                    const glm::vec3 axisA,
                                    const glm::vec3 axisB,
                                    const float radius) {
            constexpr int segments = 32;
            glm::vec3 previous = toWorld(center + axisA * radius);
            for (int segment = 1; segment <= segments; ++segment)
            {
                const float angle = glm::two_pi<float>() * static_cast<float>(segment) /
                    static_cast<float>(segments);
                const glm::vec3 next = toWorld(
                    center + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius);
                collisionLine(previous, next);
                previous = next;
            }
        };
        const auto wireBox = [&collisionLine](const auto& toWorld, const glm::vec3 halfExtent) {
            constexpr std::array<std::array<int, 2>, 12> edges{{
                {{0, 1}}, {{1, 3}}, {{3, 2}}, {{2, 0}},
                {{4, 5}}, {{5, 7}}, {{7, 6}}, {{6, 4}},
                {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}}}};
            std::array<glm::vec3, 8> points{};
            for (int index = 0; index < 8; ++index)
            {
                points[static_cast<std::size_t>(index)] = toWorld(glm::vec3{
                    (index & 1) != 0 ? halfExtent.x : -halfExtent.x,
                    (index & 4) != 0 ? halfExtent.y : -halfExtent.y,
                    (index & 2) != 0 ? halfExtent.z : -halfExtent.z});
            }
            for (const auto& edge : edges)
            {
                collisionLine(points[static_cast<std::size_t>(edge[0])],
                    points[static_cast<std::size_t>(edge[1])]);
            }
        };

        for (const auto [entity, transform, body] :
             world.Registry().view<const TransformComponent, const JoltBodyComponent>().each())
        {
            if (HiddenInCurrentView(world.Registry(), entity) ||
                world.Registry().all_of<CharacterControllerComponent>(entity))
            {
                continue;
            }
            const MeshComponent* mesh = world.Registry().try_get<MeshComponent>(entity);
            ColliderShape3D shape = ResolveColliderShape3D(body, mesh);
            const GltfMeshAsset* gltf = shape == ColliderShape3D::Mesh && mesh != nullptr &&
                    mesh->ImportedMesh.IsValid() && m_GltfMeshCache != nullptr
                ? m_GltfMeshCache->Get(mesh->ImportedMesh)
                : nullptr;
            if (shape == ColliderShape3D::Mesh &&
                (gltf == nullptr || gltf->CollisionVertices.empty() || gltf->CollisionIndices.empty()))
            {
                shape = ColliderShape3D::Box;
            }
            glm::vec3 localHalfExtent = body.HalfExtent;
            if (body.AutoFit && mesh != nullptr)
            {
                localHalfExtent = gltf != nullptr
                    ? (gltf->BoundingBoxMax - gltf->BoundingBoxMin) * 0.5F
                    : DefaultColliderHalfExtent(mesh->Kind);
            }
            const glm::vec3 halfExtent = glm::max(
                localHalfExtent * glm::abs(transform.Scale), glm::vec3{0.01F});
            const auto toWorld = [&transform](const glm::vec3 point) {
                return transform.Position + transform.Rotation * point;
            };

            if (shape == ColliderShape3D::Mesh && gltf != nullptr)
            {
                glm::vec3 meshScale = transform.Scale;
                if (!body.AutoFit)
                {
                    const glm::vec3 sourceHalf = glm::max(
                        (gltf->BoundingBoxMax - gltf->BoundingBoxMin) * 0.5F,
                        glm::vec3{0.01F});
                    meshScale *= body.HalfExtent / sourceHalf;
                }
                const std::size_t remaining = LineBufferMaxVertices > m_LineVertices.size()
                    ? LineBufferMaxVertices - m_LineVertices.size()
                    : 0U;
                const std::size_t triangleLimit = remaining / 6U;
                const std::size_t triangles = std::min(
                    gltf->CollisionIndices.size() / 3U, triangleLimit);
                for (std::size_t triangle = 0; triangle < triangles; ++triangle)
                {
                    const std::uint32_t a = gltf->CollisionIndices[triangle * 3U];
                    const std::uint32_t b = gltf->CollisionIndices[triangle * 3U + 1U];
                    const std::uint32_t c = gltf->CollisionIndices[triangle * 3U + 2U];
                    if (a >= gltf->CollisionVertices.size() || b >= gltf->CollisionVertices.size() ||
                        c >= gltf->CollisionVertices.size())
                    {
                        continue;
                    }
                    const glm::vec3 va = toWorld(gltf->CollisionVertices[a] * meshScale);
                    const glm::vec3 vb = toWorld(gltf->CollisionVertices[b] * meshScale);
                    const glm::vec3 vc = toWorld(gltf->CollisionVertices[c] * meshScale);
                    collisionLine(va, vb);
                    collisionLine(vb, vc);
                    collisionLine(vc, va);
                }
                continue;
            }

            if (shape == ColliderShape3D::Sphere)
            {
                const float radius = std::max({halfExtent.x, halfExtent.y, halfExtent.z});
                wireCircle(toWorld, {}, {1, 0, 0}, {0, 1, 0}, radius);
                wireCircle(toWorld, {}, {1, 0, 0}, {0, 0, 1}, radius);
                wireCircle(toWorld, {}, {0, 1, 0}, {0, 0, 1}, radius);
            }
            else if (shape == ColliderShape3D::Cylinder || shape == ColliderShape3D::Capsule)
            {
                const float radius = std::max(halfExtent.x, halfExtent.z);
                const float cylinderHalf = shape == ColliderShape3D::Capsule
                    ? std::max(halfExtent.y - radius, 0.01F)
                    : halfExtent.y;
                wireCircle(toWorld, {0, cylinderHalf, 0}, {1, 0, 0}, {0, 0, 1}, radius);
                wireCircle(toWorld, {0, -cylinderHalf, 0}, {1, 0, 0}, {0, 0, 1}, radius);
                for (const glm::vec3 axis : std::array<glm::vec3, 4>{
                         glm::vec3{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}})
                {
                    collisionLine(toWorld(axis * radius + glm::vec3{0, cylinderHalf, 0}),
                        toWorld(axis * radius - glm::vec3{0, cylinderHalf, 0}));
                }
                if (shape == ColliderShape3D::Capsule)
                {
                    wireCircle(toWorld, {0, cylinderHalf, 0}, {1, 0, 0}, {0, 1, 0}, radius);
                    wireCircle(toWorld, {0, -cylinderHalf, 0}, {0, 0, 1}, {0, 1, 0}, radius);
                }
            }
            else
            {
                wireBox(toWorld, halfExtent);
            }
        }

        for (const auto [entity, transform, authored] :
             world.Registry().view<const TransformComponent,
                 const CharacterControllerComponent>().each())
        {
            if (HiddenInCurrentView(world.Registry(), entity))
            {
                continue;
            }
            CharacterControllerComponent character = authored;
            SanitizeCharacterController(character);
            const float radius = character.Radius;
            const float cylinderHalf =
                std::max(character.Height * 0.5F - radius, 0.01F);
            const auto toWorld = [&transform](const glm::vec3 point) {
                return transform.Position + point;
            };
            wireCircle(toWorld, {0, cylinderHalf, 0}, {1, 0, 0}, {0, 0, 1}, radius);
            wireCircle(toWorld, {0, -cylinderHalf, 0}, {1, 0, 0}, {0, 0, 1}, radius);
            for (const glm::vec3 axis : std::array<glm::vec3, 4>{
                     glm::vec3{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}})
            {
                collisionLine(toWorld(axis * radius + glm::vec3{0, cylinderHalf, 0}),
                    toWorld(axis * radius - glm::vec3{0, cylinderHalf, 0}));
            }
            wireCircle(toWorld, {0, cylinderHalf, 0}, {1, 0, 0}, {0, 1, 0}, radius);
            wireCircle(toWorld, {0, -cylinderHalf, 0}, {0, 0, 1}, {0, 1, 0}, radius);
        }
        m_CollisionLineCount = m_LineVertices.size() - m_CollisionLineFirst;
    }

    void DrawEditorLines(rhi::CommandList& list, const bool ldrOverlay = false)
    {
        if (m_LineVertices.empty())
        {
            return;
        }
        list.BindPipeline(ldrOverlay ? *m_LineLdrPipeline : *m_LinePipeline);
        list.BindVertexBuffer(*m_LineBuffer);
        const VertexUniform vertex = MakeVertexUniform(glm::mat4{1.0F}, glm::mat4{1.0F}, glm::vec4{0.0F});
        FragmentUniform fragment;
        fragment.BaseColor = glm::vec4{1.0F};
        list.PushVertexUniform(0, &vertex, sizeof(vertex));
        PushIdentityBones(list);
        list.PushFragmentUniform(0, &fragment, sizeof(fragment));
        const std::size_t uploaded = std::min<std::size_t>(m_LineVertices.size(), LineBufferMaxVertices);
        const std::size_t normalCount = std::min(m_CollisionLineFirst, uploaded);
        if (normalCount > 0)
        {
            fragment.BaseColor.a = 0.28F;
            list.PushFragmentUniform(0, &fragment, sizeof(fragment));
            list.BindPipeline(
                ldrOverlay ? *m_CollisionLineLdrPipeline : *m_CollisionLinePipeline);
            list.Draw(static_cast<std::uint32_t>(normalCount), 0);
            fragment.BaseColor.a = 1.0F;
            list.PushFragmentUniform(0, &fragment, sizeof(fragment));
            list.BindPipeline(ldrOverlay ? *m_LineLdrPipeline : *m_LinePipeline);
            list.Draw(static_cast<std::uint32_t>(normalCount), 0);
        }
        const std::size_t collisionCount = uploaded > normalCount
            ? std::min(m_CollisionLineCount, uploaded - normalCount)
            : 0U;
        if (collisionCount > 0)
        {
            list.BindPipeline(
                ldrOverlay ? *m_CollisionLineLdrPipeline : *m_CollisionLinePipeline);
            list.Draw(static_cast<std::uint32_t>(collisionCount),
                static_cast<std::uint32_t>(normalCount));
        }
    }

    // Fills m_GizmoParts (reused across frames to avoid per-frame allocation).
    void BuildGizmoParts()
    {
        const viewport_gizmo::GizmoMeshes meshes{
            &m_Torus, &m_Cylinder, &m_Cone, &m_Cube, &m_Quad};
        viewport_gizmo::BuildGizmoParts(
            m_Gizmo,
            m_View,
            m_Projection,
            static_cast<float>(m_Extent.Height),
            meshes,
            m_GizmoParts);
    }

    void DrawGizmo(rhi::CommandList& list, const bool ldrOverlay = false)
    {
        if (!m_Gizmo.Visible || m_Gizmo.Mode == GizmoMode::Select)
        {
            return;
        }
        BuildGizmoParts();
        const std::vector<GizmoPart>& parts = m_GizmoParts;
        if (parts.empty())
        {
            return;
        }

        rhi::Pipeline& overlayPipeline = ldrOverlay ? *m_OverlayLdrPipeline : *m_OverlayPipeline;
        rhi::Pipeline& unlitDepthPipeline =
            ldrOverlay ? *m_UnlitDepthLdrPipeline : *m_UnlitDepthPipeline;

        const auto drawParts = [&](const float alphaScale) {
            for (const GizmoPart& part : parts)
            {
                FragmentUniform fragment;
                fragment.BaseColor = glm::vec4{part.Color, part.Alpha * alphaScale};
                DrawMesh(list, *part.Mesh, part.Model, fragment);
            }
        };

        // Keep occluded portions faintly visible, then draw visible portions at
        // full strength. Do not scale/draw a dark backing mesh: that produces a
        // displaced "shadow" around translate, rotate, and scale handles.
        list.BindPipeline(overlayPipeline);
        BindMeshBuffers(list);
        drawParts(0.32F);
        list.BindPipeline(unlitDepthPipeline);
        BindMeshBuffers(list);
        drawParts(1.0F);
    }

    void CreateGeometry()
    {
        std::vector<Vertex> vertices;
        std::vector<std::uint32_t> indices;
        const auto record = [&indices](MeshRange& range, const auto& append) {
            range.FirstIndex = static_cast<std::uint32_t>(indices.size());
            append();
            range.IndexCount = static_cast<std::uint32_t>(indices.size()) - range.FirstIndex;
        };

        record(m_Cube, [&]() { AppendCube(vertices, indices); });
        record(m_Plane, [&]() {
            const auto planeVertex = static_cast<std::uint32_t>(vertices.size());
            constexpr float ground = 250.0F;
            vertices.insert(vertices.end(), {
                {{-ground, 0, -ground}, {0, 1, 0}, {1, 0, 0, 1}, {-ground, -ground}, {1, 1, 1, 1}},
                {{-ground, 0, ground}, {0, 1, 0}, {1, 0, 0, 1}, {-ground, ground}, {1, 1, 1, 1}},
                {{ground, 0, ground}, {0, 1, 0}, {1, 0, 0, 1}, {ground, ground}, {1, 1, 1, 1}},
                {{ground, 0, -ground}, {0, 1, 0}, {1, 0, 0, 1}, {ground, -ground}, {1, 1, 1, 1}},
            });
            indices.insert(indices.end(), {
                planeVertex, planeVertex + 1, planeVertex + 2,
                planeVertex, planeVertex + 2, planeVertex + 3,
            });
        });
        record(m_Cylinder, [&]() { AppendCylinder(vertices, indices, 18); });
        record(m_Cone, [&]() { AppendCone(vertices, indices, 18); });
        record(m_Sphere, [&]() { AppendSphere(vertices, indices, 20, 12); });
        record(m_Torus, [&]() {
            AppendTorus(vertices, indices, gizmo_layout::RingTubeRadius, 48, 10);
        });
        record(m_Quad, [&]() { AppendQuad(vertices, indices); });
        record(m_PlanePrimitive, [&]() { AppendPlanePrimitive(vertices, indices); });
        record(m_Capsule, [&]() { AppendCapsule(vertices, indices, 20, 8); });

        auto vertexResult = m_Device.CreateBuffer(
            {vertices.size() * sizeof(Vertex), rhi::BufferUsage::Vertex, "ViewportVertices"});
        auto indexResult = m_Device.CreateBuffer(
            {indices.size() * sizeof(std::uint32_t), rhi::BufferUsage::Index, "ViewportIndices"});
        auto lineResult = m_Device.CreateBuffer(
            {static_cast<std::size_t>(LineBufferMaxVertices) * sizeof(Vertex),
                rhi::BufferUsage::Vertex,
                "ViewportLines"});
        if (!vertexResult || !indexResult || !lineResult)
        {
            throw std::runtime_error(!vertexResult ? vertexResult.ErrorMessage()
                    : !indexResult                 ? indexResult.ErrorMessage()
                                                   : lineResult.ErrorMessage());
        }
        m_VertexBuffer = std::move(vertexResult).Value();
        m_IndexBuffer = std::move(indexResult).Value();
        m_LineBuffer = std::move(lineResult).Value();
        m_VertexBuffer->Upload(std::as_bytes(std::span{vertices}));
        m_IndexBuffer->Upload(std::as_bytes(std::span{indices}));
    }

    void CreatePipelines()
    {
        const std::vector<std::byte> source = render::ReadShaderSource("viewport.hlsl");
        const auto makeShader = [&](const char* entry, const char* target, const char* name, std::uint32_t samplers = 0, std::uint32_t uniformBuffers = 1, std::uint32_t storageBuffers = 0) {
            const std::vector<std::byte> code = render::CompileShader(
                source, entry, m_Device.ShaderTarget(target[0] == 'p'), "viewport.hlsl");
            auto result = m_Device.CreateShader({entry, name, samplers, uniformBuffers, storageBuffers}, code);
            if (!result)
            {
                throw std::runtime_error(
                    std::string{"Viewport shader creation failed ("} + name +
                    "): " + result.ErrorMessage());
            }
            return std::move(result).Value();
        };
        m_MeshVertexShader = makeShader("VertexMain", "vs_5_1", "viewport_vertex", 0, 3);
        // Lit PS samples t0-t3 material maps + t4-t7 the four directional
        // cascades, plus 3 Forward+ storage buffers (t8-t10) and a second
        // uniform buffer (b1) for the tile params.
        m_LitFragmentShader = makeShader("FragmentMain", "ps_5_1", "viewport_fragment_lit", 8, 2, 3);
        m_UnlitFragmentShader = makeShader("UnlitFragmentMain", "ps_5_1", "viewport_fragment_unlit");
        m_UnlitLdrFragmentShader =
            makeShader("UnlitLdrFragmentMain", "ps_5_1", "viewport_fragment_unlit_ldr");
        m_SkyVertexShader = makeShader("SkyVertexMain", "vs_5_1", "sky_vertex");
        m_SkyFragmentShader = makeShader("SkyFragmentMain", "ps_5_1", "sky_fragment");

        const auto makePipeline = [&](rhi::PipelineDesc description) {
            ApplySceneMrt(description);
            auto result = m_Device.CreatePipeline(description);
            if (!result)
            {
                throw std::runtime_error(
                    "Viewport pipeline creation failed (" + description.DebugName +
                    "): " + result.ErrorMessage());
            }
            return std::move(result).Value();
        };
        const auto makeLdrPipeline = [&](rhi::PipelineDesc description) {
            ApplyLdrTarget(description);
            auto result = m_Device.CreatePipeline(description);
            if (!result)
            {
                throw std::runtime_error(
                    "Viewport LDR pipeline creation failed (" + description.DebugName +
                    "): " + result.ErrorMessage());
            }
            return std::move(result).Value();
        };

        rhi::PipelineDesc mesh;
        mesh.DebugName = "ViewportLitMesh";
        mesh.VertexShader = m_MeshVertexShader.get();
        mesh.FragmentShader = m_LitFragmentShader.get();
        mesh.VertexBufferLayouts = {{0, sizeof(Vertex)}};
        mesh.VertexAttributes = {
            {0, 0, rhi::VertexElementFormat::Float3, 0},
            {1, 0, rhi::VertexElementFormat::Float3, 12},
            {2, 0, rhi::VertexElementFormat::Float4, 24},
            {3, 0, rhi::VertexElementFormat::Float2, 40},
            {4, 0, rhi::VertexElementFormat::Float4, 48},
            {5, 0, rhi::VertexElementFormat::Float4, 64},
            {6, 0, rhi::VertexElementFormat::Float4, 80},
        };
        m_MeshPipeline = makePipeline(mesh);

        rhi::PipelineDesc doubleSided = mesh;
        doubleSided.DebugName = "ViewportLitMeshDoubleSided";
        doubleSided.Cull = rhi::CullMode::None;
        m_DoubleSidedMeshPipeline = makePipeline(doubleSided);

        rhi::PipelineDesc transparent = mesh;
        transparent.DebugName = "ViewportLitMeshTransparent";
        transparent.AlphaBlend = true;
        transparent.DepthWrite = false;
        transparent.Cull = rhi::CullMode::None;
        m_TransparentMeshPipeline = makePipeline(transparent);

        // The editor grid is transparent linework, visible from either side.
        rhi::PipelineDesc grid = mesh;
        grid.DebugName = "ViewportGroundGrid";
        grid.AlphaBlend = true;
        grid.DepthWrite = false;
        grid.Cull = rhi::CullMode::None;
        m_GridPipeline = makePipeline(grid);

        // Lines-only overlay drawn after post into LDR output.
        rhi::PipelineDesc gridOverlay = grid;
        gridOverlay.DebugName = "ViewportGroundGridOverlay";
        gridOverlay.AlphaBlend = true;
        gridOverlay.DepthWrite = false;
        m_GridOverlayLdrPipeline = makeLdrPipeline(gridOverlay);

        rhi::PipelineDesc unlitDepth = mesh;
        unlitDepth.DebugName = "ViewportUnlitDepth";
        // Must set the unlit fragment explicitly. Leaving it null makes
        // SdlDevice::CreatePipeline fall back to the last-created shaders
        // (sky vertex + sky fragment), which breaks sun proxies and gizmos.
        unlitDepth.FragmentShader = m_UnlitFragmentShader.get();
        unlitDepth.DepthWrite = false;
        unlitDepth.AlphaBlend = true;
        unlitDepth.Cull = rhi::CullMode::None;
        m_UnlitDepthPipeline = makePipeline(unlitDepth);

        rhi::PipelineDesc unlitDepthLdr = unlitDepth;
        unlitDepthLdr.DebugName = "ViewportUnlitDepthLdr";
        unlitDepthLdr.FragmentShader = m_UnlitLdrFragmentShader.get();
        m_UnlitDepthLdrPipeline = makeLdrPipeline(unlitDepthLdr);

        rhi::PipelineDesc overlay = unlitDepth;
        overlay.DebugName = "ViewportOverlay";
        overlay.DepthTest = false;
        m_OverlayPipeline = makePipeline(overlay);

        rhi::PipelineDesc overlayLdr = overlay;
        overlayLdr.DebugName = "ViewportOverlayLdr";
        overlayLdr.FragmentShader = m_UnlitLdrFragmentShader.get();
        m_OverlayLdrPipeline = makeLdrPipeline(overlayLdr);

        rhi::PipelineDesc lines = mesh;
        lines.DebugName = "ViewportLines";
        lines.FragmentShader = m_UnlitFragmentShader.get();
        lines.Topology = rhi::PrimitiveTopology::LineList;
        lines.DepthWrite = false;
        lines.AlphaBlend = true;
        lines.Cull = rhi::CullMode::None;
        m_LinePipeline = makePipeline(lines);

        rhi::PipelineDesc linesLdr = lines;
        linesLdr.DebugName = "ViewportLinesLdr";
        linesLdr.FragmentShader = m_UnlitLdrFragmentShader.get();
        m_LineLdrPipeline = makeLdrPipeline(linesLdr);

        rhi::PipelineDesc collisionLines = lines;
        collisionLines.DebugName = "ViewportCollisionLines";
        collisionLines.DepthTest = false;
        m_CollisionLinePipeline = makePipeline(collisionLines);

        rhi::PipelineDesc collisionLinesLdr = collisionLines;
        collisionLinesLdr.DebugName = "ViewportCollisionLinesLdr";
        collisionLinesLdr.FragmentShader = m_UnlitLdrFragmentShader.get();
        m_CollisionLineLdrPipeline = makeLdrPipeline(collisionLinesLdr);

        rhi::PipelineDesc sky;
        sky.DebugName = "ViewportSky";
        sky.VertexShader = m_SkyVertexShader.get();
        sky.FragmentShader = m_SkyFragmentShader.get();
        sky.DepthTest = false;
        sky.DepthWrite = false;
        sky.Cull = rhi::CullMode::None;
        sky.UseVertexInput = false;
        m_SkyPipeline = makePipeline(sky);

        try
        {
            const std::vector<std::byte> shadowSource = render::ReadShaderSource("shadow_depth.hlsl");
            const auto makeShadowShader =
                [&](const char* entry, const char* target, const char* name, std::uint32_t samplers,
                    std::uint32_t uniformBuffers) {
                    const std::vector<std::byte> code = render::CompileShader(
                        shadowSource,
                        entry,
                        m_Device.ShaderTarget(target[0] == 'p'),
                        "shadow_depth.hlsl");
                    auto result = m_Device.CreateShader({entry, name, samplers, uniformBuffers}, code);
                    if (!result)
                    {
                        throw std::runtime_error(
                            std::string{"Shadow shader creation failed ("} + name +
                            "): " + result.ErrorMessage());
                    }
                    return std::move(result).Value();
                };
            // VS: b0 draw + b1 bones. PS: t0/s0 alpha mask + b0 cutoff params.
            m_ShadowVertexShader =
                makeShadowShader("VertexMain", "vs_5_1", "shadow_depth_vertex", 0, 2);
            m_ShadowFragmentShader =
                makeShadowShader("FragmentMain", "ps_5_1", "shadow_depth_fragment", 1, 1);

            rhi::PipelineDesc shadow = mesh;
            shadow.DebugName = "ViewportShadowDepth";
            shadow.VertexShader = m_ShadowVertexShader.get();
            shadow.FragmentShader = m_ShadowFragmentShader.get();
            shadow.ColorFormat = rhi::Format::R16G16B16A16Float;
            // Shadow PS writes SV_Target0 only — do not inherit lit dual-MRT ColorFormat1.
            shadow.ColorFormat1 = rhi::Format::Unknown;
            shadow.DepthFormat = rhi::Format::D32Float;
            shadow.DepthTest = true;
            shadow.DepthWrite = true;
            auto shadowPipeline = m_Device.CreatePipeline(shadow);
            if (!shadowPipeline)
            {
                throw std::runtime_error(
                    "Shadow pipeline creation failed: " + shadowPipeline.ErrorMessage());
            }
            m_ShadowPipeline = std::move(shadowPipeline).Value();
        }
        catch (const std::exception& error)
        {
            m_ShadowFailed = true;
            m_ShadowPipeline.reset();
            m_ShadowVertexShader.reset();
            m_ShadowFragmentShader.reset();
            Report(std::string{"Directional shadows disabled: "} + error.what());
        }
    }

    void BuildPickables(const IWorld& world)
    {
        m_Pickables.clear();
        const auto view = world.Registry().view<const TransformComponent>();
        for (const entt::entity entity : view)
        {
            const std::optional<Uuid> id = world.GetUuid(entity);
            if (!id)
            {
                continue;
            }
            // Picking only exists in the editor view: unselectable entities and
            // entities hidden from the editor camera must never be pickable.
            if (const VisibilityComponent* visibility =
                    world.Registry().try_get<VisibilityComponent>(entity))
            {
                if (!visibility->Selectable || !visibility->VisibleInEditor)
                {
                    continue;
                }
            }
            const TransformComponent& transform = view.get<const TransformComponent>(entity);
            const MeshComponent* meshComponent = world.Registry().try_get<MeshComponent>(entity);
            const bool mesh = meshComponent != nullptr;
            const bool camera = world.Registry().all_of<CameraComponent>(entity);
            const bool light = world.Registry().all_of<DirectionalLightComponent>(entity);
            const bool pointLight = world.Registry().all_of<PointLightComponent>(entity);
            const bool spotLight = world.Registry().all_of<SpotLightComponent>(entity);
            if (!mesh && !camera && !light && !pointLight && !spotLight)
            {
                continue;
            }
            if (mesh && meshComponent->ImportedMesh.IsValid() && m_GltfMeshCache != nullptr)
            {
                if (const GltfMeshAsset* gltf = m_GltfMeshCache->Get(meshComponent->ImportedMesh);
                    gltf != nullptr && gltf->IsValid())
                {
                    const glm::vec3 scale = glm::abs(transform.Scale);
                    const glm::vec3 a = transform.Position + scale * gltf->BoundingBoxMin;
                    const glm::vec3 b = transform.Position + scale * gltf->BoundingBoxMax;
                    m_Pickables.push_back({*id, glm::min(a, b), glm::max(a, b)});
                    continue;
                }
            }
            glm::vec3 extent = glm::abs(transform.Scale) * 0.5F;
            if (mesh)
            {
                extent = glm::abs(transform.Scale) * KindHalfExtents(meshComponent->Kind);
            }
            else if (camera)
            {
                // Match the drawn camera model so clicks land where pixels are.
                extent = CameraGizmoHalfExtents * glm::abs(transform.Scale);
            }
            else if (light)
            {
                extent = glm::vec3{
                    SunGizmoRadius *
                    std::max({std::abs(transform.Scale.x),
                        std::abs(transform.Scale.y),
                        std::abs(transform.Scale.z)})};
            }
            else if (pointLight || spotLight)
            {
                // Only the proxy model is clickable, never the light's range.
                const float scale = std::max({std::abs(transform.Scale.x),
                    std::abs(transform.Scale.y), std::abs(transform.Scale.z), 0.05F});
                extent = glm::vec3{(pointLight ? 0.40F : 0.55F) * scale};
            }
            m_Pickables.push_back({*id, transform.Position - extent, transform.Position + extent});
        }
    }

    rhi::Device& m_Device;
        std::unique_ptr<AssetResourceCache> m_Cache;
    IAssetDatabase* m_Database{nullptr};
    GltfMeshCache* m_GltfMeshCache{nullptr};
    std::unordered_set<AssetHandle> m_GltfMeshFailed;
    std::unique_ptr<rhi::Sampler> m_DefaultSampler;
    std::unique_ptr<rhi::Sampler> m_ShadowSampler;
    RenderGraph m_Graph;
    rhi::sdl::DynamicBufferUploader m_Uploader;
    rhi::Extent2D m_Extent{};
    glm::mat4 m_View{1.0F};
    glm::mat4 m_BaseProjection{1.0F};
    glm::mat4 m_Projection{1.0F};
    glm::mat4 m_PrevView{1.0F};
    bool m_HasPrevView{false};
    glm::mat4 m_PrevViewProjection{1.0F};
    bool m_HasPrevViewProjection{false};
    std::uint32_t m_TaaFrameIndex{1};
    RenderQualitySettings m_Quality{};
    AntiAliasMode m_AntiAliasOverride{AntiAliasMode::Auto};
    AntiAliasMode m_ResolvedAntiAliasLast{AntiAliasMode::Fxaa};
    std::unordered_map<Uuid, glm::mat4> m_PrevModels;
    std::unordered_map<Uuid, std::vector<glm::mat4>> m_PrevBones;
    std::unordered_map<Uuid, std::vector<glm::mat4>> m_FrameBoneSnapshots;
    mutable std::vector<glm::mat4> m_BindPoseScratch;
    glm::vec3 m_CameraPosition{0.0F};
    glm::vec4 m_FrameLightDirection{-0.45F, -0.85F, -0.30F, 0.0F};
    glm::vec4 m_FrameLightColor{1.0F, 0.96F, 0.88F, 3.0F};
    glm::vec4 m_FrameSunDirection{-0.45F, -0.85F, -0.30F, 1.0F};
    glm::vec4 m_FrameSunColor{1.0F, 0.96F, 0.88F, 3.0F};
    glm::vec4 m_FrameMoonDirection{0.45F, 0.85F, 0.30F, 0.0F};
    glm::vec4 m_FrameMoonColor{0.62F, 0.72F, 1.0F, 0.35F};
    float m_FrameDayAmount{1.0F};
    glm::vec4 m_FrameShadowParams{0.0F};
    glm::vec4 m_FrameCascadeSplits{0.0F};
    glm::vec4 m_FrameCascadeTexel{0.0F};
    std::array<glm::mat4, shadow::kMaxCascades> m_FrameCascadeLightSpace{
        {glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}, glm::mat4{1.0F}}};
    glm::vec3 m_FrameCameraForward{0.0F, 0.0F, -1.0F};
    int m_FrameActiveCascades{0};
    float m_FrameShadowDistance{150.0F};
    int m_ShadowResolution{2048};
    float m_SimDelta{0.0F};
    ParticlePool m_Particles;
    std::unique_ptr<ParticleRenderer> m_ParticleRenderer;
    std::unordered_map<Uuid, ParticlePool::EmitterState> m_EmitterStates;
    bool m_ShadowFailed{false};
    std::unique_ptr<rhi::Buffer> m_VertexBuffer;
    std::unique_ptr<rhi::Buffer> m_IndexBuffer;
    std::unique_ptr<rhi::Buffer> m_LineBuffer;
    // Forward+ per-frame GPU light data (grows on demand, never shrinks).
    std::unique_ptr<rhi::Buffer> m_LightBuffer;
    std::unique_ptr<rhi::Buffer> m_TileHeaderBuffer;
    std::unique_ptr<rhi::Buffer> m_TileIndexBuffer;
    std::unique_ptr<rhi::Shader> m_MeshVertexShader;
    std::unique_ptr<rhi::Shader> m_LitFragmentShader;
    std::unique_ptr<rhi::Shader> m_UnlitFragmentShader;
    std::unique_ptr<rhi::Shader> m_UnlitLdrFragmentShader;
    std::unique_ptr<rhi::Shader> m_SkyVertexShader;
    std::unique_ptr<rhi::Shader> m_SkyFragmentShader;
    std::unique_ptr<rhi::Shader> m_ShadowVertexShader;
    std::unique_ptr<rhi::Shader> m_ShadowFragmentShader;
    std::unique_ptr<rhi::Pipeline> m_MeshPipeline;
    std::unique_ptr<rhi::Pipeline> m_DoubleSidedMeshPipeline;
    std::unique_ptr<rhi::Pipeline> m_TransparentMeshPipeline;
    std::unique_ptr<rhi::Pipeline> m_GridPipeline;
    std::unique_ptr<rhi::Pipeline> m_GridOverlayLdrPipeline;
    std::unique_ptr<rhi::Pipeline> m_UnlitDepthPipeline;
    std::unique_ptr<rhi::Pipeline> m_UnlitDepthLdrPipeline;
    std::unique_ptr<rhi::Pipeline> m_OverlayPipeline;
    std::unique_ptr<rhi::Pipeline> m_OverlayLdrPipeline;
    std::unique_ptr<rhi::Pipeline> m_LinePipeline;
    std::unique_ptr<rhi::Pipeline> m_LineLdrPipeline;
    std::unique_ptr<rhi::Pipeline> m_CollisionLinePipeline;
    std::unique_ptr<rhi::Pipeline> m_CollisionLineLdrPipeline;
    std::unique_ptr<rhi::Pipeline> m_SkyPipeline;
    std::unique_ptr<rhi::Pipeline> m_ShadowPipeline;
    std::unique_ptr<rhi::RenderTarget> m_Target;
    std::array<std::unique_ptr<rhi::RenderTarget>, shadow::kMaxCascades> m_ShadowCascades;
    std::vector<Pickable> m_Pickables;
    std::vector<Vertex> m_LineVertices;
    std::size_t m_CollisionLineFirst{0};
    std::size_t m_CollisionLineCount{0};
    std::vector<GizmoPart> m_GizmoParts;
    std::optional<PickResult> m_PickResult;
    std::optional<Uuid> m_Selection;
    GizmoVisual m_Gizmo;
    std::optional<MeshPreviewVisual> m_MeshPreview;
    std::function<void(std::string)> m_Log;
    MeshRange m_Cube;
    MeshRange m_Plane;
    MeshRange m_Cylinder;
    MeshRange m_Cone;
    MeshRange m_Sphere;
    MeshRange m_Torus;
    MeshRange m_Quad;
    MeshRange m_PlanePrimitive;
    MeshRange m_Capsule;
    LightSet m_FrameLights;
    // Forward+ frame state: uploaded point lights + the CPU tile assignment and
    // the fp_params uniform (tile size, tiles across, count, enabled).
    std::vector<forward_plus::GpuPointLight> m_FramePointLightsGpu;
    forward_plus::TileAssignment m_FrameTiles;
    glm::vec4 m_FrameForwardPlus{0.0F};
    int m_FrameTotalPointLights{0};
    int m_FrameTotalSpotLights{0};
    std::chrono::steady_clock::time_point m_LastLightCapLog{};
    glm::vec4 m_FrameAmbientSky{0.45F, 0.55F, 0.70F, 0.55F};
    glm::vec4 m_FrameAmbientGround{0.28F, 0.25F, 0.22F, 0.0F};
    glm::vec4 m_FrameEnvParams{1.0F, 0.0F, 0.0F, 0.0F};
    std::unique_ptr<PostProcessor> m_PostProcessor;
    PostProcessSettings m_FramePostSettings;
    int m_MaxBloomPasses{6};
    glm::vec4 m_FrameFogColorDensity{0.60F, 0.66F, 0.75F, 0.0F};
    glm::vec4 m_FrameFogRange{10.0F, 250.0F, 0.0F, 0.0F};
    glm::vec4 m_FrameSkyZenith{0.13F, 0.27F, 0.52F, 0.0F};
    glm::vec4 m_FrameSkyHorizon{0.63F, 0.71F, 0.82F, 0.0F};
    glm::vec4 m_FrameSkyGround{0.20F, 0.19F, 0.18F, 0.0F};
    bool m_Orthographic{false};
    bool m_EditorVisuals{false};
    bool m_CollisionVisualization{false};
    ViewportDebugView m_ViewportDebugView{ViewportDebugView::None};
    PostDebugView m_PostDebugView{PostDebugView::None};
    glm::vec4 m_FrameDebugParams{0.0F};
    glm::vec4 m_FrameDebugSplits{0.0F};
    bool m_DepthSampleable{false};
    bool m_AoDebugWarned{false};
    bool m_LineOverflowReported{false};
    RenderDiagnostics m_Diagnostics{};
    int m_FrameDrawCalls{0};
    int m_FrameVisibleMeshes{0};
    int m_FrameActivePointLights{0};
    int m_FrameActiveSpotLights{0};
    int m_FrameShadowedLights{0};
    int m_FrameShadowPasses{0};
    bool m_GpuTimestampsSupported{false};
    float m_LastGpuRenderMs{-1.0F};
};

class RhiViewportPicking final : public Picking
{
public:
    explicit RhiViewportPicking(RhiViewportRenderer& renderer) : m_Renderer(renderer) {}
    void Request(const PickRequest request) override { m_Renderer.RequestPick(request); }
    [[nodiscard]] std::optional<PickResult> Poll() override { return m_Renderer.PollPick(); }

private:
    RhiViewportRenderer& m_Renderer;
};

std::unique_ptr<ViewportRenderer> CreateViewportRenderer(rhi::Device& device, IAssetDatabase& database)
{
    return std::make_unique<RhiViewportRenderer>(device, database);
}

std::unique_ptr<Picking> CreateViewportPicking(ViewportRenderer& renderer)
{
    auto* rhiRenderer = dynamic_cast<RhiViewportRenderer*>(&renderer);
    if (rhiRenderer == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<RhiViewportPicking>(*rhiRenderer);
}

void SetViewportOrthographic(ViewportRenderer& renderer, const bool enabled) noexcept
{
    if (auto* rhiRenderer = dynamic_cast<RhiViewportRenderer*>(&renderer))
    {
        rhiRenderer->SetOrthographic(enabled);
    }
}

void SetViewportLog(ViewportRenderer& renderer, std::function<void(std::string)> log)
{
    if (auto* rhiRenderer = dynamic_cast<RhiViewportRenderer*>(&renderer))
    {
        rhiRenderer->SetLog(std::move(log));
    }
}
}
