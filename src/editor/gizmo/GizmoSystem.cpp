#include "editor/gizmo/GizmoSystem.hpp"

#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace fadix
{
void GizmoSystem::SetMode(const GizmoMode mode) noexcept { m_Mode = mode; }
void GizmoSystem::SetSnap(const GizmoSnap snap) noexcept { m_Snap = snap; }
GizmoMode GizmoSystem::Mode() const noexcept { return m_Mode; }

GizmoDrawData GizmoSystem::BuildDrawData(const IWorld& world, const Uuid& entity, const float size) const
{
    GizmoDrawData data;
    const auto found = world.Find(entity);
    if (!found)
    {
        return data;
    }
    const TransformComponent* transform = world.Registry().try_get<TransformComponent>(*found);
    if (transform == nullptr)
    {
        return data;
    }
    constexpr std::array axes{
        std::pair{glm::vec3{1.0F, 0.0F, 0.0F}, glm::vec3{1.0F, 0.15F, 0.15F}},
        std::pair{glm::vec3{0.0F, 1.0F, 0.0F}, glm::vec3{0.15F, 1.0F, 0.15F}},
        std::pair{glm::vec3{0.0F, 0.0F, 1.0F}, glm::vec3{0.2F, 0.45F, 1.0F}}};
    for (const auto& [axis, color] : axes)
    {
        data.Lines.push_back({transform->Position, transform->Position + axis * size, color});
    }
    return data;
}

glm::vec3 GizmoSystem::HandleAxisDirection() const
{
    return m_Frame * GizmoAxisVector(GizmoHandleAxis(m_Handle));
}

// Plane used to track the cursor for the current handle: the handle plane for
// plane handles, the rotation plane for rings, or a camera-facing plane
// containing the axis for shaft drags.
glm::vec3 GizmoSystem::HandlePlaneNormal(const GizmoRay& ray) const
{
    const glm::vec3 axis = HandleAxisDirection();
    if (!GizmoHandleIsAxis(m_Handle) && m_Handle != GizmoHandle::Uniform)
    {
        return axis;
    }
    if (m_Mode == GizmoMode::Rotate)
    {
        return axis;
    }
    if (m_Handle == GizmoHandle::Uniform)
    {
        return -ray.Direction;
    }
    // Plane containing the axis, as perpendicular to the view ray as possible.
    const glm::vec3 toPlane = glm::cross(axis, glm::cross(ray.Direction, axis));
    const float length = glm::length(toPlane);
    return length > 1.0e-5F ? toPlane / length : glm::vec3{0.0F, 1.0F, 0.0F};
}

std::optional<glm::vec3> GizmoSystem::IntersectPlane(
    const GizmoRay& ray, const glm::vec3& point, const glm::vec3& normal) const
{
    const float denominator = glm::dot(normal, ray.Direction);
    if (std::abs(denominator) < 1.0e-6F)
    {
        return std::nullopt;
    }
    const float t = glm::dot(point - ray.Origin, normal) / denominator;
    if (t < 0.0F)
    {
        return std::nullopt;
    }
    return ray.Origin + ray.Direction * t;
}

std::optional<float> GizmoSystem::ClosestAxisParameter(
    const GizmoRay& ray, const glm::vec3& axisPoint, const glm::vec3& axisDirection) const
{
    // Closest point between the axis line and the mouse ray, as an axis parameter.
    const glm::vec3 w = axisPoint - ray.Origin;
    const float b = glm::dot(axisDirection, ray.Direction);
    const float d = glm::dot(axisDirection, w);
    const float e = glm::dot(ray.Direction, w);
    const float denominator = 1.0F - b * b;
    if (std::abs(denominator) < 1.0e-6F)
    {
        return std::nullopt; // Axis is parallel to the view ray.
    }
    return (b * e - d) / denominator;
}

bool GizmoSystem::Begin(
    IWorld& world,
    const Uuid entity,
    const GizmoHandle handle,
    const GizmoRay& ray,
    const glm::quat& orientation)
{
    const auto found = world.Find(entity);
    if (!found)
    {
        return false;
    }
    const TransformComponent* transform = world.Registry().try_get<TransformComponent>(*found);
    if (transform == nullptr)
    {
        return false;
    }
    m_Entity = entity;
    m_Handle = handle;
    m_Frame = orientation;
    m_InitialPosition = transform->Position;
    m_InitialScale = transform->Scale;
    m_InitialRotation = transform->Rotation;

    const glm::vec3 axis = HandleAxisDirection();
    if (m_Mode == GizmoMode::Rotate)
    {
        const auto hit = IntersectPlane(ray, m_InitialPosition, axis);
        if (!hit)
        {
            m_Entity.reset();
            return false;
        }
        const glm::vec3 offset = *hit - m_InitialPosition;
        if (glm::dot(offset, offset) < 1.0e-8F)
        {
            m_Entity.reset();
            return false;
        }
        m_RotateStartVector = glm::normalize(offset);
        return true;
    }

    if (m_Handle == GizmoHandle::Uniform)
    {
        const auto hit = IntersectPlane(ray, m_InitialPosition, HandlePlaneNormal(ray));
        m_StartUniformDistance =
            hit ? std::max(glm::length(*hit - m_InitialPosition), 0.05F) : 1.0F;
        return true;
    }

    if (GizmoHandleIsAxis(m_Handle))
    {
        const auto parameter = ClosestAxisParameter(ray, m_InitialPosition, axis);
        if (!parameter)
        {
            m_Entity.reset();
            return false;
        }
        m_StartAxisParameter = *parameter;
        return true;
    }

    // Plane translate handles: anchor on the handle plane.
    const auto hit = IntersectPlane(ray, m_InitialPosition, axis);
    if (!hit)
    {
        m_Entity.reset();
        return false;
    }
    m_StartPlanePoint = *hit;
    return true;
}

float GizmoSystem::Snap(const float value, const float increment) const
{
    return !m_Snap.Enabled || increment <= 0.0F ? value : std::round(value / increment) * increment;
}

bool GizmoSystem::Update(IWorld& world, const GizmoRay& ray)
{
    if (!m_Entity)
    {
        return false;
    }
    const auto entity = world.Find(*m_Entity);
    if (!entity)
    {
        return false;
    }
    TransformComponent* transform = world.Registry().try_get<TransformComponent>(*entity);
    if (transform == nullptr)
    {
        return false;
    }

    const glm::vec3 axis = HandleAxisDirection();

    if (m_Mode == GizmoMode::Translate)
    {
        if (GizmoHandleIsAxis(m_Handle))
        {
            const auto parameter = ClosestAxisParameter(ray, m_InitialPosition, axis);
            if (!parameter)
            {
                return false;
            }
            const float amount = Snap(*parameter - m_StartAxisParameter, m_Snap.Translation);
            transform->Position = m_InitialPosition + axis * amount;
        }
        else
        {
            const auto hit = IntersectPlane(ray, m_InitialPosition, axis);
            if (!hit)
            {
                return false;
            }
            glm::vec3 delta = *hit - m_StartPlanePoint;
            if (m_Snap.Enabled)
            {
                // Snap the two in-plane components in the gizmo frame.
                const glm::quat inverseFrame = glm::inverse(m_Frame);
                glm::vec3 local = inverseFrame * delta;
                local.x = Snap(local.x, m_Snap.Translation);
                local.y = Snap(local.y, m_Snap.Translation);
                local.z = Snap(local.z, m_Snap.Translation);
                delta = m_Frame * local;
            }
            transform->Position = m_InitialPosition + delta;
        }
        return true;
    }

    if (m_Mode == GizmoMode::Rotate)
    {
        const auto hit = IntersectPlane(ray, m_InitialPosition, axis);
        if (!hit)
        {
            return false;
        }
        const glm::vec3 offset = *hit - m_InitialPosition;
        if (glm::dot(offset, offset) < 1.0e-8F)
        {
            return false;
        }
        const glm::vec3 current = glm::normalize(offset);
        const float cosine =
            std::clamp(glm::dot(m_RotateStartVector, current), -1.0F, 1.0F);
        float angle = std::acos(cosine);
        if (glm::dot(glm::cross(m_RotateStartVector, current), axis) < 0.0F)
        {
            angle = -angle;
        }
        angle = glm::radians(Snap(glm::degrees(angle), m_Snap.RotationDegrees));
        transform->Rotation = glm::normalize(glm::angleAxis(angle, axis) * m_InitialRotation);
        return true;
    }

    // Scale.
    if (m_Handle == GizmoHandle::Uniform)
    {
        const auto hit = IntersectPlane(ray, m_InitialPosition, HandlePlaneNormal(ray));
        if (!hit)
        {
            return false;
        }
        const float distance = std::max(glm::length(*hit - m_InitialPosition), 1.0e-4F);
        float factor = distance / m_StartUniformDistance;
        factor = std::max(1.0e-3F, 1.0F + Snap(factor - 1.0F, m_Snap.Scale));
        transform->Scale = glm::max(glm::abs(m_InitialScale * factor), glm::vec3{1.0e-3F});
        return true;
    }
    const auto parameter = ClosestAxisParameter(ray, m_InitialPosition, axis);
    if (!parameter)
    {
        return false;
    }
    const float amount = Snap(*parameter - m_StartAxisParameter, m_Snap.Scale);
    // Scale along the handle's local axis component only, never through zero.
    glm::vec3 scale = m_InitialScale;
    const int component = static_cast<int>(GizmoHandleAxis(m_Handle));
    scale[component] = m_InitialScale[component] + amount;
    scale[component] = scale[component] < 0.0F
        ? 1.0e-3F
        : std::max(scale[component], 1.0e-3F);
    transform->Scale = scale;
    return true;
}

void GizmoSystem::Cancel(IWorld& world)
{
    if (m_Entity)
    {
        if (const auto entity = world.Find(*m_Entity))
        {
            if (TransformComponent* transform = world.Registry().try_get<TransformComponent>(*entity))
            {
                transform->Position = m_InitialPosition;
                transform->Scale = m_InitialScale;
                transform->Rotation = m_InitialRotation;
            }
        }
    }
    End();
}

void GizmoSystem::End() noexcept { m_Entity.reset(); }
bool GizmoSystem::Active() const noexcept { return m_Entity.has_value(); }
}
