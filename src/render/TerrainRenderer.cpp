#include "render/TerrainRenderer.hpp"

#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/CommandList.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Pipeline.hpp"
#include "engine/rhi/Shader.hpp"
#include "render/ShaderCompiler.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/Components.hpp"

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

namespace fadix
{
namespace
{
struct TerrainVertexUniform
{
    glm::mat4 ViewProjection{1.0F};
    glm::mat4 PrevViewProjection{1.0F};
    glm::mat4 Model{1.0F};
    glm::mat4 PrevModel{1.0F};
};

struct TerrainFragmentUniform
{
    glm::vec4 CameraPosHeightScale{0.0F};
    glm::vec4 LightDir{0.0F};
    glm::vec4 LightColor{1.0F};
    glm::vec4 AmbientColor{0.45F, 0.55F, 0.70F, 0.55F};
    glm::vec4 Params{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec4 LayerTiling{10.0F};
    glm::vec4 LayerMinHeight{0.0F, 0.25F, 0.5F, 0.75F};
    glm::vec4 LayerMaxHeight{0.25F, 0.5F, 0.75F, 1.0F};
    glm::vec4 LayerMinSlope{0.0F};
    glm::vec4 LayerMaxSlope{1.0F};
};

struct ShadowVertexUniform
{
    glm::mat4 LightSpaceMatrix{1.0F};
    glm::mat4 Model{1.0F};
};

[[nodiscard]] float SampleHeightmap(
    const float* pixels, const int mapWidth, const int mapHeight, const float x, const float y)
{
    if (pixels == nullptr || mapWidth < 1 || mapHeight < 1)
    {
        return 0.0F;
    }
    const float maxX = static_cast<float>(mapWidth - 1);
    const float maxY = static_cast<float>(mapHeight - 1);
    const float cx = std::clamp(x, 0.0F, maxX);
    const float cy = std::clamp(y, 0.0F, maxY);
    const int x0 = static_cast<int>(cx);
    const int y0 = static_cast<int>(cy);
    const int x1 = std::min(x0 + 1, mapWidth - 1);
    const int y1 = std::min(y0 + 1, mapHeight - 1);
    const float tx = cx - static_cast<float>(x0);
    const float ty = cy - static_cast<float>(y0);
    const float h00 = pixels[y0 * mapWidth + x0];
    const float h10 = pixels[y0 * mapWidth + x1];
    const float h01 = pixels[y1 * mapWidth + x0];
    const float h11 = pixels[y1 * mapWidth + x1];
    const float h0 = h00 + (h10 - h00) * tx;
    const float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}
}

TerrainRenderer::TerrainRenderer(rhi::Device& device)
    : m_Device(device)
{
}

TerrainRenderer::~TerrainRenderer() = default;

void TerrainRenderer::Initialize()
{
    m_Ready = false;
    const std::vector<std::byte> source = render::ReadShaderSource("terrain.hlsl");
    {
        const std::vector<std::byte> code =
            render::CompileShader(source, "VertexMain", m_Device.ShaderTarget(false), "terrain.hlsl");
        auto result = m_Device.CreateShader({"VertexMain", "terrain_vertex"}, code);
        if (!result)
        {
            throw std::runtime_error("Terrain vertex shader failed: " + result.ErrorMessage());
        }
        m_VertexShader = std::move(result).Value();
    }
    {
        const std::vector<std::byte> code =
            render::CompileShader(source, "FragmentMain", m_Device.ShaderTarget(true), "terrain.hlsl");
        auto result = m_Device.CreateShader({"FragmentMain", "terrain_fragment", 4}, code);
        if (!result)
        {
            throw std::runtime_error("Terrain fragment shader failed: " + result.ErrorMessage());
        }
        m_FragmentShader = std::move(result).Value();
    }

    rhi::PipelineDesc pipeline;
    pipeline.DebugName = "TerrainLit";
    pipeline.ColorFormat = rhi::Format::R16G16B16A16Float;
    pipeline.ColorFormat1 = rhi::Format::R16G16Float;
    pipeline.VertexShader = m_VertexShader.get();
    pipeline.FragmentShader = m_FragmentShader.get();
    pipeline.DepthTest = true;
    pipeline.DepthWrite = true;
    pipeline.Cull = rhi::CullMode::Back;
    pipeline.VertexBufferLayouts = {{0, sizeof(TerrainVertex)}};
    pipeline.VertexAttributes = {
        {0, 0, rhi::VertexElementFormat::Float3, 0},
        {1, 0, rhi::VertexElementFormat::Float3, 12},
        {2, 0, rhi::VertexElementFormat::Float4, 24},
        {3, 0, rhi::VertexElementFormat::Float2, 40},
        {4, 0, rhi::VertexElementFormat::Float4, 48},
    };
    auto pipelineResult = m_Device.CreatePipeline(pipeline);
    if (!pipelineResult)
    {
        throw std::runtime_error("Terrain pipeline failed: " + pipelineResult.ErrorMessage());
    }
    m_Pipeline = std::move(pipelineResult).Value();
    m_Ready = true;
}

void TerrainRenderer::EnsureMesh(
    const Uuid& id,
    const float* heightmapPixels,
    const int mapWidth,
    const int mapHeight,
    const TerrainComponent& component,
    const std::string& signature)
{
    if (!m_Ready)
    {
        return;
    }
    auto existing = m_Meshes.find(id);
    if (existing != m_Meshes.end() && existing->second.Signature == signature)
    {
        return;
    }

    const int resX = std::clamp(component.ResolutionX, 2, 513);
    const int resZ = std::clamp(component.ResolutionZ, 2, 513);
    const float width = std::max(component.Width, 0.01F);
    const float depth = std::max(component.Depth, 0.01F);
    const float heightScale = component.HeightScale;

    std::vector<TerrainVertex> vertices;
    vertices.resize(static_cast<std::size_t>(resX) * static_cast<std::size_t>(resZ));
    for (int z = 0; z < resZ; ++z)
    {
        for (int x = 0; x < resX; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(resX - 1);
            const float v = static_cast<float>(z) / static_cast<float>(resZ - 1);
            const float sampleX = u * static_cast<float>(std::max(mapWidth - 1, 0));
            const float sampleY = v * static_cast<float>(std::max(mapHeight - 1, 0));
            const float height =
                SampleHeightmap(heightmapPixels, mapWidth, mapHeight, sampleX, sampleY);
            TerrainVertex& vert = vertices[static_cast<std::size_t>(z) * resX + x];
            vert.Position = glm::vec3((u - 0.5F) * width, height * heightScale, (v - 0.5F) * depth);
            vert.Normal = glm::vec3{0.0F};
            vert.Tangent = glm::vec4{1.0F, 0.0F, 0.0F, 1.0F};
            vert.UV = glm::vec2{u, v};
            vert.Color = glm::vec4{1.0F};
        }
    }

    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(resX - 1) * static_cast<std::size_t>(resZ - 1) * 6);
    for (int z = 0; z < resZ - 1; ++z)
    {
        for (int x = 0; x < resX - 1; ++x)
        {
            const std::uint32_t topLeft = static_cast<std::uint32_t>(z * resX + x);
            const std::uint32_t topRight = topLeft + 1;
            const std::uint32_t bottomLeft = static_cast<std::uint32_t>((z + 1) * resX + x);
            const std::uint32_t bottomRight = bottomLeft + 1;
            indices.insert(indices.end(), {topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight});
        }
    }

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
    {
        const std::uint32_t i0 = indices[i];
        const std::uint32_t i1 = indices[i + 1];
        const std::uint32_t i2 = indices[i + 2];
        const glm::vec3 edge1 = vertices[i1].Position - vertices[i0].Position;
        const glm::vec3 edge2 = vertices[i2].Position - vertices[i0].Position;
        const glm::vec3 faceNormal = glm::normalize(glm::cross(edge1, edge2));
        if (!std::isfinite(faceNormal.x))
        {
            continue;
        }
        vertices[i0].Normal += faceNormal;
        vertices[i1].Normal += faceNormal;
        vertices[i2].Normal += faceNormal;
    }
    for (TerrainVertex& vert : vertices)
    {
        if (glm::length(vert.Normal) < 1.0e-6F)
        {
            vert.Normal = glm::vec3{0.0F, 1.0F, 0.0F};
        }
        else
        {
            vert.Normal = glm::normalize(vert.Normal);
        }
    }

