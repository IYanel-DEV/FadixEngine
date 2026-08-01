#pragma once

// Pure viewport math helpers extracted from ViewportRenderer.cpp: transform
// composition, ray/AABB intersection for picking, Halton jitter for TAA, and a
// couple of small utilities. No dependency on the renderer or the GPU.

#include "runtime/Components.hpp" // TransformComponent

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace fadix::viewport_math
{
[[nodiscard]] glm::mat4 ModelMatrix(const TransformComponent& transform);
[[nodiscard]] glm::mat4 ComposeMatrix(glm::vec3 position, glm::quat rotation, glm::vec3 scale);
[[nodiscard]] glm::quat RotationBetween(glm::vec3 from, glm::vec3 to);
[[nodiscard]] bool IntersectAabb(glm::vec3 origin, glm::vec3 direction, glm::vec3 minimum, glm::vec3 maximum, float& distance);
[[nodiscard]] float SmoothStep(float edge0, float edge1, float value) noexcept;
[[nodiscard]] glm::vec3 ViewForwardWorld(const glm::mat4& view) noexcept;
[[nodiscard]] float HaltonDigit(std::uint32_t index, std::uint32_t base) noexcept;
[[nodiscard]] glm::vec2 Halton23(std::uint32_t index) noexcept;
} // namespace fadix::viewport_math
