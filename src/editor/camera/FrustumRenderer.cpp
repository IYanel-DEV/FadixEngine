#include "editor/camera/FrustumRenderer.hpp"

#include <glm/matrix.hpp>

#include <array>

namespace fadix::editor
{
std::vector<DebugLine> BuildFrustumLines(
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec4 color)
{
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
    constexpr std::array<glm::vec3, 8> clipCorners{
        glm::vec3{-1.0F, -1.0F, 0.0F},
        glm::vec3{1.0F, -1.0F, 0.0F},
        glm::vec3{1.0F, 1.0F, 0.0F},
        glm::vec3{-1.0F, 1.0F, 0.0F},
        glm::vec3{-1.0F, -1.0F, 1.0F},
        glm::vec3{1.0F, -1.0F, 1.0F},
        glm::vec3{1.0F, 1.0F, 1.0F},
        glm::vec3{-1.0F, 1.0F, 1.0F}};
    constexpr std::array<std::array<std::size_t, 2>, 12> edges{{
        {0, 1}, {1, 2}, {2, 3}, {3, 0},
        {4, 5}, {5, 6}, {6, 7}, {7, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}}};

    std::array<glm::vec3, 8> worldCorners{};
    for (std::size_t index = 0; index < clipCorners.size(); ++index)
    {
        const glm::vec4 homogeneous =
            inverseViewProjection * glm::vec4{clipCorners[index], 1.0F};
        worldCorners[index] = glm::vec3{homogeneous} / homogeneous.w;
    }

    std::vector<DebugLine> lines;
    lines.reserve(edges.size());
    for (const auto& edge : edges)
    {
        lines.push_back(DebugLine{worldCorners[edge[0]], worldCorners[edge[1]], color});
    }
    return lines;
}

void DrawFrustum(
    ICameraDebugDraw& draw,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec4 color)
{
    const std::vector<DebugLine> lines = BuildFrustumLines(view, projection, color);
    draw.DrawCameraLines(lines);
}
}
