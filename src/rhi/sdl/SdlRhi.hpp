#pragma once

#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/RenderTarget.hpp"
#include "engine/rhi/Shader.hpp"
#include "engine/rhi/Swapchain.hpp"
#include "engine/rhi/Texture.hpp"
#include "engine/rhi/Sampler.hpp"

#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

struct SDL_Window;

namespace fadix::rhi::sdl
{
class SdlDevice final : public Device
{
public:
    SdlDevice(SDL_GPUDevice* device, bool ownsDevice, SDL_Window* claimedWindow = nullptr);
    ~SdlDevice() override;

    SdlDevice(const SdlDevice&) = delete;
    SdlDevice& operator=(const SdlDevice&) = delete;

    [[nodiscard]] Result<std::unique_ptr<Buffer>> CreateBuffer(const BufferDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<Texture>> CreateTexture(const TextureDesc& description, std::span<const std::byte> initialData = {}) override;
    [[nodiscard]] Result<std::unique_ptr<Sampler>> CreateSampler(const SamplerDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<Shader>> CreateShader(
        const ShaderDesc& description,
        std::span<const std::byte> bytecode) override;
    [[nodiscard]] Result<std::unique_ptr<Pipeline>> CreatePipeline(const PipelineDesc& description) override;
    [[nodiscard]] Result<std::unique_ptr<RenderTarget>> CreateRenderTarget(
        const TextureDesc& color,
        const TextureDesc* depth,
        const TextureDesc* color1 = nullptr) override;
    [[nodiscard]] std::unique_ptr<RenderTarget> CreateRenderTargetView(
        Texture& color,
        Texture* depth,
        Texture* color1 = nullptr) override;
    [[nodiscard]] Result<std::unique_ptr<Swapchain>> CreateSwapchain(void* nativeWindow) override;
    [[nodiscard]] std::unique_ptr<CommandList> CreateCommandList() override;
    void Submit(CommandList& commands) override;
    void WaitIdle() override;
    [[nodiscard]] bool SupportsTextureFormat(Format format, bool sampledDepth) const override;

    [[nodiscard]] SDL_GPUDevice* NativeDevice() const noexcept { return m_Device; }

private:
    SDL_GPUDevice* m_Device{nullptr};
    SDL_Window* m_ClaimedWindow{nullptr};
    SDL_GPUShader* m_LastVertexShader{nullptr};
    SDL_GPUShader* m_LastFragmentShader{nullptr};
    bool m_OwnsDevice{false};
};

[[nodiscard]] SdlDevice* AsSdlDevice(Device& device) noexcept;
[[nodiscard]] SDL_GPUTexture* NativeTexture(Texture& texture) noexcept;
[[nodiscard]] SDL_GPUCommandBuffer* NativeCommandBuffer(CommandList& commands) noexcept;
[[nodiscard]] SDL_GPURenderPass* NativeRenderPass(CommandList& commands) noexcept;
void PushVertexUniform(CommandList& commands, std::uint32_t slot, const void* data, std::uint32_t size);
void PushFragmentUniform(CommandList& commands, std::uint32_t slot, const void* data, std::uint32_t size);
void BindFragmentSamplers(CommandList& commands, std::uint32_t firstSlot, std::span<Texture*> textures, std::span<Sampler*> samplers);
void BindIndexBuffer(CommandList& commands, Buffer& buffer);
void DrawIndexed(CommandList& commands, std::uint32_t indexCount);

// Streams per-frame vertex data into a GPU buffer through a copy pass recorded
// on the caller's command buffer (before any render pass begins). Uses SDL GPU
// resource cycling, so the destination buffer may be reused every frame without
// explicit fences.
class DynamicBufferUploader final
{
public:
    explicit DynamicBufferUploader(Device& device);
    ~DynamicBufferUploader();

    DynamicBufferUploader(const DynamicBufferUploader&) = delete;
    DynamicBufferUploader& operator=(const DynamicBufferUploader&) = delete;

    // `commands` must be recording and outside of a render pass.
    [[nodiscard]] bool Upload(CommandList& commands, Buffer& destination, std::span<const std::byte> data);

private:
    SDL_GPUDevice* m_Device{nullptr};
    SDL_GPUTransferBuffer* m_Transfer{nullptr};
    std::size_t m_Capacity{0};
};
}

namespace fadix
{
// Creates and owns an SDL GPU device, and claims the supplied SDL_Window.
[[nodiscard]] std::unique_ptr<rhi::Device> CreateDeviceFromWindow(void* sdlWindow);

// Wraps a device owned elsewhere (for example during incremental RmlUi
// integration). The returned RHI device never destroys nativeDevice.
[[nodiscard]] std::unique_ptr<rhi::Device> AdoptSdlGpuDevice(void* nativeDevice);

[[nodiscard]] void* GetNativeDeviceHandle(rhi::Device& device) noexcept;
[[nodiscard]] void* GetNativeTextureHandle(rhi::Texture& texture) noexcept;

// Synchronously downloads the full contents of a color texture (4 bytes per
// pixel) into host memory. Waits for all previously submitted GPU work, so it
// is intended for tools and smoke tests rather than per-frame use.
[[nodiscard]] Result<std::vector<std::byte>> ReadbackTexture(rhi::Device& device, rhi::Texture& texture);
}
