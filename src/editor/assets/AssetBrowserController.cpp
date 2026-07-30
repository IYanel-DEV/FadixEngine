#include "editor/assets/AssetBrowserController.hpp"

#include "assets/AssetDatabase.hpp"
#include "engine/project/IProjectService.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <system_error>

namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool IsSafeName(const std::string_view name)
{
    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }
    return name.find_first_of("<>:\"/\\|?*") == std::string_view::npos;
}

bool FolderHasListedChildren(const std::filesystem::path& folder)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        folder, std::filesystem::directory_options::skip_permission_denied, error};
    for (const std::filesystem::directory_iterator end; !error && iterator != end;
         iterator.increment(error))
    {
        if (iterator->path().extension() == ".fadixmeta")
        {
            continue;
        }
        return true;
    }
    return false;
}
}

namespace fadix
{
AssetDragDropBlob MakeAssetDragDropBlob(const AssetDragPayload& payload)
{
    AssetDragDropBlob blob;
    blob.Handle = payload.Handle;
    const std::string path = payload.SourcePath.generic_string();
    std::snprintf(blob.SourcePath, sizeof(blob.SourcePath), "%s", path.c_str());
    std::snprintf(blob.AssetType, sizeof(blob.AssetType), "%s", payload.AssetType.c_str());
    return blob;
}

std::optional<AssetDragPayload> ParseAssetDragDropBlob(const void* data, const std::size_t size)
{
    if (data == nullptr || size != sizeof(AssetDragDropBlob))
    {
        return std::nullopt;
    }
    const auto& blob = *static_cast<const AssetDragDropBlob*>(data);
    AssetDragPayload payload;
    payload.Handle = blob.Handle;
    payload.SourcePath = std::filesystem::path{std::string{blob.SourcePath}};
    payload.AssetType = blob.AssetType;
    return payload;
}

AssetBrowserController::AssetBrowserController(IAssetDatabase& database)
    : m_Database(database)
{
    if (auto* concrete = dynamic_cast<AssetDatabase*>(&database))
    {
        SetProjectRoot(concrete->ProjectRoot());
    }
}

AssetBrowserController::AssetBrowserController(
    IAssetDatabase& database,
    IProjectService& projects)
    : m_Database(database), m_Projects(&projects)
{
    if (projects.Active())
    {
        SetProjectRoot(projects.Active()->RootPath);
    }
}

void AssetBrowserController::SetCallbacks(AssetBrowserCallbacks callbacks)
{
    m_Callbacks = std::move(callbacks);
}

void AssetBrowserController::SetProjectRoot(std::filesystem::path projectRoot)
{
    m_ProjectRoot = std::move(projectRoot);
    m_AssetsRoot = m_ProjectRoot / "Assets";
    m_ScenesRoot = m_ProjectRoot / "Scenes";
    m_CurrentFolder = m_AssetsRoot;
    static_cast<void>(Refresh());
}

Result<void> AssetBrowserController::Refresh()
{
    if (m_Projects != nullptr && m_Projects->Active() &&
        m_ProjectRoot != m_Projects->Active()->RootPath)
    {
        m_ProjectRoot = m_Projects->Active()->RootPath;
        m_AssetsRoot = m_ProjectRoot / "Assets";
        m_ScenesRoot = m_ProjectRoot / "Scenes";
        m_CurrentFolder = m_AssetsRoot;
    }
    if (m_AssetsRoot.empty())
    {
        return Result<void>::Error("No active project is available to the asset browser");
    }

    std::error_code error;
    std::filesystem::create_directories(m_AssetsRoot, error);
    if (error)
    {
        Error("Could not create Assets directory: " + error.message());
        return Result<void>::Error("Could not create Assets directory: " + error.message());
    }
    std::filesystem::create_directories(m_ScenesRoot, error);
    if (error)
    {
        Error("Could not create Scenes directory: " + error.message());
        return Result<void>::Error("Could not create Scenes directory: " + error.message());
    }
    if (!IsInsideBrowserRoots(m_CurrentFolder))
    {
        m_CurrentFolder = m_AssetsRoot;
    }
    if (auto* concrete = dynamic_cast<AssetDatabase*>(&m_Database))
    {
        Result<void> result = concrete->Refresh();
        if (!result)
        {
            Error(result.ErrorMessage());
            return result;
        }
    }
    RebuildEntries();
    return Result<void>::Ok();
}

std::future<Result<AssetHandle>> AssetBrowserController::Import(
    const std::filesystem::path& sourcePath)
{
    return m_Database.ImportAsync(Resolve(sourcePath));
}

Result<std::filesystem::path> AssetBrowserController::ImportExternalModel(
    const std::filesystem::path& sourcePath)
{
    auto* concrete = dynamic_cast<AssetDatabase*>(&m_Database);
    if (concrete == nullptr)
    {
        return Result<std::filesystem::path>::Error(
            "The active asset database cannot import external models");
    }
    const std::filesystem::path destination =
        IsInsideAssets(m_CurrentFolder) ? m_CurrentFolder : m_AssetsRoot;
    Result<std::filesystem::path> result =
        concrete->ImportExternalModel(sourcePath, destination);
    if (!result)
    {
        Error(result.ErrorMessage());
        return result;
    }
    m_CurrentFolder = destination;
    RebuildEntries();
    return result;
}

void AssetBrowserController::PollImports()
{
    m_Database.PollImports();
    RebuildEntries();
}

void AssetBrowserController::SetSearch(std::string search)
{
    m_Search = std::move(search);
    RebuildEntries();
}

std::string_view AssetBrowserController::Search() const noexcept
{
    return m_Search;
}

Result<void> AssetBrowserController::OpenFolder(const std::filesystem::path& folder)
{
    const std::filesystem::path resolved = Resolve(folder);
    std::error_code error;
    if (!IsInsideBrowserRoots(resolved) || !std::filesystem::is_directory(resolved, error) || error)
    {
        return Result<void>::Error("Asset folder is invalid: " + resolved.string());
    }
    m_CurrentFolder = resolved;
    RebuildEntries();
    return Result<void>::Ok();
}

Result<void> AssetBrowserController::NavigateUp()
{
    if (m_CurrentFolder == m_AssetsRoot)
    {
        return Result<void>::Ok();
    }
    if (m_CurrentFolder == m_ScenesRoot)
    {
        return OpenFolder(m_AssetsRoot);
    }
    return OpenFolder(m_CurrentFolder.parent_path());
}

Result<void> AssetBrowserController::NavigateHome()
{
    return OpenFolder(m_AssetsRoot);
}

const std::filesystem::path& AssetBrowserController::CurrentFolder() const noexcept
{
    return m_CurrentFolder;
}

std::string AssetBrowserController::CurrentFolderDisplay() const
{
    if (m_ProjectRoot.empty() || m_CurrentFolder.empty())
    {
        return "Assets/";
    }

    const std::filesystem::path relative = m_CurrentFolder.lexically_normal().lexically_relative(
        m_ProjectRoot.lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
    {
        return "Assets/";
    }

    std::string display = relative.generic_string();
    if (display == ".")
    {
        return "Assets/";
    }
    if (!display.empty() && display.back() != '/')
    {
        display.push_back('/');
    }
    return display;
}

std::span<const AssetBrowserEntry> AssetBrowserController::Entries() const noexcept
{
    return m_Entries;
}

Result<void> AssetBrowserController::CreateFolder(const std::string_view name)
{
    if (!IsSafeName(name))
    {
        return Result<void>::Error("Folder name is empty or contains reserved characters");
    }
    if (m_CurrentFolder.empty() || !IsInsideBrowserRoots(m_CurrentFolder))
    {
        return Result<void>::Error("Create folder requires a valid Assets location");
    }

    std::filesystem::path destination = m_CurrentFolder / std::string{name};
    if (!IsInsideBrowserRoots(destination))
    {
        return Result<void>::Error("Folder must remain inside the Assets directory");
    }

    std::error_code error;
    if (std::filesystem::exists(destination, error))
    {
        const std::string base{name};
        int suffix = 2;
        do
        {
            destination = m_CurrentFolder / (base + " " + std::to_string(suffix++));
            error.clear();
        } while (std::filesystem::exists(destination, error));
    }

    if (!std::filesystem::create_directory(destination, error) || error)
    {
        Error("Could not create folder: " + error.message());
        return Result<void>::Error("Could not create folder: " + error.message());
    }
    return Refresh();
}

Result<void> AssetBrowserController::Rename(
    const std::filesystem::path& path,
    const std::string_view newName)
{
    if (!IsSafeName(newName))
    {
        return Result<void>::Error("Asset name is empty or contains reserved characters");
    }
    const std::filesystem::path source = Resolve(path);
    const std::filesystem::path destination = source.parent_path() / newName;
    if (!IsInsideBrowserRoots(source) || !IsInsideBrowserRoots(destination) ||
        source == m_AssetsRoot || source == m_ScenesRoot)
    {
        return Result<void>::Error("Asset rename must remain inside the Assets directory");
    }
    if (source.filename() == newName)
    {
        return Result<void>::Ok();
    }
    return MoveTo(source, destination);
}

Result<void> AssetBrowserController::Move(
    const std::filesystem::path& path,
    const std::filesystem::path& destinationFolder)
{
    const std::filesystem::path source = Resolve(path);
    const std::filesystem::path folder = Resolve(destinationFolder);
    if (!IsInsideBrowserRoots(source) || !IsInsideBrowserRoots(folder) ||
        source == m_AssetsRoot || source == m_ScenesRoot)
    {
        return Result<void>::Error("Asset move must remain inside the Assets directory");
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error) || error)
    {
        return Result<void>::Error("Move destination is not a folder");
    }
    const std::filesystem::path destination = folder / source.filename();
    if (destination.lexically_normal() == source.lexically_normal())
    {
        return Result<void>::Ok();
    }
    return MoveTo(source, destination);
}

Result<void> AssetBrowserController::MoveTo(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::error_code error;
    if (std::filesystem::exists(destination, error))
    {
        return Result<void>::Error("An asset with that name already exists");
    }
    std::filesystem::rename(source, destination, error);
    if (error)
    {
        Error("Could not rename asset: " + error.message());
        return Result<void>::Error("Could not rename asset: " + error.message());
    }

    const std::filesystem::path sourceMetadata{source.string() + ".fadixmeta"};
    const std::filesystem::path destinationMetadata{destination.string() + ".fadixmeta"};
    if (std::filesystem::exists(sourceMetadata, error))
    {
        error.clear();
        std::filesystem::rename(sourceMetadata, destinationMetadata, error);
        if (error)
        {
            std::error_code rollbackError;
            std::filesystem::rename(destination, source, rollbackError);
            Error("Could not rename asset metadata: " + error.message());
            return Result<void>::Error("Could not rename asset metadata: " + error.message());
        }
    }

    const std::filesystem::path sourceNorm = source.lexically_normal();
    const std::filesystem::path currentNorm = m_CurrentFolder.lexically_normal();
    if (currentNorm == sourceNorm)
    {
        m_CurrentFolder = destination.lexically_normal();
    }
    else
    {
        const std::filesystem::path underSource = currentNorm.lexically_relative(sourceNorm);
        if (!underSource.empty() && *underSource.begin() != "..")
        {
            m_CurrentFolder = (destination / underSource).lexically_normal();
        }
    }
    return Refresh();
}

Result<void> AssetBrowserController::Delete(const std::filesystem::path& path)
{
    const std::filesystem::path resolved = Resolve(path);
    if (!IsInsideBrowserRoots(resolved) || resolved == m_AssetsRoot || resolved == m_ScenesRoot)
    {
        return Result<void>::Error("Asset deletion must target an item inside Assets");
    }
    if (m_Callbacks.ConfirmDelete && !m_Callbacks.ConfirmDelete(resolved))
    {
        return Result<void>::Error("Asset deletion was cancelled");
    }

    std::optional<std::filesystem::path> importedPath;
    if (const std::optional<AssetHandle> handle = m_Database.FindByPath(resolved))
    {
        if (const AssetMetadata* metadata = m_Database.Meta(*handle))
        {
            importedPath = metadata->ImportedPath;
        }
    }

    std::error_code error;
    std::filesystem::remove_all(resolved, error);
    if (error)
    {
        Error("Could not delete asset: " + error.message());
        return Result<void>::Error("Could not delete asset: " + error.message());
    }
    std::filesystem::remove(std::filesystem::path{resolved.string() + ".fadixmeta"}, error);
    if (importedPath)
    {
        error.clear();
        std::filesystem::remove(*importedPath, error);
    }
    return Refresh();
}

Result<AssetHandle> AssetBrowserController::CreateMaterial(const std::string_view name)
{
    auto* concrete = dynamic_cast<AssetDatabase*>(&m_Database);
    if (concrete == nullptr)
    {
        return Result<AssetHandle>::Error("Material creation requires the project asset database");
    }
    Result<AssetHandle> result = concrete->CreateMaterial(name, m_CurrentFolder);
    if (!result)
    {
        Error(result.ErrorMessage());
    }
    else
    {
        static_cast<void>(Refresh());
    }
    return result;
}

Result<AssetHandle> AssetBrowserController::Duplicate(const std::filesystem::path& path)
{
    auto* concrete = dynamic_cast<AssetDatabase*>(&m_Database);
    if (concrete == nullptr)
    {
        return Result<AssetHandle>::Error("Asset duplication requires the project asset database");
    }
    Result<AssetHandle> result = concrete->DuplicateAsset(Resolve(path));
    if (!result)
    {
        Error(result.ErrorMessage());
    }
    else
    {
        static_cast<void>(Refresh());
    }
    return result;
}

std::future<Result<AssetHandle>> AssetBrowserController::ReimportTexture(const AssetHandle& handle)
{
    auto* concrete = dynamic_cast<AssetDatabase*>(&m_Database);
    if (concrete == nullptr)
    {
        std::promise<Result<AssetHandle>> promise;
        promise.set_value(Result<AssetHandle>::Error("Texture reimport requires the project asset database"));
        return promise.get_future();
    }
    return concrete->ReimportTexture(handle);
}

void AssetBrowserController::Activate(const AssetBrowserEntry& entry)
{
    if (entry.Kind == AssetBrowserEntryKind::Folder)
    {
        static_cast<void>(OpenFolder(entry.Path));
        return;
    }
    if (entry.AssetType == "Scene" && entry.Handle && m_Callbacks.LoadScene)
    {
        m_Callbacks.LoadScene(*entry.Handle, entry.Path);
    }
    else if (entry.AssetType == "Material" && entry.Handle && m_Callbacks.OpenMaterial)
    {
        m_Callbacks.OpenMaterial(*entry.Handle, entry.Path);
    }
    else if (entry.AssetType == "Script" && m_Callbacks.OpenScript)
    {
        m_Callbacks.OpenScript(entry.Path.stem().string());
    }
    else if (entry.AssetType == "Mesh" && entry.Handle && m_Callbacks.CreateMeshEntity)
    {
        m_Callbacks.CreateMeshEntity(*entry.Handle, entry.Path);
    }
}

std::optional<AssetDragPayload> AssetBrowserController::BeginDrag(
    const AssetBrowserEntry& entry) const
{
    if (entry.Kind != AssetBrowserEntryKind::Asset || !entry.Handle)
    {
        return std::nullopt;
    }
    return AssetDragPayload{*entry.Handle, entry.Path, entry.AssetType};
}

void AssetBrowserController::DropInScene(const AssetDragPayload& payload)
{
    if (payload.AssetType == "Mesh" && m_Callbacks.CreateMeshEntity)
    {
        m_Callbacks.CreateMeshEntity(payload.Handle, payload.SourcePath);
    }
    if (m_Callbacks.AssetDroppedInScene)
    {
        m_Callbacks.AssetDroppedInScene(payload);
    }
}

std::filesystem::path AssetBrowserController::Resolve(const std::filesystem::path& path) const
{
    if (path.is_absolute())
    {
        return path.lexically_normal();
    }
    return (m_CurrentFolder / path).lexically_normal();
}

bool AssetBrowserController::IsInsideAssets(const std::filesystem::path& path) const
{
    if (m_AssetsRoot.empty())
    {
        return false;
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::filesystem::path root = m_AssetsRoot.lexically_normal();
    if (normalized == root)
    {
        return true;
    }
    const std::filesystem::path relative = normalized.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

bool AssetBrowserController::IsInsideBrowserRoots(const std::filesystem::path& path) const
{
    if (IsInsideAssets(path) || m_ScenesRoot.empty())
    {
        return IsInsideAssets(path);
    }
    const std::filesystem::path normalized = path.lexically_normal();
    const std::filesystem::path root = m_ScenesRoot.lexically_normal();
    if (normalized == root)
    {
        return true;
    }
    const std::filesystem::path relative = normalized.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

void AssetBrowserController::RebuildEntries()
{
    std::vector<AssetBrowserEntry> entries;
    if (!m_CurrentFolder.empty())
    {
        const std::string search = Lower(m_Search);
        std::error_code error;
        if (search.empty())
        {
            if (m_CurrentFolder == m_AssetsRoot && !m_ScenesRoot.empty())
            {
                entries.push_back(AssetBrowserEntry{
                    AssetBrowserEntryKind::Folder,
                    m_ScenesRoot,
                    "Project Scenes",
                    {},
                    std::nullopt,
                    FolderHasListedChildren(m_ScenesRoot)});
            }
            std::filesystem::directory_iterator iterator{
                m_CurrentFolder, std::filesystem::directory_options::skip_permission_denied, error};
            for (const std::filesystem::directory_iterator end; !error && iterator != end; iterator.increment(error))
            {
                const std::filesystem::path path = iterator->path();
                if (path.extension() == ".fadixmeta")
                {
                    continue;
                }
                if (iterator->is_directory(error))
                {
                    entries.push_back(AssetBrowserEntry{
                        AssetBrowserEntryKind::Folder,
                        path,
                        path.filename().string(),
                        {},
                        std::nullopt,
                        FolderHasListedChildren(path)});
                }
                else if (const std::optional<AssetHandle> handle = m_Database.FindByPath(path))
                {
                    const AssetMetadata* metadata = m_Database.Meta(*handle);
                    entries.push_back(AssetBrowserEntry{
                        AssetBrowserEntryKind::Asset,
                        path,
                        path.filename().string(),
                        metadata == nullptr ? std::string{} : metadata->Type,
                        handle});
                }
            }
        }
        else
        {
            for (const AssetMetadata& metadata : m_Database.List())
            {
                const std::string relative =
                    metadata.SourcePath.lexically_relative(m_ProjectRoot).generic_string();
                if (Lower(relative).find(search) != std::string::npos)
                {
                    entries.push_back(AssetBrowserEntry{
                        AssetBrowserEntryKind::Asset,
                        metadata.SourcePath,
                        relative,
                        metadata.Type,
                        metadata.Handle});
                }
            }
        }

        std::sort(entries.begin(), entries.end(), [](const AssetBrowserEntry& left, const AssetBrowserEntry& right)
        {
            if (left.Kind != right.Kind)
            {
                return left.Kind == AssetBrowserEntryKind::Folder;
            }
            return Lower(left.DisplayName) < Lower(right.DisplayName);
        });
    }

    // PollImports() drives RebuildEntries every frame. Only notify (which rebuilds the
    // asset-list DOM) when the entries actually change, otherwise the constant SetInnerRML
    // destroys the row elements mid-click and breaks select/drag/rename/delete.
    if (entries == m_Entries)
    {
        return;
    }
    m_Entries = std::move(entries);
    if (m_Callbacks.EntriesChanged)
    {
        m_Callbacks.EntriesChanged();
    }
}

void AssetBrowserController::Error(std::string message) const
{
    if (m_Callbacks.ReportError)
    {
        m_Callbacks.ReportError(message);
    }
}
}
