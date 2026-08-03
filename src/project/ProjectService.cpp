#include "project/ProjectService.hpp"

#include "assets/EmbeddedAssetProvider.hpp"

#include "engine/Version.hpp"

#include "project/ProjectJson.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")
#else
#include <cstdlib>
#endif

namespace fadix
{
namespace
{
constexpr char ProjectFileName[] = "project.fadix";

[[nodiscard]] std::string NarrowPath(const std::filesystem::path& path)
{
    return path.generic_string();
}

[[nodiscard]] std::filesystem::path WidenPath(std::string_view text)
{
    return std::filesystem::path{std::string{text}};
}

[[nodiscard]] Result<void> MakeDirectory(const std::filesystem::path& path)
{
    std::error_code error;
    if (std::filesystem::exists(path, error))
    {
        if (error)
        {
            return Result<void>::Error("Failed to inspect path: " + path.string() + " (" + error.message() + ")");
        }
        if (!std::filesystem::is_directory(path, error))
        {
            return Result<void>::Error("Path exists and is not a directory: " + path.string());
        }
        return Result<void>::Ok();
    }
    if (!std::filesystem::create_directories(path, error) || error)
    {
        return Result<void>::Error(
            "Could not create directory: " + path.string() + " (" + error.message() + ")");
    }
    return Result<void>::Ok();
}

[[nodiscard]] Result<void> CopyTree(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::error_code error;
    if (!std::filesystem::exists(source, error) || error)
    {
        return Result<void>::Error(
            "Template source missing: " + source.string() +
            (error ? (" (" + error.message() + ")") : std::string{}));
    }

    for (std::filesystem::recursive_directory_iterator it{source, error}, end;
         !error && it != end;
         it.increment(error))
    {
        const std::filesystem::path relative = std::filesystem::relative(it->path(), source, error);
        if (error)
        {
            return Result<void>::Error("Failed to resolve template path: " + error.message());
        }
        const std::filesystem::path target = destination / relative;
        if (it->is_directory(error))
        {
            if (const auto result = MakeDirectory(target); !result)
            {
                return result;
            }
            continue;
        }
        if (error)
        {
            return Result<void>::Error("Failed to inspect template entry: " + error.message());
        }
        if (const auto parent = target.parent_path(); !parent.empty())
        {
            if (const auto result = MakeDirectory(parent); !result)
            {
                return result;
            }
        }
        std::filesystem::copy_file(
            it->path(),
            target,
            std::filesystem::copy_options::overwrite_existing,
            error);
        if (error)
        {
            return Result<void>::Error(
                "Failed to copy template file to " + target.string() + " (" + error.message() + ")");
        }
    }
    if (error)
    {
        return Result<void>::Error("Template copy failed: " + error.message());
    }
    return Result<void>::Ok();
}

[[nodiscard]] Result<std::string> ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input)
    {
        return Result<std::string>::Error("Could not open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return Result<std::string>::Ok(buffer.str());
}

[[nodiscard]] Result<void> WriteTextFile(const std::filesystem::path& path, std::string_view contents)
{
    if (const auto parent = path.parent_path(); !parent.empty())
    {
        if (const auto result = MakeDirectory(parent); !result)
        {
            return result;
        }
    }
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
    {
        return Result<void>::Error("Could not write file: " + path.string());
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output)
    {
        return Result<void>::Error("Failed while writing file: " + path.string());
    }
    return Result<void>::Ok();
}

[[nodiscard]] std::string FormatTime(const std::chrono::system_clock::time_point& time)
{
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(time);
    const std::time_t value = std::chrono::system_clock::to_time_t(seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

[[nodiscard]] std::chrono::system_clock::time_point ParseTime(std::string_view text)
{
    std::tm utc{};
    if (text.size() < 20)
    {
        return std::chrono::system_clock::now();
    }
    std::istringstream stream{std::string{text}};
    stream >> std::get_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    if (stream.fail())
    {
        return std::chrono::system_clock::now();
    }
#ifdef _WIN32
    const std::time_t value = _mkgmtime(&utc);
#else
    const std::time_t value = timegm(&utc);
#endif
    if (value < 0)
    {
        return std::chrono::system_clock::now();
    }
    return std::chrono::system_clock::from_time_t(value);
}

#ifdef _WIN32
[[nodiscard]] std::wstring ToWide(const std::filesystem::path& path)
{
    return path.wstring();
}

[[nodiscard]] Result<std::filesystem::path> WinBrowseFolder(const std::filesystem::path& initial)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr)
    {
        return Result<std::filesystem::path>::Error("Could not create folder dialog");
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);

    if (!initial.empty())
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(ToWide(initial).c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            item != nullptr)
        {
            dialog->SetFolder(item);
            item->Release();
        }
    }

    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        dialog->Release();
        return Result<std::filesystem::path>::Error("Folder selection cancelled");
    }
    if (FAILED(hr))
    {
        dialog->Release();
        return Result<std::filesystem::path>::Error("Folder dialog failed");
    }

