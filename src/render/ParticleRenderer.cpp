#include "render/ParticleRenderer.hpp"

#include "assets/EmbeddedAssetProvider.hpp"
#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/Shader.hpp"
#include "render/ShaderCompiler.hpp"
#include "rhi/sdl/SdlRhi.hpp"

#include <glm/geometric.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3dcompiler.h>
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fadix
{
namespace
{
constexpr std::size_t kMaxParticles = 4096;
constexpr std::size_t kMaxVertices = kMaxParticles * 4;
constexpr std::size_t kMaxIndices = kMaxParticles * 6;
// ParticleVertex: float3 + float3 + float2 + float4
constexpr std::size_t kParticleVertexStride = 48;

[[nodiscard]] std::vector<std::byte> ReadShaderSource()
{
    const std::filesystem::path path = RuntimeAssetRoot() / "shaders" / "particle.hlsl";
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        throw std::runtime_error("Could not open particle shader: " + path.string());
    }
    const std::streamsize size = stream.tellg();
    stream.seekg(0);
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(result.data()), size))
    {
        throw std::runtime_error("Could not read particle shader: " + path.string());
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> CompileShader(
    const std::span<const std::byte> source,
    const char* entryPoint,
    const char* target)
{
    return render::CompileShader(source, entryPoint, target, "particle.hlsl");
}
} // namespace

ParticleRenderer::ParticleRenderer(rhi::Device& device)
    : m_Device(device)
{
}

ParticleRenderer::~ParticleRenderer() = default;

