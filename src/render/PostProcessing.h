#pragma once

#include "engine/render/RenderQuality.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/rhi/Types.hpp"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <memory>

namespace fadix::rhi
{
class Device;
class CommandList;
class Pipeline;
class RenderTarget;
class Sampler;
class Shader;
class Texture;
}

namespace fadix
{
struct PostProcessSettings
{
    bool BloomEnabled{true};
    float BloomThreshold{1.0F};
    float BloomIntensity{0.3F};
    int BloomPasses{5};

    bool TonemapEnabled{true};
    float Exposure{1.0F};

    bool ColorGradingEnabled{false};
    glm::vec3 Lift{0.0F};
    glm::vec3 Gamma{1.0F};
    glm::vec3 Gain{1.0F};

    bool FxaaEnabled{true};
    float FxaaStrength{1.0F};

    AntiAliasMode ResolvedAntiAlias{AntiAliasMode::Fxaa};
    float TaaHistoryWeight{0.9F};
    float TaaSharpness{0.0F};
    bool ResetHistory{false};
    rhi::Texture* Velocity{nullptr};
    rhi::Texture* Depth{nullptr};
    PostDebugView DebugView{PostDebugView::None};
    bool AoDebug{false};
    glm::mat4 AoProjection{1.0F};
    glm::mat4 AoInvProjection{1.0F};
};

class PostProcessor
{
public:
    PostProcessor() = default;
    ~PostProcessor();

    PostProcessor(const PostProcessor&) = delete;
    PostProcessor& operator=(const PostProcessor&) = delete;

    // Compiles shaders and creates pipelines + samplers. Returns false on failure.
    [[nodiscard]] bool Initialize(rhi::Device& device);
    // (Re)creates the size-dependent intermediate + output targets.
    void Resize(rhi::Device& device, int width, int height);
    // Runs the chain, reading sceneColor (HDR) and writing the final LDR result
    // into the internal Output target. No-op if not ready.
    void Execute(rhi::CommandList& list, rhi::Texture& sceneColor, const PostProcessSettings& settings);
    // Final LDR texture for the sceneview, or nullptr when unavailable.
    [[nodiscard]] rhi::Texture* OutputTexture() const noexcept;
    // Blit passes recorded by the last Execute() call (0 if not ready / no-op).
    [[nodiscard]] int LastPassCount() const noexcept { return m_LastPassCount; }
    // Clears temporal history; call on resize, scene load, cuts, quality/AA changes.
    void ResetHistory() noexcept;
    void Shutdown();

    // Capability flags. Mandatory tone mapping must be ready for the renderer to
    // present a tone-mapped image at all; optional effects run only when ready.
    [[nodiscard]] bool CoreTonemapReady() const noexcept { return m_CoreTonemapReady; }
    [[nodiscard]] bool BloomReady() const noexcept { return m_BloomReady; }
    [[nodiscard]] bool FxaaReady() const noexcept { return m_FxaaReady; }
    [[nodiscard]] bool TaaReady() const noexcept { return m_TaaReady; }
    [[nodiscard]] bool SsaoReady() const noexcept { return m_SsaoReady; }

private:
    static constexpr int kMaxBloomMips = 6;
    static constexpr float kBloomKnee = 0.5F;

    void BlitPass(
        rhi::CommandList& list,
        rhi::RenderTarget& target,
        rhi::Pipeline& pipeline,
        rhi::Texture* source0,
        rhi::Texture* source1,
        const void* uniforms,
        unsigned uniformSize);

    void BlitPass3(
        rhi::CommandList& list,
        rhi::RenderTarget& target,
        rhi::Pipeline& pipeline,
        rhi::Texture* source0,
        rhi::Texture* source1,
        rhi::Texture* velocity,
        rhi::Texture* depth,
        const void* uniforms,
        unsigned uniformSize);

    [[nodiscard]] int MipWidth(int level) const noexcept;
    [[nodiscard]] int MipHeight(int level) const noexcept;
    [[nodiscard]] glm::vec2 MipInvResolution(int level) const noexcept;
    [[nodiscard]] int EffectiveBloomLevels(int requestedLevels) const noexcept;

    rhi::Device* m_Device{nullptr};
    int m_Width{0};
    int m_Height{0};
    bool m_Ready{false};

    // Per-capability readiness resolved once in Initialize. Core tonemap is
    // mandatory; the rest gate optional passes so one broken pipeline can never
    // disable tone mapping or expose raw HDR.
    bool m_CoreTonemapReady{false};
    bool m_BloomReady{false};
    bool m_ColorGradeReady{false};
    bool m_FxaaReady{false};
    bool m_TaaReady{false};
    bool m_MotionVectorsReady{false};
    bool m_SsaoReady{false};

    std::unique_ptr<rhi::Shader> m_FullscreenVS;
    std::unique_ptr<rhi::Shader> m_BrightExtractPS;
    std::unique_ptr<rhi::Shader> m_BloomDownsamplePS;
    std::unique_ptr<rhi::Shader> m_BloomUpsamplePS;
    std::unique_ptr<rhi::Shader> m_CompositePS;
    std::unique_ptr<rhi::Shader> m_TonemapPS;
    std::unique_ptr<rhi::Shader> m_ColorGradePS;
    std::unique_ptr<rhi::Shader> m_FxaaPS;
    std::unique_ptr<rhi::Shader> m_TaaPS;
    std::unique_ptr<rhi::Shader> m_MotionVectorsPS;
    std::unique_ptr<rhi::Shader> m_SsaoFullscreenVS;
    std::unique_ptr<rhi::Shader> m_SsaoPS;
    std::unique_ptr<rhi::Shader> m_CopyDitherPS;
    std::unique_ptr<rhi::Shader> m_CopyPS;

    std::unique_ptr<rhi::Pipeline> m_BrightExtractPipeline;
    std::unique_ptr<rhi::Pipeline> m_BloomDownsamplePipeline;
    std::unique_ptr<rhi::Pipeline> m_BloomUpsamplePipeline;
    std::unique_ptr<rhi::Pipeline> m_CompositePipeline;
    std::unique_ptr<rhi::Pipeline> m_TonemapPipeline;
    std::unique_ptr<rhi::Pipeline> m_ColorGradePipeline;
    std::unique_ptr<rhi::Pipeline> m_FxaaPipeline;
    std::unique_ptr<rhi::Pipeline> m_TaaPipeline;
    std::unique_ptr<rhi::Pipeline> m_MotionVectorsPipeline;
    std::unique_ptr<rhi::Pipeline> m_SsaoPipeline;
    std::unique_ptr<rhi::Pipeline> m_CopyDitherPipeline;
    std::unique_ptr<rhi::Pipeline> m_CopyPipeline;

    std::unique_ptr<rhi::Sampler> m_LinearSampler;

    std::array<std::unique_ptr<rhi::RenderTarget>, kMaxBloomMips> m_BloomMip{};
    std::array<std::unique_ptr<rhi::RenderTarget>, kMaxBloomMips> m_BloomUpsampleScratch{};
    std::unique_ptr<rhi::RenderTarget> m_HdrResolve;
    std::unique_ptr<rhi::RenderTarget> m_LdrA;
    std::unique_ptr<rhi::RenderTarget> m_LdrB;
    std::unique_ptr<rhi::RenderTarget> m_HistoryA;
    std::unique_ptr<rhi::RenderTarget> m_HistoryB;
    std::unique_ptr<rhi::RenderTarget> m_Output;

    bool m_HistoryValid{false};
    bool m_ResetHistoryNextFrame{false};
    bool m_HistoryReadIsA{true};
    int m_LastPassCount{0};
};
}
