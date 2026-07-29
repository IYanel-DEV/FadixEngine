#include "rhi/sdl/SdlRhi.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace fadix::rhi::sdl
{
namespace
{
[[nodiscard]] SDL_GPUFilter ToSdlFilterMode(const FilterMode mode)
{
    return mode == FilterMode::Nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
}

[[nodiscard]] SDL_GPUSamplerMipmapMode ToSdlMipmapMode(const FilterMode mode)
{
    return mode == FilterMode::Nearest ? SDL_GPU_SAMPLERMIPMAPMODE_NEAREST : SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
}

[[nodiscard]] SDL_GPUSamplerAddressMode ToSdlWrapMode(const WrapMode mode)
{
    switch (mode)
    {
    case WrapMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case WrapMode::Mirror: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case WrapMode::Clamp: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
}

[[nodiscard]] SDL_GPUVertexElementFormat ToSdlVertexElementFormat(const VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float2: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    case VertexElementFormat::Float3: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    case VertexElementFormat::Float4: return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    case VertexElementFormat::Color: return SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM;
    }
    return SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
}

[[nodiscard]] SDL_GPUTextureFormat ToSdlFormat(const Format format)
{
    switch (format)
    {
    case Format::R8G8B8A8Unorm: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8UnormSrgb: return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::B8G8R8A8Unorm: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case Format::R16G16Float: return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case Format::R16G16B16A16Float: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case Format::D24UnormS8Uint: return SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    case Format::D32Float: return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case Format::D16Unorm: return SDL_GPU_TEXTUREFORMAT_D16_UNORM;
    default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

[[nodiscard]] Format FromSdlFormat(const SDL_GPUTextureFormat format)
{
    switch (format)
    {
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return Format::R8G8B8A8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return Format::R8G8B8A8UnormSrgb;
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM: return Format::B8G8R8A8Unorm;
    case SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT: return Format::R16G16Float;
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return Format::R16G16B16A16Float;
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT: return Format::D24UnormS8Uint;
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT: return Format::D32Float;
    case SDL_GPU_TEXTUREFORMAT_D16_UNORM: return Format::D16Unorm;
    default: return Format::Unknown;
    }
}

[[nodiscard]] SDL_GPUBufferUsageFlags ToSdlBufferUsage(const BufferUsage usage)
{
    switch (usage)
    {
    case BufferUsage::Vertex: return SDL_GPU_BUFFERUSAGE_VERTEX;
    case BufferUsage::Index: return SDL_GPU_BUFFERUSAGE_INDEX;
    case BufferUsage::Uniform:
    case BufferUsage::Storage: return SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    case BufferUsage::Staging: return SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    }
    return SDL_GPU_BUFFERUSAGE_VERTEX;
}

[[nodiscard]] SDL_GPUTextureUsageFlags ToSdlTextureUsage(const TextureUsage usage)
{
    switch (usage)
    {
    case TextureUsage::Sampled: return SDL_GPU_TEXTUREUSAGE_SAMPLER;
    case TextureUsage::RenderTarget:
        return SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    case TextureUsage::DepthStencil: return SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    case TextureUsage::Storage: return SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    }
    return SDL_GPU_TEXTUREUSAGE_SAMPLER;
}

[[nodiscard]] std::string SdlError(const char* operation)
{
    return std::string{operation} + ": " + SDL_GetError();
}

[[nodiscard]] bool IsFragmentShader(const std::string& name)
{
    // Post shaders use entry names like TonemapPS / TaaPS (no "fragment" / "_ps").
    if (name.size() >= 2 && (name.compare(name.size() - 2, 2, "PS") == 0 ||
                             name.compare(name.size() - 2, 2, "Ps") == 0))
    {
        return true;
    }
    return name.find("fragment") != std::string::npos || name.find("Fragment") != std::string::npos ||
           name.find(".frag") != std::string::npos || name.find("_ps") != std::string::npos;
}

[[nodiscard]] SDL_GPUShaderFormat DetectShaderFormat(const std::span<const std::byte> bytecode)
{
    if (bytecode.size() >= 4)
    {
        const auto* bytes = reinterpret_cast<const unsigned char*>(bytecode.data());
        if (std::memcmp(bytes, "DXBC", 4) == 0)
        {
            return SDL_GPU_SHADERFORMAT_DXBC;
        }
        const std::uint32_t magic = static_cast<std::uint32_t>(bytes[0]) |
                                    (static_cast<std::uint32_t>(bytes[1]) << 8U) |
                                    (static_cast<std::uint32_t>(bytes[2]) << 16U) |
                                    (static_cast<std::uint32_t>(bytes[3]) << 24U);
        if (magic == 0x07230203U)
        {
            return SDL_GPU_SHADERFORMAT_SPIRV;
        }
    }
    return SDL_GPU_SHADERFORMAT_DXIL;
}
}

class SdlBuffer final : public Buffer
{
public:
    SdlBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, BufferDesc description)
        : m_Device(device), m_Buffer(buffer), m_Description(std::move(description))
    {
    }

    ~SdlBuffer() override
    {
        if (m_Buffer != nullptr)
        {
            SDL_ReleaseGPUBuffer(m_Device, m_Buffer);
        }
    }

    [[nodiscard]] std::size_t Size() const noexcept override { return m_Description.Size; }

    void Upload(const std::span<const std::byte> data, const std::size_t offset) override
    {
        if (offset > m_Description.Size || data.size() > m_Description.Size - offset)
        {
            throw std::out_of_range("RHI buffer upload exceeds buffer size");
        }
        if (data.empty())
        {
            return;
        }
        if (data.size() > std::numeric_limits<Uint32>::max())
        {
            throw std::length_error("RHI buffer upload exceeds SDL GPU limit");
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(data.size());
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_Device, &transferInfo);
        if (transfer == nullptr)
        {
            throw std::runtime_error(SdlError("SDL_CreateGPUTransferBuffer"));
        }

        void* destination = SDL_MapGPUTransferBuffer(m_Device, transfer, false);
        if (destination == nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
            throw std::runtime_error(SdlError("SDL_MapGPUTransferBuffer"));
        }
        std::memcpy(destination, data.data(), data.size());
        SDL_UnmapGPUTransferBuffer(m_Device, transfer);

        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_Device);
        if (commands == nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
            throw std::runtime_error(SdlError("SDL_AcquireGPUCommandBuffer"));
        }
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTransferBufferLocation source{};
        source.transfer_buffer = transfer;
        SDL_GPUBufferRegion destinationRegion{};
        destinationRegion.buffer = m_Buffer;
        destinationRegion.offset = static_cast<Uint32>(offset);
        destinationRegion.size = static_cast<Uint32>(data.size());
        SDL_UploadToGPUBuffer(copy, &source, &destinationRegion, false);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(commands))
        {
            SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
            throw std::runtime_error(SdlError("SDL_SubmitGPUCommandBuffer"));
        }
        SDL_WaitForGPUIdle(m_Device);
        SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
    }

    [[nodiscard]] SDL_GPUBuffer* Native() const noexcept { return m_Buffer; }

private:
    SDL_GPUDevice* m_Device;
    SDL_GPUBuffer* m_Buffer;
    BufferDesc m_Description;
};

