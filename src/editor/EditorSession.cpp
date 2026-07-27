#include "editor/EditorSession.hpp"

#include "editor/play/PlaySession.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/project/IProjectService.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/scene/IWorld.hpp"
#include "project/ProjectService.hpp"
#include "runtime/Components.hpp"

#include <glm/gtc/constants.hpp>

#include <system_error>
#include <utility>

namespace fadix
{
namespace
{
bool EnsureCelestialLights(IWorld& world)
{
    entt::registry& registry = world.Registry();
    entt::entity sunEntity = entt::null;
    entt::entity fallbackEntity = entt::null;
    for (const auto [entity, light] : registry.view<DirectionalLightComponent>().each())
    {
        if (light.CelestialBody == CelestialLightType::Sun && sunEntity == entt::null)
        {
            sunEntity = entity;
        }
        if (light.CelestialBody == CelestialLightType::None && fallbackEntity == entt::null)
        {
            fallbackEntity = entity;
        }
    }

    bool changed = false;
    if (sunEntity == entt::null && fallbackEntity != entt::null)
    {
        sunEntity = fallbackEntity;
        registry.get<DirectionalLightComponent>(sunEntity).CelestialBody = CelestialLightType::Sun;
        changed = true;
    }
    if (sunEntity == entt::null)
    {
        sunEntity = world.Create();
        registry.emplace<NameComponent>(sunEntity, "Sun Light");
        registry.emplace<TransformComponent>(sunEntity,
            TransformComponent{{2.0F, 4.0F, 1.0F},
                glm::quat{glm::radians(glm::vec3{-45.0F, -30.0F, 0.0F})}});
        registry.emplace<DirectionalLightComponent>(sunEntity, MakeSunLight());
        changed = true;
    }

    bool hasMoon = false;
    for (const auto [entity, light] : registry.view<const DirectionalLightComponent>().each())
    {
        hasMoon = hasMoon || light.CelestialBody == CelestialLightType::Moon;
        static_cast<void>(entity);
    }
    if (!hasMoon)
    {
        const TransformComponent* sunTransform = registry.try_get<TransformComponent>(sunEntity);
        TransformComponent moonTransform;
        moonTransform.Position = {-2.0F, 4.0F, -1.0F};
        if (sunTransform != nullptr)
        {
            moonTransform.Rotation =
                sunTransform->Rotation *
                glm::angleAxis(glm::pi<float>(), glm::vec3{0.0F, 1.0F, 0.0F});
        }
        const entt::entity moonEntity = world.Create();
        registry.emplace<NameComponent>(moonEntity, "Moon Light");
        registry.emplace<TransformComponent>(moonEntity, moonTransform);
        if (const RelationshipComponent* relationship =
                registry.try_get<RelationshipComponent>(sunEntity))
        {
            registry.emplace<RelationshipComponent>(moonEntity, *relationship);
        }
        registry.emplace<DirectionalLightComponent>(moonEntity, MakeMoonLight());
        changed = true;
    }
    return changed;
}
}

EditorSession::EditorSession()
    : projectService(project::CreateService()),
      assetDatabase(assets::CreateDatabase()),
      editWorld(sceneplay::CreateEditWorld()),
      scenes(std::make_unique<SceneService>()),
      play(std::make_unique<PlaySession>())
{
}

EditorSession::~EditorSession() = default;

IProjectService& EditorSession::Projects() noexcept
{
    return *projectService;
}

IWorld& EditorSession::EditWorld() noexcept
{
    return *editWorld;
}

IWorld* EditorSession::PlayWorld() noexcept
{
    return play ? play->RuntimeWorld() : nullptr;
}

ViewportRenderer& EditorSession::Viewport() noexcept
{
    return *viewport;
}

UndoStack& EditorSession::History() noexcept
{
    return history;
}

IAssetDatabase& EditorSession::Assets() noexcept
{
    return *assetDatabase;
}

EditorPlayMode EditorSession::PlayMode() const noexcept
{
    return playMode;
}

ProjectMetadata& EditorSession::ActiveProject() noexcept
{
    return activeProject;
}

const ProjectMetadata& EditorSession::ActiveProject() const noexcept
{
    return activeProject;
}

SceneDocument& EditorSession::Document() noexcept
{
    return document;
}

const SceneDocument& EditorSession::Document() const noexcept
{
    return document;
}

SceneService& EditorSession::Scenes() noexcept
{
    return *scenes;
}

PlaySession& EditorSession::Play() noexcept
{
    return *play;
}

bool EditorSession::SaveAllPending() const noexcept
{
    return saveAllPending;
}

void EditorSession::SetSaveAllPending(const bool pending) noexcept
{
    saveAllPending = pending;
}

void EditorSession::SetProjectService(std::unique_ptr<IProjectService> service) noexcept
{
    if (service)
    {
        projectService = std::move(service);
    }
}

void EditorSession::SetAssetDatabase(std::unique_ptr<IAssetDatabase> database) noexcept
{
    if (database)
    {
        assetDatabase = std::move(database);
        if (viewport != nullptr && assetDatabase != nullptr)
        {
            viewport->SetAssetDatabase(*assetDatabase);
        }
    }
}

void EditorSession::SetViewportRenderer(std::unique_ptr<ViewportRenderer> renderer) noexcept
{
    if (renderer)
    {
        viewport = std::move(renderer);
    }
}

void EditorSession::SetEditWorld(std::unique_ptr<IWorld> world) noexcept
{
    if (world)
    {
        editWorld = std::move(world);
    }
}

Result<void> EditorSession::BeginProject(const ProjectMetadata& project)
{
    activeProject = project;
    const std::string relative =
        project.DefaultScene.empty() ? "Scenes/Main.scene" : project.DefaultScene;
    document = SceneDocument{
        Uuid::Generate(),
        std::filesystem::path{relative}.stem().string(),
        project.RootPath / relative,
        false};
    if (!std::filesystem::exists(document.Path))
    {
        return Result<void>::Ok();
    }
    return scenes->Load(document, *editWorld, document.Path);
}

void EditorSession::LeavePlay() noexcept
{
    if (play)
    {
        play->Stop();
    }
    playMode = EditorPlayMode::Edit;
}

bool EditorSession::SetPlayMode(const EditorPlayMode mode)
{
    if (mode == EditorPlayMode::Play)
    {
        if (play->State() == PlayState::Paused)
        {
            play->Resume();
        }
        else if (play->State() == PlayState::Stopped)
        {
            auto physics = sceneplay::CreatePhysicsWorldAdapter(collisionMeshes);
            if (!play->Start(*editWorld, std::move(physics)))
            {
                return false;
            }
        }
        playMode = EditorPlayMode::Play;
        return true;
    }
    if (mode == EditorPlayMode::Paused)
    {
        play->Pause();
        playMode = EditorPlayMode::Paused;
        return true;
    }
    play->Stop();
    playMode = EditorPlayMode::Edit;
    return true;
}

void EditorSession::NewScene(std::string name)
{
    scenes->New(document, *editWorld, std::move(name));
    static_cast<void>(EnsureCelestialLights(*editWorld));
    history.Clear();
}

Result<void> EditorSession::OpenScene(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return Result<void>::Error("Choose a scene file");
    }
    Result<void> result = scenes->Load(document, *editWorld, path);
    if (result)
    {
        document.Dirty = EnsureCelestialLights(*editWorld);
        history.Clear();
    }
    return result;
}

Result<void> EditorSession::SaveScene()
{
    if (document.Path.empty())
    {
        return Result<void>::Error("Scene path is empty");
    }
    return SaveSceneTo(document.Path);
}

Result<void> EditorSession::SaveSceneTo(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return Result<void>::Error("Scene path is empty");
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        return Result<void>::Error(
            "Could not create " + path.parent_path().generic_string() + ": " + error.message());
    }
    Result<void> result = scenes->SaveAs(document, *editWorld, path);
    if (result)
    {
        document.Name = path.stem().string();
    }
    return result;
}

EditorSession::SaveAllSceneResult EditorSession::SaveAllScene()
{
    if (document.Path.empty())
    {
        saveAllPending = true;
        return SaveAllSceneResult::NeedsPath;
    }
    return SaveSceneTo(document.Path) ? SaveAllSceneResult::Saved : SaveAllSceneResult::Failed;
}
}