    IShellItem* resultItem = nullptr;
    hr = dialog->GetResult(&resultItem);
    dialog->Release();
    if (FAILED(hr) || resultItem == nullptr)
    {
        return Result<std::filesystem::path>::Error("Folder dialog returned no path");
    }

    PWSTR filePath = nullptr;
    hr = resultItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
    resultItem->Release();
    if (FAILED(hr) || filePath == nullptr)
    {
        return Result<std::filesystem::path>::Error("Could not read selected folder path");
    }

    std::filesystem::path selected{filePath};
    CoTaskMemFree(filePath);
    return Result<std::filesystem::path>::Ok(std::move(selected));
}

[[nodiscard]] Result<std::filesystem::path> WinBrowseProjectFile(const std::filesystem::path& initial)
{
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || dialog == nullptr)
    {
        return Result<std::filesystem::path>::Error("Could not create open-file dialog");
    }

    COMDLG_FILTERSPEC filters[] = {
        {L"Fadix Project (*.fadix)", L"*.fadix"},
        {L"All Files (*.*)", L"*.*"},
    };
    dialog->SetFileTypes(2, filters);
    dialog->SetDefaultExtension(L"fadix");

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    if (!initial.empty())
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(ToWide(initial).c_str(), nullptr, IID_PPV_ARGS(&item))) &&
            item != nullptr)
        {
            dialog->SetFolder(item);
            item->Release();
        }
    }

    hr = dialog->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED))
    {
        dialog->Release();
        return Result<std::filesystem::path>::Error("Open cancelled");
    }
    if (FAILED(hr))
    {
        dialog->Release();
        return Result<std::filesystem::path>::Error("Open-file dialog failed");
    }

    IShellItem* resultItem = nullptr;
    hr = dialog->GetResult(&resultItem);
    dialog->Release();
    if (FAILED(hr) || resultItem == nullptr)
    {
        return Result<std::filesystem::path>::Error("Open dialog returned no path");
    }

    PWSTR filePath = nullptr;
    hr = resultItem->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
    resultItem->Release();
    if (FAILED(hr) || filePath == nullptr)
    {
        return Result<std::filesystem::path>::Error("Could not read selected project path");
    }

    std::filesystem::path selected{filePath};
    CoTaskMemFree(filePath);
    return Result<std::filesystem::path>::Ok(std::move(selected));
}
#endif
}

ProjectService::ProjectService()
{
    static_cast<void>(LoadRecents());
}

ProjectService::~ProjectService() = default;

Result<void> ProjectService::ValidateProjectName(const std::string_view name)
{
    if (name.empty())
    {
        return Result<void>::Error("Project name is required");
    }
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || name.back() == '.')
    {
        return Result<void>::Error("Project name cannot start or end with space or '.'");
    }
    constexpr std::string_view Invalid = "<>:\"/\\|?*";
    for (const char ch : name)
    {
        if (Invalid.find(ch) != std::string_view::npos || static_cast<unsigned char>(ch) < 0x20)
        {
            return Result<void>::Error("Project name contains invalid characters");
        }
    }
    if (name == "." || name == "..")
    {
        return Result<void>::Error("Project name is reserved");
    }
    return Result<void>::Ok();
}