class SdlTexture final : public Texture
{
public:
    SdlTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture, TextureDesc description, const bool ownsTexture)
        : m_Device(device), m_Texture(texture), m_Description(std::move(description)), m_OwnsTexture(ownsTexture)
    {
    }

    ~SdlTexture() override
    {
        if (m_OwnsTexture && m_Texture != nullptr)
        {
            SDL_ReleaseGPUTexture(m_Device, m_Texture);
        }
    }

    [[nodiscard]] const TextureDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] SDL_GPUTexture* Native() const noexcept { return m_Texture; }
    void ResetExternal(SDL_GPUTexture* texture, TextureDesc description)
    {
        m_Texture = texture;
        m_Description = std::move(description);
    }

private:
    SDL_GPUDevice* m_Device;
    SDL_GPUTexture* m_Texture;
    TextureDesc m_Description;
    bool m_OwnsTexture;
};

class SdlShader final : public Shader
{
public:
    SdlShader(SDL_GPUDevice* device, SDL_GPUShader* shader, ShaderDesc description, const SDL_GPUShaderStage stage)
        : m_Device(device), m_Shader(shader), m_Description(std::move(description)), m_Stage(stage)
    {
    }
    ~SdlShader() override
    {
        if (m_Shader != nullptr)
        {
            SDL_ReleaseGPUShader(m_Device, m_Shader);
        }
    }
    [[nodiscard]] const ShaderDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] SDL_GPUShader* Native() const noexcept { return m_Shader; }
    [[nodiscard]] SDL_GPUShaderStage Stage() const noexcept { return m_Stage; }

private:
    SDL_GPUDevice* m_Device;
    SDL_GPUShader* m_Shader;
    ShaderDesc m_Description;
    SDL_GPUShaderStage m_Stage;
};

class SdlSampler final : public Sampler
{
public:
    SdlSampler(SDL_GPUDevice* device, SDL_GPUSampler* sampler)
        : m_Device(device), m_Sampler(sampler)
    {
    }
    ~SdlSampler() override
    {
        if (m_Sampler != nullptr)
        {
            SDL_ReleaseGPUSampler(m_Device, m_Sampler);
        }
    }
    [[nodiscard]] SDL_GPUSampler* Native() const noexcept { return m_Sampler; }
private:
    SDL_GPUDevice* m_Device;
    SDL_GPUSampler* m_Sampler;
};

class SdlPipeline final : public Pipeline
{
public:
    SdlPipeline(SDL_GPUDevice* device, SDL_GPUGraphicsPipeline* pipeline, PipelineDesc description)
        : m_Device(device), m_Pipeline(pipeline), m_Description(std::move(description))
    {
    }
    ~SdlPipeline() override
    {
        if (m_Pipeline != nullptr)
        {
            SDL_ReleaseGPUGraphicsPipeline(m_Device, m_Pipeline);
        }
    }
    [[nodiscard]] const PipelineDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] SDL_GPUGraphicsPipeline* Native() const noexcept { return m_Pipeline; }

private:
    SDL_GPUDevice* m_Device;
    SDL_GPUGraphicsPipeline* m_Pipeline;
    PipelineDesc m_Description;
};

class SdlRenderTarget final : public RenderTarget
{
public:
    SdlRenderTarget(
        std::unique_ptr<SdlTexture> color,
        std::unique_ptr<SdlTexture> depth,
        std::unique_ptr<SdlTexture> color1,
        const Extent2D extent)
        : m_Color(std::move(color)), m_Depth(std::move(depth)), m_Color1(std::move(color1)), m_Extent(extent)
    {
    }
    [[nodiscard]] Texture* ColorTexture() const noexcept override { return m_Color.get(); }
    [[nodiscard]] Texture* ColorTexture1() const noexcept override { return m_Color1.get(); }
    [[nodiscard]] Texture* DepthTexture() const noexcept override { return m_Depth.get(); }
    [[nodiscard]] Extent2D Extent() const noexcept override { return m_Extent; }
    [[nodiscard]] SdlTexture* Color() const noexcept { return m_Color.get(); }
    [[nodiscard]] SdlTexture* Color1() const noexcept { return m_Color1.get(); }
    [[nodiscard]] SdlTexture* Depth() const noexcept { return m_Depth.get(); }
    void ResetExtent(const Extent2D extent) noexcept { m_Extent = extent; }

private:
    std::unique_ptr<SdlTexture> m_Color;
    std::unique_ptr<SdlTexture> m_Depth;
    std::unique_ptr<SdlTexture> m_Color1;
    Extent2D m_Extent;
};

