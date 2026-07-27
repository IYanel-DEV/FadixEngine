#pragma once

#include "engine/Result.hpp"
#include "engine/project/ProjectMetadata.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace fadix
{
enum class ExportSeverity
{
    Info,
    Warning,
    Error
};

struct ExportMessage
{
    ExportSeverity Severity{ExportSeverity::Info};
    std::string Text;
};

struct ExportProgress
{
    float Fraction{0.0F};
    std::string Stage;
};

struct ExportOptions
{
    std::filesystem::path ProjectFile;
    /// Staging root. Created if missing. Must be empty if it already exists.
    std::filesystem::path DestinationDirectory;
    /// Built fadix_player (or renamed) binary to copy into the stage.
    std::filesystem::path PlayerExecutableSource;
    std::string ExecutableName{"fadix_player.exe"};
    /// Optional override of project.fadix defaultScene (project-relative).
    std::string BootScene;
    bool Fullscreen{false};
    int Width{1280};
    int Height{720};
    bool VSync{true};
    /// Optional hook so the editor can flush dirty docs before staging.
    std::function<Result<void>()> SaveAll;
};

struct ExportResult
{
    bool Ok{false};
    std::filesystem::path StagedRoot;
    std::filesystem::path ManifestPath;
    std::vector<ExportMessage> Messages;
    std::vector<std::string> StagedAssets; // project-relative paths
};

using ExportProgressFn = std::function<void(const ExportProgress&)>;

/// Validates project, stages player + referenced assets into a new/empty directory,
/// writes export.manifest.json. Never deletes or clears a non-empty user folder.
[[nodiscard]] ExportResult ExportProject(
    const ExportOptions& options,
    const ExportProgressFn& onProgress = {});
}
