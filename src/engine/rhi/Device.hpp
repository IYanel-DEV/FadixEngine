#pragma once

#include "engine/Result.hpp"
#include "engine/rhi/Types.hpp"

#include <memory>
#include <span>

namespace fadix::rhi
{
class Buffer;
class CommandList;
class Pipeline;
class RenderTarget;
class Shader;
class Swapchain;
class Texture;
class Sampler;

class Device
{
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual Result<std::unique_ptr<Buffer>> CreateBuffer(const BufferDesc& description) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Texture>> CreateTexture(const TextureDesc& description, std::span<const std::byte> initialData = {}) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Sampler>> CreateSampler(const SamplerDesc& description) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Shader>> CreateShader(
        const ShaderDesc& description,
        std::span<const std::byte> bytecode) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Pipeline>> CreatePipeline(const PipelineDesc& description) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<RenderTarget>> CreateRenderTarget(
        const TextureDesc& color,
        const TextureDesc* depth,
        const TextureDesc* color1 = nullptr) = 0;
    // Non-owning view for overlay passes (caller keeps textures alive).
    [[nodiscard]] virtual std::unique_ptr<RenderTarget> CreateRenderTargetView(
        Texture& color,
        Texture* depth,
        Texture* color1 = nullptr) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<Swapchain>> CreateSwapchain(void* nativeWindow) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandList> CreateCommandList() = 0;
    virtual void Submit(CommandList& commands) = 0;
    virtual void WaitIdle() = 0;

    // True when the backend supports `format` for the given usage. Depth formats
    // vary by GPU (see SDL docs), so query before allocating sampleable depth.
    // `sampledDepth` asks whether the format works as a sampled depth target.
    // Default true keeps non-SDL/mock devices working; SdlDevice overrides it.
    [[nodiscard]] virtual bool SupportsTextureFormat(
        Format /*format*/, bool /*sampledDepth*/) const
    {
        return true;
    }

    [[nodiscard]] virtual const char* ShaderTarget(bool fragment) const noexcept
    {
        return fragment ? "ps_5_1" : "vs_5_1";
    }
};
}