class SdlRenderTargetView final : public RenderTarget
{
public:
    SdlRenderTargetView(Texture* color, Texture* depth, Texture* color1, const Extent2D extent)
        : m_Color(color), m_Depth(depth), m_Color1(color1), m_Extent(extent)
    {
    }
    [[nodiscard]] Texture* ColorTexture() const noexcept override { return m_Color; }
    [[nodiscard]] Texture* ColorTexture1() const noexcept override { return m_Color1; }
    [[nodiscard]] Texture* DepthTexture() const noexcept override { return m_Depth; }
    [[nodiscard]] Extent2D Extent() const noexcept override { return m_Extent; }

private:
    Texture* m_Color;
    Texture* m_Depth;
    Texture* m_Color1;
    Extent2D m_Extent;
};

[[nodiscard]] SdlTexture* AsSdlTexture(Texture* texture) noexcept
{
    return dynamic_cast<SdlTexture*>(texture);
}

class SdlCommandList final : public CommandList
{
public:
    explicit SdlCommandList(SDL_GPUDevice* device) : m_Device(device) {}
    ~SdlCommandList() override
    {
        if (m_Commands != nullptr && !m_Submitted)
        {
            SDL_CancelGPUCommandBuffer(m_Commands);
        }
    }

    void Begin() override
    {
        if (m_Commands != nullptr)
        {
            throw std::logic_error("RHI command list is already recording");
        }
        m_Commands = SDL_AcquireGPUCommandBuffer(m_Device);
        if (m_Commands == nullptr)
        {
            throw std::runtime_error(SdlError("SDL_AcquireGPUCommandBuffer"));
        }
    }

    void BeginRenderPass(
        RenderTarget& target,
        const float clearR,
        const float clearG,
        const float clearB,
        const float clearA) override
    {
        BeginRenderPassInternal(target, true, clearR, clearG, clearB, clearA);
    }

    void BeginRenderPassLoad(RenderTarget& target) override
    {
        BeginRenderPassInternal(target, false, 0.0F, 0.0F, 0.0F, 0.0F);
    }

private:
    void BeginRenderPassInternal(
        RenderTarget& target,
        const bool clearTargets,
        const float clearR,
        const float clearG,
        const float clearB,
        const float clearA)
    {
        if (m_Commands == nullptr || m_RenderPass != nullptr)
        {
            throw std::logic_error("Invalid SDL GPU render pass state");
        }
        auto* colorTexture = AsSdlTexture(target.ColorTexture());
        if (colorTexture == nullptr)
        {
            throw std::logic_error("Invalid SDL GPU render target color texture");
        }

        SDL_GPUColorTargetInfo colors[2]{};
        colors[0].texture = colorTexture->Native();
        colors[0].clear_color = SDL_FColor{clearR, clearG, clearB, clearA};
        colors[0].load_op = clearTargets ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        colors[0].store_op = SDL_GPU_STOREOP_STORE;

        std::uint32_t colorCount = 1;
        if (Texture* color1 = target.ColorTexture1(); color1 != nullptr)
        {
            auto* sdlColor1 = AsSdlTexture(color1);
            if (sdlColor1 == nullptr)
            {
                throw std::logic_error("Invalid SDL GPU render target color1 texture");
            }
            colors[1].texture = sdlColor1->Native();
            colors[1].clear_color = SDL_FColor{0.0F, 0.0F, 0.0F, 0.0F};
            colors[1].load_op = clearTargets ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            colors[1].store_op = SDL_GPU_STOREOP_STORE;
            colorCount = 2;
        }

        SDL_GPUDepthStencilTargetInfo depth{};
        SDL_GPUDepthStencilTargetInfo* depthPointer = nullptr;
        if (Texture* depthTexture = target.DepthTexture(); depthTexture != nullptr)
        {
            auto* sdlDepth = AsSdlTexture(depthTexture);
            if (sdlDepth == nullptr)
            {
                throw std::logic_error("Invalid SDL GPU render target depth texture");
            }
            depth.texture = sdlDepth->Native();
            depth.clear_depth = 1.0F;
            depth.load_op = clearTargets ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            depthPointer = &depth;
        }
        m_RenderPass = SDL_BeginGPURenderPass(m_Commands, colors, colorCount, depthPointer);
        if (m_RenderPass == nullptr)
        {
            throw std::runtime_error(SdlError("SDL_BeginGPURenderPass"));
        }
    }

public:

    void BindPipeline(Pipeline& pipeline) override
    {
        auto* sdlPipeline = dynamic_cast<SdlPipeline*>(&pipeline);
        if (sdlPipeline == nullptr || m_RenderPass == nullptr)
        {
            throw std::logic_error("Invalid SDL GPU pipeline binding");
        }
        SDL_BindGPUGraphicsPipeline(m_RenderPass, sdlPipeline->Native());
    }

    void BindVertexBuffer(Buffer& buffer) override
    {
        auto* sdlBuffer = dynamic_cast<SdlBuffer*>(&buffer);
        if (sdlBuffer == nullptr || m_RenderPass == nullptr)
        {
            throw std::logic_error("Invalid SDL GPU vertex buffer binding");
        }
        SDL_GPUBufferBinding binding{};
        binding.buffer = sdlBuffer->Native();
        SDL_BindGPUVertexBuffers(m_RenderPass, 0, &binding, 1);
    }

