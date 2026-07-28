#pragma once

#include "engine/scene/ISceneSerializer.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace fadix
{
class SceneSerializer final : public ISceneSerializer
{
public:
    [[nodiscard]] Result<void> Save(const SceneDocument& document, const IWorld& world) override;
    [[nodiscard]] Result<void> Load(const SceneDocument& document, IWorld& world) override;
    [[nodiscard]] Result<void> SaveAutosave(const SceneDocument& document, const IWorld& world);
    [[nodiscard]] bool HasRecoverableAutosave(const SceneDocument& document) const;
    [[nodiscard]] Result<void> RecoverAutosave(const SceneDocument& document, IWorld& world);

    // The scene's Hierarchy display name (root entity name) without loading it into a
    // live world. Used to resolve Scene.load("Lobby Scene") to a file. nullopt if the
    // file is not a readable scene.
    [[nodiscard]] static std::optional<std::string> PeekDisplayName(
        const std::filesystem::path& path);

private:
    [[nodiscard]] Result<void> SavePath(const std::filesystem::path& path, const IWorld& world);
    [[nodiscard]] Result<void> LoadPath(const std::filesystem::path& path, IWorld& world);
};

class SceneService final
{
public:
    explicit SceneService(std::unique_ptr<ISceneSerializer> serializer = std::make_unique<SceneSerializer>());

    void New(SceneDocument& document, IWorld& world, std::string name = "Untitled Scene");
    [[nodiscard]] Result<void> Save(SceneDocument& document, const IWorld& world);
    [[nodiscard]] Result<void> SaveAs(
        SceneDocument& document, const IWorld& world, const std::filesystem::path& path);
    [[nodiscard]] Result<void> Load(
        SceneDocument& document, IWorld& world, const std::filesystem::path& path);

private:
    std::unique_ptr<ISceneSerializer> m_Serializer;
};
}