std::filesystem::path ProjectService::DefaultProjectsDirectory()
{
#ifdef _WIN32
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr)
    {
        std::filesystem::path path = std::filesystem::path{documents} / "FadixProjects";
        CoTaskMemFree(documents);
        return path;
    }
    return std::filesystem::path{"FadixProjects"};
#else
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path{home} / "FadixProjects";
    }
    return std::filesystem::path{"FadixProjects"};
#endif
}

std::filesystem::path ProjectService::TemplateDirectory(const ProjectTemplate projectTemplate)
{
    const char* folder = "empty_3d";
    switch (projectTemplate)
    {
    case ProjectTemplate::Empty2D: folder = "empty_2d"; break;
    case ProjectTemplate::Empty3D: folder = "empty_3d"; break;
    case ProjectTemplate::TinyGame: folder = "tiny_game"; break;
    }
    return RuntimeAssetRoot() / "templates" / folder;
}

std::string ProjectService::TemplateToString(const ProjectTemplate value)
{
    switch (value)
    {
    case ProjectTemplate::Empty2D: return "Empty2D";
    case ProjectTemplate::TinyGame: return "TinyGame";
    case ProjectTemplate::Empty3D: return "Empty3D";
    }
    return "Empty3D";
}

std::optional<ProjectTemplate> ProjectService::TemplateFromString(const std::string_view text)
{
    if (text == "Empty2D" || text == "empty_2d" || text == "2d")
    {
        return ProjectTemplate::Empty2D;
    }
    if (text == "TinyGame" || text == "tiny_game" || text == "tinygame")
    {
        return ProjectTemplate::TinyGame;
    }
    if (text == "Empty3D" || text == "empty_3d" || text == "3d")
    {
        return ProjectTemplate::Empty3D;
    }
    return std::nullopt;
}

std::filesystem::path ProjectService::RecentsConfigPath()
{
#ifdef _WIN32
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) &&
        appData != nullptr)
    {
        std::filesystem::path path =
            std::filesystem::path{appData} / "FadixEngine" / "recent_projects.json";
        CoTaskMemFree(appData);
        return path;
    }
    return std::filesystem::path{"recent_projects.json"};
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0')
    {
        return std::filesystem::path{xdg} / "fadixengine" / "recent_projects.json";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
    {
        return std::filesystem::path{home} / ".config" / "fadixengine" / "recent_projects.json";
    }
    return std::filesystem::path{"recent_projects.json"};
#endif
}

Result<ProjectMetadata> ProjectService::Create(
    const std::string_view name,
    const std::filesystem::path& parentDirectory,
    const ProjectTemplate projectTemplate)
{
    if (const auto valid = ValidateProjectName(name); !valid)
    {
        return Result<ProjectMetadata>::Error(valid.ErrorMessage());
    }
    if (parentDirectory.empty())
    {
        return Result<ProjectMetadata>::Error("Parent directory is required");
    }

    std::error_code error;
    const std::filesystem::path parent = std::filesystem::absolute(parentDirectory, error);
    if (error)
    {
        return Result<ProjectMetadata>::Error(
            "Invalid parent directory: " + parentDirectory.string() + " (" + error.message() + ")");
    }
    if (const auto result = MakeDirectory(parent); !result)
    {
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }

    const std::filesystem::path root = parent / std::string{name};
    if (std::filesystem::exists(root, error))
    {
        return Result<ProjectMetadata>::Error("Project folder already exists: " + root.string());
    }
    if (error)
    {
        return Result<ProjectMetadata>::Error(
            "Could not inspect destination: " + root.string() + " (" + error.message() + ")");
    }
    if (const auto result = MakeDirectory(root); !result)
    {
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }
    // From here on, failures remove the root. Half-created projects become folklore fast.
    if (const auto result = CopyTemplate(projectTemplate, root); !result)
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }
    if (const auto result = EnsureProjectFolders(root); !result)
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }

    ProjectMetadata metadata;
    metadata.Id = Uuid::Generate();
    metadata.Name = std::string{name};
    metadata.RootPath = root;
    metadata.ProjectFile = root / ProjectFileName;
    metadata.Template = projectTemplate;
    metadata.DefaultScene = "Scenes/Main.scene";

    if (const auto result = WriteProjectFile(metadata); !result)
    {
        std::error_code cleanupError;
        std::filesystem::remove_all(root, cleanupError);
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }

    m_Active = metadata;
    Remember(metadata);
    static_cast<void>(SaveRecents());
    return Result<ProjectMetadata>::Ok(std::move(metadata));
}

