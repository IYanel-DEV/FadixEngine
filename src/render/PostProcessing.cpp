#include "render/PostProcessing.h"

#include "render/ShaderCompiler.hpp"

#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/RenderTarget.hpp"
#include "engine/rhi/Sampler.hpp"
#include "engine/rhi/Shader.hpp"
#include "engine/rhi/Texture.hpp"
#include "rhi/sdl/SdlRhi.hpp"

#include <SDL3/SDL_gpu.h>

#include <glm/mat4x4.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <span>

namespace fadix
{
namespace
{
std::unique_ptr<rhi::Shader> MakeShaderFromFile(
    rhi::Device& device,
    const char* file,
    const char* entry,
    const char* target,
    std::uint32_t numSamplers,
    std::uint32_t numUniformBuffers = 1)
{
    const std::vector<std::byte> source = render::ReadShaderSource(file);
    const std::vector<std::byte> code =
        render::CompileShader(source, entry, device.ShaderTarget(target[0] == 'p'), file);
    auto result = device.CreateShader({entry, entry, numSamplers, numUniformBuffers}, code);
    if (!result)
    {
        std::fprintf(
            stderr,
            "[Fadix] Post shader '%s' (%s) FAILED: %s\n",
            entry,
            file,
            result.ErrorMessage().c_str());
        return nullptr;
    }
    return std::move(result).Value();
}

std::unique_ptr<rhi::Shader> MakeShader(
    rhi::Device& device,
    std::span<const std::byte> source,
    const char* entry,
    const char* target,
    std::uint32_t numSamplers,
    std::uint32_t numUniformBuffers = 1)
{
    const std::vector<std::byte> code = render::CompileShader(
        source, entry, device.ShaderTarget(target[0] == 'p'), "postprocess.hlsl");
    auto result = device.CreateShader({entry, entry, numSamplers, numUniformBuffers}, code);
    if (!result)
    {
        std::fprintf(
            stderr,
            "[Fadix] Post shader '%s' (postprocess.hlsl) FAILED: %s\n",
            entry,
            result.ErrorMessage().c_str());
        return nullptr;
    }
    return std::move(result).Value();
}

const char* FormatName(rhi::Format format) noexcept
{
    switch (format)
    {
    case rhi::Format::Unknown:
        return "Unknown";
    case rhi::Format::R8G8B8A8Unorm:
        return "R8G8B8A8Unorm";
    case rhi::Format::R16G16B16A16Float:
        return "R16G16B16A16Float";
    default:
        return "(other)";
    }
}

// Creates a fullscreen-blit pipeline. On failure it logs the pipeline name, both
// shader names, color/depth formats, sampler + uniform-buffer counts, and the
// full SDL error so the console names the exact pipeline that failed.
std::unique_ptr<rhi::Pipeline> MakePipeline(
    rhi::Device& device,
    rhi::Shader* vs,
    rhi::Shader* ps,
    rhi::Format colorFormat,
    const char* name)
{
    rhi::PipelineDesc desc;
    desc.DebugName = name;
    desc.ColorFormat = colorFormat;
    desc.DepthFormat = rhi::Format::Unknown;
    desc.DepthTest = false;
    desc.DepthWrite = false;
    desc.Cull = rhi::CullMode::None;
    desc.UseVertexInput = false;
    desc.VertexShader = vs;
    desc.FragmentShader = ps;
    auto result = device.CreatePipeline(desc);
    if (!result)
    {
        const rhi::ShaderDesc emptyDesc{};
        const rhi::ShaderDesc& vsDesc = vs != nullptr ? vs->Description() : emptyDesc;
        const rhi::ShaderDesc& psDesc = ps != nullptr ? ps->Description() : emptyDesc;
        std::fprintf(
            stderr,
            "[Fadix] Post pipeline '%s' FAILED:\n"
            "  vertex shader : %s (samplers=%u, uniformBuffers=%u)\n"
            "  fragment shader: %s (samplers=%u, uniformBuffers=%u)\n"
            "  color format  : %s\n"
            "  depth format  : %s\n"
            "  SDL error     : %s\n",
            name,
            vs != nullptr ? vsDesc.DebugName.c_str() : "(null)",
            vsDesc.NumSamplers,
            vsDesc.NumUniformBuffers,
            ps != nullptr ? psDesc.DebugName.c_str() : "(null)",
            psDesc.NumSamplers,
            psDesc.NumUniformBuffers,
            FormatName(colorFormat),
            FormatName(rhi::Format::Unknown),
            result.ErrorMessage().c_str());
        return nullptr;
    }
    return std::move(result).Value();
}

std::unique_ptr<rhi::RenderTarget> MakeColorTarget(
    rhi::Device& device, int width, int height, rhi::Format format, const char* name)
{
    const rhi::TextureDesc color{
        {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
        format,
        rhi::TextureUsage::RenderTarget,
        1,
        name};
    auto result = device.CreateRenderTarget(color, nullptr);
    if (!result)
    {
        return nullptr;
    }
    return std::move(result).Value();
}
}

PostProcessor::~PostProcessor() { Shutdown(); }

bool PostProcessor::Initialize(rhi::Device& device)
{
    m_Device = &device;
    try
    {
        const auto fail = [](const char* what) {
            std::fprintf(stderr, "[Fadix] Post-processing MANDATORY init failed: %s\n", what);
            return false;
        };
        const auto reportOptional = [](const char* name, const bool ready) {
            std::fprintf(
                stderr,
                "[Fadix] Post optional '%s': %s\n",
                name,
                ready ? "ready" : "UNSUPPORTED (disabled)");
        };

        const rhi::Format hdr = rhi::Format::R16G16B16A16Float;
        const rhi::Format ldr = rhi::Format::R8G8B8A8Unorm;
        const std::vector<std::byte> source = render::ReadShaderSource("postprocess.hlsl");

        // ---- Mandatory: fullscreen VS, sampler, tone map, copy/dither ---------
        // Fullscreen triangle VS has no samplers / uniform buffers.
        m_FullscreenVS = MakeShader(device, source, "FullscreenVS", "vs_5_1", 0, 0);
        if (!m_FullscreenVS)
        {
            return fail("FullscreenVS shader");
        }

        rhi::SamplerDesc linear;
        linear.AddressU = rhi::WrapMode::Clamp;
        linear.AddressV = rhi::WrapMode::Clamp;
        linear.AddressW = rhi::WrapMode::Clamp;
        linear.DebugName = "PostLinearClamp";
        auto sampler = device.CreateSampler(linear);
        if (!sampler)
        {
            return fail(sampler.ErrorMessage().c_str());
        }
        m_LinearSampler = std::move(sampler).Value();

        m_TonemapPS = MakeShader(device, source, "TonemapPS", "ps_5_1", 1);
        m_CopyDitherPS = MakeShader(device, source, "CopyDitherPS", "ps_5_1", 1);
        if (!m_TonemapPS || !m_CopyDitherPS)
        {
            return fail("mandatory tone-map / copy-dither shader");
        }
        m_TonemapPipeline = MakePipeline(device, m_FullscreenVS.get(), m_TonemapPS.get(), ldr, "PostTonemap");
        if (m_TonemapPipeline == nullptr)
        {
            return fail("mandatory PostTonemap pipeline");
        }
        m_CopyDitherPipeline =
            MakePipeline(device, m_FullscreenVS.get(), m_CopyDitherPS.get(), ldr, "PostCopyDither");
        if (m_CopyDitherPipeline == nullptr)
        {
            return fail("mandatory PostCopyDither pipeline");
        }
        m_CoreTonemapReady = true;

        // ---- Optional: each capability built individually; failure is logged by
        // MakePipeline (name + shaders + formats + SDL error) and disables only
        // that capability. Tone mapping stays alive regardless. ----------------

        // Plain copy: used by the TAA history write and as a safe fallback.
        m_CopyPS = MakeShader(device, source, "CopyPS", "ps_5_1", 1);
        if (m_CopyPS)
        {
            m_CopyPipeline = MakePipeline(device, m_FullscreenVS.get(), m_CopyPS.get(), ldr, "PostCopy");
        }

        // Bloom.
        m_BrightExtractPS = MakeShader(device, source, "BrightExtractPS", "ps_5_1", 1);
        m_BloomDownsamplePS = MakeShader(device, source, "BloomDownsamplePS", "ps_5_1", 1);
        m_BloomUpsamplePS = MakeShader(device, source, "BloomUpsamplePS", "ps_5_1", 2);
        m_CompositePS = MakeShader(device, source, "CompositePS", "ps_5_1", 2);
        if (m_BrightExtractPS && m_BloomDownsamplePS && m_BloomUpsamplePS && m_CompositePS)
        {
            m_BrightExtractPipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_BrightExtractPS.get(), hdr, "PostBrightExtract");
            m_BloomDownsamplePipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_BloomDownsamplePS.get(), hdr, "PostBloomDownsample");
            m_BloomUpsamplePipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_BloomUpsamplePS.get(), hdr, "PostBloomUpsample");
            m_CompositePipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_CompositePS.get(), hdr, "PostComposite");
            m_BloomReady = m_BrightExtractPipeline && m_BloomDownsamplePipeline &&
                m_BloomUpsamplePipeline && m_CompositePipeline;
        }
        reportOptional("bloom", m_BloomReady);

        // Color grading.
        m_ColorGradePS = MakeShader(device, source, "ColorGradePS", "ps_5_1", 1);
        if (m_ColorGradePS)
        {
            m_ColorGradePipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_ColorGradePS.get(), ldr, "PostColorGrade");
            m_ColorGradeReady = m_ColorGradePipeline != nullptr;
        }
        reportOptional("color grading", m_ColorGradeReady);

        // FXAA.
        m_FxaaPS = MakeShader(device, source, "FxaaPS", "ps_5_1", 1);
        if (m_FxaaPS)
        {
            m_FxaaPipeline = MakePipeline(device, m_FullscreenVS.get(), m_FxaaPS.get(), ldr, "PostFxaa");
            m_FxaaReady = m_FxaaPipeline != nullptr;
        }
        reportOptional("FXAA", m_FxaaReady);

        // TAA (experimental). Requires the plain copy pipeline for history.
        m_TaaPS = MakeShader(device, source, "TaaPS", "ps_5_1", 4);
        if (m_TaaPS)
        {
            m_TaaPipeline = MakePipeline(device, m_FullscreenVS.get(), m_TaaPS.get(), ldr, "PostTaa");
            m_TaaReady = m_TaaPipeline != nullptr && m_CopyPipeline != nullptr;
        }
        reportOptional("TAA (experimental)", m_TaaReady);

        // Motion-vector debug view. The shader reads velocity_tex at register
        // t2,space2, so the pipeline must reserve at least 3 sampler slots (t0..t2)
        // or SDL's D3D12 root signature omits t2 and SDL_CreateGPUGraphicsPipeline
        // fails with 0x80070057. It is dispatched via BlitPass3 (4 textures), so
        // declare 4 samplers to match the bind site exactly (like TaaPS).
        m_MotionVectorsPS = MakeShader(device, source, "MotionVectorsPS", "ps_5_1", 4);
        if (m_MotionVectorsPS)
        {
            m_MotionVectorsPipeline =
                MakePipeline(device, m_FullscreenVS.get(), m_MotionVectorsPS.get(), ldr, "PostMotionVectors");
            m_MotionVectorsReady = m_MotionVectorsPipeline != nullptr;
        }
        reportOptional("motion-vector debug", m_MotionVectorsReady);

        // SSAO debug view (experimental).
        m_SsaoFullscreenVS = MakeShaderFromFile(device, "ssao.hlsl", "FullscreenVS", "vs_5_1", 0, 0);
        m_SsaoPS = MakeShaderFromFile(device, "ssao.hlsl", "SsaoPS", "ps_5_1", 1);
        if (m_SsaoFullscreenVS && m_SsaoPS)
        {
            m_SsaoPipeline =
                MakePipeline(device, m_SsaoFullscreenVS.get(), m_SsaoPS.get(), ldr, "PostSsaoDebug");
            m_SsaoReady = m_SsaoPipeline != nullptr;
        }
        reportOptional("SSAO debug (experimental)", m_SsaoReady);

        return m_CoreTonemapReady;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "[Fadix] Post-processing init failed: %s\n", error.what());
        return false;
    }
}

