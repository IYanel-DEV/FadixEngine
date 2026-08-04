#include "editor/camera/CameraModule.hpp"

#include "engine/render/ViewportRenderer.hpp"

#include <algorithm>
#include <utility>

namespace fadix::editor
{
void CameraModule::Update(const float deltaSeconds)
{
    if (m_ProjectionMode == ViewportProjectionMode::Ortho2D)
    {
        const float hw = std::max(m_Ortho2D.ViewportSize().x, 1.0F);
        const float hh = std::max(m_Ortho2D.ViewportSize().y, 1.0F);
        const glm::vec2 ndc{
            2.0F * m_CursorViewportPos.x / hw - 1.0F,
            1.0F - 2.0F * m_CursorViewportPos.y / hh};
        m_Input.UpdateOrtho2D(m_Ortho2D, ndc);
    }
    else
    {
        m_Input.Update(m_Camera, deltaSeconds);
    }
}

void CameraModule::ApplyToViewport(ViewportRenderer& viewport) const
{
    if (m_ViewportMode == ViewportCameraMode::Game && m_GameCamera.has_value())
    {
        viewport.SetCamera(m_GameCamera->View, m_GameCamera->Projection);
        return;
    }
    if (m_ProjectionMode == ViewportProjectionMode::Ortho2D)
    {
        viewport.SetCamera(m_Ortho2D.View(), m_Ortho2D.Projection());
        return;
    }
    viewport.SetCamera(m_Camera.View(), m_Camera.Projection());
}

void CameraModule::SetViewportMode(const ViewportCameraMode mode) noexcept
{
    m_ViewportMode = mode;
}

void CameraModule::SetProjectionMode(const ViewportProjectionMode mode) noexcept
{
    m_ProjectionMode = mode;
}

void CameraModule::SetCursorViewportPos(const glm::vec2 viewportPos) noexcept
{
    m_CursorViewportPos = viewportPos;
}

void CameraModule::SetGameCamera(std::optional<CameraPreview> preview) noexcept
{
    m_GameCamera = std::move(preview);
}

WorkbenchCamera& CameraModule::Camera() noexcept
{
    return m_Camera;
}

const WorkbenchCamera& CameraModule::Camera() const noexcept
{
    return m_Camera;
}

Ortho2DCamera& CameraModule::Ortho2D() noexcept
{
    return m_Ortho2D;
}

const Ortho2DCamera& CameraModule::Ortho2D() const noexcept
{
    return m_Ortho2D;
}

EditorCameraInput& CameraModule::Input() noexcept
{
    return m_Input;
}

const EditorCameraInput& CameraModule::Input() const noexcept
{
    return m_Input;
}

ViewportCameraMode CameraModule::ViewportMode() const noexcept
{
    return m_ViewportMode;
}

ViewportProjectionMode CameraModule::ProjectionMode() const noexcept
{
    return m_ProjectionMode;
}

const std::optional<CameraPreview>& CameraModule::GameCamera() const noexcept
{
    return m_GameCamera;
}
}
