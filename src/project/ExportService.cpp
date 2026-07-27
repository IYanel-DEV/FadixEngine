#include "project/ExportService.hpp"

#include "engine/Version.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/ScriptDatabase.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/scene/IWorld.hpp"
#include "project/ProjectJson.hpp"
#include "project/ProjectService.hpp"
#include "runtime/Components.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace fadix
{
namespace
{
void Progress(const ExportProgressFn& fn, const float fraction, std::string stage)
{
    if (fn)
    {
        fn(ExportProgress{fraction, std::move(stage)});
    }
}

void AddMessage(
    ExportResult& result, const ExportSeverity severity, std::string text)
{
    result.Messages.push_back(ExportMessage{severity, std::move(text)});
}

[[nodiscard]] bool IsEmptyDirectory(const std::filesystem::path& path, std::error_code& error)
{
    const auto it = std::filesystem::directory_iterator{path, error};
    if (error)
    {
        return false;
    }
    return it == std::filesystem::directory_iterator{};
}

[[nodiscard]] Result<void> EnsureStagingRoot(const std::filesystem::path& dest)
{
    std::error_code error;
    if (dest.empty())
    {
        return Result<void>::Error("Destination directory is empty");
    }
    if (std::filesystem::exists(dest, error))
    {
        if (error)
        {
            return Result<void>::Error("Could not inspect destination: " + error.message());
        }
        if (!std::filesystem::is_directory(dest, error) || error)
        {
            return Result<void>::Error("Destination is not a directory: " + dest.string());
        }
        if (!IsEmptyDirectory(dest, error) || error)
        {
            return Result<void>::Error(
                "Destination is not empty; export only into a new or empty folder: " +
                dest.string());
        }
        return Result<void>::Ok();
    }
    std::filesystem::create_directories(dest, error);
    if (error)
    {
        return Result<void>::Error("Could not create destination: " + error.message());
    }
    return Result<void>::Ok();
}

[[nodiscard]] std::filesystem::path Relativize(
    const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    if (error || relative.empty() || *relative.begin() == "..")
    {
        return {};
    }
    return relative.generic_string();
}

[[nodiscard]] Result<void> CopyRelativeFile(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& stageRoot,
    const std::filesystem::path& absoluteSource,
    std::set<std::string>& staged,
    ExportResult& result)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(absoluteSource, error) || error)
    {
        AddMessage(
            result,
            ExportSeverity::Warning,
            "Missing file skipped: " + absoluteSource.string());
        return Result<void>::Ok();
    }
    const auto relative = Relativize(projectRoot, absoluteSource);
    if (relative.empty())
    {
        AddMessage(
            result,
            ExportSeverity::Warning,
            "File outside project skipped: " + absoluteSource.string());
        return Result<void>::Ok();
    }
    const std::string key = relative.generic_string();
    if (!staged.insert(key).second)
    {
        return Result<void>::Ok();
    }
    const std::filesystem::path dest = stageRoot / relative;
    std::filesystem::create_directories(dest.parent_path(), error);
    if (error)
    {
        return Result<void>::Error("Could not create asset folder: " + error.message());
    }
    std::filesystem::copy_file(
        absoluteSource, dest, std::filesystem::copy_options::overwrite_existing, error);
    if (error)
    {
        return Result<void>::Error(
            "Could not copy " + absoluteSource.string() + ": " + error.message());
    }
    return Result<void>::Ok();
}

void CollectHandle(
    const AssetHandle& handle,
    AssetDatabase& assets,
    std::unordered_set<AssetHandle>& handles)
{
    if (handle.IsValid())
    {
        handles.insert(handle);
    }
    static_cast<void>(assets);
}

[[nodiscard]] Result<void> CollectReferencedAssets(
    IWorld& world,
    AssetDatabase& assets,
    ScriptDatabase* scripts,
    std::unordered_set<AssetHandle>& handles,
    std::set<std::string>& scriptNames)
{
    auto& registry = world.Registry();
    for (const auto [entity, mesh] : registry.view<const MeshComponent>().each())
    {
        static_cast<void>(entity);
        CollectHandle(mesh.Material, assets, handles);
        CollectHandle(mesh.ImportedMesh, assets, handles);
        if (mesh.Material.IsValid())
        {
            if (auto material = assets.LoadMaterial(mesh.Material))
            {
                CollectHandle(material.Value().BaseColorTexture, assets, handles);
                CollectHandle(material.Value().NormalTexture, assets, handles);
                CollectHandle(material.Value().MetallicRoughnessTexture, assets, handles);
                CollectHandle(material.Value().EmissiveTexture, assets, handles);
            }
        }
    }
    for (const auto [entity, ui] : registry.view<const UICanvasComponent>().each())
    {
        static_cast<void>(entity);
        CollectHandle(ui.UIAsset, assets, handles);
        CollectHandle(ui.StyleAsset, assets, handles);
    }
    for (const auto [entity, script] : registry.view<const ScriptComponent>().each())
    {
        static_cast<void>(entity);
        for (const std::string& name : script.ScriptNames)
        {
            scriptNames.insert(name);
            if (scripts != nullptr)
            {
                if (auto asset = scripts->Get(name))
                {
                    static_cast<void>(asset);
                }
            }
        }
    }
    return Result<void>::Ok();
}