    void Draw(const std::uint32_t vertexCount, const std::uint32_t firstVertex) override
    {
        SDL_DrawGPUPrimitives(m_RenderPass, vertexCount, 1, firstVertex, 0);
    }

    void EndRenderPass() override
    {
        if (m_RenderPass != nullptr)
        {
            SDL_EndGPURenderPass(m_RenderPass);
            m_RenderPass = nullptr;
        }
    }
    void End() override
    {
        if (m_RenderPass != nullptr)
        {
            throw std::logic_error("RHI command list ended inside a render pass");
        }
    }

    void PushVertexUniform(const std::uint32_t slot, const void* data, const std::uint32_t size)
    {
        SDL_PushGPUVertexUniformData(m_Commands, slot, data, size);
    }
    void PushFragmentUniform(const std::uint32_t slot, const void* data, const std::uint32_t size)
    {
        SDL_PushGPUFragmentUniformData(m_Commands, slot, data, size);
    }
    void BindIndexBuffer(Buffer& buffer)
    {
        auto& sdlBuffer = dynamic_cast<SdlBuffer&>(buffer);
        SDL_GPUBufferBinding binding{};
        binding.buffer = sdlBuffer.Native();
        SDL_BindGPUIndexBuffer(m_RenderPass, &binding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    }
    void DrawIndexed(const std::uint32_t indexCount)
    {
        SDL_DrawGPUIndexedPrimitives(m_RenderPass, indexCount, 1, 0, 0, 0);
    }

    [[nodiscard]] SDL_GPUCommandBuffer* NativeCommands() const noexcept { return m_Commands; }
    [[nodiscard]] SDL_GPURenderPass* NativeRenderPass() const noexcept { return m_RenderPass; }
    bool Submit()
    {
        if (m_Commands == nullptr || m_Submitted)
        {
            return false;
        }
        m_Submitted = SDL_SubmitGPUCommandBuffer(m_Commands);
        m_Commands = nullptr;
        return m_Submitted;
    }

private:
    SDL_GPUDevice* m_Device;
    SDL_GPUCommandBuffer* m_Commands{nullptr};
    SDL_GPURenderPass* m_RenderPass{nullptr};
    bool m_Submitted{false};
};

class SdlSwapchain final : public Swapchain
{
public:
    SdlSwapchain(SDL_GPUDevice* device, SDL_Window* window)
        : m_Device(device), m_Window(window)
    {
        const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(device, window);
        TextureDesc description{{}, FromSdlFormat(format), TextureUsage::RenderTarget, 1, "Swapchain"};
        m_Texture = std::make_unique<SdlTexture>(device, nullptr, description, false);
        m_Target = std::make_unique<SdlRenderTarget>(std::move(m_Texture), nullptr, nullptr, Extent2D{});
    }

    [[nodiscard]] Result<void> Resize(const Extent2D extent) override
    {
        m_Extent = extent;
        return Result<void>::Ok();
    }

    [[nodiscard]] RenderTarget* AcquireNextTarget() override
    {
        if (m_Commands != nullptr)
        {
            return nullptr;
        }
        m_Commands = SDL_AcquireGPUCommandBuffer(m_Device);
        if (m_Commands == nullptr)
        {
            return nullptr;
        }
        SDL_GPUTexture* texture = nullptr;
        Uint32 width = 0;
        Uint32 height = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(m_Commands, m_Window, &texture, &width, &height))
        {
            SDL_CancelGPUCommandBuffer(m_Commands);
            m_Commands = nullptr;
            return nullptr;
        }
        if (texture == nullptr)
        {
            SDL_CancelGPUCommandBuffer(m_Commands);
            m_Commands = nullptr;
            return nullptr;
        }
        m_Extent = {width, height};
        m_Target->ResetExtent(m_Extent);
        auto* color = m_Target->Color();
        TextureDesc description{{width, height},
                                FromSdlFormat(SDL_GetGPUSwapchainTextureFormat(m_Device, m_Window)),
                                TextureUsage::RenderTarget,
                                1,
                                "Swapchain"};
        color->ResetExternal(texture, std::move(description));
        return m_Target.get();
    }

    [[nodiscard]] Result<void> Present() override
    {
        if (m_Commands == nullptr)
        {
            return Result<void>::Error("Swapchain has no acquired image");
        }
        if (!SDL_SubmitGPUCommandBuffer(m_Commands))
        {
            m_Commands = nullptr;
            return Result<void>::Error(SdlError("SDL_SubmitGPUCommandBuffer"));
        }
        m_Commands = nullptr;
        return Result<void>::Ok();
    }

private:
    SDL_GPUDevice* m_Device;
    SDL_Window* m_Window;
    SDL_GPUCommandBuffer* m_Commands{nullptr};
    Extent2D m_Extent{};
    std::unique_ptr<SdlTexture> m_Texture;
    std::unique_ptr<SdlRenderTarget> m_Target;
};

SdlDevice::SdlDevice(SDL_GPUDevice* device, const bool ownsDevice, SDL_Window* claimedWindow)
    : m_Device(device), m_ClaimedWindow(claimedWindow), m_OwnsDevice(ownsDevice)
{
    if (m_Device == nullptr)
    {
        throw std::invalid_argument("Cannot construct SDL RHI device from null");
    }
}

SdlDevice::~SdlDevice()
{
    if (m_OwnsDevice && m_Device != nullptr)
    {
        SDL_WaitForGPUIdle(m_Device);
        if (m_ClaimedWindow != nullptr)
        {
            SDL_ReleaseWindowFromGPUDevice(m_Device, m_ClaimedWindow);
        }
        SDL_DestroyGPUDevice(m_Device);
    }
}

