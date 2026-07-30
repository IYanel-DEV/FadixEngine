#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "rhi/d3d11/D3D11Rhi.hpp"

#include <SDL3/SDL.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace fadix::rhi::d3d11
{
using Microsoft::WRL::ComPtr;

namespace
{
[[nodiscard]] std::string HrError(const char* operation, const HRESULT result)
{
    char code[16]{};
    std::snprintf(code, sizeof(code), "%08lX", static_cast<unsigned long>(result));
    return std::string{operation} + " failed (0x" + code + ")";
}

[[nodiscard]] bool IsDepthFormat(const Format format)
{
    return format == Format::D24UnormS8Uint || format == Format::D32Float ||
        format == Format::D16Unorm;
}

[[nodiscard]] DXGI_FORMAT ResourceFormat(const Format format, const bool sampledDepth)
{
    switch (format)
    {
    case Format::R8G8B8A8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case Format::R8G8B8A8UnormSrgb: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case Format::B8G8R8A8Unorm: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case Format::R16G16Float: return DXGI_FORMAT_R16G16_FLOAT;
    case Format::R16G16B16A16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case Format::D24UnormS8Uint:
        return sampledDepth ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32Float:
        return sampledDepth ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
    case Format::D16Unorm:
        return sampledDepth ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] DXGI_FORMAT ShaderViewFormat(const Format format)
{
    switch (format)
    {
    case Format::D24UnormS8Uint: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case Format::D32Float: return DXGI_FORMAT_R32_FLOAT;
    case Format::D16Unorm: return DXGI_FORMAT_R16_UNORM;
    default: return ResourceFormat(format, false);
    }
}

[[nodiscard]] DXGI_FORMAT DepthViewFormat(const Format format)
{
    switch (format)
    {
    case Format::D24UnormS8Uint: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32Float: return DXGI_FORMAT_D32_FLOAT;
    case Format::D16Unorm: return DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

[[nodiscard]] UINT BytesPerPixel(const Format format)
{
    switch (format)
    {
    case Format::R16G16Float: return 4;
    case Format::R16G16B16A16Float: return 8;
    case Format::D16Unorm: return 2;
    default: return 4;
    }
}

[[nodiscard]] DXGI_FORMAT VertexFormat(const VertexElementFormat format)
{
    switch (format)
    {
    case VertexElementFormat::Float2: return DXGI_FORMAT_R32G32_FLOAT;
    case VertexElementFormat::Float3: return DXGI_FORMAT_R32G32B32_FLOAT;
    case VertexElementFormat::Float4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VertexElementFormat::Color: return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

[[nodiscard]] bool IsFragmentShader(const std::string& name)
{
    if (name.size() >= 2 &&
        (name.compare(name.size() - 2, 2, "PS") == 0 ||
         name.compare(name.size() - 2, 2, "Ps") == 0))
    {
        return true;
    }
    return name.find("fragment") != std::string::npos ||
        name.find("Fragment") != std::string::npos || name.find("pixel") != std::string::npos ||
        name.find("Pixel") != std::string::npos;
}

class D3D11Buffer final : public Buffer
{
public:
    D3D11Buffer(
        ID3D11DeviceContext* context,
        ComPtr<ID3D11Buffer> buffer,
        ComPtr<ID3D11ShaderResourceView> view,
        const BufferDesc& description)
        : m_Context(context), m_Buffer(std::move(buffer)), m_View(std::move(view)),
          m_Description(description)
    {
    }

    [[nodiscard]] std::size_t Size() const noexcept override { return m_Description.Size; }
    void Upload(const std::span<const std::byte> data, const std::size_t offset) override
    {
        if (data.empty())
        {
            return;
        }
        if (offset > m_Description.Size || data.size() > m_Description.Size - offset)
        {
            throw std::out_of_range("D3D11 buffer upload exceeds allocation");
        }
        D3D11_BOX box{};
        box.left = static_cast<UINT>(offset);
        box.right = static_cast<UINT>(offset + data.size());
        box.bottom = box.back = 1;
        m_Context->UpdateSubresource(m_Buffer.Get(), 0, &box, data.data(), 0, 0);
    }

    [[nodiscard]] ID3D11Buffer* Native() const noexcept { return m_Buffer.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* View() const noexcept { return m_View.Get(); }

private:
    ID3D11DeviceContext* m_Context;
    ComPtr<ID3D11Buffer> m_Buffer;
    ComPtr<ID3D11ShaderResourceView> m_View;
    BufferDesc m_Description;
};

class D3D11Texture final : public Texture
{
public:
    D3D11Texture(
        ComPtr<ID3D11Texture2D> texture,
        ComPtr<ID3D11RenderTargetView> renderView,
        ComPtr<ID3D11DepthStencilView> depthView,
        ComPtr<ID3D11ShaderResourceView> shaderView,
        const TextureDesc& description)
        : m_Texture(std::move(texture)), m_RenderView(std::move(renderView)),
          m_DepthView(std::move(depthView)), m_ShaderView(std::move(shaderView)),
          m_Description(description)
    {
    }

    [[nodiscard]] const TextureDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] ID3D11Texture2D* Native() const noexcept { return m_Texture.Get(); }
    [[nodiscard]] ID3D11RenderTargetView* RenderView() const noexcept { return m_RenderView.Get(); }
    [[nodiscard]] ID3D11DepthStencilView* DepthView() const noexcept { return m_DepthView.Get(); }
    [[nodiscard]] ID3D11ShaderResourceView* ShaderView() const noexcept { return m_ShaderView.Get(); }

private:
    ComPtr<ID3D11Texture2D> m_Texture;
    ComPtr<ID3D11RenderTargetView> m_RenderView;
    ComPtr<ID3D11DepthStencilView> m_DepthView;
    ComPtr<ID3D11ShaderResourceView> m_ShaderView;
    TextureDesc m_Description;
};

class D3D11Sampler final : public Sampler
{
public:
    explicit D3D11Sampler(ComPtr<ID3D11SamplerState> sampler) : m_Sampler(std::move(sampler)) {}
    [[nodiscard]] ID3D11SamplerState* Native() const noexcept { return m_Sampler.Get(); }

private:
    ComPtr<ID3D11SamplerState> m_Sampler;
};

class D3D11Shader final : public Shader
{
public:
    D3D11Shader(
        ShaderDesc description,
        ComPtr<ID3D11VertexShader> vertex,
        ComPtr<ID3D11PixelShader> fragment,
        std::vector<std::byte> bytecode)
        : m_Description(std::move(description)), m_Vertex(std::move(vertex)),
          m_Fragment(std::move(fragment)), m_Bytecode(std::move(bytecode))
    {
    }

    [[nodiscard]] const ShaderDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] ID3D11VertexShader* Vertex() const noexcept { return m_Vertex.Get(); }
    [[nodiscard]] ID3D11PixelShader* Fragment() const noexcept { return m_Fragment.Get(); }
    [[nodiscard]] const std::vector<std::byte>& Bytecode() const noexcept { return m_Bytecode; }

private:
    ShaderDesc m_Description;
    ComPtr<ID3D11VertexShader> m_Vertex;
    ComPtr<ID3D11PixelShader> m_Fragment;
    std::vector<std::byte> m_Bytecode;
};

class D3D11Pipeline final : public Pipeline
{
public:
    D3D11Pipeline(
        PipelineDesc description,
        D3D11Shader* vertex,
        D3D11Shader* fragment,
        ComPtr<ID3D11InputLayout> input,
        ComPtr<ID3D11RasterizerState> rasterizer,
        ComPtr<ID3D11DepthStencilState> depth,
        ComPtr<ID3D11BlendState> blend)
        : m_Description(std::move(description)), m_Vertex(vertex), m_Fragment(fragment),
          m_Input(std::move(input)), m_Rasterizer(std::move(rasterizer)),
          m_Depth(std::move(depth)), m_Blend(std::move(blend))
    {
    }

    [[nodiscard]] const PipelineDesc& Description() const noexcept override { return m_Description; }
    [[nodiscard]] D3D11Shader* Vertex() const noexcept { return m_Vertex; }
    [[nodiscard]] D3D11Shader* Fragment() const noexcept { return m_Fragment; }
    [[nodiscard]] ID3D11InputLayout* Input() const noexcept { return m_Input.Get(); }
    [[nodiscard]] ID3D11RasterizerState* Rasterizer() const noexcept { return m_Rasterizer.Get(); }
    [[nodiscard]] ID3D11DepthStencilState* Depth() const noexcept { return m_Depth.Get(); }
    [[nodiscard]] ID3D11BlendState* Blend() const noexcept { return m_Blend.Get(); }

private:
    PipelineDesc m_Description;
    D3D11Shader* m_Vertex;
    D3D11Shader* m_Fragment;
    ComPtr<ID3D11InputLayout> m_Input;
    ComPtr<ID3D11RasterizerState> m_Rasterizer;
    ComPtr<ID3D11DepthStencilState> m_Depth;
    ComPtr<ID3D11BlendState> m_Blend;
};

class D3D11RenderTarget final : public RenderTarget
{
public:
    D3D11RenderTarget(
        std::unique_ptr<D3D11Texture> color,
        std::unique_ptr<D3D11Texture> depth,
        std::unique_ptr<D3D11Texture> color1,
        const Extent2D extent)
        : m_Color(std::move(color)), m_Depth(std::move(depth)), m_Color1(std::move(color1)),
          m_Extent(extent)
    {
    }
    [[nodiscard]] Texture* ColorTexture() const noexcept override { return m_Color.get(); }
    [[nodiscard]] Texture* ColorTexture1() const noexcept override { return m_Color1.get(); }
    [[nodiscard]] Texture* DepthTexture() const noexcept override { return m_Depth.get(); }
    [[nodiscard]] Extent2D Extent() const noexcept override { return m_Extent; }

private:
    std::unique_ptr<D3D11Texture> m_Color;
    std::unique_ptr<D3D11Texture> m_Depth;
    std::unique_ptr<D3D11Texture> m_Color1;
    Extent2D m_Extent;
};

class D3D11RenderTargetView final : public RenderTarget
{
public:
    D3D11RenderTargetView(Texture* color, Texture* depth, Texture* color1, const Extent2D extent)
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

class D3D11CommandList final : public CommandList
{
public:
    explicit D3D11CommandList(ID3D11Device* device, ID3D11DeviceContext* context)
        : m_Device(device), m_Context(context)
    {
    }

    void Begin() override
    {
        if (m_Recording)
        {
            throw std::logic_error("D3D11 command list is already recording");
        }
        m_Recording = true;
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

    void BindPipeline(Pipeline& pipeline) override
    {
        m_Pipeline = dynamic_cast<D3D11Pipeline*>(&pipeline);
        if (m_Pipeline == nullptr || !m_InPass)
        {
            throw std::logic_error("Invalid D3D11 pipeline binding");
        }
        m_Context->IASetInputLayout(m_Pipeline->Input());
        m_Context->VSSetShader(m_Pipeline->Vertex()->Vertex(), nullptr, 0);
        m_Context->PSSetShader(m_Pipeline->Fragment()->Fragment(), nullptr, 0);
        m_Context->RSSetState(m_Pipeline->Rasterizer());
        m_Context->OMSetDepthStencilState(m_Pipeline->Depth(), 0);
        const float blendFactor[4]{};
        m_Context->OMSetBlendState(m_Pipeline->Blend(), blendFactor, 0xFFFFFFFFU);
        m_Context->IASetPrimitiveTopology(
            m_Pipeline->Description().Topology == PrimitiveTopology::LineList
                ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    void BindVertexBuffer(Buffer& buffer) override
    {
        auto& native = dynamic_cast<D3D11Buffer&>(buffer);
        UINT stride = 0;
        if (m_Pipeline != nullptr && !m_Pipeline->Description().VertexBufferLayouts.empty())
        {
            stride = m_Pipeline->Description().VertexBufferLayouts.front().Stride;
        }
        constexpr UINT offset = 0;
        ID3D11Buffer* value = native.Native();
        m_Context->IASetVertexBuffers(0, 1, &value, &stride, &offset);
    }

    void BindIndexBuffer(Buffer& buffer) override
    {
        auto& native = dynamic_cast<D3D11Buffer&>(buffer);
        m_Context->IASetIndexBuffer(native.Native(), DXGI_FORMAT_R32_UINT, 0);
    }

    void BindFragmentSamplers(
        const std::uint32_t firstSlot,
        const std::span<Texture*> textures,
        const std::span<Sampler*> samplers) override
    {
        if (textures.empty() || textures.size() != samplers.size())
        {
            return;
        }
        std::vector<ID3D11ShaderResourceView*> views;
        std::vector<ID3D11SamplerState*> states;
        views.reserve(textures.size());
        states.reserve(samplers.size());
        for (std::size_t index = 0; index < textures.size(); ++index)
        {
            views.push_back(dynamic_cast<D3D11Texture&>(*textures[index]).ShaderView());
            states.push_back(dynamic_cast<D3D11Sampler&>(*samplers[index]).Native());
        }
        m_Context->PSSetShaderResources(firstSlot, static_cast<UINT>(views.size()), views.data());
        m_Context->PSSetSamplers(firstSlot, static_cast<UINT>(states.size()), states.data());
    }

    void BindFragmentStorageBuffers(
        const std::uint32_t firstSlot,
        const std::span<Buffer* const> buffers) override
    {
        if (buffers.empty())
        {
            return;
        }
        const UINT samplerCount = m_Pipeline != nullptr
            ? m_Pipeline->Fragment()->Description().NumSamplers
            : 0;
        std::vector<ID3D11ShaderResourceView*> views;
        views.reserve(buffers.size());
        for (Buffer* buffer : buffers)
        {
            views.push_back(dynamic_cast<D3D11Buffer&>(*buffer).View());
        }
        m_Context->PSSetShaderResources(
            samplerCount + firstSlot, static_cast<UINT>(views.size()), views.data());
    }

    void PushVertexUniform(
        const std::uint32_t slot, const void* data, const std::uint32_t size) override
    {
        PushUniform(true, slot, data, size);
    }

    void PushFragmentUniform(
        const std::uint32_t slot, const void* data, const std::uint32_t size) override
    {
        PushUniform(false, slot, data, size);
    }

    void Draw(const std::uint32_t vertexCount, const std::uint32_t firstVertex) override
    {
        m_Context->Draw(vertexCount, firstVertex);
    }

    void DrawIndexed(
        const std::uint32_t indexCount,
        const std::uint32_t firstIndex,
        const std::int32_t vertexOffset) override
    {
        m_Context->DrawIndexed(indexCount, firstIndex, vertexOffset);
    }

    void EndRenderPass() override
    {
        if (!m_InPass)
        {
            return;
        }
        std::array<ID3D11ShaderResourceView*, 16> empty{};
        m_Context->PSSetShaderResources(0, static_cast<UINT>(empty.size()), empty.data());
        m_InPass = false;
        m_Pipeline = nullptr;
    }

    void End() override
    {
        if (m_InPass)
        {
            throw std::logic_error("D3D11 command list ended inside a render pass");
        }
        m_Recording = false;
    }

private:
    void BeginRenderPassInternal(
        RenderTarget& target,
        const bool clear,
        const float clearR,
        const float clearG,
        const float clearB,
        const float clearA)
    {
        if (!m_Recording || m_InPass)
        {
            throw std::logic_error("Invalid D3D11 render pass state");
        }
        auto* color = dynamic_cast<D3D11Texture*>(target.ColorTexture());
        auto* color1 = dynamic_cast<D3D11Texture*>(target.ColorTexture1());
        auto* depth = dynamic_cast<D3D11Texture*>(target.DepthTexture());
        if (color == nullptr || color->RenderView() == nullptr)
        {
            throw std::logic_error("Invalid D3D11 color target");
        }
        std::array<ID3D11RenderTargetView*, 2> views{color->RenderView(), nullptr};
        UINT viewCount = 1;
        if (color1 != nullptr)
        {
            views[1] = color1->RenderView();
            viewCount = 2;
        }
        m_Context->OMSetRenderTargets(
            viewCount, views.data(), depth != nullptr ? depth->DepthView() : nullptr);
        const Extent2D extent = target.Extent();
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(extent.Width);
        viewport.Height = static_cast<float>(extent.Height);
        viewport.MaxDepth = 1.0F;
        m_Context->RSSetViewports(1, &viewport);
        if (clear)
        {
            const float colorValue[4]{clearR, clearG, clearB, clearA};
            m_Context->ClearRenderTargetView(views[0], colorValue);
            if (views[1] != nullptr)
            {
                const float zero[4]{};
                m_Context->ClearRenderTargetView(views[1], zero);
            }
            if (depth != nullptr && depth->DepthView() != nullptr)
            {
                const UINT flags = depth->Description().PixelFormat == Format::D24UnormS8Uint
                    ? D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL
                    : D3D11_CLEAR_DEPTH;
                m_Context->ClearDepthStencilView(depth->DepthView(), flags, 1.0F, 0);
            }
        }
        m_InPass = true;
    }

    void PushUniform(
        const bool vertex, const std::uint32_t slot, const void* data, const std::uint32_t size)
    {
        if (data == nullptr || size == 0 || size > D3D11_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16U)
        {
            return;
        }
        auto& buffers = vertex ? m_VertexConstants : m_FragmentConstants;
        if (slot >= buffers.size())
        {
            buffers.resize(slot + 1);
        }
        const UINT aligned = (size + 15U) & ~15U;
        if (buffers[slot].Size < aligned)
        {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = aligned;
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ComPtr<ID3D11Buffer> buffer;
            const HRESULT result = m_Device->CreateBuffer(&description, nullptr, &buffer);
            if (FAILED(result))
            {
                throw std::runtime_error(HrError("Create constant buffer", result));
            }
            buffers[slot] = {std::move(buffer), aligned};
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = m_Context->Map(
            buffers[slot].Buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(result))
        {
            throw std::runtime_error(HrError("Map constant buffer", result));
        }
        std::memcpy(mapped.pData, data, size);
        if (aligned > size)
        {
            std::memset(static_cast<std::byte*>(mapped.pData) + size, 0, aligned - size);
        }
        m_Context->Unmap(buffers[slot].Buffer.Get(), 0);
        ID3D11Buffer* buffer = buffers[slot].Buffer.Get();
        if (vertex)
        {
            m_Context->VSSetConstantBuffers(slot, 1, &buffer);
        }
        else
        {
            m_Context->PSSetConstantBuffers(slot, 1, &buffer);
        }
    }

    struct ConstantBuffer
    {
        ComPtr<ID3D11Buffer> Buffer;
        UINT Size{0};
    };

    ID3D11Device* m_Device;
    ID3D11DeviceContext* m_Context;
    D3D11Pipeline* m_Pipeline{nullptr};
    std::vector<ConstantBuffer> m_VertexConstants;
    std::vector<ConstantBuffer> m_FragmentConstants;
    bool m_Recording{false};
    bool m_InPass{false};
};

class D3D11Swapchain final : public Swapchain
{
public:
    explicit D3D11Swapchain(D3D11Device& device) : m_Device(device) {}
    [[nodiscard]] Result<void> Resize(const Extent2D extent) override
    {
        return m_Device.ResizeBackbuffer(extent.Width, extent.Height)
            ? Result<void>::Ok()
            : Result<void>::Error("D3D11 swapchain resize failed");
    }
    [[nodiscard]] RenderTarget* AcquireNextTarget() override { return nullptr; }
    [[nodiscard]] Result<void> Present() override
    {
        m_Device.Present();
        return Result<void>::Ok();
    }

private:
    D3D11Device& m_Device;
};

[[nodiscard]] D3D11_TEXTURE_ADDRESS_MODE AddressMode(const WrapMode mode)
{
    switch (mode)
    {
    case WrapMode::Repeat: return D3D11_TEXTURE_ADDRESS_WRAP;
    case WrapMode::Clamp: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case WrapMode::Mirror: return D3D11_TEXTURE_ADDRESS_MIRROR;
    }
    return D3D11_TEXTURE_ADDRESS_WRAP;
}
}

struct D3D11Device::Impl
{
    ComPtr<ID3D11Device> Device;
    ComPtr<ID3D11DeviceContext> Context;
    ComPtr<IDXGISwapChain1> Swapchain;
    ComPtr<ID3D11RenderTargetView> BackbufferView;
    void* Window{nullptr};
};

D3D11Device::D3D11Device(void* sdlWindow) : m_Impl(std::make_unique<Impl>())
{
    auto* window = static_cast<SDL_Window*>(sdlWindow);
    if (window == nullptr)
    {
        throw std::invalid_argument("D3D11 requires an SDL window");
    }
    void* hwnd = SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (hwnd == nullptr)
    {
        throw std::runtime_error("SDL window has no Win32 HWND");
    }

    constexpr std::array<D3D_FEATURE_LEVEL, 2> levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL selected{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels.data(),
        static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION,
        &m_Impl->Device,
        &selected,
        &m_Impl->Context);
    if (result == E_INVALIDARG)
    {
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels.data() + 1,
            static_cast<UINT>(levels.size() - 1),
            D3D11_SDK_VERSION,
            &m_Impl->Device,
            &selected,
            &m_Impl->Context);
    }
    if (FAILED(result))
    {
        m_Impl->Device.Reset();
        m_Impl->Context.Reset();
        result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            flags,
            levels.data() + 1,
            static_cast<UINT>(levels.size() - 1),
            D3D11_SDK_VERSION,
            &m_Impl->Device,
            &selected,
            &m_Impl->Context);
        if (SUCCEEDED(result))
        {
            SDL_Log("[Fadix] Hardware D3D11 unavailable; using built-in WARP renderer");
        }
    }
    if (FAILED(result))
    {
        throw std::runtime_error(HrError("D3D11CreateDevice", result));
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_Impl->Device.As(&dxgiDevice)) ||
        FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
    {
        throw std::runtime_error("Could not obtain the D3D11 DXGI factory");
    }

    int width = 1;
    int height = 1;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    DXGI_SWAP_CHAIN_DESC1 swapDescription{};
    swapDescription.Width = static_cast<UINT>(std::max(width, 1));
    swapDescription.Height = static_cast<UINT>(std::max(height, 1));
    swapDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDescription.SampleDesc.Count = 1;
    swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDescription.BufferCount = 2;
    swapDescription.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    result = factory->CreateSwapChainForHwnd(
        m_Impl->Device.Get(),
        static_cast<HWND>(hwnd),
        &swapDescription,
        nullptr,
        nullptr,
        &m_Impl->Swapchain);
    if (FAILED(result))
    {
        throw std::runtime_error(HrError("CreateSwapChainForHwnd", result));
    }
    factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);
    m_Impl->Window = window;
    if (!ResizeBackbuffer(swapDescription.Width, swapDescription.Height))
    {
        throw std::runtime_error("Could not create the D3D11 backbuffer view");
    }
    SDL_Log(
        "[Fadix] Direct3D 11 compatibility renderer ready (feature level 0x%X)",
        static_cast<unsigned>(selected));
}

D3D11Device::~D3D11Device()
{
    if (m_Impl && m_Impl->Context)
    {
        m_Impl->Context->ClearState();
        m_Impl->Context->Flush();
    }
}

ID3D11Device* D3D11Device::NativeDevice() const noexcept { return m_Impl->Device.Get(); }
ID3D11DeviceContext* D3D11Device::NativeContext() const noexcept { return m_Impl->Context.Get(); }
ID3D11RenderTargetView* D3D11Device::BackbufferView() const noexcept
{
    return m_Impl->BackbufferView.Get();
}

bool D3D11Device::ResizeBackbuffer(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0 || height == 0)
    {
        return true;
    }
    m_Impl->Context->OMSetRenderTargets(0, nullptr, nullptr);
    m_Impl->BackbufferView.Reset();
    HRESULT result = m_Impl->Swapchain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(result))
    {
        SDL_Log("[Fadix] %s", HrError("ResizeBuffers", result).c_str());
        return false;
    }
    ComPtr<ID3D11Texture2D> backbuffer;
    result = m_Impl->Swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (SUCCEEDED(result))
    {
        result = m_Impl->Device->CreateRenderTargetView(
            backbuffer.Get(), nullptr, &m_Impl->BackbufferView);
    }
    if (FAILED(result))
    {
        SDL_Log("[Fadix] %s", HrError("Create backbuffer view", result).c_str());
        return false;
    }
    return true;
}

void D3D11Device::Present(const bool vsync)
{
    const HRESULT result = m_Impl->Swapchain->Present(vsync ? 1U : 0U, 0);
    if (FAILED(result))
    {
        SDL_Log("[Fadix] %s", HrError("D3D11 Present", result).c_str());
    }
}

void D3D11Device::PresentTexture(Texture* source, const bool vsync)
{
    if (auto* texture = dynamic_cast<D3D11Texture*>(source); texture != nullptr)
    {
        ComPtr<ID3D11Texture2D> backbuffer;
        if (SUCCEEDED(m_Impl->Swapchain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))))
        {
            D3D11_TEXTURE2D_DESC sourceDescription{};
            D3D11_TEXTURE2D_DESC destinationDescription{};
            texture->Native()->GetDesc(&sourceDescription);
            backbuffer->GetDesc(&destinationDescription);
            if (sourceDescription.Width == destinationDescription.Width &&
                sourceDescription.Height == destinationDescription.Height &&
                sourceDescription.Format == destinationDescription.Format)
            {
                m_Impl->Context->CopyResource(backbuffer.Get(), texture->Native());
            }
        }
    }
    Present(vsync);
}

Result<std::unique_ptr<Buffer>> D3D11Device::CreateBuffer(const BufferDesc& description)
{
    if (description.Size == 0 || description.Size > std::numeric_limits<UINT>::max())
    {
        return Result<std::unique_ptr<Buffer>>::Error("Invalid D3D11 buffer size");
    }
    D3D11_BUFFER_DESC native{};
    native.ByteWidth = static_cast<UINT>(description.Size);
    native.Usage = D3D11_USAGE_DEFAULT;
    switch (description.Usage)
    {
    case BufferUsage::Vertex: native.BindFlags = D3D11_BIND_VERTEX_BUFFER; break;
    case BufferUsage::Index: native.BindFlags = D3D11_BIND_INDEX_BUFFER; break;
    case BufferUsage::Uniform: native.BindFlags = D3D11_BIND_CONSTANT_BUFFER; break;
    case BufferUsage::Storage:
        if (description.StructureStride == 0 ||
            description.Size % description.StructureStride != 0)
        {
            return Result<std::unique_ptr<Buffer>>::Error(
                "D3D11 storage buffer requires a valid StructureStride");
        }
        native.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        native.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        native.StructureByteStride = description.StructureStride;
        break;
    case BufferUsage::Staging: break;
    }
    ComPtr<ID3D11Buffer> buffer;
    const HRESULT result = m_Impl->Device->CreateBuffer(&native, nullptr, &buffer);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Buffer>>::Error(HrError("CreateBuffer", result));
    }

    ComPtr<ID3D11ShaderResourceView> view;
    if (description.Usage == BufferUsage::Storage)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDescription{};
        viewDescription.Format = DXGI_FORMAT_UNKNOWN;
        viewDescription.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        viewDescription.Buffer.NumElements =
            static_cast<UINT>(description.Size / description.StructureStride);
        const HRESULT viewResult = m_Impl->Device->CreateShaderResourceView(
            buffer.Get(), &viewDescription, &view);
        if (FAILED(viewResult))
        {
            return Result<std::unique_ptr<Buffer>>::Error(
                HrError("Create buffer shader view", viewResult));
        }
    }
    return Result<std::unique_ptr<Buffer>>::Ok(std::make_unique<D3D11Buffer>(
        m_Impl->Context.Get(), std::move(buffer), std::move(view), description));
}

Result<std::unique_ptr<Texture>> D3D11Device::CreateTexture(
    const TextureDesc& description, const std::span<const std::byte> initialData)
{
    if (description.Extent.Width == 0 || description.Extent.Height == 0)
    {
        return Result<std::unique_ptr<Texture>>::Error("Invalid D3D11 texture extent");
    }
    const bool depth = IsDepthFormat(description.PixelFormat);
    const bool sampled = description.Usage == TextureUsage::Sampled ||
        description.Usage == TextureUsage::RenderTarget || description.AllowSampling;
    D3D11_TEXTURE2D_DESC native{};
    native.Width = description.Extent.Width;
    native.Height = description.Extent.Height;
    native.MipLevels = description.MipLevels;
    native.ArraySize = description.Type == TextureType::Cube
        ? 6U
        : (description.Type == TextureType::Array2D ? std::max(1U, description.LayerCount) : 1U);
    native.Format = ResourceFormat(description.PixelFormat, depth && sampled);
    native.SampleDesc.Count = 1;
    native.Usage = D3D11_USAGE_DEFAULT;
    if (description.Usage == TextureUsage::RenderTarget)
    {
        native.BindFlags |= D3D11_BIND_RENDER_TARGET;
    }
    if (description.Usage == TextureUsage::DepthStencil)
    {
        native.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    }
    if (sampled)
    {
        native.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (description.Type == TextureType::Cube)
    {
        native.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    }
    if (native.Format == DXGI_FORMAT_UNKNOWN)
    {
        return Result<std::unique_ptr<Texture>>::Error("Unsupported D3D11 texture format");
    }

    D3D11_SUBRESOURCE_DATA data{};
    D3D11_SUBRESOURCE_DATA* dataPointer = nullptr;
    if (!initialData.empty() && native.ArraySize == 1 && native.MipLevels == 1)
    {
        data.pSysMem = initialData.data();
        data.SysMemPitch = native.Width * BytesPerPixel(description.PixelFormat);
        dataPointer = &data;
    }
    ComPtr<ID3D11Texture2D> texture;
    HRESULT result = m_Impl->Device->CreateTexture2D(&native, dataPointer, &texture);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Texture>>::Error(HrError("CreateTexture2D", result));
    }

    ComPtr<ID3D11RenderTargetView> renderView;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11ShaderResourceView> shaderView;
    if ((native.BindFlags & D3D11_BIND_RENDER_TARGET) != 0)
    {
        result = m_Impl->Device->CreateRenderTargetView(texture.Get(), nullptr, &renderView);
    }
    if (SUCCEEDED(result) && (native.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DepthViewFormat(description.PixelFormat);
        view.ViewDimension = native.ArraySize > 1
            ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY
            : D3D11_DSV_DIMENSION_TEXTURE2D;
        if (native.ArraySize > 1)
        {
            view.Texture2DArray.ArraySize = native.ArraySize;
        }
        result = m_Impl->Device->CreateDepthStencilView(texture.Get(), &view, &depthView);
    }
    if (SUCCEEDED(result) && (native.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = ShaderViewFormat(description.PixelFormat);
        view.ViewDimension = description.Type == TextureType::Cube
            ? D3D11_SRV_DIMENSION_TEXTURECUBE
            : (native.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY
                                    : D3D11_SRV_DIMENSION_TEXTURE2D);
        if (view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
        {
            view.Texture2D.MipLevels = native.MipLevels;
        }
        else if (view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
        {
            view.Texture2DArray.MipLevels = native.MipLevels;
            view.Texture2DArray.ArraySize = native.ArraySize;
        }
        else
        {
            view.TextureCube.MipLevels = native.MipLevels;
        }
        result = m_Impl->Device->CreateShaderResourceView(texture.Get(), &view, &shaderView);
    }
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Texture>>::Error(HrError("Create texture view", result));
    }
    return Result<std::unique_ptr<Texture>>::Ok(std::make_unique<D3D11Texture>(
        std::move(texture),
        std::move(renderView),
        std::move(depthView),
        std::move(shaderView),
        description));
}

Result<std::unique_ptr<Sampler>> D3D11Device::CreateSampler(const SamplerDesc& description)
{
    D3D11_SAMPLER_DESC native{};
    if (description.EnableAnisotropy)
    {
        native.Filter = D3D11_FILTER_ANISOTROPIC;
        native.MaxAnisotropy = static_cast<UINT>(std::clamp(description.MaxAnisotropy, 1.0F, 16.0F));
    }
    else if (description.MinFilter == FilterMode::Nearest &&
             description.MagFilter == FilterMode::Nearest &&
             description.MipFilter == FilterMode::Nearest)
    {
        native.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    }
    else
    {
        native.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    }
    native.AddressU = AddressMode(description.AddressU);
    native.AddressV = AddressMode(description.AddressV);
    native.AddressW = AddressMode(description.AddressW);
    native.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    const HRESULT result = m_Impl->Device->CreateSamplerState(&native, &sampler);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Sampler>>::Error(HrError("CreateSamplerState", result));
    }
    return Result<std::unique_ptr<Sampler>>::Ok(
        std::make_unique<D3D11Sampler>(std::move(sampler)));
}

Result<std::unique_ptr<Shader>> D3D11Device::CreateShader(
    const ShaderDesc& description, const std::span<const std::byte> bytecode)
{
    if (bytecode.empty())
    {
        return Result<std::unique_ptr<Shader>>::Error("D3D11 shader bytecode is empty");
    }
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> fragment;
    HRESULT result = S_OK;
    if (IsFragmentShader(description.DebugName))
    {
        result = m_Impl->Device->CreatePixelShader(
            bytecode.data(), bytecode.size(), nullptr, &fragment);
    }
    else
    {
        result = m_Impl->Device->CreateVertexShader(
            bytecode.data(), bytecode.size(), nullptr, &vertex);
    }
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Shader>>::Error(HrError("Create shader", result));
    }
    return Result<std::unique_ptr<Shader>>::Ok(std::make_unique<D3D11Shader>(
        description,
        std::move(vertex),
        std::move(fragment),
        std::vector<std::byte>(bytecode.begin(), bytecode.end())));
}

Result<std::unique_ptr<Pipeline>> D3D11Device::CreatePipeline(const PipelineDesc& description)
{
    auto* vertex = dynamic_cast<D3D11Shader*>(description.VertexShader);
    auto* fragment = dynamic_cast<D3D11Shader*>(description.FragmentShader);
    if (vertex == nullptr || fragment == nullptr || vertex->Vertex() == nullptr ||
        fragment->Fragment() == nullptr)
    {
        return Result<std::unique_ptr<Pipeline>>::Error(
            "D3D11 pipeline requires explicit vertex and fragment shaders");
    }

    ComPtr<ID3D11InputLayout> input;
    if (description.UseVertexInput && !description.VertexAttributes.empty())
    {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
        elements.reserve(description.VertexAttributes.size());
        for (const VertexAttribute& attribute : description.VertexAttributes)
        {
            D3D11_INPUT_ELEMENT_DESC element{};
            element.SemanticName = "TEXCOORD";
            element.SemanticIndex = attribute.Location;
            element.Format = VertexFormat(attribute.Format);
            element.InputSlot = attribute.BufferSlot;
            element.AlignedByteOffset = attribute.Offset;
            element.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            elements.push_back(element);
        }
        const HRESULT result = m_Impl->Device->CreateInputLayout(
            elements.data(),
            static_cast<UINT>(elements.size()),
            vertex->Bytecode().data(),
            vertex->Bytecode().size(),
            &input);
        if (FAILED(result))
        {
            return Result<std::unique_ptr<Pipeline>>::Error(HrError("CreateInputLayout", result));
        }
    }

    D3D11_RASTERIZER_DESC rasterDescription{};
    rasterDescription.FillMode = D3D11_FILL_SOLID;
    rasterDescription.CullMode = description.Cull == CullMode::None
        ? D3D11_CULL_NONE
        : D3D11_CULL_BACK;
    rasterDescription.FrontCounterClockwise = TRUE;
    rasterDescription.DepthClipEnable = TRUE;
    if (description.DepthBias)
    {
        rasterDescription.DepthBias = static_cast<INT>(description.DepthBiasConstant);
        rasterDescription.SlopeScaledDepthBias = description.DepthBiasSlope;
        rasterDescription.DepthBiasClamp = description.DepthBiasClamp;
    }
    ComPtr<ID3D11RasterizerState> rasterizer;
    HRESULT result = m_Impl->Device->CreateRasterizerState(&rasterDescription, &rasterizer);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Pipeline>>::Error(HrError("CreateRasterizerState", result));
    }

    D3D11_DEPTH_STENCIL_DESC depthDescription{};
    depthDescription.DepthEnable = description.DepthTest;
    depthDescription.DepthWriteMask = description.DepthWrite
        ? D3D11_DEPTH_WRITE_MASK_ALL
        : D3D11_DEPTH_WRITE_MASK_ZERO;
    depthDescription.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> depth;
    result = m_Impl->Device->CreateDepthStencilState(&depthDescription, &depth);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Pipeline>>::Error(HrError("CreateDepthStencilState", result));
    }

    D3D11_BLEND_DESC blendDescription{};
    blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blendDescription.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (description.AlphaBlend)
    {
        auto& target = blendDescription.RenderTarget[0];
        target.BlendEnable = TRUE;
        target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    ComPtr<ID3D11BlendState> blend;
    result = m_Impl->Device->CreateBlendState(&blendDescription, &blend);
    if (FAILED(result))
    {
        return Result<std::unique_ptr<Pipeline>>::Error(HrError("CreateBlendState", result));
    }

    return Result<std::unique_ptr<Pipeline>>::Ok(std::make_unique<D3D11Pipeline>(
        description,
        vertex,
        fragment,
        std::move(input),
        std::move(rasterizer),
        std::move(depth),
        std::move(blend)));
}

Result<std::unique_ptr<RenderTarget>> D3D11Device::CreateRenderTarget(
    const TextureDesc& color, const TextureDesc* depth, const TextureDesc* color1)
{
    auto colorResult = CreateTexture(color);
    if (!colorResult)
    {
        return Result<std::unique_ptr<RenderTarget>>::Error(colorResult.ErrorMessage());
    }
    auto colorTexture = std::unique_ptr<D3D11Texture>(
        static_cast<D3D11Texture*>(std::move(colorResult).Value().release()));
    std::unique_ptr<D3D11Texture> depthTexture;
    std::unique_ptr<D3D11Texture> colorTexture1;
    if (depth != nullptr)
    {
        auto depthResult = CreateTexture(*depth);
        if (!depthResult)
        {
            return Result<std::unique_ptr<RenderTarget>>::Error(depthResult.ErrorMessage());
        }
        depthTexture.reset(static_cast<D3D11Texture*>(std::move(depthResult).Value().release()));
    }
    if (color1 != nullptr)
    {
        auto color1Result = CreateTexture(*color1);
        if (!color1Result)
        {
            return Result<std::unique_ptr<RenderTarget>>::Error(color1Result.ErrorMessage());
        }
        colorTexture1.reset(static_cast<D3D11Texture*>(std::move(color1Result).Value().release()));
    }
    return Result<std::unique_ptr<RenderTarget>>::Ok(std::make_unique<D3D11RenderTarget>(
        std::move(colorTexture),
        std::move(depthTexture),
        std::move(colorTexture1),
        color.Extent));
}

std::unique_ptr<RenderTarget> D3D11Device::CreateRenderTargetView(
    Texture& color, Texture* depth, Texture* color1)
{
    return std::make_unique<D3D11RenderTargetView>(
        &color, depth, color1, color.Description().Extent);
}

Result<std::unique_ptr<Swapchain>> D3D11Device::CreateSwapchain(void*)
{
    return Result<std::unique_ptr<Swapchain>>::Ok(std::make_unique<D3D11Swapchain>(*this));
}

std::unique_ptr<CommandList> D3D11Device::CreateCommandList()
{
    return std::make_unique<D3D11CommandList>(m_Impl->Device.Get(), m_Impl->Context.Get());
}

void D3D11Device::Submit(CommandList& commands)
{
    if (dynamic_cast<D3D11CommandList*>(&commands) == nullptr)
    {
        throw std::invalid_argument("Cannot submit a non-D3D11 command list");
    }
    m_Impl->Context->Flush();
}

void D3D11Device::WaitIdle()
{
    m_Impl->Context->Flush();
}

bool D3D11Device::SupportsTextureFormat(const Format format, const bool sampledDepth) const
{
    static_cast<void>(sampledDepth);
    const DXGI_FORMAT native = IsDepthFormat(format)
        ? DepthViewFormat(format)
        : ResourceFormat(format, false);
    if (native == DXGI_FORMAT_UNKNOWN)
    {
        return false;
    }
    UINT support = 0;
    if (FAILED(m_Impl->Device->CheckFormatSupport(native, &support)))
    {
        return false;
    }
    const UINT required = IsDepthFormat(format)
        ? D3D11_FORMAT_SUPPORT_DEPTH_STENCIL
        : D3D11_FORMAT_SUPPORT_TEXTURE2D;
    return (support & required) == required;
}

D3D11Device* AsD3D11Device(Device& device) noexcept
{
    return dynamic_cast<D3D11Device*>(&device);
}

std::unique_ptr<Device> CreateDeviceFromWindow(void* sdlWindow)
{
    try
    {
        return std::make_unique<D3D11Device>(sdlWindow);
    }
    catch (const std::exception& error)
    {
        SDL_SetError("%s", error.what());
        return {};
    }
}

void* NativeTextureView(Texture& texture) noexcept
{
    auto* native = dynamic_cast<D3D11Texture*>(&texture);
    return native != nullptr ? native->ShaderView() : nullptr;
}

Result<std::vector<std::byte>> ReadbackTexture(Device& device, Texture& texture)
{
    auto* nativeDevice = AsD3D11Device(device);
    auto* nativeTexture = dynamic_cast<D3D11Texture*>(&texture);
    if (nativeDevice == nullptr || nativeTexture == nullptr)
    {
        return Result<std::vector<std::byte>>::Error("Texture is not owned by D3D11");
    }
    D3D11_TEXTURE2D_DESC description{};
    nativeTexture->Native()->GetDesc(&description);
    D3D11_TEXTURE2D_DESC stagingDescription = description;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.MiscFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT result = nativeDevice->NativeDevice()->CreateTexture2D(
        &stagingDescription, nullptr, &staging);
    if (FAILED(result))
    {
        return Result<std::vector<std::byte>>::Error(HrError("Create readback texture", result));
    }
    nativeDevice->NativeContext()->CopySubresourceRegion(
        staging.Get(), 0, 0, 0, 0, nativeTexture->Native(), 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = nativeDevice->NativeContext()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result))
    {
        return Result<std::vector<std::byte>>::Error(HrError("Map readback texture", result));
    }
    const std::size_t rowSize =
        static_cast<std::size_t>(description.Width) * BytesPerPixel(texture.Description().PixelFormat);
    std::vector<std::byte> bytes(rowSize * description.Height);
    for (UINT row = 0; row < description.Height; ++row)
    {
        std::memcpy(
            bytes.data() + row * rowSize,
            static_cast<const std::byte*>(mapped.pData) + row * mapped.RowPitch,
            rowSize);
    }
    nativeDevice->NativeContext()->Unmap(staging.Get(), 0);
    return Result<std::vector<std::byte>>::Ok(std::move(bytes));
}
}

#endif
