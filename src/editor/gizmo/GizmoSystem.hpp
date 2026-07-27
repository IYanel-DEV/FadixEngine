#pragma once

#include "engine/Uuid.hpp"
#include "engine/render/GizmoTypes.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <optional>
#include <vector>

namespace fadix
{
class IWorld;

struct GizmoLine
{
    glm::vec3 Start;
    glm::vec3 End;
    glm::vec3 Color;
};

struct GizmoDrawData
{
    std::vector<GizmoLine> Lines;
};

struct GizmoSnap
{
    bool Enabled{false};
    float Translation{0.5F};
    float RotationDegrees{15.0F};
    float Scale{0.1F};
};

// A world-space mouse ray inside the viewport.
struct GizmoRay
{
    glm::vec3 Origin{0.0F};
    glm::vec3 Direction{0.0F, 0.0F, -1.0F};
};

class GizmoSystem final
{
public:
    void SetMode(GizmoMode mode) noexcept;
    void SetSnap(GizmoSnap snap) noexcept;
    [[nodiscard]] GizmoMode Mode() const noexcept;
    [[nodiscard]] GizmoDrawData BuildDrawData(const IWorld& world, const Uuid& entity, float size = 1.0F) const;

    // Starts a drag on `handle`. `orientation` is the gizmo's frame (identity
    // for world space, the entity rotation for local space); the ray anchors
    // the drag so the grabbed point follows the cursor exactly.
    [[nodiscard]] bool Begin(
        IWorld& world,
        Uuid entity,
        GizmoHandle handle,
        const GizmoRay& ray,
        const glm::quat& orientation);

    // Updates the active drag from the current mouse ray.
    [[nodiscard]] bool Update(IWorld& world, const GizmoRay& ray);

    void Cancel(IWorld& world);
    void End() noexcept;
    [[nodiscard]] bool Active() const noexcept;

private:
    [[nodiscard]] float Snap(float value, float increment) const;
    [[nodiscard]] glm::vec3 HandleAxisDirection() const;
    [[nodiscard]] glm::vec3 HandlePlaneNormal(const GizmoRay& ray) const;
    [[nodiscard]] std::optional<glm::vec3> IntersectPlane(
        const GizmoRay& ray, const glm::vec3& point, const glm::vec3& normal) const;
    [[nodiscard]] std::optional<float> ClosestAxisParameter(
        const GizmoRay& ray, const glm::vec3& axisPoint, const glm::vec3& axisDirection) const;

    GizmoMode m_Mode{GizmoMode::Translate};
    GizmoSnap m_Snap;
    std::optional<Uuid> m_Entity;
    GizmoHandle m_Handle{GizmoHandle::AxisX};
    glm::quat m_Frame{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 m_InitialPosition{0.0F};
    glm::vec3 m_InitialScale{1.0F};
    glm::quat m_InitialRotation{1.0F, 0.0F, 0.0F, 0.0F};
    // Drag anchors captured in Begin.
    float m_StartAxisParameter{0.0F};
    glm::vec3 m_StartPlanePoint{0.0F};
    glm::vec3 m_RotateStartVector{1.0F, 0.0F, 0.0F};
    float m_StartUniformDistance{1.0F};
};
}