int PostProcessor::MipWidth(const int level) const noexcept
{
    return std::max(1, m_Width >> (level + 1));
}

int PostProcessor::MipHeight(const int level) const noexcept
{
    return std::max(1, m_Height >> (level + 1));
}

glm::vec2 PostProcessor::MipInvResolution(const int level) const noexcept
{
    const int width = MipWidth(level);
    const int height = MipHeight(level);
    return {width > 0 ? 1.0F / static_cast<float>(width) : 0.0F,
        height > 0 ? 1.0F / static_cast<float>(height) : 0.0F};
}

int PostProcessor::EffectiveBloomLevels(const int requestedLevels) const noexcept
{
    int levels = std::clamp(requestedLevels, 1, kMaxBloomMips);
    while (levels > 1 && MipWidth(levels - 1) <= 1 && MipHeight(levels - 1) <= 1)
    {
        --levels;
    }
    return levels;
}

void PostProcessor::Resize(rhi::Device& device, int width, int height)
{
    m_Device = &device;
    m_Width = width;
    m_Height = height;
    m_Ready = false;
    ResetHistory();
    for (std::unique_ptr<rhi::RenderTarget>& mip : m_BloomMip)
    {
        mip.reset();
    }
    for (std::unique_ptr<rhi::RenderTarget>& scratch : m_BloomUpsampleScratch)
    {
        scratch.reset();
    }
    m_HdrResolve.reset();
    m_LdrA.reset();
    m_LdrB.reset();
    m_HistoryA.reset();
    m_HistoryB.reset();
    m_Output.reset();
    if (width <= 0 || height <= 0 || m_FullscreenVS == nullptr)
    {
        return;
    }
    const rhi::Format hdr = rhi::Format::R16G16B16A16Float;
    const rhi::Format ldr = rhi::Format::R8G8B8A8Unorm;
    for (int level = 0; level < kMaxBloomMips; ++level)
    {
        const int mipWidth = MipWidth(level);
        const int mipHeight = MipHeight(level);
        char name[32];
        std::snprintf(name, sizeof(name), "PostBloomMip%d", level);
        m_BloomMip[static_cast<std::size_t>(level)] =
            MakeColorTarget(device, mipWidth, mipHeight, hdr, name);
        std::snprintf(name, sizeof(name), "PostBloomScratch%d", level);
        m_BloomUpsampleScratch[static_cast<std::size_t>(level)] =
            MakeColorTarget(device, mipWidth, mipHeight, hdr, name);
    }
    m_HdrResolve = MakeColorTarget(device, width, height, hdr, "PostHdrResolve");
    m_LdrA = MakeColorTarget(device, width, height, ldr, "PostLdrA");
    m_LdrB = MakeColorTarget(device, width, height, ldr, "PostLdrB");
    m_HistoryA = MakeColorTarget(device, width, height, ldr, "PostHistoryA");
    m_HistoryB = MakeColorTarget(device, width, height, ldr, "PostHistoryB");
    m_Output = MakeColorTarget(device, width, height, ldr, "PostOutput");
    m_Ready = m_HdrResolve && m_LdrA && m_LdrB && m_HistoryA && m_HistoryB && m_Output;
    for (int level = 0; level < kMaxBloomMips && m_Ready; ++level)
    {
        m_Ready = m_BloomMip[static_cast<std::size_t>(level)] != nullptr &&
            m_BloomUpsampleScratch[static_cast<std::size_t>(level)] != nullptr;
    }
}

