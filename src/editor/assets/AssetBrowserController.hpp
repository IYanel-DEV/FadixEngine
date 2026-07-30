#pragma once

#include "engine/Result.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fadix
{
class IAssetDatabase;
class IProjectService;

enum class AssetBrowserEntryKind
{
    Folder,
    Asset
};

struct AssetBrowserEntry
{
    AssetBrowserEntryKind Kind{AssetBrowserEntryKind::Asset};
    std::filesystem::path Path;
    std::string DisplayName;
    std::string AssetType;
    std::optional<AssetHandle> Handle;
    bool FolderHasChildren{false};

    bool operator==(const AssetBrowserEntry&) const = default;
};

struct AssetDragPayload
{
    AssetHandle Handle;
    std::filesystem::path SourcePath;
    std::string AssetType;
};

/// Fixed-size drag blob for ImGui (and any UI). Never store pointers into Entries().
struct AssetDragDropBlob
{
    AssetHandle Handle{};
    char SourcePath[512]{};
    char AssetType[64]{};
};

[[nodiscard]] AssetDragDropBlob MakeAssetDragDropBlob(const AssetDragPayload& payload);
[[nodiscard]] std::optional<AssetDragPayload> ParseAssetDragDropBlob(
    const void* data, std::size_t size);

struct AssetBrowserCallbacks
{
    std::function<void(const AssetHandle&, const std::filesystem::path&)> LoadScene;
    std::function<void(const AssetHandle&, const std::filesystem::path&)> OpenMaterial;
    std::function<void(const std::string& scriptName)> OpenScript;
    std::function<void(const AssetHandle&, const std::filesystem::path&)> CreateMeshEntity;
    std::function<void(const AssetDragPayload&)> AssetDroppedInScene;
    std::function<bool(const std::filesystem::path&)> ConfirmDelete;
    std::function<void(std::string_view)> ReportError;
    std::function<void()> EntriesChanged;
};

class AssetBrowserController final
{
public:
    explicit AssetBrowserController(IAssetDatabase& database);
    AssetBrowserController(IAssetDatabase& database, IProjectService& projects);

    void SetCallbacks(AssetBrowserCallbacks callbacks);
    void SetProjectRoot(std::filesystem::path projectRoot);
    [[nodiscard]] Result<void> Refresh();
    [[nodiscard]] std::future<Result<AssetHandle>> Import(
        const std::filesystem::path& sourcePath);
    [[nodiscard]] Result<std::filesystem::path> ImportExternalModel(
        const std::filesystem::path& sourcePath);
    void PollImports();

    void SetSearch(std::string search);
    [[nodiscard]] std::string_view Search() const noexcept;
    [[nodiscard]] Result<void> OpenFolder(const std::filesystem::path& folder);
    [[nodiscard]] Result<void> NavigateHome();
    [[nodiscard]] Result<void> NavigateUp();
    [[nodiscard]] const std::filesystem::path& CurrentFolder() const noexcept;
    [[nodiscard]] std::string CurrentFolderDisplay() const;
    [[nodiscard]] std::span<const AssetBrowserEntry> Entries() const noexcept;

    [[nodiscard]] Result<void> CreateFolder(std::string_view name);
    [[nodiscard]] Result<void> Rename(
        const std::filesystem::path& path,
        std::string_view newName);
    [[nodiscard]] Result<void> Move(
        const std::filesystem::path& path,
        const std::filesystem::path& destinationFolder);
    [[nodiscard]] Result<void> Delete(const std::filesystem::path& path);
    [[nodiscard]] Result<AssetHandle> CreateMaterial(std::string_view name);
    [[nodiscard]] Result<AssetHandle> Duplicate(const std::filesystem::path& path);
    [[nodiscard]] std::future<Result<AssetHandle>> ReimportTexture(const AssetHandle& handle);
    void Activate(const AssetBrowserEntry& entry);
    [[nodiscard]] std::optional<AssetDragPayload> BeginDrag(
        const AssetBrowserEntry& entry) const;
    void DropInScene(const AssetDragPayload& payload);

private:
    [[nodiscard]] Result<void> MoveTo(
        const std::filesystem::path& source,
        const std::filesystem::path& destination);
    [[nodiscard]] std::filesystem::path Resolve(const std::filesystem::path& path) const;
    [[nodiscard]] bool IsInsideAssets(const std::filesystem::path& path) const;
    [[nodiscard]] bool IsInsideBrowserRoots(const std::filesystem::path& path) const;
    void RebuildEntries();
    void Error(std::string message) const;

    IAssetDatabase& m_Database;
    IProjectService* m_Projects{};
    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_AssetsRoot;
    std::filesystem::path m_ScenesRoot;
    std::filesystem::path m_CurrentFolder;
    std::string m_Search;
    std::vector<AssetBrowserEntry> m_Entries;
    AssetBrowserCallbacks m_Callbacks;
};
}
