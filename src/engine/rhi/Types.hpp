#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fadix::rhi
{
enum class Format : std::uint8_t
{
    Unknown,
    R8G8B8A8Unorm,
    R8G8B8A8UnormSrgb, // color data decoded sRGB->linear by hardware on sample
    B8G8R8A8Unorm,
    R16G16Float,
    R16G16B16A16Float,
    D24UnormS8Uint,
    D32Float,
    D16Unorm // widely-supported sampleable depth fallback
};

enum class BufferUsage : std::uint8_t
{
    Vertex,
    Index,
    Uniform,
    Storage,
    Staging
};

enum class TextureUsage : std::uint8_t
{
    Sampled,
    RenderTarget,
    DepthStencil,
    Storage
};

// Real texture topology so point-light shadows can use one cubemap resource
// instead of six faked 2D textures. Array covers stacked local-light maps.
enum class TextureType : std::uint8_t
{
    Texture2D,
    Cube,      // LayerCount is forced to 6
    Array2D
};

struct Extent2D
{
    std::uint32_t Width{0};
    std::uint32_t Height{0};
};

struct BufferDesc
{
    std::size_t Size{0};
    BufferUsage Usage{BufferUsage::Vertex};
    std::string DebugName;
    std::uint32_t StructureStride{0};
};

struct TextureDesc
{
    Extent2D Extent;
    Format PixelFormat{Format::R8G8B8A8Unorm};
    TextureUsage Usage{TextureUsage::Sampled};
    std::uint32_t MipLevels{1};
    std::string DebugName;
    TextureType Type{TextureType::Texture2D};
    std::uint32_t LayerCount{1}; // 6 for Cube; N for Array2D
    // ORs the SAMPLER usage flag onto the base usage when the backend permits it.
    // Lets a depth-stencil target additionally be sampled (for SSAO) where the
    // GPU/format supports it; query Device::SupportsTextureFormat first.
    bool AllowSampling{false};
};

struct ShaderDesc
{
    std::string EntryPoint{"main"};
    std::string DebugName;
    std::uint32_t NumSamplers{0};
    std::uint32_t NumUniformBuffers{1};
    // Read-only storage (structured) buffers the fragment stage samples, e.g.
    // the Forward+ light + tile buffers. They occupy t-registers immediately
    // after the sampled textures (see SdlRhi register mapping).
    std::uint32_t NumStorageBuffers{0};
};

enum class PrimitiveTopology : std::uint8_t
{
    TriangleList,
    LineList
};

enum class CullMode : std::uint8_t
{
    Back,
    None
};


enum class FilterMode : std::uint8_t
{
    Nearest,
    Linear
};

enum class WrapMode : std::uint8_t
{
    Repeat,
    Clamp,
    Mirror
};

struct SamplerDesc
{
    FilterMode MinFilter{FilterMode::Linear};
    FilterMode MagFilter{FilterMode::Linear};
    FilterMode MipFilter{FilterMode::Linear};
    WrapMode AddressU{WrapMode::Repeat};
    WrapMode AddressV{WrapMode::Repeat};
    WrapMode AddressW{WrapMode::Repeat};
    float MaxAnisotropy{1.0f};
    bool EnableAnisotropy{false};
    std::string DebugName;
};

enum class VertexElementFormat : std::uint8_t
{
    Float2,
    Float3,
    Float4,
    Color // 4 unorm bytes? Let's stick to Float4 if we change vertex color to vec4, or Float4 for float color. Wait, SdlRhi hardcodes float3 pos, float3 norm, float3 col... wait, it was float3 color! I'll use Float4 for tangent and Float2 for UV.
};

struct VertexAttribute
{
    std::uint32_t Location{0};
    std::uint32_t BufferSlot{0};
    VertexElementFormat Format{VertexElementFormat::Float3};
    std::uint32_t Offset{0};
};

struct VertexBufferLayout
{
    std::uint32_t Slot{0};
    std::uint32_t Stride{0};
};

class Shader;

struct PipelineDesc
{
    Format ColorFormat{Format::R8G8B8A8Unorm};
    Format ColorFormat1{Format::Unknown}; // Unknown = single RT (backward compatible)
    Format DepthFormat{Format::D24UnormS8Uint};
    std::string DebugName;
    PrimitiveTopology Topology{PrimitiveTopology::TriangleList};
    bool DepthTest{true};
    bool DepthWrite{true};
    // Explicit pipeline shaders. When both are set the backend must use them;
    // the legacy "last created shaders" fallback only applies when unset.
    Shader* VertexShader{nullptr};
    Shader* FragmentShader{nullptr};
    // Standard (non-premultiplied) alpha blending on the color target.
    bool AlphaBlend{false};
    CullMode Cull{CullMode::Back};
    // Raster depth bias, used by the shadow pass to push casters off the
    // receiver and suppress self-shadow acne. Constant is in depth units;
    // slope scales with the polygon's depth gradient. Left disabled elsewhere.
    bool DepthBias{false};
    float DepthBiasConstant{0.0F};
    float DepthBiasSlope{0.0F};
    float DepthBiasClamp{0.0F};
    // False for shaders that generate vertices from SV_VertexID
    // (e.g. full-screen sky triangle) and bind no vertex buffers.
    bool UseVertexInput{true};
    std::vector<VertexBufferLayout> VertexBufferLayouts;
    std::vector<VertexAttribute> VertexAttributes;

};
}