void ParticleRenderer::Initialize()
{
    m_Ready = false;
    try
    {
        static_assert(sizeof(ParticleVertex) == kParticleVertexStride);

        const std::vector<std::byte> source = ReadShaderSource();
        {
            const std::vector<std::byte> code =
                CompileShader(source, "VertexMain", m_Device.ShaderTarget(false));
            auto result = m_Device.CreateShader({"VertexMain", "particle_vertex"}, code);
            if (!result)
            {
                throw std::runtime_error("Particle vertex shader creation failed: " + result.ErrorMessage());
            }
            m_VertexShader = std::move(result).Value();
        }
        {
            const std::vector<std::byte> code =
                CompileShader(source, "FragmentMain", m_Device.ShaderTarget(true));
            auto result = m_Device.CreateShader({"FragmentMain", "particle_fragment"}, code);
            if (!result)
            {
                throw std::runtime_error("Particle fragment shader creation failed: " + result.ErrorMessage());
            }
            m_FragmentShader = std::move(result).Value();
        }

        rhi::PipelineDesc pipeline;
        pipeline.DebugName = "ParticleBillboard";
        pipeline.ColorFormat = rhi::Format::R16G16B16A16Float;
        pipeline.ColorFormat1 = rhi::Format::R16G16Float;
        pipeline.VertexShader = m_VertexShader.get();
        pipeline.FragmentShader = m_FragmentShader.get();
        pipeline.AlphaBlend = true;
        pipeline.DepthWrite = false;
        pipeline.DepthTest = true;
        pipeline.Cull = rhi::CullMode::None;
        pipeline.VertexBufferLayouts = {{0, sizeof(ParticleVertex)}};
        pipeline.VertexAttributes = {
            {0, 0, rhi::VertexElementFormat::Float3, 0},
            {1, 0, rhi::VertexElementFormat::Float3, 12},
            {2, 0, rhi::VertexElementFormat::Float2, 24},
            {3, 0, rhi::VertexElementFormat::Float4, 32},
        };
        auto pipelineResult = m_Device.CreatePipeline(pipeline);
        if (!pipelineResult)
        {
            throw std::runtime_error(
                "Particle pipeline creation failed: " + pipelineResult.ErrorMessage());
        }
        m_Pipeline = std::move(pipelineResult).Value();

        auto vertexResult = m_Device.CreateBuffer(
            {kMaxVertices * sizeof(ParticleVertex), rhi::BufferUsage::Vertex, "ParticleVertices"});
        if (!vertexResult)
        {
            throw std::runtime_error("Particle vertex buffer creation failed: " + vertexResult.ErrorMessage());
        }
        m_VertexBuffer = std::move(vertexResult).Value();

        m_Indices.clear();
        m_Indices.reserve(kMaxIndices);
        for (std::uint32_t quad = 0; quad < static_cast<std::uint32_t>(kMaxParticles); ++quad)
        {
            const std::uint32_t base = quad * 4;
            m_Indices.insert(m_Indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        }
        auto indexResult = m_Device.CreateBuffer(
            {m_Indices.size() * sizeof(std::uint32_t), rhi::BufferUsage::Index, "ParticleIndices"});
        if (!indexResult)
        {
            throw std::runtime_error("Particle index buffer creation failed: " + indexResult.ErrorMessage());
        }
        m_IndexBuffer = std::move(indexResult).Value();
        m_IndexBuffer->Upload(std::as_bytes(std::span{m_Indices}));

        m_Vertices.reserve(kMaxVertices);
        m_Ready = true;
    }
    catch (...)
    {
        m_VertexShader.reset();
        m_FragmentShader.reset();
        m_Pipeline.reset();
        m_VertexBuffer.reset();
        m_IndexBuffer.reset();
        m_Ready = false;
    }
}

void ParticleRenderer::Prepare(
    const std::vector<Particle>& particles, const glm::vec3& cameraPosition)
{
    m_PreparedIndexCount = 0;
    if (!m_Ready)
    {
        return;
    }

    m_Vertices.clear();
    std::size_t poolIndex = 0;
    for (const Particle& particle : particles)
    {
        if (!particle.Alive)
        {
            ++poolIndex;
            continue;
        }
        if (m_Vertices.size() + 4 > kMaxVertices)
        {
            break;
        }

        glm::vec3 prevCenter = particle.Position;
        if (const auto it = m_PrevCenters.find(poolIndex); it != m_PrevCenters.end())
        {
            prevCenter = it->second;
        }

        glm::vec3 forward = cameraPosition - particle.Position;
        const float forwardLen2 = glm::dot(forward, forward);
        if (forwardLen2 < 1.0e-12F)
        {
            continue;
        }
        forward *= 1.0F / std::sqrt(forwardLen2);

        glm::vec3 upHint{0.0F, 1.0F, 0.0F};
        glm::vec3 right = glm::cross(upHint, forward);
        float rightLen2 = glm::dot(right, right);
        if (rightLen2 < 1.0e-8F)
        {
            upHint = {0.0F, 0.0F, 1.0F};
            right = glm::cross(upHint, forward);
            rightLen2 = glm::dot(right, right);
            if (rightLen2 < 1.0e-8F)
            {
                continue;
            }
        }
        right *= 1.0F / std::sqrt(rightLen2);
        const glm::vec3 up = glm::cross(forward, right);

        const float size = particle.Size;
        const glm::vec3 rightExtent = right * size;
        const glm::vec3 upExtent = up * size;
        const glm::vec3& pos = particle.Position;
        const glm::vec4& color = particle.Color;

        m_Vertices.push_back({pos - rightExtent - upExtent, prevCenter - rightExtent - upExtent, {0.0F, 0.0F}, color});
        m_Vertices.push_back({pos + rightExtent - upExtent, prevCenter + rightExtent - upExtent, {1.0F, 0.0F}, color});
        m_Vertices.push_back({pos + rightExtent + upExtent, prevCenter + rightExtent + upExtent, {1.0F, 1.0F}, color});
        m_Vertices.push_back({pos - rightExtent + upExtent, prevCenter - rightExtent + upExtent, {0.0F, 1.0F}, color});
        m_PrevCenters[poolIndex] = particle.Position;
        ++poolIndex;
    }

    for (auto it = m_PrevCenters.begin(); it != m_PrevCenters.end();)
    {
        if (it->first >= particles.size() || !particles[it->first].Alive)
        {
            it = m_PrevCenters.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (m_Vertices.empty())
    {
        return;
    }

    m_VertexBuffer->Upload(std::as_bytes(std::span{m_Vertices}));
    m_PreparedIndexCount = static_cast<std::uint32_t>((m_Vertices.size() / 4) * 6);
}

void ParticleRenderer::Draw(
    rhi::CommandList& list,
    const glm::mat4& viewProjection,
    const glm::mat4& prevViewProjection)
{
    if (!m_Ready || m_PreparedIndexCount == 0)
    {
        return;
    }

    list.BindPipeline(*m_Pipeline);
    list.BindVertexBuffer(*m_VertexBuffer);
    list.BindIndexBuffer(*m_IndexBuffer);
    const ParticleVertexUniform uniforms{viewProjection, prevViewProjection};
    list.PushVertexUniform(0, &uniforms, sizeof(uniforms));
    list.DrawIndexed(m_PreparedIndexCount);
}

} // namespace fadix