Result<ProjectMetadata> ProjectService::Open(const std::filesystem::path& projectFile)
{
    auto metadata = ReadProjectFile(projectFile);
    if (!metadata)
    {
        return metadata;
    }
    m_Active = metadata.Value();
    Remember(metadata.Value());
    static_cast<void>(SaveRecents());
    return metadata;
}

Result<void> ProjectService::Save()
{
    if (!m_Active)
    {
        return Result<void>::Error("No active project to save");
    }
    if (const auto result = WriteProjectFile(*m_Active); !result)
    {
        return result;
    }
    Remember(*m_Active);
    return SaveRecents();
}

std::span<const RecentProject> ProjectService::Recents() const noexcept
{
    return m_Recents;
}

const std::optional<ProjectMetadata>& ProjectService::Active() const noexcept
{
    return m_Active;
}

Result<void> ProjectService::RemoveRecent(const std::filesystem::path& projectFile)
{
    std::error_code error;
    const std::filesystem::path normalized = std::filesystem::weakly_canonical(projectFile, error);
    const std::filesystem::path key = error ? projectFile : normalized;
    const auto before = m_Recents.size();
    m_Recents.erase(
        std::remove_if(
            m_Recents.begin(),
            m_Recents.end(),
            [&](const RecentProject& entry)
            {
                std::error_code localError;
                const std::filesystem::path candidate =
                    std::filesystem::weakly_canonical(entry.Project.ProjectFile, localError);
                return (localError ? entry.Project.ProjectFile : candidate) == key ||
                       entry.Project.ProjectFile == projectFile;
            }),
        m_Recents.end());
    if (m_Recents.size() == before)
    {
        return Result<void>::Error("Project was not in the recent list");
    }
    return SaveRecents();
}

Result<ProjectMetadata> ProjectService::Rename(
    const std::filesystem::path& projectFile,
    const std::string_view newName)
{
    if (const auto valid = ValidateProjectName(newName); !valid)
    {
        return Result<ProjectMetadata>::Error(valid.ErrorMessage());
    }

    auto metadataResult = ReadProjectFile(projectFile);
    if (!metadataResult)
    {
        return metadataResult;
    }
    ProjectMetadata metadata = std::move(metadataResult).Value();
    const std::string oldName = metadata.Name;
    metadata.Name = std::string{newName};

    std::error_code error;
    const std::filesystem::path currentRoot = metadata.RootPath;
    const std::filesystem::path parent = currentRoot.parent_path();
    const std::filesystem::path desiredRoot = parent / std::string{newName};
    bool renamedFolder = false;

    if (currentRoot.filename() != newName)
    {
        if (std::filesystem::exists(desiredRoot, error))
        {
            return Result<ProjectMetadata>::Error(
                "Cannot rename folder; destination already exists: " + desiredRoot.string());
        }
        if (error)
        {
            return Result<ProjectMetadata>::Error(
                "Could not inspect rename destination (" + error.message() + ")");
        }
        std::filesystem::rename(currentRoot, desiredRoot, error);
        if (error)
        {
            return Result<ProjectMetadata>::Error(
                "Folder rename failed: " + error.message() +
                ". Metadata was not changed.");
        }
        renamedFolder = true;
        metadata.RootPath = desiredRoot;
        metadata.ProjectFile = desiredRoot / ProjectFileName;
    }

    if (const auto result = WriteProjectFile(metadata); !result)
    {
        if (renamedFolder)
        {
            std::error_code rollbackError;
            std::filesystem::rename(desiredRoot, currentRoot, rollbackError);
        }
        return Result<ProjectMetadata>::Error(result.ErrorMessage());
    }

    if (m_Active && m_Active->ProjectFile == projectFile)
    {
        m_Active = metadata;
    }

    for (RecentProject& entry : m_Recents)
    {
        if (entry.Project.ProjectFile == projectFile || entry.Project.Name == oldName)
        {
            entry.Project = metadata;
        }
    }
    Remember(metadata);
    static_cast<void>(SaveRecents());
    return Result<ProjectMetadata>::Ok(std::move(metadata));
}

