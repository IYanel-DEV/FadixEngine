#pragma once

#include "engine/camera/EditorCamera.hpp"

#include <glm/vec3.hpp>

namespace fadix::editor
{
class WorkbenchCamera final : public fadix::EditorCamera
{
public:
    WorkbenchCamera();

    void Orbit(glm::vec2 delta) override;
    void Pan(glm::vec2 delta) override;
    void Dolly(float delta) override;
    void FocusBounds(const Bounds& bounds) override;
    [[nodiscard]] glm::mat4 View() const override;
    [[nodiscard]] glm::mat4 Projection() const override;
    [[nodiscard]] Frustum GetFrustum() const override;

    void Fly(glm::vec3 localDirection, float deltaSeconds, bool fast);
    void Look(glm::vec2 delta);
    void SetViewportSize(glm::vec2 size) noexcept;
    void SetPerspective(float verticalFieldOfViewDegrees, float nearPlane, float farPlane) noexcept;
    void SetFlySpeed(float unitsPerSecond) noexcept;
    void AdjustFlySpeed(float wheelDelta) noexcept;

    [[nodiscard]] const glm::vec3& Position() const noexcept;
    [[nodiscard]] glm::vec3 Forward() const noexcept;
    [[nodiscard]] glm::vec3 Right() const noexcept;
    [[nodiscard]] glm::vec3 Up() const noexcept;
    [[nodiscard]] float FlySpeed() const noexcept;
    [[nodiscard]] float AspectRatio() const noexcept;

private:
    void UpdateOrbitPosition();
    void UpdateAnglesFromForward(glm::vec3 forward);

    glm::vec3 m_Position{0.0F, 2.0F, 6.0F};
    glm::vec3 m_Pivot{0.0F};
    glm::vec2 m_ViewportSize{1280.0F, 720.0F};
    float m_Yaw{-90.0F};
    float m_Pitch{-18.434948F};
    float m_OrbitDistance{6.324555F};
    float m_FieldOfView{60.0F};
    float m_NearPlane{0.05F};
    float m_FarPlane{5000.0F};
    float m_FlySpeed{5.0F};
};
}
