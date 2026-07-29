#pragma once

#ifdef _WIN32

#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/RenderTarget.hpp"
#include "engine/rhi/Sampler.hpp"
#include "engine/rhi/Shader.hpp"
#include "engine/rhi/Swapchain.hpp"
#include "engine/rhi/Texture.hpp"

#include <memory>
#include <span>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace fadix::rhi::d3d11
{
class D3D11Device final : public Device
{
public:
    explicit D3D11Device(void* sdlWindow);
    ~D3D11Device() override;

    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    [[nodiscard]] Result<std::unique_ptr<Buffer>> CreateBuffer(const BufferDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<Texture>> CreateTexture(
        const TextureDesc& description, std::span<const std::byte> initialData = {}) override;
    [[nodiscard]] Result<std::unique_ptr<Sampler>> CreateSampler(const SamplerDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<Shader>> CreateShader(
        const ShaderDesc& description, std::span<const std::byte> bytecode) override;
    [[nodiscard]] Result<std::unique_ptr<Pipeline>> CreatePipeline(const PipelineDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<RenderTarget>> CreateRenderTarget(
        const TextureDesc& color, const TextureDesc* depth, const TextureDesc* color1 = nullptr) override;
    [[nodiscard]] std::unique_ptr<RenderTarget> CreateRenderTargetView(
        Texture& color, Texture* depth, Texture* color1 = nullptr) override;
    [[nodiscard]] Result<std::unique_ptr<Swapchain>> CreateSwapchain(void* nativeWindow) override;
    [[nodiscard]] std::unique_ptr<CommandList> CreateCommandList() override;
    void Submit(CommandList& commands) override;
    void WaitIdle() override;
    [[nodiscard]] bool SupportsTextureFormat(Format format, bool sampledDepth) const override;
    [[nodiscard]] const char* ShaderTarget(bool fragment) const noexcept override
    {
        return fragment ? "ps_5_0" : "vs_5_0";
    }

    [[nodiscard]] ID3D11Device* NativeDevice() const noexcept;
    [[nodiscard]] ID3D11DeviceContext* NativeContext() const noexcept;
    [[nodiscard]] ID3D11RenderTargetView* BackbufferView() const noexcept;
    [[nodiscard]] bool ResizeBackbuffer(std::uint32_t width, std::uint32_t height);
    void Present(bool vsync = true);
    void PresentTexture(Texture* source, bool vsync = true);

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

[[nodiscard]] D3D11Device* AsD3D11Device(Device& device) noexcept;
[[nodiscard]] std::unique_ptr<Device> CreateDeviceFromWindow(void* sdlWindow);
[[nodiscard]] void* NativeTextureView(Texture& texture) noexcept;
[[nodiscard]] Result<std::vector<std::byte>> ReadbackTexture(Device& device, Texture& texture);
}

#endif