Result<void> ProjectService::RevealInFileManager(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error)
    {
        return Result<void>::Error(
            "Path does not exist: " + path.string() +
            (error ? (" (" + error.message() + ")") : std::string{}));
    }

#ifdef _WIN32
    const std::filesystem::path absolute = std::filesystem::absolute(path, error);
    std::wstring params = L"/select,\"" + absolute.wstring() + L"\"";
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        return Result<void>::Error("Could not open Windows Explorer");
    }
    return Result<void>::Ok();
#else
    const std::filesystem::path target =
        std::filesystem::is_directory(path, error) ? path : path.parent_path();
    const std::string command = "xdg-open \"" + target.string() + "\" >/dev/null 2>&1 &";
    if (std::system(command.c_str()) != 0)
    {
        return Result<void>::Error("Could not open file manager via xdg-open");
    }
    return Result<void>::Ok();
#endif
}

Result<std::filesystem::path> ProjectService::BrowseForFolder(
    const std::filesystem::path& initialDirectory)
{
#ifdef _WIN32
    return WinBrowseFolder(initialDirectory.empty() ? DefaultProjectsDirectory() : initialDirectory);
#else
    static_cast<void>(initialDirectory);
    return Result<std::filesystem::path>::Error(
        "Folder browsing is not available on this platform; enter the path manually");
#endif
}

Result<std::filesystem::path> ProjectService::BrowseForProjectFile(
    const std::filesystem::path& initialDirectory)
{
#ifdef _WIN32
    return WinBrowseProjectFile(initialDirectory.empty() ? DefaultProjectsDirectory() : initialDirectory);
#else
    static_cast<void>(initialDirectory);
    return Result<std::filesystem::path>::Error(
        "File browsing is not available on this platform; enter the path manually");
#endif
}

