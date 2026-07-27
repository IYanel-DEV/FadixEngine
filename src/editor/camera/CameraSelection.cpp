#include "editor/camera/CameraSelection.hpp"

#include <glm/common.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace fadix::editor
{
GameCameraSettings CameraSettingsFromComponent(const CameraComponent& component) noexcept
{
    GameCameraSettings settings{};
    settings.FieldOfView = component.FieldOfView;
    settings.NearPlane = component.NearPlane;
    settings.FarPlane = component.FarPlane;
    settings.Primary = component.Primary;
    SanitizeCameraSettings(settings);
    return settings;
}

void ApplyCameraSettings(const GameCameraSettings& settings, CameraComponent& component) noexcept
{
    GameCameraSettings sanitized = settings;
    SanitizeCameraSettings(sanitized);
    component.FieldOfView = sanitized.FieldOfView;
    component.NearPlane = sanitized.NearPlane;
    component.FarPlane = sanitized.FarPlane;
    component.Primary = sanitized.Primary;
}

void SanitizeCameraSettings(GameCameraSettings& settings) noexcept
{
    settings.FieldOfView = glm::clamp(settings.FieldOfView, 1.0F, 179.0F);
    settings.OrthographicSize = std::max(settings.OrthographicSize, 0.0001F);
    settings.NearPlane = std::max(settings.NearPlane, 0.0001F);
    settings.FarPlane = std::max(settings.FarPlane, settings.NearPlane + 0.0001F);
    settings.ClearColor = glm::clamp(settings.ClearColor, glm::vec4{0.0F}, glm::vec4{1.0F});
}

std::optional<std::size_t> SelectPrimaryCamera(const std::span<const CameraCandidate> cameras) noexcept
{
    const CameraCandidate* selected = nullptr;
    for (const CameraCandidate& camera : cameras)
    {
        if (!camera.Settings.Enabled)
        {
            continue;
        }
        if (selected == nullptr ||
            (camera.Settings.Primary && !selected->Settings.Primary) ||
            (camera.Settings.Primary == selected->Settings.Primary &&
             camera.Settings.Priority > selected->Settings.Priority))
        {
            selected = &camera;
        }
    }
    return selected != nullptr ? std::optional{selected->Id} : std::nullopt;
}

void MakePrimaryCamera(const std::span<CameraCandidate> cameras, const std::size_t id) noexcept
{
    for (CameraCandidate& camera : cameras)
    {
        camera.Settings.Primary = camera.Id == id;
    }
}

glm::mat4 BuildGameCameraView(
    const glm::vec3& position,
    const glm::vec3& forward,
    const glm::vec3& up)
{
    glm::vec3 safeForward = forward;
    if (glm::length(safeForward) < 0.0001F)
    {
        safeForward = glm::vec3{0.0F, 0.0F, -1.0F};
    }
    safeForward = glm::normalize(safeForward);

    glm::vec3 safeUp = up;
    if (glm::length(safeUp) < 0.0001F || std::abs(glm::dot(glm::normalize(safeUp), safeForward)) > 0.999F)
    {
        safeUp = std::abs(safeForward.y) < 0.999F
            ? glm::vec3{0.0F, 1.0F, 0.0F}
            : glm::vec3{0.0F, 0.0F, 1.0F};
    }
    return glm::lookAtRH(position, position + safeForward, glm::normalize(safeUp));
}

glm::mat4 BuildGameCameraProjection(const GameCameraSettings& settings, const float aspectRatio)
{
    GameCameraSettings sanitized = settings;
    SanitizeCameraSettings(sanitized);
    const float safeAspect = std::max(aspectRatio, 0.0001F);
    if (sanitized.Projection == CameraProjection::Orthographic)
    {
        const float halfHeight = sanitized.OrthographicSize * 0.5F;
        const float halfWidth = halfHeight * safeAspect;
        return glm::orthoRH_ZO(
            -halfWidth,
            halfWidth,
            -halfHeight,
            halfHeight,
            sanitized.NearPlane,
            sanitized.FarPlane);
    }
    return glm::perspectiveRH_ZO(
        glm::radians(sanitized.FieldOfView),
        safeAspect,
        sanitized.NearPlane,
        sanitized.FarPlane);
}
}
