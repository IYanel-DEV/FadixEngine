#pragma once

#include <memory>

namespace fadix
{
class IApplicationHost;
class IAssetDatabase;
class IPhysicsWorld;
class IProjectService;
class IWorld;
class ViewportRenderer;
class ICollisionMeshProvider;

namespace rhi
{
class Device;
}

namespace editor
{
class CameraModule;
}

// Feature factories wired by EditorApplication after modules land.
namespace project
{
#ifdef CreateService
#undef CreateService
#endif
[[nodiscard]] std::unique_ptr<IProjectService> CreateService();
}

namespace camera
{
void RegisterModule(IApplicationHost& host);
[[nodiscard]] editor::CameraModule* Module() noexcept;
}

namespace sceneplay
{
[[nodiscard]] std::unique_ptr<IWorld> CreateEditWorld();
[[nodiscard]] std::unique_ptr<IPhysicsWorld> CreatePhysicsWorldAdapter(
    ICollisionMeshProvider* meshes = nullptr);
}

namespace assets
{
[[nodiscard]] std::unique_ptr<IAssetDatabase> CreateDatabase();
}
}
