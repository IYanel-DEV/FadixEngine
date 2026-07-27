#include "engine/app/ModuleRegistration.hpp"

#include "editor/camera/CameraModule.hpp"
#include "engine/app/IApplicationHost.hpp"
#include "engine/camera/EditorMode.hpp"

#include <memory>

namespace fadix::camera
{
namespace
{
std::unique_ptr<editor::CameraModule> g_Module;
}

void RegisterModule(IApplicationHost& host)
{
    if (g_Module == nullptr)
    {
        g_Module = std::make_unique<editor::CameraModule>();
    }

    // Keep edit/game viewport mode aligned with the host play state on register.
    g_Module->SetViewportMode(
        host.PlayMode() == EditorPlayMode::Edit ? ViewportCameraMode::Edit
                                                : ViewportCameraMode::Game);
    g_Module->ApplyToViewport(host.Viewport());
}

editor::CameraModule* Module() noexcept
{
    return g_Module.get();
}
}