rhi::Texture* PostProcessor::OutputTexture() const noexcept
{
    return m_Ready && m_Output != nullptr ? m_Output->ColorTexture() : nullptr;
}

void PostProcessor::ResetHistory() noexcept
{
    m_HistoryValid = false;
    m_ResetHistoryNextFrame = true;
    m_HistoryReadIsA = true;
}

void PostProcessor::BlitPass(
    rhi::CommandList& list,
    rhi::RenderTarget& target,
    rhi::Pipeline& pipeline,
    rhi::Texture* source0,
    rhi::Texture* source1,
    const void* uniforms,
    const unsigned uniformSize)
{
    ++m_LastPassCount;
    list.BeginRenderPass(target, 0.0F, 0.0F, 0.0F, 1.0F);
    list.BindPipeline(pipeline);
    if (source1 != nullptr)
    {
        std::array<rhi::Texture*, 2> textures = {source0, source1};
        std::array<rhi::Sampler*, 2> samplers = {m_LinearSampler.get(), m_LinearSampler.get()};
        list.BindFragmentSamplers(0, textures, samplers);
    }
    else
    {
        std::array<rhi::Texture*, 1> textures = {source0};
        std::array<rhi::Sampler*, 1> samplers = {m_LinearSampler.get()};
        list.BindFragmentSamplers(0, textures, samplers);
    }
    list.PushFragmentUniform(0, uniforms, uniformSize);
    list.Draw(3);
    list.EndRenderPass();
}

