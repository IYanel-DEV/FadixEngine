#include "project/ExportService.hpp"
#include "project/ProjectService.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
[[nodiscard]] bool ExpectFile(const std::filesystem::path& path, const char* label)
{
    std::error_code error;
    const bool ok = std::filesystem::is_regular_file(path, error) && !error;
    if (!ok)
    {
        std::cerr << "[export-smoke] missing " << label << ": " << path.string() << '\n';
    }
    return ok;
}

[[nodiscard]] bool Contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "Usage: fadix_export_smoke <path-to-fadix_player.exe>\n";
        return 2;
    }
    const std::filesystem::path playerExe = argv[1];
    std::error_code error;
    if (!std::filesystem::is_regular_file(playerExe, error) || error)
    {
        std::cerr << "[export-smoke] player exe missing: " << playerExe.string() << '\n';
        return 3;
    }

    const auto root = std::filesystem::temp_directory_path() / "fadix_export_smoke";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
    {
        std::cerr << "[export-smoke] temp root failed\n";
        return 4;
    }

    fadix::ProjectService projects;
    auto created = projects.Create("ExportSmoke3D", root, fadix::ProjectTemplate::Empty3D);
    if (!created)
    {
        std::cerr << "[export-smoke] create failed: " << created.ErrorMessage() << '\n';
        return 5;
    }
    const fadix::ProjectMetadata project = std::move(created).Value();

    const std::filesystem::path prefab = project.RootPath / "Prefabs" / "Pickup.prefab";
    std::filesystem::create_directories(prefab.parent_path(), error);
    std::ofstream{prefab.string()} << "FADIX_PREFAB 1\n";
    const std::filesystem::path cooked = project.RootPath / ".fadix" / "imported" / "cooked.bin";
    std::filesystem::create_directories(cooked.parent_path(), error);
    std::ofstream{cooked.string(), std::ios::binary} << "cooked";
    const std::filesystem::path saved = project.RootPath / "Saved" / "editor-only.txt";
    std::ofstream{saved.string()} << "do not ship";

    const std::filesystem::path stage = root / "stage_empty_ok";
    // Safety: non-empty destination must fail.
    {
        const std::filesystem::path dirty = root / "dirty_dest";
        std::filesystem::create_directories(dirty, error);
        std::ofstream{(dirty / "keep.txt").string()} << "x";
        fadix::ExportOptions bad{};
        bad.ProjectFile = project.ProjectFile;
        bad.DestinationDirectory = dirty;
        bad.PlayerExecutableSource = playerExe;
        bad.ExecutableName = "Game.exe";
        bad.BootScene = "Scenes/Main.scene";
        const fadix::ExportResult rejected = fadix::ExportProject(bad);
        if (rejected.Ok)
        {
            std::cerr << "[export-smoke] expected non-empty dest rejection\n";
            return 6;
        }
    }

    // Safety: boot scenes cannot escape the project root.
    {
        const std::filesystem::path outsideScene = root / "outside.scene";
        std::ofstream{outsideScene.string()} << "FADIX_SCENE 1\n";
        fadix::ExportOptions bad{};
        bad.ProjectFile = project.ProjectFile;
        bad.DestinationDirectory = root / "scene_traversal_dest";
        bad.PlayerExecutableSource = playerExe;
        bad.ExecutableName = "Game.exe";
        bad.BootScene = "../outside.scene";
        if (fadix::ExportProject(bad).Ok)
        {
            std::cerr << "[export-smoke] expected boot-scene traversal rejection\n";
            return 7;
        }
    }

    // Safety: the executable name cannot escape the staging root.
    {
        fadix::ExportOptions bad{};
        bad.ProjectFile = project.ProjectFile;
        bad.DestinationDirectory = root / "exe_traversal_dest";
        bad.PlayerExecutableSource = playerExe;
        bad.ExecutableName = "../escaped.exe";
        bad.BootScene = "Scenes/Main.scene";
        if (fadix::ExportProject(bad).Ok)
        {
            std::cerr << "[export-smoke] expected executable traversal rejection\n";
            return 8;
        }
    }

    fadix::ExportOptions options{};
    options.ProjectFile = project.ProjectFile;
    options.DestinationDirectory = stage;
    options.PlayerExecutableSource = playerExe;
    options.ExecutableName = "ExportSmokeGame.exe";
    options.BootScene = "Scenes/Main.scene";
    options.Width = 800;
    options.Height = 600;
    options.VSync = true;
    options.Fullscreen = false;

    float lastProgress = -1.0F;
    const fadix::ExportResult exported = fadix::ExportProject(
        options, [&](const fadix::ExportProgress& progress) { lastProgress = progress.Fraction; });
    if (!exported.Ok)
    {
        std::cerr << "[export-smoke] export failed\n";
        for (const auto& message : exported.Messages)
        {
            std::cerr << "  - " << message.Text << '\n';
        }
        return 9;
    }
    if (lastProgress < 0.99F)
    {
        std::cerr << "[export-smoke] progress callback missing final tick\n";
        return 10;
    }

    const bool filesOk =
        ExpectFile(stage / "export.manifest.json", "manifest") &&
        ExpectFile(stage / "ExportSmokeGame.exe", "player") &&
        ExpectFile(stage / "project.fadix", "project") &&
        ExpectFile(stage / "Scenes" / "Main.scene", "boot scene") &&
        ExpectFile(stage / "Prefabs" / "Pickup.prefab", "runtime prefab") &&
        ExpectFile(stage / ".fadix" / "imported" / "cooked.bin", "cooked asset");
    if (!filesOk)
    {
        return 11;
    }
    if (std::filesystem::exists(stage / "Saved" / "editor-only.txt"))
    {
        std::cerr << "[export-smoke] editor Saved content was staged\n";
        return 12;
    }

    std::ifstream manifest{stage / "export.manifest.json"};
    const std::string manifestText{std::istreambuf_iterator<char>{manifest}, {}};
    if (!Contains(manifestText, "Scenes/Main.scene") ||
        !Contains(manifestText, "ExportSmokeGame.exe") ||
        !Contains(manifestText, "ExportSmoke3D") ||
        !Contains(manifestText, "Prefabs/Pickup.prefab") ||
        !Contains(manifestText, ".fadix/imported/cooked.bin"))
    {
        std::cerr << "[export-smoke] manifest content incomplete\n";
        return 13;
    }

    std::ifstream projectFile{stage / "project.fadix"};
    const std::string projectText{std::istreambuf_iterator<char>{projectFile}, {}};
    if (!Contains(projectText, "\"defaultScene\"") || !Contains(projectText, "Scenes/Main.scene"))
    {
        std::cerr << "[export-smoke] staged project.fadix missing defaultScene\n";
        return 14;
    }
    if (!Contains(projectText, "\"formatVersion\""))
    {
        std::cerr << "[export-smoke] staged project.fadix missing formatVersion\n";
        return 15;
    }

    // The exported player must boot the staged project with no editor. It reads
    // project.fadix + export.manifest.json beside its own exe; --smoke-exit loads
    // the boot scene and exits 0 before opening a window.
    const std::filesystem::path exportedPlayer = stage / "ExportSmokeGame.exe";
    const std::string command = "\"" + exportedPlayer.string() + "\" --smoke-exit";
    const int playerExit = std::system(command.c_str());
    if (playerExit != 0)
    {
        std::cerr << "[export-smoke] exported player did not boot (exit " << playerExit << ")\n";
        return 16;
    }

    std::cout << "[export-smoke] PASS staged export + player boot\n";
    return 0;
}
