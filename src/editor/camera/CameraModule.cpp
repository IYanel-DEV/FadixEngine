#include "editor/camera/CameraModule.hpp"

#include "engine/render/ViewportRenderer.hpp"

#include <utility>

namespace fadix::editor
{
void CameraModule::Update(const float deltaSeconds)
{
    m_Input.Update(m_Camera, deltaSeconds);
}

void CameraModule::ApplyToViewport(ViewportRenderer& viewport) const
{
    if (m_ViewportMode == ViewportCameraMode::Game && m_GameCamera.has_value())
    {
        viewport.SetCamera(m_GameCamera->View, m_GameCamera->Projection);
        return;
    }
    viewport.SetCamera(m_Camera.View(), m_Camera.Projection());
}

void CameraModule::SetViewportMode(const ViewportCameraMode mode) noexcept
{
    m_ViewportMode = mode;
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

const std::optional<CameraPreview>& CameraModule::GameCamera() const noexcept
{
    return m_GameCamera;
}
}