Result<std::unique_ptr<Buffer>> SdlDevice::CreateBuffer(const BufferDesc& description)
{
    if (description.Size == 0 || description.Size > std::numeric_limits<Uint32>::max())
    {
        return Result<std::unique_ptr<Buffer>>::Error("Invalid SDL GPU buffer size");
    }
    SDL_GPUBufferCreateInfo info{};
    info.usage = ToSdlBufferUsage(description.Usage);
    info.size = static_cast<Uint32>(description.Size);
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(m_Device, &info);
    if (buffer == nullptr)
    {
        return Result<std::unique_ptr<Buffer>>::Error(SdlError("SDL_CreateGPUBuffer"));
    }
    return Result<std::unique_ptr<Buffer>>::Ok(
        std::make_unique<SdlBuffer>(m_Device, buffer, description));
}

Result<std::unique_ptr<Sampler>> SdlDevice::CreateSampler(const SamplerDesc& description)
{
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = ToSdlFilterMode(description.MinFilter);
    info.mag_filter = ToSdlFilterMode(description.MagFilter);
    info.mipmap_mode = ToSdlMipmapMode(description.MipFilter);
    info.address_mode_u = ToSdlWrapMode(description.AddressU);
    info.address_mode_v = ToSdlWrapMode(description.AddressV);
    info.address_mode_w = ToSdlWrapMode(description.AddressW);
    info.max_anisotropy = description.MaxAnisotropy;
    info.enable_anisotropy = description.EnableAnisotropy;

    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(m_Device, &info);
    if (sampler == nullptr)
    {
        return Result<std::unique_ptr<Sampler>>::Error(SdlError("SDL_CreateGPUSampler"));
    }
    return Result<std::unique_ptr<Sampler>>::Ok(std::make_unique<SdlSampler>(m_Device, sampler));
}

Result<std::unique_ptr<Texture>> SdlDevice::CreateTexture(const TextureDesc& description, std::span<const std::byte> initialData)
{
    if (description.Extent.Width == 0 || description.Extent.Height == 0)
    {
        return Result<std::unique_ptr<Texture>>::Error("SDL GPU texture extent must be non-zero");
    }
    SDL_GPUTextureCreateInfo info{};
    Uint32 layers = std::max(1U, description.LayerCount);
    switch (description.Type)
    {
    case TextureType::Cube:
        info.type = SDL_GPU_TEXTURETYPE_CUBE;
        layers = 6; // a cube is always six faces
        break;
    case TextureType::Array2D:
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        break;
    case TextureType::Texture2D:
    default:
        info.type = SDL_GPU_TEXTURETYPE_2D;
        layers = 1;
        break;
    }
    info.format = ToSdlFormat(description.PixelFormat);
    info.usage = ToSdlTextureUsage(description.Usage);
    if (description.AllowSampling)
    {
        info.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER; // e.g. sampleable depth for SSAO
    }
    info.width = description.Extent.Width;
    info.height = description.Extent.Height;
    info.layer_count_or_depth = layers;
    info.num_levels = std::max(1U, description.MipLevels);
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_Device, &info);
    if (texture == nullptr)
    {
        return Result<std::unique_ptr<Texture>>::Error(SdlError("SDL_CreateGPUTexture"));
    }

    if (!initialData.empty())
    {
        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = static_cast<Uint32>(initialData.size());
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(m_Device, &transferInfo);
        if (transfer)
        {
            void* map = SDL_MapGPUTransferBuffer(m_Device, transfer, false);
            if (map)
            {
                std::memcpy(map, initialData.data(), initialData.size());
                SDL_UnmapGPUTransferBuffer(m_Device, transfer);
                
                SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_Device);
                if (commands)
                {
                    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
                    SDL_GPUTextureTransferInfo source{};
                    source.transfer_buffer = transfer;
                    SDL_GPUTextureRegion dest{};
                    dest.texture = texture;
                    dest.w = info.width;
                    dest.h = info.height;
                    dest.d = 1;
                    SDL_UploadToGPUTexture(copy, &source, &dest, false);
                    SDL_EndGPUCopyPass(copy);
                    SDL_SubmitGPUCommandBuffer(commands);
                    SDL_WaitForGPUIdle(m_Device);
                }
            }
            SDL_ReleaseGPUTransferBuffer(m_Device, transfer);
        }
    }

    return Result<std::unique_ptr<Texture>>::Ok(
        std::make_unique<SdlTexture>(m_Device, texture, description, true));
}

Result<std::unique_ptr<Shader>> SdlDevice::CreateShader(
    const ShaderDesc& description,
    const std::span<const std::byte> bytecode)
{
    if (bytecode.empty())
    {
        return Result<std::unique_ptr<Shader>>::Error("SDL GPU shader bytecode is empty");
    }
    const SDL_GPUShaderStage stage =
        IsFragmentShader(description.DebugName) ? SDL_GPU_SHADERSTAGE_FRAGMENT : SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(bytecode.data());
    info.code_size = bytecode.size();
    info.entrypoint = description.EntryPoint.c_str();
    info.format = DetectShaderFormat(bytecode);
    info.stage = stage;
    info.num_uniform_buffers = description.NumUniformBuffers;
    info.num_samplers = description.NumSamplers;
    info.num_storage_buffers = description.NumStorageBuffers;
    SDL_GPUShader* shader = SDL_CreateGPUShader(m_Device, &info);
    if (shader == nullptr)
    {
        return Result<std::unique_ptr<Shader>>::Error(SdlError("SDL_CreateGPUShader"));
    }
    if (stage == SDL_GPU_SHADERSTAGE_VERTEX)
    {
        m_LastVertexShader = shader;
    }
    else
    {
        m_LastFragmentShader = shader;
    }
    return Result<std::unique_ptr<Shader>>::Ok(
        std::make_unique<SdlShader>(m_Device, shader, description, stage));
}

