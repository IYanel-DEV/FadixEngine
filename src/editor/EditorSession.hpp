#pragma once

#include "engine/camera/EditorMode.hpp"
#include "engine/command/UndoStack.hpp"
#include "engine/project/ProjectMetadata.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "engine/Result.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace fadix
{
class IAssetDatabase;
class IProjectService;
class IWorld;
class ICollisionMeshProvider;
class PlaySession;
class SceneService;
class ViewportRenderer;

/// UI-independent editor ownership and commands shared by any editor shell.
/// RmlUi / ImGui presentation stays outside this type.
class EditorSession final
{
public:
    EditorSession();
    ~EditorSession();

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    [[nodiscard]] IProjectService& Projects() noexcept;
    [[nodiscard]] IWorld& EditWorld() noexcept;
    [[nodiscard]] IWorld* PlayWorld() noexcept;
    [[nodiscard]] ViewportRenderer& Viewport() noexcept;
    [[nodiscard]] UndoStack& History() noexcept;
    [[nodiscard]] IAssetDatabase& Assets() noexcept;
    [[nodiscard]] EditorPlayMode PlayMode() const noexcept;
    [[nodiscard]] ProjectMetadata& ActiveProject() noexcept;
    [[nodiscard]] const ProjectMetadata& ActiveProject() const noexcept;
    [[nodiscard]] SceneDocument& Document() noexcept;
    [[nodiscard]] const SceneDocument& Document() const noexcept;
    [[nodiscard]] SceneService& Scenes() noexcept;
    [[nodiscard]] PlaySession& Play() noexcept;
    [[nodiscard]] bool SaveAllPending() const noexcept;
    void SetSaveAllPending(bool pending) noexcept;

    void SetProjectService(std::unique_ptr<IProjectService> service) noexcept;
    void SetAssetDatabase(std::unique_ptr<IAssetDatabase> database) noexcept;
    void SetViewportRenderer(std::unique_ptr<ViewportRenderer> renderer) noexcept;
    void SetEditWorld(std::unique_ptr<IWorld> world) noexcept;
    void SetCollisionMeshProvider(ICollisionMeshProvider* meshes) noexcept { collisionMeshes = meshes; }

    /// Sets active project and default Main.scene document; loads it when present.
    [[nodiscard]] Result<void> BeginProject(const ProjectMetadata& project);
    /// Stops play and returns to edit mode (no UI side effects).
    void LeavePlay() noexcept;

    /// Play/pause/stop without audio, GameUI, or chrome. Returns false if Play start fails.
    [[nodiscard]] bool SetPlayMode(EditorPlayMode mode);

    void NewScene(std::string name = "Untitled Scene");
    [[nodiscard]] Result<void> OpenScene(const std::filesystem::path& path);
    /// Saves to the current document path. Error if path empty or save fails.
    [[nodiscard]] Result<void> SaveScene();
    [[nodiscard]] Result<void> SaveSceneTo(const std::filesystem::path& path);

    enum class SaveAllSceneResult
    {
        NeedsPath,
        Saved,
        Failed
    };
    /// Scene half of Save All. Caller still persists workspace layout.
    [[nodiscard]] SaveAllSceneResult SaveAllScene();

    // Owned services (public so the shell can wire UI controllers without extra getters).
    std::unique_ptr<IProjectService> projectService;
    std::unique_ptr<IAssetDatabase> assetDatabase;
    std::unique_ptr<ViewportRenderer> viewport;
    std::unique_ptr<IWorld> editWorld;
    std::unique_ptr<SceneService> scenes;
    std::unique_ptr<PlaySession> play;
    UndoStack history;
    ProjectMetadata activeProject;
    SceneDocument document;
    EditorPlayMode playMode{EditorPlayMode::Edit};
    bool saveAllPending{false};
    ICollisionMeshProvider* collisionMeshes{};
};
}