Result<ProjectMetadata> ProjectService::ReadProjectFile(const std::filesystem::path& projectFile) const
{
    std::error_code error;
    if (!std::filesystem::exists(projectFile, error) || error)
    {
        return Result<ProjectMetadata>::Error(
            "Project file not found: " + projectFile.string() +
            (error ? (" (" + error.message() + ")") : std::string{}));
    }
    if (!std::filesystem::is_regular_file(projectFile, error) || error)
    {
        return Result<ProjectMetadata>::Error("Project path is not a file: " + projectFile.string());
    }
    if (projectFile.filename() != ProjectFileName)
    {
        return Result<ProjectMetadata>::Error(
            "Expected a project.fadix file, got: " + projectFile.filename().string());
    }

    const auto text = ReadTextFile(projectFile);
    if (!text)
    {
        return Result<ProjectMetadata>::Error(text.ErrorMessage());
    }
    const auto json = project_json::Parse(text.Value());
    if (!json || !json->IsObject())
    {
        return Result<ProjectMetadata>::Error("project.fadix is not valid JSON");
    }
    if (auto versionError = project_json::CheckFormatVersion(
            *json, "formatVersion", project_json::kProjectFormatVersion, "project.fadix"))
    {
        return Result<ProjectMetadata>::Error(std::move(*versionError));
    }

    ProjectMetadata metadata;
    metadata.ProjectFile = std::filesystem::weakly_canonical(projectFile, error);
    if (error)
    {
        metadata.ProjectFile = projectFile;
    }
    metadata.RootPath = metadata.ProjectFile.parent_path();

    if (!json->Contains("name") || !json->at("name").IsString() || json->at("name").AsString().empty())
    {
        return Result<ProjectMetadata>::Error("project.fadix is missing a valid \"name\"");
    }
    metadata.Name = json->at("name").AsString();

    if (json->Contains("id") && json->at("id").IsString())
    {
        if (auto id = Uuid::Parse(json->at("id").AsString()))
        {
            metadata.Id = *id;
        }
    }
    if (!metadata.Id.IsValid())
    {
        metadata.Id = Uuid::Generate();
    }

    if (json->Contains("template") && json->at("template").IsString())
    {
        if (auto templ = TemplateFromString(json->at("template").AsString()))
        {
            metadata.Template = *templ;
        }
        else
        {
            return Result<ProjectMetadata>::Error(
                "Unknown template in project.fadix: " + json->at("template").AsString());
        }
    }

    if (json->Contains("engineVersion") && json->at("engineVersion").IsString())
    {
        const std::string& version = json->at("engineVersion").AsString();
        if (version.empty())
        {
            return Result<ProjectMetadata>::Error("project.fadix engineVersion is empty");
        }
    }
    else
    {
        return Result<ProjectMetadata>::Error("project.fadix is missing \"engineVersion\"");
    }

    if (!json->Contains("defaultScene") || !json->at("defaultScene").IsString() ||
        json->at("defaultScene").AsString().empty())
    {
        return Result<ProjectMetadata>::Error("project.fadix is missing \"defaultScene\"");
    }
    metadata.DefaultScene = json->at("defaultScene").AsString();

    if (!json->Contains("folders") || !json->at("folders").IsObject())
    {
        return Result<ProjectMetadata>::Error("project.fadix is missing \"folders\"");
    }

    return Result<ProjectMetadata>::Ok(std::move(metadata));
}

Result<void> ProjectService::WriteProjectFile(const ProjectMetadata& metadata) const
{
    project_json::Value root = project_json::Value::MakeObject();
    root["formatVersion"] = project_json::Value::MakeNumber(project_json::kProjectFormatVersion);
    root["id"] = project_json::Value::MakeString(metadata.Id.ToString());
    root["name"] = project_json::Value::MakeString(metadata.Name);
    root["engineVersion"] = project_json::Value::MakeString(std::string{EngineVersion});
    root["template"] = project_json::Value::MakeString(TemplateToString(metadata.Template));
    root["defaultScene"] = project_json::Value::MakeString(
        metadata.DefaultScene.empty() ? "Scenes/Main.scene" : metadata.DefaultScene);

    project_json::Value folders = project_json::Value::MakeObject();
    folders["assets"] = project_json::Value::MakeString("Assets");
    folders["scenes"] = project_json::Value::MakeString("Scenes");
    folders["scripts"] = project_json::Value::MakeString("Scripts");
    folders["audio"] = project_json::Value::MakeString("Audio");
    folders["ui"] = project_json::Value::MakeString("UI");
    folders["cache"] = project_json::Value::MakeString("Cache");
    folders["saved"] = project_json::Value::MakeString("Saved");
    root["folders"] = std::move(folders);

    return WriteTextFile(metadata.ProjectFile, project_json::Stringify(root));
}

Result<void> ProjectService::EnsureProjectFolders(const std::filesystem::path& root) const
{
    for (const char* folder : {"Assets", "Scenes", "Scripts", "Audio", "UI", "Cache", "Saved"})
    {
        if (const auto result = MakeDirectory(root / folder); !result)
        {
            return result;
        }
    }
    if (const auto result = MakeDirectory(root / "Assets" / "Prefabs"); !result)
    {
        return result;
    }
    return Result<void>::Ok();
}

Result<void> ProjectService::CopyTemplate(
    const ProjectTemplate projectTemplate,
    const std::filesystem::path& destinationRoot) const
{
    return CopyTree(TemplateDirectory(projectTemplate), destinationRoot);
}

