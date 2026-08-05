#include "editor/camera/WorkbenchCamera.hpp"

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace fadix::editor
{
namespace
{
constexpr glm::vec3 WorldUp{0.0F, 1.0F, 0.0F};
constexpr float MinimumDistance = 0.05F;

glm::vec4 NormalizePlane(const glm::vec4& plane)
{
    const float length = glm::length(glm::vec3{plane});
    return length > 0.0F ? plane / length : plane;
}
}

WorkbenchCamera::WorkbenchCamera()
{
    UpdateAnglesFromForward(glm::normalize(m_Pivot - m_Position));
}

void WorkbenchCamera::Orbit(const glm::vec2 delta)
{
    m_Yaw += delta.x * 0.2F;
    m_Pitch = glm::clamp(m_Pitch - delta.y * 0.2F, -89.0F, 89.0F);
    UpdateOrbitPosition();
}

void WorkbenchCamera::Pan(const glm::vec2 delta)
{
    const float worldUnitsPerPixel =
        2.0F * m_OrbitDistance * std::tan(glm::radians(m_FieldOfView) * 0.5F) /
        std::max(m_ViewportSize.y, 1.0F);
    const glm::vec3 translation =
        Right() * (-delta.x * worldUnitsPerPixel) + Up() * (delta.y * worldUnitsPerPixel);
    m_Position += translation;
    m_Pivot += translation;
}

void WorkbenchCamera::Dolly(const float delta)
{
    const float scale = std::exp(-delta * 0.12F);
    m_OrbitDistance = glm::clamp(m_OrbitDistance * scale, MinimumDistance, m_FarPlane * 0.5F);
    UpdateOrbitPosition();
}

void WorkbenchCamera::FocusBounds(const Bounds& bounds)
{
    const glm::vec3 center = (bounds.Minimum + bounds.Maximum) * 0.5F;
    const glm::vec3 extents = glm::max((bounds.Maximum - bounds.Minimum) * 0.5F, glm::vec3{0.0F});
    const float radius = std::max(glm::length(extents), 0.1F);
    const float halfVerticalFov = glm::radians(m_FieldOfView) * 0.5F;
    const float halfHorizontalFov = std::atan(std::tan(halfVerticalFov) * AspectRatio());
    const float limitingHalfFov = std::max(std::min(halfVerticalFov, halfHorizontalFov), 0.01F);

    m_Pivot = center;
    m_OrbitDistance = glm::clamp(radius / std::sin(limitingHalfFov) * 1.1F, MinimumDistance, m_FarPlane * 0.5F);
    UpdateOrbitPosition();
}

glm::mat4 WorkbenchCamera::View() const
{
    return glm::lookAtRH(m_Position, m_Position + Forward(), Up());
}

glm::mat4 WorkbenchCamera::Projection() const
{
    return glm::perspectiveRH_ZO(glm::radians(m_FieldOfView), AspectRatio(), m_NearPlane, m_FarPlane);
}

Frustum WorkbenchCamera::GetFrustum() const
{
    const glm::mat4 matrix = glm::transpose(Projection() * View());
    Frustum frustum{};
    frustum.Planes[0] = NormalizePlane(matrix[3] + matrix[0]);
    frustum.Planes[1] = NormalizePlane(matrix[3] - matrix[0]);
    frustum.Planes[2] = NormalizePlane(matrix[3] + matrix[1]);
    frustum.Planes[3] = NormalizePlane(matrix[3] - matrix[1]);
    frustum.Planes[4] = NormalizePlane(matrix[2]);
    frustum.Planes[5] = NormalizePlane(matrix[3] - matrix[2]);
    return frustum;
}

void WorkbenchCamera::Fly(const glm::vec3 localDirection, const float deltaSeconds, const bool fast)
{
    glm::vec3 movement =
        Right() * localDirection.x + WorldUp * localDirection.y + Forward() * localDirection.z;
    const float length = glm::length(movement);
    if (length <= 0.0F || deltaSeconds <= 0.0F)
    {
        return;
    }

    movement /= length;
    const float multiplier = fast ? 4.0F : 1.0F;
    const glm::vec3 translation = movement * m_FlySpeed * multiplier * deltaSeconds;
    m_Position += translation;
    m_Pivot = m_Position + Forward() * m_OrbitDistance;
}

void WorkbenchCamera::Look(const glm::vec2 delta)
{
    m_Yaw += delta.x * 0.15F;
    m_Pitch = glm::clamp(m_Pitch - delta.y * 0.15F, -89.0F, 89.0F);
    m_Pivot = m_Position + Forward() * m_OrbitDistance;
}

void WorkbenchCamera::SetLookDirection(glm::vec3 forward) noexcept
{
    const float length = glm::length(forward);
    if (length <= 1.0e-6F)
    {
        return;
    }
    forward /= length;
    // Clamp near-vertical looks so Right()/Up() (cross with WorldUp) stay well-defined.
    m_Pitch = glm::clamp(
        glm::degrees(std::asin(glm::clamp(forward.y, -1.0F, 1.0F))), -89.0F, 89.0F);
    m_Yaw = glm::degrees(std::atan2(forward.z, forward.x));
    UpdateOrbitPosition();
}

void WorkbenchCamera::SetViewportSize(const glm::vec2 size) noexcept
{
    m_ViewportSize = glm::max(size, glm::vec2{1.0F});
}

void WorkbenchCamera::SetPerspective(
    const float verticalFieldOfViewDegrees,
    const float nearPlane,
    const float farPlane) noexcept
{
    m_FieldOfView = glm::clamp(verticalFieldOfViewDegrees, 1.0F, 179.0F);
    m_NearPlane = std::max(nearPlane, 0.0001F);
    m_FarPlane = std::max(farPlane, m_NearPlane + 0.0001F);
}

void WorkbenchCamera::SetFlySpeed(const float unitsPerSecond) noexcept
{
    m_FlySpeed = glm::clamp(unitsPerSecond, 0.01F, 10000.0F);
}

void WorkbenchCamera::AdjustFlySpeed(const float wheelDelta) noexcept
{
    SetFlySpeed(m_FlySpeed * std::exp(wheelDelta * 0.15F));
}

const glm::vec3& WorkbenchCamera::Position() const noexcept
{
    return m_Position;
}

glm::vec3 WorkbenchCamera::Forward() const noexcept
{
    const float yaw = glm::radians(m_Yaw);
    const float pitch = glm::radians(m_Pitch);
    return glm::normalize(glm::vec3{
        std::cos(yaw) * std::cos(pitch),
        std::sin(pitch),
        std::sin(yaw) * std::cos(pitch)});
}

glm::vec3 WorkbenchCamera::Right() const noexcept
{
    return glm::normalize(glm::cross(Forward(), WorldUp));
}

glm::vec3 WorkbenchCamera::Up() const noexcept
{
    return glm::normalize(glm::cross(Right(), Forward()));
}

float WorkbenchCamera::FlySpeed() const noexcept
{
    return m_FlySpeed;
}

float WorkbenchCamera::AspectRatio() const noexcept
{
    return m_ViewportSize.x / std::max(m_ViewportSize.y, 1.0F);
}

void WorkbenchCamera::UpdateOrbitPosition()
{
    m_Position = m_Pivot - Forward() * m_OrbitDistance;
}

void WorkbenchCamera::UpdateAnglesFromForward(const glm::vec3 forward)
{
    m_Pitch = glm::degrees(std::asin(glm::clamp(forward.y, -1.0F, 1.0F)));
    m_Yaw = glm::degrees(std::atan2(forward.z, forward.x));
}
}