void PostProcessor::BlitPass3(
    rhi::CommandList& list,
    rhi::RenderTarget& target,
    rhi::Pipeline& pipeline,
    rhi::Texture* source0,
    rhi::Texture* source1,
    rhi::Texture* velocity,
    rhi::Texture* depth,
    const void* uniforms,
    const unsigned uniformSize)
{
    ++m_LastPassCount;
    list.BeginRenderPass(target, 0.0F, 0.0F, 0.0F, 1.0F);
    list.BindPipeline(pipeline);
    rhi::Texture* depthBinding = depth != nullptr ? depth : velocity;
    std::array<rhi::Texture*, 4> textures = {source0, source1, velocity, depthBinding};
    std::array<rhi::Sampler*, 4> samplers = {
        m_LinearSampler.get(), m_LinearSampler.get(), m_LinearSampler.get(), m_LinearSampler.get()};
    list.BindFragmentSamplers(0, textures, samplers);
    list.PushFragmentUniform(0, uniforms, uniformSize);
    list.Draw(3);
    list.EndRenderPass();
}

void PostProcessor::Execute(
    rhi::CommandList& list, rhi::Texture& sceneColor, const PostProcessSettings& settings)
{
    m_LastPassCount = 0;
    if (!m_Ready)
    {
        return;
    }
    const glm::vec2 fullInv{
        m_Width > 0 ? 1.0F / static_cast<float>(m_Width) : 0.0F,
        m_Height > 0 ? 1.0F / static_cast<float>(m_Height) : 0.0F};
    const float fullInvArray[2] = {fullInv.x, fullInv.y};

    if (settings.DebugView == PostDebugView::MotionVectors && settings.Velocity != nullptr &&
        m_MotionVectorsReady)
    {
        struct MotionVectorsUniforms
        {
            float InvResolution[2];
            float Scale;
            float Pad0;
        } motionVectors{{fullInvArray[0], fullInvArray[1]}, 8.0F, 0.0F};
        BlitPass3(
            list,
            *m_Output,
            *m_MotionVectorsPipeline,
            settings.Velocity,
            settings.Velocity,
            settings.Velocity,
            settings.Velocity,
            &motionVectors,
            sizeof(motionVectors));
        return;
    }

    if (settings.AoDebug && settings.Depth != nullptr && m_SsaoReady)
    {
        struct SsaoUniforms
        {
            glm::mat4 Projection;
            glm::mat4 InvProjection;
            glm::vec4 Params0; // radius, intensity, power, bias
            glm::vec4 Params1; // sampleCount, fadeStart, fadeEnd, debug
            glm::vec4 Params2; // inv_resolution.xy, maxDepth, unused
        } ssao{
            settings.AoProjection,
            settings.AoInvProjection,
            {0.5F, 1.0F, 1.5F, 0.025F},
            {16.0F, 25.0F, 45.0F, 1.0F},
            {fullInvArray[0], fullInvArray[1], 1.0F, 0.0F}};
        BlitPass(list, *m_Output, *m_SsaoPipeline, settings.Depth, nullptr, &ssao, sizeof(ssao));
        return;
    }

    rhi::Texture* hdrInput = &sceneColor;

    if (settings.BloomEnabled && m_BloomReady)
    {
        const int levels = EffectiveBloomLevels(settings.BloomPasses);
        const glm::vec2 mip0Inv = MipInvResolution(0);

        struct BrightExtractUniforms
        {
            float InvResolution[2];
            float Threshold;
            float Knee;
        } extract{{mip0Inv.x, mip0Inv.y}, settings.BloomThreshold, kBloomKnee};
        BlitPass(
            list, *m_BloomMip[0], *m_BrightExtractPipeline, &sceneColor, nullptr, &extract, sizeof(extract));

        for (int level = 1; level < levels; ++level)
        {
            const glm::vec2 inv = MipInvResolution(level);
            struct BloomDownsampleUniforms
            {
                float InvResolution[2];
                float Pad0;
                float Pad1;
            } down{{inv.x, inv.y}, 0.0F, 0.0F};
            BlitPass(
                list,
                *m_BloomMip[static_cast<std::size_t>(level)],
                *m_BloomDownsamplePipeline,
                m_BloomMip[static_cast<std::size_t>(level - 1)]->ColorTexture(),
                nullptr,
                &down,
                sizeof(down));
        }

        for (int level = levels - 2; level >= 0; --level)
        {
            const glm::vec2 inv = MipInvResolution(level);
            struct BloomUpsampleUniforms
            {
                float InvResolution[2];
                float Pad0;
                float Pad1;
            } up{{inv.x, inv.y}, 0.0F, 0.0F};
            BlitPass(
                list,
                *m_BloomUpsampleScratch[static_cast<std::size_t>(level)],
                *m_BloomUpsamplePipeline,
                m_BloomMip[static_cast<std::size_t>(level)]->ColorTexture(),
                m_BloomMip[static_cast<std::size_t>(level + 1)]->ColorTexture(),
                &up,
                sizeof(up));
            std::swap(m_BloomMip[static_cast<std::size_t>(level)], m_BloomUpsampleScratch[static_cast<std::size_t>(level)]);
        }

        struct CompositeUniforms
        {
            float InvResolution[2];
            float Intensity;
            float Pad0;
        } composite{{fullInvArray[0], fullInvArray[1]}, settings.BloomIntensity, 0.0F};
        BlitPass(
            list,
            *m_HdrResolve,
            *m_CompositePipeline,
            &sceneColor,
            m_BloomMip[0]->ColorTexture(),
            &composite,
            sizeof(composite));
        hdrInput = m_HdrResolve->ColorTexture();
    }

    struct TonemapUniforms
    {
        float InvResolution[2];
        float Exposure;
        float ApplyAces;
    } tonemap{{fullInvArray[0], fullInvArray[1]}, settings.Exposure, settings.TonemapEnabled ? 1.0F : 0.0F};
    BlitPass(list, *m_LdrA, *m_TonemapPipeline, hdrInput, nullptr, &tonemap, sizeof(tonemap));

    rhi::Texture* ldr = m_LdrA->ColorTexture();
    if (settings.ColorGradingEnabled && m_ColorGradeReady)
    {
        struct ColorGradeUniforms
        {
            float InvResolution[2];
            float Pad0;
            float Pad1;
            float Lift[4];
            float Gamma[4];
            float Gain[4];
        } grade{
            {fullInvArray[0], fullInvArray[1]},
            0.0F,
            0.0F,
            {settings.Lift.x, settings.Lift.y, settings.Lift.z, 0.0F},
            {settings.Gamma.x, settings.Gamma.y, settings.Gamma.z, 0.0F},
            {settings.Gain.x, settings.Gain.y, settings.Gain.z, 0.0F}};
        BlitPass(list, *m_LdrB, *m_ColorGradePipeline, ldr, nullptr, &grade, sizeof(grade));
        ldr = m_LdrB->ColorTexture();
    }

    const bool resetHistory = settings.ResetHistory || m_ResetHistoryNextFrame || !m_HistoryValid;
    m_ResetHistoryNextFrame = false;
    // TAA-rejection debug only runs when the TAA pipeline actually exists.
    const bool taaRejectionDebug =
        settings.DebugView == PostDebugView::TaaRejection && m_TaaReady;

    // Resolve the AA mode against what the GPU actually supports so a missing
    // optional pipeline degrades gracefully instead of skipping the final write.
    AntiAliasMode aa = settings.ResolvedAntiAlias;
    if (aa == AntiAliasMode::Taa && !m_TaaReady)
    {
        aa = m_FxaaReady ? AntiAliasMode::Fxaa : AntiAliasMode::Off;
    }
    if (aa == AntiAliasMode::Fxaa && !m_FxaaReady)
    {
        aa = AntiAliasMode::Off;
    }

    switch (aa)
    {
    case AntiAliasMode::Taa:
    {
        if (settings.Velocity == nullptr && !taaRejectionDebug)
        {
            struct CopyDitherUniforms
            {
                float InvResolution[2];
                float Pad0;
                float Pad1;
            } copy{{fullInvArray[0], fullInvArray[1]}, 0.0F, 0.0F};
            BlitPass(list, *m_Output, *m_CopyDitherPipeline, ldr, nullptr, &copy, sizeof(copy));
            break;
        }
        rhi::RenderTarget* historyRead =
            m_HistoryReadIsA ? m_HistoryA.get() : m_HistoryB.get();
        rhi::RenderTarget* historyWrite =
            m_HistoryReadIsA ? m_HistoryB.get() : m_HistoryA.get();
        struct TaaUniforms
        {
            float InvResolution[2];
            float HistoryWeight;
            float Reset;
            float Sharpness;
            float DepthReject;
            float DebugRejection;
            float Pad0;
        } taa{
            {fullInvArray[0], fullInvArray[1]},
            settings.TaaHistoryWeight,
            resetHistory ? 1.0F : 0.0F,
            settings.TaaSharpness,
            settings.Depth != nullptr ? 1.0F : 0.0F,
            taaRejectionDebug ? 1.0F : 0.0F,
            0.0F};
        rhi::Texture* historyTex =
            resetHistory ? ldr : historyRead->ColorTexture();
        BlitPass3(
            list,
            *m_Output,
            *m_TaaPipeline,
            ldr,
            historyTex,
            settings.Velocity,
            settings.Depth,
            &taa,
            sizeof(taa));
        struct CopyDitherUniforms
        {
            float InvResolution[2];
            float Pad0;
            float Pad1;
        } historyCopy{{fullInvArray[0], fullInvArray[1]}, 0.0F, 0.0F};
        BlitPass(
            list,
            *historyWrite,
            *m_CopyPipeline,
            m_Output->ColorTexture(),
            nullptr,
            &historyCopy,
            sizeof(historyCopy));
        if (!taaRejectionDebug)
        {
            m_HistoryReadIsA = !m_HistoryReadIsA;
            m_HistoryValid = true;
        }
        break;
    }
    case AntiAliasMode::Fxaa:
    {
        if (taaRejectionDebug && settings.Velocity != nullptr)
        {
            rhi::RenderTarget* historyRead =
                m_HistoryReadIsA ? m_HistoryA.get() : m_HistoryB.get();
            struct TaaUniforms
            {
                float InvResolution[2];
                float HistoryWeight;
                float Reset;
                float Sharpness;
                float DepthReject;
                float DebugRejection;
                float Pad0;
            } taa{
                {fullInvArray[0], fullInvArray[1]},
                settings.TaaHistoryWeight,
                resetHistory ? 1.0F : 0.0F,
                settings.TaaSharpness,
                settings.Depth != nullptr ? 1.0F : 0.0F,
                1.0F,
                0.0F};
            rhi::Texture* historyTex =
                resetHistory ? ldr : historyRead->ColorTexture();
            BlitPass3(
                list,
                *m_Output,
                *m_TaaPipeline,
                ldr,
                historyTex,
                settings.Velocity,
                settings.Depth,
                &taa,
                sizeof(taa));
            break;
        }
        struct FxaaUniforms
        {
            float InvResolution[2];
            float Strength;
            float Enabled;
        } fxaa{
            {fullInvArray[0], fullInvArray[1]},
            settings.FxaaStrength,
            settings.FxaaStrength > 0.0F ? 1.0F : 0.0F};
        BlitPass(list, *m_Output, *m_FxaaPipeline, ldr, nullptr, &fxaa, sizeof(fxaa));
        break;
    }
    case AntiAliasMode::Off:
    default:
    {
        if (taaRejectionDebug && settings.Velocity != nullptr)
        {
            rhi::RenderTarget* historyRead =
                m_HistoryReadIsA ? m_HistoryA.get() : m_HistoryB.get();
            struct TaaUniforms
            {
                float InvResolution[2];
                float HistoryWeight;
                float Reset;
                float Sharpness;
                float DepthReject;
                float DebugRejection;
                float Pad0;
            } taa{
                {fullInvArray[0], fullInvArray[1]},
                settings.TaaHistoryWeight,
                resetHistory ? 1.0F : 0.0F,
                settings.TaaSharpness,
                settings.Depth != nullptr ? 1.0F : 0.0F,
                1.0F,
                0.0F};
            rhi::Texture* historyTex =
                resetHistory ? ldr : historyRead->ColorTexture();
            BlitPass3(
                list,
                *m_Output,
                *m_TaaPipeline,
                ldr,
                historyTex,
                settings.Velocity,
                settings.Depth,
                &taa,
                sizeof(taa));
            break;
        }
        struct CopyDitherUniforms
        {
            float InvResolution[2];
            float Pad0;
            float Pad1;
        } copy{{fullInvArray[0], fullInvArray[1]}, 0.0F, 0.0F};
        BlitPass(list, *m_Output, *m_CopyDitherPipeline, ldr, nullptr, &copy, sizeof(copy));
        break;
    }
    }
}

