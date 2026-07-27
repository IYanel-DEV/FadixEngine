#pragma once

#include "engine/Uuid.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace fadix::rhi
{
class Buffer;
class CommandList;
class Device;
class Pipeline;
class Sampler;
class Shader;
class Texture;
}

namespace fadix
{
struct TerrainComponent;

struct TerrainDrawData
{
    glm::mat4 ViewProjection{1.0F};
    glm::mat4 PrevViewProjection{1.0F};
    glm::mat4 Model{1.0F};
    glm::mat4 PrevModel{1.0F};
    glm::vec3 CameraPos{0.0F};
    float HeightScale{30.0F};
    glm::vec4 LightDir{-0.4F, -1.0F, -0.25F, 0.0F};
    glm::vec4 LightColor{1.0F, 0.96F, 0.88F, 3.0F};
    glm::vec4 AmbientColor{0.45F, 0.55F, 0.70F, 0.55F};
    int LayerCount{1};
    glm::vec4 LayerTiling{10.0F};
    glm::vec4 LayerMinHeight{0.0F, 0.25F, 0.5F, 0.75F};
    glm::vec4 LayerMaxHeight{0.25F, 0.5F, 0.75F, 1.0F};
    glm::vec4 LayerMinSlope{0.0F};
    glm::vec4 LayerMaxSlope{1.0F};
};

class TerrainRenderer
{
public:
    explicit TerrainRenderer(rhi::Device& device);
    ~TerrainRenderer();

    TerrainRenderer(const TerrainRenderer&) = delete;
    TerrainRenderer& operator=(const TerrainRenderer&) = delete;

    void Initialize();
    [[nodiscard]] bool Ready() const noexcept { return m_Ready; }

    void EnsureMesh(
        const Uuid& id,
        const float* heightmapPixels,
        int mapWidth,
        int mapHeight,
        const TerrainComponent& component,
        const std::string& signature);

    void Prune(const std::unordered_set<Uuid>& live);

    void Draw(
        rhi::CommandList& list,
        const Uuid& id,
        const TerrainDrawData& data,
        rhi::Texture* layerTextures[4],
        rhi::Sampler* sampler);

    void DrawShadow(
        rhi::CommandList& list,
        const Uuid& id,
        const glm::mat4& lightSpace,
        const glm::mat4& model);

    [[nodiscard]] bool HasMesh(const Uuid& id) const;

private:
    struct TerrainVertex
    {
        glm::vec3 Position;
        glm::vec3 Normal;
        glm::vec4 Tangent;
        glm::vec2 UV;
        glm::vec4 Color;
    };

    struct MeshEntry
    {
        std::unique_ptr<rhi::Buffer> VertexBuffer;
        std::unique_ptr<rhi::Buffer> IndexBuffer;
        std::uint32_t IndexCount{0};
        std::string Signature;
    };

    rhi::Device& m_Device;
    std::unique_ptr<rhi::Shader> m_VertexShader;
    std::unique_ptr<rhi::Shader> m_FragmentShader;
    std::unique_ptr<rhi::Pipeline> m_Pipeline;
    std::unordered_map<Uuid, MeshEntry> m_Meshes;
    bool m_Ready{false};
};
}
