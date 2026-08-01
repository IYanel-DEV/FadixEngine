#include "render/ViewportMath.hpp"

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace fadix::viewport_math
{
glm::mat4 ModelMatrix(const TransformComponent& transform)
{
    return glm::translate(glm::mat4{1.0F}, transform.Position) *
           glm::mat4_cast(transform.Rotation) *
           glm::scale(glm::mat4{1.0F}, transform.Scale);
}

glm::mat4 ComposeMatrix(const glm::vec3 position, const glm::quat rotation, const glm::vec3 scale)
{
    return glm::translate(glm::mat4{1.0F}, position) * glm::mat4_cast(rotation) *
        glm::scale(glm::mat4{1.0F}, scale);
}

glm::quat RotationBetween(const glm::vec3 from, const glm::vec3 to)
{
    const glm::vec3 f = glm::normalize(from);
    const glm::vec3 t = glm::normalize(to);
    const float cosine = glm::dot(f, t);
    if (cosine > 0.9999F)
    {
        return glm::quat{1.0F, 0.0F, 0.0F, 0.0F};
    }
    if (cosine < -0.9999F)
    {
        const glm::vec3 orthogonal = std::abs(f.x) < 0.9F
            ? glm::normalize(glm::cross(f, glm::vec3{1, 0, 0}))
            : glm::normalize(glm::cross(f, glm::vec3{0, 1, 0}));
        return glm::angleAxis(glm::pi<float>(), orthogonal);
    }
    const glm::vec3 axis = glm::normalize(glm::cross(f, t));
    return glm::angleAxis(std::acos(std::clamp(cosine, -1.0F, 1.0F)), axis);
}

bool IntersectAabb(
    const glm::vec3 origin,
    const glm::vec3 direction,
    const glm::vec3 minimum,
    const glm::vec3 maximum,
    float& distance)
{
    float nearest = 0.0F;
    float farthest = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis)
    {
        if (std::abs(direction[axis]) < 1.0e-6F)
        {
            if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
            {
                return false;
            }
            continue;
        }
        const float inverse = 1.0F / direction[axis];
        float first = (minimum[axis] - origin[axis]) * inverse;
        float second = (maximum[axis] - origin[axis]) * inverse;
        if (first > second)
        {
            std::swap(first, second);
        }
        nearest = std::max(nearest, first);
        farthest = std::min(farthest, second);
        if (nearest > farthest)
        {
            return false;
        }
    }
    distance = nearest;
    return true;
}

float SmoothStep(const float edge0, const float edge1, const float value) noexcept
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

glm::vec3 ViewForwardWorld(const glm::mat4& view) noexcept
{
    // GLM lookAtRH stores -forward in column 2 of the view matrix.
    return glm::normalize(-glm::vec3{view[0][2], view[1][2], view[2][2]});
}

float HaltonDigit(const std::uint32_t index, const std::uint32_t base) noexcept
{
    float f = 1.0F;
    float r = 0.0F;
    std::uint32_t i = index;
    while (i > 0)
    {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

// Halton (2,3) in [0,1)^2 for projection jitter.
glm::vec2 Halton23(const std::uint32_t index) noexcept
{
    return {HaltonDigit(index, 2), HaltonDigit(index, 3)};
}
} // namespace fadix::viewport_math
