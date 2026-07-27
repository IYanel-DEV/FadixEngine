#pragma once

#include "engine/camera/CameraComponent.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <optional>
#include <span>

namespace fadix::editor
{
enum class CameraProjection
{
    Perspective,
    Orthographic
};

struct GameCameraSettings
{
    CameraProjection Projection{CameraProjection::Perspective};
    float FieldOfView{60.0F};
    float OrthographicSize{10.0F};
    float NearPlane{0.05F};
    float FarPlane{5000.0F};
    glm::vec4 ClearColor{0.05F, 0.05F, 0.07F, 1.0F};
    int Priority{0};
    bool Enabled{true};
    bool Primary{false};
};

struct CameraCandidate
{
    std::size_t Id{0};
    GameCameraSettings Settings{};
};

[[nodiscard]] GameCameraSettings CameraSettingsFromComponent(const CameraComponent& component) noexcept;
void ApplyCameraSettings(const GameCameraSettings& settings, CameraComponent& component) noexcept;
void SanitizeCameraSettings(GameCameraSettings& settings) noexcept;

[[nodiscard]] std::optional<std::size_t> SelectPrimaryCamera(
    std::span<const CameraCandidate> cameras) noexcept;
void MakePrimaryCamera(std::span<CameraCandidate> cameras, std::size_t id) noexcept;

[[nodiscard]] glm::mat4 BuildGameCameraView(
    const glm::vec3& position,
    const glm::vec3& forward,
    const glm::vec3& up);
[[nodiscard]] glm::mat4 BuildGameCameraProjection(
    const GameCameraSettings& settings,
    float aspectRatio);
}