Result<std::unique_ptr<Pipeline>> SdlDevice::CreatePipeline(const PipelineDesc& description)
{
    // Prefer explicit PipelineDesc shaders. Do not silently fall back to
    // "last created" shaders when only one side is set — that previously
    // bound sky shaders onto unlit/gizmo pipelines.
    SDL_GPUShader* vertexShader = nullptr;
    SDL_GPUShader* fragmentShader = nullptr;
    if (description.VertexShader != nullptr || description.FragmentShader != nullptr)
    {
        auto* sdlVertex = dynamic_cast<SdlShader*>(description.VertexShader);
        auto* sdlFragment = dynamic_cast<SdlShader*>(description.FragmentShader);
        if (sdlVertex == nullptr || sdlFragment == nullptr ||
            sdlVertex->Stage() != SDL_GPU_SHADERSTAGE_VERTEX ||
            sdlFragment->Stage() != SDL_GPU_SHADERSTAGE_FRAGMENT)
        {
            return Result<std::unique_ptr<Pipeline>>::Error(
                "PipelineDesc must set both VertexShader and FragmentShader "
                "(SDL GPU vertex/fragment shaders)");
        }
        vertexShader = sdlVertex->Native();
        fragmentShader = sdlFragment->Native();
    }
    else
    {
        vertexShader = m_LastVertexShader;
        fragmentShader = m_LastFragmentShader;
    }
    if (vertexShader == nullptr || fragmentShader == nullptr)
    {
        return Result<std::unique_ptr<Pipeline>>::Error(
            "Create vertex and fragment shaders before creating an SDL GPU pipeline");
    }
    std::vector<SDL_GPUVertexBufferDescription> vertexBuffers;
    for (const auto& layout : description.VertexBufferLayouts)
    {
        SDL_GPUVertexBufferDescription bufDesc{};
        bufDesc.slot = layout.Slot;
        bufDesc.pitch = layout.Stride;
        bufDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        vertexBuffers.push_back(bufDesc);
    }

    std::vector<SDL_GPUVertexAttribute> attributes;
    for (const auto& attr : description.VertexAttributes)
    {
        SDL_GPUVertexAttribute sdlAttr{};
        sdlAttr.location = attr.Location;
        sdlAttr.buffer_slot = attr.BufferSlot;
        sdlAttr.format = ToSdlVertexElementFormat(attr.Format);
        sdlAttr.offset = attr.Offset;
        attributes.push_back(sdlAttr);
    }
    SDL_GPUColorTargetDescription colors[2]{};
    colors[0].format = ToSdlFormat(description.ColorFormat);
    colors[0].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colors[0].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    colors[0].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colors[0].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colors[0].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    colors[0].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    colors[0].blend_state.color_write_mask = 0xF;
    colors[0].blend_state.enable_color_write_mask = true;
    colors[0].blend_state.enable_blend = description.AlphaBlend;
    if (description.AlphaBlend)
    {
        colors[0].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colors[0].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colors[0].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    }

    const bool dualColor = description.ColorFormat1 != Format::Unknown;
    if (dualColor)
    {
        colors[1].format = ToSdlFormat(description.ColorFormat1);
        colors[1].blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colors[1].blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        colors[1].blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colors[1].blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colors[1].blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        colors[1].blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        colors[1].blend_state.color_write_mask = 0xF;
        colors[1].blend_state.enable_color_write_mask = true;
        colors[1].blend_state.enable_blend = false;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertexShader;
    info.fragment_shader = fragmentShader;
    if (description.UseVertexInput)
    {
        info.vertex_input_state = {vertexBuffers.data(), static_cast<Uint32>(vertexBuffers.size()), attributes.data(), static_cast<Uint32>(attributes.size())};
    }
    info.primitive_type = description.Topology == PrimitiveTopology::LineList
        ? SDL_GPU_PRIMITIVETYPE_LINELIST
        : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = description.Cull == CullMode::None
        ? SDL_GPU_CULLMODE_NONE
        : SDL_GPU_CULLMODE_BACK;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.rasterizer_state.enable_depth_bias = description.DepthBias;
    info.rasterizer_state.depth_bias_constant_factor = description.DepthBiasConstant;
    info.rasterizer_state.depth_bias_slope_factor = description.DepthBiasSlope;
    info.rasterizer_state.depth_bias_clamp = description.DepthBiasClamp;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.depth_stencil_state.back_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.back_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.back_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.back_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    info.depth_stencil_state.front_stencil_state.fail_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.front_stencil_state.pass_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.front_stencil_state.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
    info.depth_stencil_state.front_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    info.depth_stencil_state.enable_depth_test = description.DepthTest;
    info.depth_stencil_state.enable_depth_write = description.DepthWrite;
    info.target_info.color_target_descriptions = description.ColorFormat != Format::Unknown ? colors : nullptr;
    info.target_info.num_color_targets = description.ColorFormat != Format::Unknown ? (dualColor ? 2U : 1U) : 0;
    info.target_info.depth_stencil_format = ToSdlFormat(description.DepthFormat);
    info.target_info.has_depth_stencil_target = description.DepthFormat != Format::Unknown;
    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(m_Device, &info);
    if (pipeline == nullptr)
    {
        return Result<std::unique_ptr<Pipeline>>::Error(SdlError("SDL_CreateGPUGraphicsPipeline"));
    }
    return Result<std::unique_ptr<Pipeline>>::Ok(
        std::make_unique<SdlPipeline>(m_Device, pipeline, description));
}

Result<std::unique_ptr<RenderTarget>> SdlDevice::CreateRenderTarget(
    const TextureDesc& color,
    const TextureDesc* depth,
    const TextureDesc* color1)
{
    auto colorResult = CreateTexture(color);
    if (!colorResult)
    {
        return Result<std::unique_ptr<RenderTarget>>::Error(colorResult.ErrorMessage());
    }
    std::unique_ptr<SdlTexture> colorTexture(
        static_cast<SdlTexture*>(std::move(colorResult).Value().release()));
    std::unique_ptr<SdlTexture> depthTexture;
    if (depth != nullptr)
    {
        auto depthResult = CreateTexture(*depth);
        if (!depthResult)
        {
            return Result<std::unique_ptr<RenderTarget>>::Error(depthResult.ErrorMessage());
        }
        depthTexture.reset(static_cast<SdlTexture*>(std::move(depthResult).Value().release()));
    }
    std::unique_ptr<SdlTexture> color1Texture;
    if (color1 != nullptr)
    {
        auto color1Result = CreateTexture(*color1);
        if (!color1Result)
        {
            return Result<std::unique_ptr<RenderTarget>>::Error(color1Result.ErrorMessage());
        }
        color1Texture.reset(static_cast<SdlTexture*>(std::move(color1Result).Value().release()));
    }
    return Result<std::unique_ptr<RenderTarget>>::Ok(
        std::make_unique<SdlRenderTarget>(
            std::move(colorTexture), std::move(depthTexture), std::move(color1Texture), color.Extent));
}

std::unique_ptr<RenderTarget> SdlDevice::CreateRenderTargetView(
    Texture& color,
    Texture* depth,
    Texture* color1)
{
    return std::make_unique<SdlRenderTargetView>(&color, depth, color1, color.Description().Extent);
}

Result<std::unique_ptr<Swapchain>> SdlDevice::CreateSwapchain(void* nativeWindow)
{
    if (nativeWindow == nullptr)
    {
        return Result<std::unique_ptr<Swapchain>>::Error("Cannot create swapchain for null window");
    }
    return Result<std::unique_ptr<Swapchain>>::Ok(
        std::make_unique<SdlSwapchain>(m_Device, static_cast<SDL_Window*>(nativeWindow)));
}

std::unique_ptr<CommandList> SdlDevice::CreateCommandList()
{
    return std::make_unique<SdlCommandList>(m_Device);
}

void SdlDevice::Submit(CommandList& commands)
{
    auto* sdlCommands = dynamic_cast<SdlCommandList*>(&commands);
    if (sdlCommands == nullptr || !sdlCommands->Submit())
    {
        throw std::runtime_error(SdlError("SDL_SubmitGPUCommandBuffer"));
    }
}

void SdlDevice::WaitIdle()
{
    if (!SDL_WaitForGPUIdle(m_Device))
    {
        throw std::runtime_error(SdlError("SDL_WaitForGPUIdle"));
    }
}

bool SdlDevice::SupportsTextureFormat(const Format format, const bool sampledDepth) const
{
    const SDL_GPUTextureFormat sdlFormat = ToSdlFormat(format);
    if (sdlFormat == SDL_GPU_TEXTUREFORMAT_INVALID)
    {
        return false;
    }
    // Depth formats vary per GPU; SDL recommends querying support before
    // allocation. For SSAO the depth target must also be sampleable.
    SDL_GPUTextureUsageFlags usage = sampledDepth
        ? (SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
        : SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return SDL_GPUTextureSupportsFormat(m_Device, sdlFormat, SDL_GPU_TEXTURETYPE_2D, usage);
}

SdlDevice* AsSdlDevice(Device& device) noexcept
{
    return dynamic_cast<SdlDevice*>(&device);
}

SDL_GPUTexture* NativeTexture(Texture& texture) noexcept
{
    auto* result = dynamic_cast<SdlTexture*>(&texture);
    return result != nullptr ? result->Native() : nullptr;
}

SdlCommandList* AsSdlCommandList(CommandList& commands) noexcept
{
    return dynamic_cast<SdlCommandList*>(&commands);
}

SDL_GPUCommandBuffer* NativeCommandBuffer(CommandList& commands) noexcept
{
    auto* sdlCommands = AsSdlCommandList(commands);
    return sdlCommands != nullptr ? sdlCommands->NativeCommands() : nullptr;
}

SDL_GPURenderPass* NativeRenderPass(CommandList& commands) noexcept
{
    auto* sdlCommands = AsSdlCommandList(commands);
    return sdlCommands != nullptr ? sdlCommands->NativeRenderPass() : nullptr;
}

void PushVertexUniform(
    CommandList& commands,
    const std::uint32_t slot,
    const void* data,
    const std::uint32_t size)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    sdlCommands.PushVertexUniform(slot, data, size);
}

void PushFragmentUniform(
    CommandList& commands,
    const std::uint32_t slot,
    const void* data,
    const std::uint32_t size)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    sdlCommands.PushFragmentUniform(slot, data, size);
}

