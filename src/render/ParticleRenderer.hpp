#pragma once

#include "engine/rhi/Device.hpp"
#include "render/ParticleSystem.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace fadix
{

class ParticleRenderer
{
public:
    explicit ParticleRenderer(rhi::Device& device);
    ~ParticleRenderer();

    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    void Initialize();
    void Prepare(const std::vector<Particle>& particles, const glm::vec3& cameraPosition);
    void Draw(
        rhi::CommandList& list,
        const glm::mat4& viewProjection,
        const glm::mat4& prevViewProjection);

private:
    struct ParticleVertex
    {
        glm::vec3 Position;
        glm::vec3 PrevPosition;
        glm::vec2 Uv;
        glm::vec4 Color;
    };

    struct ParticleVertexUniform
    {
        glm::mat4 ViewProjection{1.0F};
        glm::mat4 PrevViewProjection{1.0F};
    };

    rhi::Device& m_Device;
    std::unique_ptr<rhi::Shader> m_VertexShader;
    std::unique_ptr<rhi::Shader> m_FragmentShader;
    std::unique_ptr<rhi::Pipeline> m_Pipeline;
    std::unique_ptr<rhi::Buffer> m_VertexBuffer;
    std::unique_ptr<rhi::Buffer> m_IndexBuffer;
    std::vector<ParticleVertex> m_Vertices;
    std::vector<std::uint32_t> m_Indices;
    std::unordered_map<std::size_t, glm::vec3> m_PrevCenters;
    std::uint32_t m_PreparedIndexCount{0};
    bool m_Ready{false};
};

} // namespace fadix
