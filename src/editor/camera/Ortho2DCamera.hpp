#pragma once

#include "engine/camera/EditorCamera.hpp"

#include <glm/vec2.hpp>

namespace fadix::editor
{
class Ortho2DCamera final : public fadix::EditorCamera
{
public:
    void Pan(glm::vec2 delta) override;
    void Dolly(float delta) override;
    void FocusBounds(const Bounds& bounds) override;
    [[nodiscard]] glm::mat4 View() const override;
    [[nodiscard]] glm::mat4 Projection() const override;

    void SetViewportSize(glm::vec2 size) noexcept;
    void ZoomAt(float delta, glm::vec2 cursorNdc);

    [[nodiscard]] glm::vec2 ScreenToWorld(glm::vec2 screenPos) const noexcept;
    [[nodiscard]] glm::vec2 WorldToScreen(glm::vec2 worldPos) const noexcept;
    [[nodiscard]] float OrthoHalfHeight() const noexcept { return m_OrthoHalfHeight; }
    [[nodiscard]] glm::vec2 Position() const noexcept { return m_Position; }
    [[nodiscard]] glm::vec2 ViewportSize() const noexcept { return m_ViewportSize; }

private:
    [[nodiscard]] float AspectRatio() const noexcept;

    glm::vec2 m_Position{0.0F};
    float m_OrthoHalfHeight{5.0F};
    glm::vec2 m_ViewportSize{1280.0F, 720.0F};
};
}