void BindIndexBuffer(CommandList& commands, Buffer& buffer)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    sdlCommands.BindIndexBuffer(buffer);
}

void DrawIndexed(CommandList& commands, const std::uint32_t indexCount)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    sdlCommands.DrawIndexed(indexCount);
}

void BindFragmentSamplers(CommandList& commands, std::uint32_t firstSlot, std::span<Texture*> textures, std::span<Sampler*> samplers)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    SDL_GPURenderPass* pass = sdlCommands.NativeRenderPass();
    if (pass == nullptr || textures.empty() || textures.size() != samplers.size())
    {
        return;
    }
    std::vector<SDL_GPUTextureSamplerBinding> bindings;
    bindings.reserve(textures.size());
    for (std::size_t i = 0; i < textures.size(); ++i)
    {
        SDL_GPUTextureSamplerBinding binding{};
        binding.texture = static_cast<SdlTexture*>(textures[i])->Native();
        binding.sampler = static_cast<SdlSampler*>(samplers[i])->Native();
        bindings.push_back(binding);
    }
    SDL_BindGPUFragmentSamplers(pass, firstSlot, bindings.data(), static_cast<Uint32>(bindings.size()));
}

void BindFragmentStorageBuffers(
    CommandList& commands, std::uint32_t firstSlot, std::span<Buffer* const> buffers)
{
    auto& sdlCommands = dynamic_cast<SdlCommandList&>(commands);
    SDL_GPURenderPass* pass = sdlCommands.NativeRenderPass();
    if (pass == nullptr || buffers.empty())
    {
        return;
    }
    std::vector<SDL_GPUBuffer*> natives;
    natives.reserve(buffers.size());
    for (Buffer* buffer : buffers)
    {
        natives.push_back(static_cast<SdlBuffer*>(buffer)->Native());
    }
    SDL_BindGPUFragmentStorageBuffers(
        pass, firstSlot, natives.data(), static_cast<Uint32>(natives.size()));
}