[[nodiscard]] Result<void> WriteManifest(
    const ExportOptions& options,
    const ProjectMetadata& project,
    const std::string& bootScene,
    const ExportResult& result)
{
    project_json::Value root = project_json::Value::MakeObject();
    root["formatVersion"] = project_json::Value::MakeNumber(1);
    root["projectName"] = project_json::Value::MakeString(project.Name);
    root["bootScene"] = project_json::Value::MakeString(bootScene);
    root["executable"] = project_json::Value::MakeString(options.ExecutableName);
    project_json::Value window = project_json::Value::MakeObject();
    window["width"] = project_json::Value::MakeNumber(options.Width);
    window["height"] = project_json::Value::MakeNumber(options.Height);
    window["fullscreen"] = project_json::Value::MakeBool(options.Fullscreen);
    window["vsync"] = project_json::Value::MakeBool(options.VSync);
    root["window"] = std::move(window);
    project_json::Value assets = project_json::Value::MakeArray();
    for (const std::string& path : result.StagedAssets)
    {
        assets.Push(project_json::Value::MakeString(path));
    }
    root["assets"] = std::move(assets);

    std::ofstream out{result.ManifestPath, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return Result<void>::Error("Could not write export.manifest.json");
    }
    out << project_json::Stringify(root);
    return Result<void>::Ok();
}
}