void ProjectService::Remember(const ProjectMetadata& metadata)
{
    const auto now = std::chrono::system_clock::now();
    m_Recents.erase(
        std::remove_if(
            m_Recents.begin(),
            m_Recents.end(),
            [&](const RecentProject& entry)
            {
                return entry.Project.ProjectFile == metadata.ProjectFile ||
                       entry.Project.Id == metadata.Id;
            }),
        m_Recents.end());
    m_Recents.insert(m_Recents.begin(), RecentProject{metadata, now});
    constexpr std::size_t kMaxRecents = 20;
    if (m_Recents.size() > kMaxRecents)
    {
        m_Recents.resize(kMaxRecents);
    }
}

Result<void> ProjectService::LoadRecents()
{
    m_Recents.clear();
    const std::filesystem::path path = RecentsConfigPath();
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error)
    {
        return Result<void>::Ok();
    }
    const auto text = ReadTextFile(path);
    if (!text)
    {
        return Result<void>::Error(text.ErrorMessage());
    }
    const auto json = project_json::Parse(text.Value());
    if (!json || !json->IsObject() || !json->Contains("projects") || !json->at("projects").IsArray())
    {
        return Result<void>::Error("recent_projects.json is invalid");
    }

    for (const project_json::Value& entry : json->at("projects").Array())
    {
        if (!entry.IsObject() || !entry.Contains("projectFile") || !entry.at("projectFile").IsString())
        {
            continue;
        }
        RecentProject recent;
        recent.Project.ProjectFile = WidenPath(entry.at("projectFile").AsString());
        recent.Project.RootPath = recent.Project.ProjectFile.parent_path();
        if (entry.Contains("name") && entry.at("name").IsString())
        {
            recent.Project.Name = entry.at("name").AsString();
        }
        if (entry.Contains("rootPath") && entry.at("rootPath").IsString())
        {
            recent.Project.RootPath = WidenPath(entry.at("rootPath").AsString());
        }
        if (entry.Contains("id") && entry.at("id").IsString())
        {
            if (auto id = Uuid::Parse(entry.at("id").AsString()))
            {
                recent.Project.Id = *id;
            }
        }
        if (entry.Contains("template") && entry.at("template").IsString())
        {
            if (auto templ = TemplateFromString(entry.at("template").AsString()))
            {
                recent.Project.Template = *templ;
            }
        }
        if (entry.Contains("lastOpened") && entry.at("lastOpened").IsString())
        {
            recent.LastOpened = ParseTime(entry.at("lastOpened").AsString());
        }
        else
        {
            recent.LastOpened = std::chrono::system_clock::now();
        }
        if (recent.Project.Name.empty())
        {
            recent.Project.Name = recent.Project.RootPath.filename().string();
        }
        m_Recents.push_back(std::move(recent));
    }
    return Result<void>::Ok();
}

Result<void> ProjectService::SaveRecents() const
{
    project_json::Value root = project_json::Value::MakeObject();
    project_json::Value projects = project_json::Value::MakeArray();
    for (const RecentProject& entry : m_Recents)
    {
        project_json::Value item = project_json::Value::MakeObject();
        item["id"] = project_json::Value::MakeString(entry.Project.Id.ToString());
        item["name"] = project_json::Value::MakeString(entry.Project.Name);
        item["rootPath"] = project_json::Value::MakeString(NarrowPath(entry.Project.RootPath));
        item["projectFile"] = project_json::Value::MakeString(NarrowPath(entry.Project.ProjectFile));
        item["template"] =
            project_json::Value::MakeString(TemplateToString(entry.Project.Template));
        item["lastOpened"] = project_json::Value::MakeString(FormatTime(entry.LastOpened));
        projects.Push(std::move(item));
    }
    root["projects"] = std::move(projects);
    return WriteTextFile(RecentsConfigPath(), project_json::Stringify(root));
}

#ifdef CreateService
#undef CreateService
#endif

std::unique_ptr<IProjectService> CreateProjectService()
{
    return std::make_unique<ProjectService>();
}

namespace project
{
std::unique_ptr<IProjectService> CreateService()
{
    return CreateProjectService();
}
}
}