void PostProcessor::Shutdown()
{
    m_Ready = false;
    m_CoreTonemapReady = false;
    m_BloomReady = false;
    m_ColorGradeReady = false;
    m_FxaaReady = false;
    m_TaaReady = false;
    m_MotionVectorsReady = false;
    m_SsaoReady = false;
    m_HistoryValid = false;
    m_ResetHistoryNextFrame = false;
    m_Output.reset();
    m_HistoryB.reset();
    m_HistoryA.reset();
    m_LdrB.reset();
    m_LdrA.reset();
    m_HdrResolve.reset();
    for (std::unique_ptr<rhi::RenderTarget>& scratch : m_BloomUpsampleScratch)
    {
        scratch.reset();
    }
    for (std::unique_ptr<rhi::RenderTarget>& mip : m_BloomMip)
    {
        mip.reset();
    }
    m_LinearSampler.reset();
    m_CopyDitherPipeline.reset();
    m_CopyPipeline.reset();
    m_MotionVectorsPipeline.reset();
    m_TaaPipeline.reset();
    m_FxaaPipeline.reset();
    m_ColorGradePipeline.reset();
    m_TonemapPipeline.reset();
    m_CompositePipeline.reset();
    m_BloomUpsamplePipeline.reset();
    m_BloomDownsamplePipeline.reset();
    m_BrightExtractPipeline.reset();
    m_FxaaPS.reset();
    m_TaaPS.reset();
    m_MotionVectorsPS.reset();
    m_CopyDitherPS.reset();
    m_CopyPS.reset();
    m_ColorGradePS.reset();
    m_TonemapPS.reset();
    m_CompositePS.reset();
    m_BloomUpsamplePS.reset();
    m_BloomDownsamplePS.reset();
    m_BrightExtractPS.reset();
    m_FullscreenVS.reset();
    m_Device = nullptr;
}
}
