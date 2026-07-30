#include "editor/scene/SceneSerializer.hpp"

#include "editor/scene/EntityTextIO.hpp"
#include "engine/scene/IWorld.hpp"
#include "runtime/Components.hpp"
#include "runtime/World.hpp"

#include <fstream>
#include <optional>
#include <string>

namespace fadix
{
namespace
{
// Bump when the scene text format changes incompatibly.
constexpr unsigned kSceneFormatVersion = 1;

std::filesystem::path AutosavePath(const SceneDocument& document)
{
    if (!document.Path.empty())
    {
        return document.Path.string() + ".autosave";
    }
    return std::filesystem::temp_directory_path() / (document.Id.ToString() + ".fadixscene.autosave");
}
}

Result<void> SceneSerializer::Save(const SceneDocument& document, const IWorld& world)
{
    if (document.Path.empty())
    {
        return Result<void>::Error("Scene has no path; use Save As");
    }
    return SavePath(document.Path, world);
}

Result<void> SceneSerializer::Load(const SceneDocument& document, IWorld& world)
{
    if (document.Path.empty())
    {
        return Result<void>::Error("Scene has no path");
    }
    return LoadPath(document.Path, world);
}

Result<void> SceneSerializer::SaveAutosave(const SceneDocument& document, const IWorld& world)
{
    return SavePath(AutosavePath(document), world);
}

bool SceneSerializer::HasRecoverableAutosave(const SceneDocument& document) const
{
    std::error_code error;
    const auto autosave = AutosavePath(document);
    if (!std::filesystem::exists(autosave, error))
    {
        return false;
    }
    if (document.Path.empty() || !std::filesystem::exists(document.Path, error))
    {
        return true;
    }
    return std::filesystem::last_write_time(autosave, error) >
           std::filesystem::last_write_time(document.Path, error);
}

Result<void> SceneSerializer::RecoverAutosave(const SceneDocument& document, IWorld& world)
{
    return LoadPath(AutosavePath(document), world);
}

Result<void> SceneSerializer::SavePath(const std::filesystem::path& path, const IWorld& world)
{
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
        {
            return Result<void>::Error("Could not create scene directory: " + error.message());
        }
    }

    const std::filesystem::path temporary = path.string() + ".tmp";
    std::ofstream output{temporary, std::ios::trunc};
    if (!output)
    {
        return Result<void>::Error("Could not open scene for writing: " + temporary.string());
    }

    output << "FADIX_SCENE " << kSceneFormatVersion << "\n";
    const entt::registry& registry = world.Registry();
    for (const auto [entity, uuid] : registry.view<const UuidComponent>().each())
    {
        static_cast<void>(uuid);
        WriteEntityLine(output, world, entity);
    }
    output.flush();
    output.close();
    if (!output)
    {
        std::error_code cleanup;
        std::filesystem::remove(temporary, cleanup);
        return Result<void>::Error("Failed while writing scene: " + path.string());
    }
    return AtomicReplaceFile(temporary, path);
}

Result<void> SceneSerializer::LoadPath(const std::filesystem::path& path, IWorld& world)
{
    std::ifstream input{path};
    std::string header;
    unsigned version = 0;
    if (!(input >> header >> version) || header != "FADIX_SCENE")
    {
        return Result<void>::Error("Not a Fadix scene file: " + path.string());
    }
    if (version > kSceneFormatVersion)
    {
        return Result<void>::Error(
            "Scene was written by a newer Fadix (format version " + std::to_string(version) +
            "; this build supports up to " + std::to_string(kSceneFormatVersion) + "): " +
            path.string());
    }
    if (version < 1)
    {
        return Result<void>::Error("Scene has an invalid format version: " + path.string());
    }

    World staging{false};
    std::string line;
    std::getline(input, line);
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (Result<void> parsed = ParseEntityLine(line, staging); !parsed)
        {
            return parsed;
        }
    }

    world.Clear();
    CopyWorldInto(staging, world);
    return Result<void>::Ok();
}

std::optional<std::string> SceneSerializer::PeekDisplayName(const std::filesystem::path& path)
{
    std::ifstream input{path};
    std::string header;
    unsigned version = 0;
    if (!(input >> header >> version) || header != "FADIX_SCENE" || version < 1)
    {
        return std::nullopt;
    }
    World staging{false};
    std::string line;
    std::getline(input, line); // rest of header line
    while (std::getline(input, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (!ParseEntityLine(line, staging))
        {
            return std::nullopt;
        }
    }
    const std::optional<Uuid> root = FindSceneRootId(staging);
    if (!root)
    {
        return std::nullopt;
    }
    const std::optional<entt::entity> entity = staging.Find(*root);
    if (!entity)
    {
        return std::nullopt;
    }
    const NameComponent* name = staging.Registry().try_get<NameComponent>(*entity);
    return name != nullptr ? std::optional<std::string>{name->Name} : std::nullopt;
}

SceneService::SceneService(std::unique_ptr<ISceneSerializer> serializer)
    : m_Serializer(std::move(serializer))
{
}

void SceneService::New(SceneDocument& document, IWorld& world, std::string name)
{
    world.Clear();
    document = SceneDocument{Uuid::Generate(), std::move(name), {}, false};
}

Result<void> SceneService::Save(SceneDocument& document, const IWorld& world)
{
    Result<void> result = m_Serializer->Save(document, world);
    if (result)
    {
        document.Dirty = false;
    }
    return result;
}

Result<void> SceneService::SaveAs(
    SceneDocument& document, const IWorld& world, const std::filesystem::path& path)
{
    const auto oldPath = document.Path;
    document.Path = path;
    Result<void> result = Save(document, world);
    if (!result)
    {
        document.Path = oldPath;
    }
    return result;
}

Result<void> SceneService::Load(
    SceneDocument& document, IWorld& world, const std::filesystem::path& path)
{
    SceneDocument candidate{Uuid::Generate(), path.stem().string(), path, false};
    Result<void> result = m_Serializer->Load(candidate, world);
    if (result)
    {
        document = std::move(candidate);
    }
    return result;
}
}
