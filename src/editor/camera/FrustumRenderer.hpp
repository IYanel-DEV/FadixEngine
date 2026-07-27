#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <span>
#include <vector>

namespace fadix::editor
{
struct DebugLine
{
    glm::vec3 Start{0.0F};
    glm::vec3 End{0.0F};
    glm::vec4 Color{1.0F};
};

class ICameraDebugDraw
{
public:
    virtual ~ICameraDebugDraw() = default;
    virtual void DrawCameraLines(std::span<const DebugLine> lines) = 0;
};

[[nodiscard]] std::vector<DebugLine> BuildFrustumLines(
    const glm::mat4& view,
    const glm::mat4& projection,
    glm::vec4 color = glm::vec4{1.0F, 0.75F, 0.1F, 1.0F});

void DrawFrustum(
    ICameraDebugDraw& draw,
    const glm::mat4& view,
    const glm::mat4& projection,
    glm::vec4 color = glm::vec4{1.0F, 0.75F, 0.1F, 1.0F});
}
