#include "engine/app/ModuleRegistration.hpp"
#include "engine/project/IProjectService.hpp"
#include "engine/project/ProjectMetadata.hpp"
#include "engine/Version.hpp"
#include "project/ProjectService.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
[[nodiscard]] bool ExpectDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    const bool ok = std::filesystem::is_directory(path, error) && !error;
    if (!ok)
    {
        std::cerr << "[smoke] missing directory: " << path.string() << '\n';
    }
    return ok;
}
}

int main()
{
    const auto root = std::filesystem::temp_directory_path() / "fadix_project_smoke";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    if (error)
    {
        std::cerr << "[smoke] could not create temp root\n";
        return 1;
    }

    auto service = fadix::project::CreateService();
    auto created = service->Create("SmokeEmpty3D", root, fadix::ProjectTemplate::Empty3D);
    if (!created)
    {
        std::cerr << "[smoke] Create Empty3D failed: " << created.ErrorMessage() << '\n';
        return 2;
    }

    const auto projectRoot = created.Value().RootPath;
    const bool foldersOk =
        ExpectDirectory(projectRoot / "Assets") && ExpectDirectory(projectRoot / "Scenes") &&
        ExpectDirectory(projectRoot / "Scripts") && ExpectDirectory(projectRoot / "Cache") &&
        ExpectDirectory(projectRoot / "Saved");
    const bool projectFileOk = std::filesystem::is_regular_file(created.Value().ProjectFile, error);
    if (!foldersOk || !projectFileOk)
    {
        std::cerr << "[smoke] Empty3D layout incomplete\n";
        return 3;
    }
    std::ifstream projectInput(created.Value().ProjectFile, std::ios::binary);
    const std::string projectJson{
        std::istreambuf_iterator<char>{projectInput}, std::istreambuf_iterator<char>{}};
    projectInput.close();
    if (projectJson.find(std::string{fadix::EngineVersion}) == std::string::npos)
    {
        std::cerr << "[smoke] project file does not contain current engine version\n";
        return 13;
    }

    auto created2d = service->Create("SmokeEmpty2D", root, fadix::ProjectTemplate::Empty2D);
    if (!created2d)
    {
        std::cerr << "[smoke] Create Empty2D failed: " << created2d.ErrorMessage() << '\n';
        return 4;
    }

    auto opened = service->Open(created.Value().ProjectFile);
    if (!opened)
    {
        std::cerr << "[smoke] Open failed: " << opened.ErrorMessage() << '\n';
        return 5;
    }

    if (service->Recents().empty())
    {
        std::cerr << "[smoke] Recents empty after create/open\n";
        return 6;
    }

    auto* concrete = dynamic_cast<fadix::ProjectService*>(service.get());
    if (concrete == nullptr)
    {
        std::cerr << "[smoke] Expected concrete ProjectService\n";
        return 7;
    }

    auto renamed = concrete->Rename(created.Value().ProjectFile, "SmokeRenamed3D");
    if (!renamed)
    {
        std::cerr << "[smoke] Rename failed: " << renamed.ErrorMessage() << '\n';
        return 8;
    }
    if (renamed.Value().Name != "SmokeRenamed3D")
    {
        std::cerr << "[smoke] Rename did not update project name\n";
        return 9;
    }

    if (auto reveal = concrete->RevealInFileManager(renamed.Value().ProjectFile); !reveal)
    {
        std::cerr << "[smoke] Reveal failed: " << reveal.ErrorMessage() << '\n';
        return 10;
    }

    if (auto removed = concrete->RemoveRecent(renamed.Value().ProjectFile); !removed)
    {
        std::cerr << "[smoke] RemoveRecent failed: " << removed.ErrorMessage() << '\n';
        return 11;
    }
    for (const auto& entry : service->Recents())
    {
        if (entry.Project.ProjectFile == renamed.Value().ProjectFile)
        {
            std::cerr << "[smoke] RemoveRecent left project in recents\n";
            return 12;
        }
    }

    std::cout << "[smoke] PASS create/open/rename/reveal/remove for Empty3D and Empty2D\n";
    std::filesystem::remove_all(root, error);
    return 0;
}