    MeshEntry entry;
    entry.Signature = signature;
    entry.IndexCount = static_cast<std::uint32_t>(indices.size());
    auto vertexResult = m_Device.CreateBuffer(
        {vertices.size() * sizeof(TerrainVertex), rhi::BufferUsage::Vertex, "TerrainVertices"});
    if (!vertexResult)
    {
        return;
    }
    entry.VertexBuffer = std::move(vertexResult).Value();
    entry.VertexBuffer->Upload(std::as_bytes(std::span{vertices}));

    auto indexResult = m_Device.CreateBuffer(
        {indices.size() * sizeof(std::uint32_t), rhi::BufferUsage::Index, "TerrainIndices"});
    if (!indexResult)
    {
        return;
    }
    entry.IndexBuffer = std::move(indexResult).Value();
    entry.IndexBuffer->Upload(std::as_bytes(std::span{indices}));
    m_Meshes[id] = std::move(entry);
}

void TerrainRenderer::Prune(const std::unordered_set<Uuid>& live)
{
    for (auto it = m_Meshes.begin(); it != m_Meshes.end();)
    {
        if (live.count(it->first) == 0)
        {
            it = m_Meshes.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

bool TerrainRenderer::HasMesh(const Uuid& id) const
{
    return m_Meshes.find(id) != m_Meshes.end();
}

void TerrainRenderer::Draw(
    rhi::CommandList& list,
    const Uuid& id,
    const TerrainDrawData& data,
    rhi::Texture* layerTextures[4],
    rhi::Sampler* sampler)
{
    if (!m_Ready || sampler == nullptr)
    {
        return;
    }
    const auto it = m_Meshes.find(id);
    if (it == m_Meshes.end() || it->second.IndexCount == 0)
    {
        return;
    }

    list.BindPipeline(*m_Pipeline);
    list.BindVertexBuffer(*it->second.VertexBuffer);
    list.BindIndexBuffer(*it->second.IndexBuffer);

    const TerrainVertexUniform vertex{
        data.ViewProjection, data.PrevViewProjection, data.Model, data.PrevModel};
    list.PushVertexUniform(0, &vertex, sizeof(vertex));

    TerrainFragmentUniform fragment{};
    fragment.CameraPosHeightScale = glm::vec4{data.CameraPos, data.HeightScale};
    fragment.LightDir = data.LightDir;
    fragment.LightColor = data.LightColor;
    fragment.AmbientColor = data.AmbientColor;
    fragment.Params = glm::vec4{static_cast<float>(std::clamp(data.LayerCount, 1, 4)), 0.0F, 0.0F, 0.0F};
    fragment.LayerTiling = data.LayerTiling;
    fragment.LayerMinHeight = data.LayerMinHeight;
    fragment.LayerMaxHeight = data.LayerMaxHeight;
    fragment.LayerMinSlope = data.LayerMinSlope;
    fragment.LayerMaxSlope = data.LayerMaxSlope;
    list.PushFragmentUniform(0, &fragment, sizeof(fragment));

    std::array<rhi::Texture*, 4> textures = {
        layerTextures[0], layerTextures[1], layerTextures[2], layerTextures[3]};
    std::array<rhi::Sampler*, 4> samplers = {sampler, sampler, sampler, sampler};
    list.BindFragmentSamplers(0, textures, samplers);
    list.DrawIndexed(it->second.IndexCount);
}

void TerrainRenderer::DrawShadow(
    rhi::CommandList& list,
    const Uuid& id,
    const glm::mat4& lightSpace,
    const glm::mat4& model)
{
    const auto it = m_Meshes.find(id);
    if (it == m_Meshes.end() || it->second.IndexCount == 0)
    {
        return;
    }
    list.BindVertexBuffer(*it->second.VertexBuffer);
    list.BindIndexBuffer(*it->second.IndexBuffer);
    const ShadowVertexUniform vertex{lightSpace, model};
    list.PushVertexUniform(0, &vertex, sizeof(vertex));
    list.DrawIndexed(it->second.IndexCount);
}
}