DynamicBufferUploader::DynamicBufferUploader(Device& device)
{
    auto* sdlDevice = AsSdlDevice(device);
    if (sdlDevice == nullptr)
    {
        throw std::invalid_argument("DynamicBufferUploader requires the SDL GPU RHI backend");
    }
    m_Device = sdlDevice->NativeDevice();
}

DynamicBufferUploader::~DynamicBufferUploader()
{
    if (m_Transfer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_Device, m_Transfer);
    }
}

bool DynamicBufferUploader::Upload(
    CommandList& commands, Buffer& destination, const std::span<const std::byte> data)
{
    if (data.empty())
    {
        return true;
    }
    auto* sdlCommands = AsSdlCommandList(commands);
    auto* sdlBuffer = dynamic_cast<SdlBuffer*>(&destination);
    if (sdlCommands == nullptr || sdlBuffer == nullptr ||
        sdlCommands->NativeCommands() == nullptr ||
        sdlCommands->NativeRenderPass() != nullptr ||
        data.size() > destination.Size() ||
        data.size() > std::numeric_limits<Uint32>::max())
    {
        return false;
    }
    if (m_Transfer == nullptr || m_Capacity < data.size())
    {
        if (m_Transfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_Device, m_Transfer);
            m_Transfer = nullptr;
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = static_cast<Uint32>(data.size());
        m_Transfer = SDL_CreateGPUTransferBuffer(m_Device, &info);
        if (m_Transfer == nullptr)
        {
            return false;
        }
        m_Capacity = data.size();
    }
    // Cycle so last frame's contents can still be in flight.
    void* mapped = SDL_MapGPUTransferBuffer(m_Device, m_Transfer, true);
    if (mapped == nullptr)
    {
        return false;
    }
    std::memcpy(mapped, data.data(), data.size());
    SDL_UnmapGPUTransferBuffer(m_Device, m_Transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(sdlCommands->NativeCommands());
    if (copy == nullptr)
    {
        return false;
    }
    SDL_GPUTransferBufferLocation source{};
    source.transfer_buffer = m_Transfer;
    SDL_GPUBufferRegion region{};
    region.buffer = sdlBuffer->Native();
    region.size = static_cast<Uint32>(data.size());
    SDL_UploadToGPUBuffer(copy, &source, &region, true);
    SDL_EndGPUCopyPass(copy);
    return true;
}
}

namespace fadix
{
std::unique_ptr<rhi::Device> CreateDeviceFromWindow(void* sdlWindow)
{
    if (sdlWindow == nullptr)
    {
        return nullptr;
    }
    SDL_PropertiesID properties = SDL_CreateProperties();
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    // Fadix uses three storage buffers, so D3D12 Tier-1 resource binding is sufficient.
    SDL_SetBooleanProperty(
        properties, SDL_PROP_GPU_DEVICE_CREATE_D3D12_ALLOW_FEWER_RESOURCE_SLOTS_BOOLEAN, true);
#ifndef NDEBUG
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
#endif
    SDL_GPUDevice* nativeDevice = SDL_CreateGPUDeviceWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (nativeDevice == nullptr)
    {
        return nullptr;
    }
    if (!SDL_ClaimWindowForGPUDevice(nativeDevice, static_cast<SDL_Window*>(sdlWindow)))
    {
        const std::string claimError = SDL_GetError();
        SDL_DestroyGPUDevice(nativeDevice);
        SDL_SetError("%s", claimError.c_str());
        return nullptr;
    }
    return std::make_unique<rhi::sdl::SdlDevice>(
        nativeDevice, true, static_cast<SDL_Window*>(sdlWindow));
}

std::unique_ptr<rhi::Device> AdoptSdlGpuDevice(void* nativeDevice)
{
    if (nativeDevice == nullptr)
    {
        return nullptr;
    }
    return std::make_unique<rhi::sdl::SdlDevice>(static_cast<SDL_GPUDevice*>(nativeDevice), false);
}

void* GetNativeDeviceHandle(rhi::Device& device) noexcept
{
    auto* sdlDevice = rhi::sdl::AsSdlDevice(device);
    return sdlDevice != nullptr ? sdlDevice->NativeDevice() : nullptr;
}

void* GetNativeTextureHandle(rhi::Texture& texture) noexcept
{
    return rhi::sdl::NativeTexture(texture);
}
}
