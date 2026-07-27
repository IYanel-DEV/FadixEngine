#pragma once

#include "engine/camera/EditorMode.hpp"
#include "engine/project/ProjectMetadata.hpp"

#include <memory>

namespace fadix
{
class IAssetDatabase;
class IProjectService;
class IWorld;
class UndoStack;
class ViewportRenderer;

class IApplicationHost
{
public:
    virtual ~IApplicationHost() = default;

    virtual void ShowProjectManager() = 0;
    virtual void EnterWorkbench(const ProjectMetadata& project) = 0;
    virtual void SetPlayMode(EditorPlayMode mode) = 0;
    [[nodiscard]] virtual EditorPlayMode PlayMode() const noexcept = 0;
    [[nodiscard]] virtual IProjectService& Projects() noexcept = 0;
    [[nodiscard]] virtual IWorld& EditWorld() noexcept = 0;
    [[nodiscard]] virtual IWorld* PlayWorld() noexcept = 0;
    [[nodiscard]] virtual ViewportRenderer& Viewport() noexcept = 0;
    [[nodiscard]] virtual UndoStack& History() noexcept = 0;
    [[nodiscard]] virtual IAssetDatabase& Assets() noexcept = 0;

    virtual void SetProjectService(std::unique_ptr<IProjectService> service) noexcept = 0;
    virtual void SetAssetDatabase(std::unique_ptr<IAssetDatabase> database) noexcept = 0;
    virtual void SetViewportRenderer(std::unique_ptr<ViewportRenderer> renderer) noexcept = 0;
    virtual void SetEditWorld(std::unique_ptr<IWorld> world) noexcept = 0;
};
}
