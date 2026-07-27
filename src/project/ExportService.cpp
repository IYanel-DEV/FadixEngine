#include "project/ExportService.hpp"

#include "engine/Version.hpp"

#include "project/ProjectJson.hpp"
#include "project/ProjectService.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <system_error>
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

// Copies every regular file under projectRoot/<folder> into the staging tree,
// preserving relative layout. The player's AssetDatabase/ScriptDatabase scan the
// whole staged root, so shipping the content folders wholesale is what makes an
// export bootable without the editor.
[[nodiscard]] Result<void> StageContentFolder(
    const std::filesystem::path& projectRoot,
    const std::filesystem::path& stageRoot,
    const char* folder,
    std::set<std::string>& staged,
    ExportResult& result)
{
    const std::filesystem::path source = projectRoot / folder;
    std::error_code error;
    if (!std::filesystem::is_directory(source, error) || error)
    {
        return Result<void>::Ok();
    }
    for (std::filesystem::recursive_directory_iterator it{source, error}, end;
         !error && it != end;
         it.increment(error))
    {
        if (!it->is_regular_file(error) || error)
        {
            continue;
        }
        if (const Result<void> copied =
                CopyRelativeFile(projectRoot, stageRoot, it->path(), staged, result);
            !copied)
        {
            return copied;
        }
    }
    if (error)
    {
        return Result<void>::Error("Failed to walk " + source.string() + ": " + error.message());
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
    root["formatVersion"] = project_json::Value::MakeNumber(project_json::kManifestFormatVersion);
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

    Progress(onProgress, 0.45F, "Stage project files");
    std::set<std::string> stagedKeys;
    {
        project_json::Value root = project_json::Value::MakeObject();
        root["formatVersion"] = project_json::Value::MakeNumber(project_json::kProjectFormatVersion);
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

    Progress(onProgress, 0.6F, "Stage content");
    for (const char* folder : {"Scenes", "Assets", "Scripts", "Audio", "UI"})
    {
        if (const Result<void> staged =
                StageContentFolder(project.RootPath, result.StagedRoot, folder, stagedKeys, result);
            !staged)
        {
            AddMessage(result, ExportSeverity::Error, staged.ErrorMessage());
            return result;
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
    // ponytail: ships the whole Scenes/Assets/Scripts/Audio/UI trees (Cache/Saved
    // excluded) so the player boots without the editor. Bootable but unpruned; add a
    // dependency walk to drop unreferenced assets when export size matters.
    Progress(onProgress, 1.0F, "Done");
    result.Ok = true;
    return result;
}
}