ExportResult ExportProject(const ExportOptions& options, const ExportProgressFn& onProgress)
{
    ExportResult result;
    Progress(onProgress, 0.0F, "Validate");

    if (options.ProjectFile.empty())
    {
        AddMessage(result, ExportSeverity::Error, "Project file path is empty");
        return result;
    }
    if (options.PlayerExecutableSource.empty())
    {
        AddMessage(result, ExportSeverity::Error, "Player executable source is empty");
        return result;
    }
    if (options.ExecutableName.empty())
    {
        AddMessage(result, ExportSeverity::Error, "Executable name is empty");
        return result;
    }
    if (options.Width < 320 || options.Height < 240)
    {
        AddMessage(result, ExportSeverity::Error, "Resolution too small (min 320x240)");
        return result;
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(options.PlayerExecutableSource, error) || error)
    {
        AddMessage(
            result,
            ExportSeverity::Error,
            "Player executable not found: " + options.PlayerExecutableSource.string());
        return result;
    }

    ProjectService projects;
    auto opened = projects.Open(options.ProjectFile);
    if (!opened)
    {
        AddMessage(result, ExportSeverity::Error, opened.ErrorMessage());
        return result;
    }
    ProjectMetadata project = std::move(opened).Value();
    const std::string bootScene = options.BootScene.empty()
        ? (project.DefaultScene.empty() ? "Scenes/Main.scene" : project.DefaultScene)
        : options.BootScene;
    project.DefaultScene = bootScene;

    const std::filesystem::path bootAbsolute = project.RootPath / bootScene;
    if (!std::filesystem::is_regular_file(bootAbsolute, error) || error)
    {
        AddMessage(
            result,
            ExportSeverity::Error,
            "Boot scene missing: " + bootAbsolute.string());
        return result;
    }

    if (options.SaveAll)
    {
        Progress(onProgress, 0.08F, "Save All");
        if (const Result<void> saved = options.SaveAll(); !saved)
        {
            AddMessage(result, ExportSeverity::Error, "Save All failed: " + saved.ErrorMessage());
            return result;
        }
    }

    Progress(onProgress, 0.15F, "Prepare staging");
    if (const Result<void> staging = EnsureStagingRoot(options.DestinationDirectory); !staging)
    {
        AddMessage(result, ExportSeverity::Error, staging.ErrorMessage());
        return result;
    }
    result.StagedRoot = options.DestinationDirectory;
    result.ManifestPath = result.StagedRoot / "export.manifest.json";

    Progress(onProgress, 0.25F, "Load boot scene");
    auto world = sceneplay::CreateEditWorld();
    SceneDocument document{
        Uuid::Generate(),
        std::filesystem::path{bootScene}.stem().string(),
        bootAbsolute,
        false};
    SceneService scenes;
    if (const Result<void> loaded = scenes.Load(document, *world, bootAbsolute); !loaded)
    {
        AddMessage(result, ExportSeverity::Error, "Boot scene load failed: " + loaded.ErrorMessage());
        return result;
    }

    auto assets = std::make_unique<AssetDatabase>(project.RootPath);
    if (const Result<void> refreshed = assets->Refresh(); !refreshed)
    {
        AddMessage(result, ExportSeverity::Warning, refreshed.ErrorMessage());
    }
    ScriptDatabase scriptDb;
    scriptDb.ScanFolder(project.RootPath);

    std::unordered_set<AssetHandle> handles;
    std::set<std::string> scriptNames;
    static_cast<void>(CollectReferencedAssets(*world, *assets, &scriptDb, handles, scriptNames));

    Progress(onProgress, 0.45F, "Stage project files");
    std::set<std::string> stagedKeys;
    {
        project_json::Value root = project_json::Value::MakeObject();
        root["id"] = project_json::Value::MakeString(project.Id.ToString());
        root["name"] = project_json::Value::MakeString(project.Name);
        root["engineVersion"] = project_json::Value::MakeString(std::string{EngineVersion});
        root["template"] = project_json::Value::MakeString(
            project.Template == ProjectTemplate::Empty2D ? "Empty2D" : "Empty3D");
        root["defaultScene"] = project_json::Value::MakeString(bootScene);
        project_json::Value folders = project_json::Value::MakeObject();
        folders["assets"] = project_json::Value::MakeString("Assets");
        folders["scenes"] = project_json::Value::MakeString("Scenes");
        folders["scripts"] = project_json::Value::MakeString("Scripts");
        folders["audio"] = project_json::Value::MakeString("Audio");
        folders["ui"] = project_json::Value::MakeString("UI");
        folders["cache"] = project_json::Value::MakeString("Cache");
        folders["saved"] = project_json::Value::MakeString("Saved");
        root["folders"] = std::move(folders);
        const std::filesystem::path stagedProject = result.StagedRoot / "project.fadix";
        std::ofstream out{stagedProject, std::ios::binary | std::ios::trunc};
        if (!out)
        {
            AddMessage(result, ExportSeverity::Error, "Could not write staged project.fadix");
            return result;
        }
        out << project_json::Stringify(root);
        stagedKeys.insert("project.fadix");
    }

    if (const Result<void> copied =
            CopyRelativeFile(project.RootPath, result.StagedRoot, bootAbsolute, stagedKeys, result);
        !copied)
    {
        AddMessage(result, ExportSeverity::Error, copied.ErrorMessage());
        return result;
    }
    const std::filesystem::path bootMeta = std::filesystem::path{bootAbsolute.string() + ".fadixmeta"};
    static_cast<void>(
        CopyRelativeFile(project.RootPath, result.StagedRoot, bootMeta, stagedKeys, result));

    Progress(onProgress, 0.6F, "Stage referenced assets");
    for (const AssetHandle& handle : handles)
    {
        const AssetMetadata* meta = assets->Meta(handle);
        if (meta == nullptr)
        {
            AddMessage(
                result,
                ExportSeverity::Warning,
                "Unknown asset handle skipped: " + handle.ToString());
            continue;
        }
        if (const Result<void> copied = CopyRelativeFile(
                project.RootPath, result.StagedRoot, meta->SourcePath, stagedKeys, result);
            !copied)
        {
            AddMessage(result, ExportSeverity::Error, copied.ErrorMessage());
            return result;
        }
        const std::filesystem::path sidecar =
            std::filesystem::path{meta->SourcePath.string() + ".fadixmeta"};
        static_cast<void>(
            CopyRelativeFile(project.RootPath, result.StagedRoot, sidecar, stagedKeys, result));
        if (!meta->ImportedPath.empty())
        {
            static_cast<void>(CopyRelativeFile(
                project.RootPath, result.StagedRoot, meta->ImportedPath, stagedKeys, result));
        }
    }

    for (const std::string& name : scriptNames)
    {
        if (auto asset = scriptDb.Get(name))
        {
            if (const Result<void> copied = CopyRelativeFile(
                    project.RootPath,
                    result.StagedRoot,
                    asset->get().SourcePath,
                    stagedKeys,
                    result);
                !copied)
            {
                AddMessage(result, ExportSeverity::Error, copied.ErrorMessage());
                return result;
            }
        }
        else
        {
            AddMessage(result, ExportSeverity::Warning, "Script not found: " + name);
        }
    }

    Progress(onProgress, 0.8F, "Stage player");
    const std::filesystem::path playerDest = result.StagedRoot / options.ExecutableName;
    std::filesystem::copy_file(
        options.PlayerExecutableSource,
        playerDest,
        std::filesystem::copy_options::overwrite_existing,
        error);
    if (error)
    {
        AddMessage(result, ExportSeverity::Error, "Could not copy player: " + error.message());
        return result;
    }

    result.StagedAssets.assign(stagedKeys.begin(), stagedKeys.end());
    std::sort(result.StagedAssets.begin(), result.StagedAssets.end());

    Progress(onProgress, 0.92F, "Write manifest");
    if (const Result<void> manifest =
            WriteManifest(options, project, bootScene, result);
        !manifest)
    {
        AddMessage(result, ExportSeverity::Error, manifest.ErrorMessage());
        return result;
    }

    AddMessage(
        result,
        ExportSeverity::Info,
        "Export staged to " + result.StagedRoot.string());
    // ponytail: referenced-only assets; full Assets/ tree + dependency graph when shipping.
    Progress(onProgress, 1.0F, "Done");
    result.Ok = true;
    return result;
}
}
