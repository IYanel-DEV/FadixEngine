#include "engine/app/ModuleRegistration.hpp"

#include "assets/AssetDatabase.hpp"
#include "engine/physics/IPhysicsWorld.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/scene/IWorld.hpp"
#include "render/ViewportRendererFactory.hpp"
#include "runtime/World.hpp"

#include <filesystem>

#ifdef FADIX_ENABLE_PHYSICS
#include "physics/PhysicsWorld.hpp"
#endif

namespace fadix::sceneplay
{
std::unique_ptr<IWorld> CreateEditWorld()
{
    return CreateEditorWorld();
}

std::unique_ptr<IPhysicsWorld> CreatePhysicsWorldAdapter(
    ICollisionMeshProvider* meshes)
{
    // Optional means optional: the editor should still boot when physics stayed home.
#ifdef FADIX_ENABLE_PHYSICS
    return CreatePhysicsWorld(meshes);
#else
    static_cast<void>(meshes);
    return nullptr;
#endif
}
}

namespace fadix::assets
{
std::unique_ptr<IAssetDatabase> CreateDatabase()
{
    return CreateAssetDatabase(std::filesystem::path{});
}
}
