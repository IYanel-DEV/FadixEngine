#include "render/ViewportGeometry.hpp"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>

namespace fadix::viewport_geometry
{
namespace
{
void AppendFace(
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& indices,
    const glm::vec3 normal,
    const glm::vec4 tangent,
    const std::array<glm::vec3, 4>& corners,
    const glm::vec4 color)
{
    const auto start = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({corners[0], normal, tangent, {0, 0}, color});
    vertices.push_back({corners[1], normal, tangent, {1, 0}, color});
    vertices.push_back({corners[2], normal, tangent, {1, 1}, color});
    vertices.push_back({corners[3], normal, tangent, {0, 1}, color});
    indices.insert(indices.end(), {start, start + 1, start + 2, start, start + 2, start + 3});
}
} // namespace

void AppendCube(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
{
    constexpr float h = 0.5F;
    AppendFace(vertices, indices, {0, 0, 1}, {1, 0, 0, 1}, {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {0, 0, -1}, {-1, 0, 0, 1}, {{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {1, 0, 0}, {0, 0, -1, 1}, {{{h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {-1, 0, 0}, {0, 0, 1, 1}, {{{-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {0, 1, 0}, {1, 0, 0, 1}, {{{-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {0, -1, 0}, {1, 0, 0, 1}, {{{-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h}}}, {1, 1, 1, 1});
}

void AppendCylinder(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const int segments)
{
    const auto ring = [&](const float y, const float v) {
        const auto start = static_cast<std::uint32_t>(vertices.size());
        for (int i = 0; i < segments; ++i)
        {
            const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
            const glm::vec3 normal{std::cos(angle), 0.0F, std::sin(angle)};
            const glm::vec4 tangent{-std::sin(angle), 0.0F, std::cos(angle), 1.0F};
            const float u = static_cast<float>(i) / static_cast<float>(segments);
            vertices.push_back({{normal.x * 0.5F, y, normal.z * 0.5F}, normal, tangent, {u, v}, {1, 1, 1, 1}});
        }
        return start;
    };
    const std::uint32_t bottom = ring(0.0F, 0.0F);
    const std::uint32_t top = ring(1.0F, 1.0F);
    for (int i = 0; i < segments; ++i)
    {
        const std::uint32_t next = static_cast<std::uint32_t>((i + 1) % segments);
        const auto b0 = bottom + static_cast<std::uint32_t>(i);
        const auto b1 = bottom + next;
        const auto t0 = top + static_cast<std::uint32_t>(i);
        const auto t1 = top + next;
        indices.insert(indices.end(), {b0, t0, t1, b0, t1, b1});
    }
    const auto capCenterBottom = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, 0, 0}, {0, -1, 0}, {1, 0, 0, 1}, {0.5F, 0.5F}, {1, 1, 1, 1}});
    const auto capCenterTop = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, 1, 0}, {0, 1, 0}, {1, 0, 0, 1}, {0.5F, 0.5F}, {1, 1, 1, 1}});
    for (int i = 0; i < segments; ++i)
    {
        const std::uint32_t next = static_cast<std::uint32_t>((i + 1) % segments);
        indices.insert(indices.end(), {capCenterBottom, bottom + static_cast<std::uint32_t>(i), bottom + next});
        indices.insert(indices.end(), {capCenterTop, top + next, top + static_cast<std::uint32_t>(i)});
    }
}

void AppendCone(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const int segments)
{
    const auto baseStart = static_cast<std::uint32_t>(vertices.size());
    for (int i = 0; i < segments; ++i)
    {
        const float angle = glm::two_pi<float>() * static_cast<float>(i) / static_cast<float>(segments);
        const glm::vec3 radial{std::cos(angle), 0.0F, std::sin(angle)};
        const glm::vec3 normal = glm::normalize(glm::vec3{radial.x, 0.5F, radial.z});
        const glm::vec4 tangent{-std::sin(angle), 0.0F, std::cos(angle), 1.0F};
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        vertices.push_back({{radial.x * 0.5F, 0.0F, radial.z * 0.5F}, normal, tangent, {u, 0.0F}, {1, 1, 1, 1}});
    }
    const auto apex = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, 1, 0}, {0, 1, 0}, {1, 0, 0, 1}, {0.5F, 1.0F}, {1, 1, 1, 1}});
    const auto baseCenter = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back({{0, 0, 0}, {0, -1, 0}, {1, 0, 0, 1}, {0.5F, 0.5F}, {1, 1, 1, 1}});
    for (int i = 0; i < segments; ++i)
    {
        const std::uint32_t next = static_cast<std::uint32_t>((i + 1) % segments);
        indices.insert(indices.end(), {baseStart + static_cast<std::uint32_t>(i), apex, baseStart + next});
        indices.insert(indices.end(), {baseCenter, baseStart + static_cast<std::uint32_t>(i), baseStart + next});
    }
}

void AppendSphere(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const int slices, const int stacks)
{
    const auto start = static_cast<std::uint32_t>(vertices.size());
    for (int stack = 0; stack <= stacks; ++stack)
    {
        const float phi = glm::pi<float>() * static_cast<float>(stack) / static_cast<float>(stacks);
        const float v = 1.0F - static_cast<float>(stack) / static_cast<float>(stacks);
        for (int slice = 0; slice <= slices; ++slice)
        {
            const float theta = glm::two_pi<float>() * static_cast<float>(slice) / static_cast<float>(slices);
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const glm::vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            const glm::vec4 tangent{-std::sin(theta), 0.0F, std::cos(theta), 1.0F};
            vertices.push_back({normal * 0.5F, normal, tangent, {u, v}, {1, 1, 1, 1}});
        }
    }
    const auto stride = static_cast<std::uint32_t>(slices + 1);
    for (int stack = 0; stack < stacks; ++stack)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            const std::uint32_t a = start + static_cast<std::uint32_t>(stack) * stride + static_cast<std::uint32_t>(slice);
            const std::uint32_t b = a + stride;
            indices.insert(indices.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    }
}

void AppendTorus(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const float tubeRadius, const int majorSegments, const int minorSegments)
{
    const auto start = static_cast<std::uint32_t>(vertices.size());
    for (int major = 0; major <= majorSegments; ++major)
    {
        const float u_val = static_cast<float>(major) / static_cast<float>(majorSegments);
        const float u = glm::two_pi<float>() * u_val;
        const glm::vec3 center{std::cos(u), std::sin(u), 0.0F};
        for (int minor = 0; minor <= minorSegments; ++minor)
        {
            const float v_val = static_cast<float>(minor) / static_cast<float>(minorSegments);
            const float v = glm::two_pi<float>() * v_val;
            const glm::vec3 normal = glm::normalize(center * std::cos(v) + glm::vec3{0.0F, 0.0F, std::sin(v)});
            const glm::vec4 tangent{-std::sin(u), std::cos(u), 0.0F, 1.0F};
            vertices.push_back({center + normal * tubeRadius, normal, tangent, {u_val, v_val}, {1, 1, 1, 1}});
        }
    }
    const auto stride = static_cast<std::uint32_t>(minorSegments + 1);
    for (int major = 0; major < majorSegments; ++major)
    {
        for (int minor = 0; minor < minorSegments; ++minor)
        {
            const std::uint32_t a = start + static_cast<std::uint32_t>(major) * stride + static_cast<std::uint32_t>(minor);
            const std::uint32_t b = a + stride;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
}

void AppendQuad(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
{
    AppendFace(vertices, indices, {0, 0, 1}, {1, 0, 0, 1}, {{{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {0, 0, -1}, {-1, 0, 0, 1}, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}, {1, 1, 1, 1});
}

void AppendSpriteQuad(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
{
    // Centered at origin in XY plane. UV(0,0) maps to world top-left (Y+),
    // UV(1,1) maps to world bottom-right (Y-) to match image-space convention.
    constexpr float h = 0.5F;
    AppendFace(vertices, indices, {0, 0, 1}, {1, 0, 0, 1},
        {{{-h, h, 0}, {h, h, 0}, {h, -h, 0}, {-h, -h, 0}}}, {1, 1, 1, 1});
}

void AppendPlanePrimitive(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices)
{
    constexpr float h = 0.5F;
    AppendFace(vertices, indices, {0, 1, 0}, {1, 0, 0, 1}, {{{-h, 0, h}, {h, 0, h}, {h, 0, -h}, {-h, 0, -h}}}, {1, 1, 1, 1});
    AppendFace(vertices, indices, {0, -1, 0}, {1, 0, 0, 1}, {{{-h, 0, -h}, {h, 0, -h}, {h, 0, h}, {-h, 0, h}}}, {1, 1, 1, 1});
}

void AppendCapsule(std::vector<Vertex>& vertices, std::vector<std::uint32_t>& indices, const int slices, const int stacks)
{
    const auto start = static_cast<std::uint32_t>(vertices.size());
    const auto pushRing = [&](const float phi, const float offset, const float v) {
        for (int slice = 0; slice <= slices; ++slice)
        {
            const float theta = glm::two_pi<float>() * static_cast<float>(slice) / static_cast<float>(slices);
            const float u = static_cast<float>(slice) / static_cast<float>(slices);
            const glm::vec3 normal{std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta)};
            const glm::vec4 tangent{-std::sin(theta), 0.0F, std::cos(theta), 1.0F};
            vertices.push_back({normal * 0.5F + glm::vec3{0.0F, offset, 0.0F}, normal, tangent, {u, v}, {1, 1, 1, 1}});
        }
    };
    for (int stack = 0; stack <= stacks; ++stack)
    {
        const float v = static_cast<float>(stack) / static_cast<float>(stacks * 2);
        pushRing(glm::half_pi<float>() * static_cast<float>(stack) / static_cast<float>(stacks), 0.5F, v);
    }
    for (int stack = 0; stack <= stacks; ++stack)
    {
        const float v = 0.5F + static_cast<float>(stack) / static_cast<float>(stacks * 2);
        pushRing(glm::half_pi<float>() + glm::half_pi<float>() * static_cast<float>(stack) / static_cast<float>(stacks), -0.5F, v);
    }
    const auto stride = static_cast<std::uint32_t>(slices + 1);
    const int ringCount = 2 * (stacks + 1);
    for (int ring = 0; ring < ringCount - 1; ++ring)
    {
        for (int slice = 0; slice < slices; ++slice)
        {
            const std::uint32_t a = start + static_cast<std::uint32_t>(ring) * stride + static_cast<std::uint32_t>(slice);
            const std::uint32_t b = a + stride;
            indices.insert(indices.end(), {a, a + 1, b + 1, a, b + 1, b});
        }
    }
}
} // namespace fadix::viewport_geometry
