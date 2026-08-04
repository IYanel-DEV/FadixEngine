#pragma once

#include "editor/camera/EditorCameraInput.hpp"
#include "editor/camera/Ortho2DCamera.hpp"
#include "editor/camera/WorkbenchCamera.hpp"
#include "engine/camera/EditorMode.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <optional>

namespace fadix
{
class ViewportRenderer;
}

namespace fadix::editor
{
struct CameraPreview
{
    glm::mat4 View{1.0F};
    glm::mat4 Projection{1.0F};
    glm::vec4 ClearColor{0.05F, 0.05F, 0.07F, 1.0F};
};

class CameraModule
{
public:
    void Update(float deltaSeconds);
    void ApplyToViewport(ViewportRenderer& viewport) const;

    void SetViewportMode(ViewportCameraMode mode) noexcept;
    void SetGameCamera(std::optional<CameraPreview> preview) noexcept;
    void SetProjectionMode(ViewportProjectionMode mode) noexcept;
    void SetCursorViewportPos(glm::vec2 viewportPos) noexcept;

    [[nodiscard]] WorkbenchCamera& Camera() noexcept;
    [[nodiscard]] const WorkbenchCamera& Camera() const noexcept;
    [[nodiscard]] Ortho2DCamera& Ortho2D() noexcept;
    [[nodiscard]] const Ortho2DCamera& Ortho2D() const noexcept;
    [[nodiscard]] EditorCameraInput& Input() noexcept;
    [[nodiscard]] const EditorCameraInput& Input() const noexcept;
    [[nodiscard]] ViewportCameraMode ViewportMode() const noexcept;
    [[nodiscard]] ViewportProjectionMode ProjectionMode() const noexcept;
    [[nodiscard]] const std::optional<CameraPreview>& GameCamera() const noexcept;

private:
    WorkbenchCamera m_Camera;
    Ortho2DCamera m_Ortho2D;
    EditorCameraInput m_Input;
    std::optional<CameraPreview> m_GameCamera;
    ViewportCameraMode m_ViewportMode{ViewportCameraMode::Edit};
    ViewportProjectionMode m_ProjectionMode{ViewportProjectionMode::Perspective};
    glm::vec2 m_CursorViewportPos{0.0F};
};
}
