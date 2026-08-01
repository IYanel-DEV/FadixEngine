#pragma once

#include "engine/Uuid.hpp"

#include <chrono>
#include <filesystem>
#include <string>

namespace fadix
{
enum class ProjectTemplate
{
    Empty2D,
    Empty3D,
    TinyGame
};

struct ProjectMetadata
{
    Uuid Id;
    std::string Name;
    std::filesystem::path RootPath;
    std::filesystem::path ProjectFile;
    ProjectTemplate Template{ProjectTemplate::Empty3D};
    /// Project-relative boot scene path (e.g. "Scenes/Main.scene").
    std::string DefaultScene{"Scenes/Main.scene"};
};

struct RecentProject
{
    ProjectMetadata Project;
    std::chrono::system_clock::time_point LastOpened{};
};
}
