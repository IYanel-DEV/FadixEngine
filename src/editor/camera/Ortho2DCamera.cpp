#include "editor/camera/Ortho2DCamera.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace fadix::editor
{
float Ortho2DCamera::AspectRatio() const noexcept
{
    return m_ViewportSize.x / std::max(m_ViewportSize.y, 1.0F);
}

void Ortho2DCamera::Pan(glm::vec2 delta)
{
    const float aspect = AspectRatio();
    const float scaleX = (2.0F * m_OrthoHalfHeight * aspect) / std::max(m_ViewportSize.x, 1.0F);
    const float scaleY = (2.0F * m_OrthoHalfHeight) / std::max(m_ViewportSize.y, 1.0F);
    m_Position.x -= delta.x * scaleX;
    m_Position.y += delta.y * scaleY;
}

void Ortho2DCamera::Dolly(float delta)
{
    const float factor = std::pow(0.85F, delta);
    m_OrthoHalfHeight = std::clamp(m_OrthoHalfHeight * factor, 0.01F, 100000.0F);
}

void Ortho2DCamera::ZoomAt(const float delta, const glm::vec2 cursorNdc)
{
    const float oldH = m_OrthoHalfHeight;
    const float factor = std::pow(0.85F, delta);
    const float newH = std::clamp(oldH * factor, 0.01F, 100000.0F);
    const float aspect = AspectRatio();
    m_Position.x += cursorNdc.x * (oldH * aspect - newH * aspect);
    m_Position.y += cursorNdc.y * (oldH - newH);
    m_OrthoHalfHeight = newH;
}

void Ortho2DCamera::FocusBounds(const Bounds& bounds)
{
    m_Position.x = (bounds.Minimum.x + bounds.Maximum.x) * 0.5F;
    m_Position.y = (bounds.Minimum.y + bounds.Maximum.y) * 0.5F;
    const float extentX = (bounds.Maximum.x - bounds.Minimum.x) * 0.6F;
    const float extentY = (bounds.Maximum.y - bounds.Minimum.y) * 0.6F;
    const float aspect = AspectRatio();
    m_OrthoHalfHeight = std::max({extentY, extentX / std::max(aspect, 0.001F), 0.5F});
}

glm::mat4 Ortho2DCamera::View() const
{
    return glm::lookAt(
        glm::vec3{m_Position.x, m_Position.y, 10.0F},
        glm::vec3{m_Position.x, m_Position.y, 0.0F},
        glm::vec3{0.0F, 1.0F, 0.0F});
}

glm::mat4 Ortho2DCamera::Projection() const
{
    const float hw = m_OrthoHalfHeight * AspectRatio();
    return glm::ortho(-hw, hw, -m_OrthoHalfHeight, m_OrthoHalfHeight, -1000.0F, 1000.0F);
}

void Ortho2DCamera::SetViewportSize(const glm::vec2 size) noexcept
{
    m_ViewportSize = {std::max(size.x, 1.0F), std::max(size.y, 1.0F)};
}

glm::vec2 Ortho2DCamera::ScreenToWorld(const glm::vec2 screenPos) const noexcept
{
    const float ndcX = 2.0F * screenPos.x / std::max(m_ViewportSize.x, 1.0F) - 1.0F;
    const float ndcY = 1.0F - 2.0F * screenPos.y / std::max(m_ViewportSize.y, 1.0F);
    const float hw = m_OrthoHalfHeight * AspectRatio();
    return {m_Position.x + ndcX * hw, m_Position.y + ndcY * m_OrthoHalfHeight};
}

glm::vec2 Ortho2DCamera::WorldToScreen(const glm::vec2 worldPos) const noexcept
{
    const float hw = std::max(m_OrthoHalfHeight * AspectRatio(), 0.0001F);
    const float hh = std::max(m_OrthoHalfHeight, 0.0001F);
    const float ndcX = (worldPos.x - m_Position.x) / hw;
    const float ndcY = (worldPos.y - m_Position.y) / hh;
    return {(ndcX + 1.0F) * 0.5F * m_ViewportSize.x,
        (1.0F - ndcY) * 0.5F * m_ViewportSize.y};
}
}
