#pragma once

#include "engine/assets/IAssetDatabase.hpp"
#include "engine/assets/MaterialAsset.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
enum class AssetImportState
{
    Queued,
    Importing,
    Completed,
    Failed
};

struct AssetImportStatus
{
    AssetHandle Handle;
    std::filesystem::path SourcePath;
    AssetImportState State{AssetImportState::Queued};
    float Progress{0.0F};
    std::string Error;
};

class AssetDatabase final : public IAssetDatabase
{
public:
    explicit AssetDatabase(std::filesystem::path projectRoot);
    ~AssetDatabase() override;

    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;

    [[nodiscard]] std::future<Result<AssetHandle>> ImportAsync(
        const std::filesystem::path& sourcePath) override;
    [[nodiscard]] std::optional<AssetHandle> FindByPath(
        const std::filesystem::path& path) const override;
    [[nodiscard]] const AssetMetadata* Meta(const AssetHandle& handle) const noexcept override;
    void PollImports() override;
    [[nodiscard]] std::span<const AssetMetadata> List() const noexcept override;

    [[nodiscard]] Result<void> Refresh();
    [[nodiscard]] std::span<const AssetImportStatus> ImportStatuses() const noexcept;
    [[nodiscard]] const std::filesystem::path& ProjectRoot() const noexcept;
    [[nodiscard]] const std::filesystem::path& AssetsRoot() const noexcept;
    [[nodiscard]] Result<std::filesystem::path> ImportExternalModel(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationFolder,
        bool refreshDatabase = true);

    [[nodiscard]] Result<AssetHandle> CreateMaterial(
        std::string_view name,
        const std::filesystem::path& folder = {});
    [[nodiscard]] Result<AssetHandle> DuplicateAsset(const std::filesystem::path& sourcePath);
    [[nodiscard]] std::future<Result<AssetHandle>> ReimportTexture(const AssetHandle& handle);
    void PollHotReimports();
    [[nodiscard]] Result<MaterialAsset> LoadMaterial(const AssetHandle& handle) const;
    [[nodiscard]] Result<void> SaveMaterial(const MaterialAsset& asset);
    [[nodiscard]] std::optional<AssetHandle> FindByHandle(const AssetHandle& handle) const;

private:
    struct ImportJob;

    [[nodiscard]] std::filesystem::path ResolveSource(const std::filesystem::path& path) const;
    [[nodiscard]] Result<AssetMetadata> ReadOrCreateMetadata(
        const std::filesystem::path& sourcePath);
    void Upsert(AssetMetadata metadata);

    std::filesystem::path m_ProjectRoot;
    std::filesystem::path m_AssetsRoot;
    std::filesystem::path m_ImportRoot;
    std::vector<AssetMetadata> m_Assets;
    std::vector<std::unique_ptr<ImportJob>> m_Jobs;
    std::vector<AssetImportStatus> m_Statuses;
    std::unordered_map<AssetHandle, std::filesystem::file_time_type> m_SourceTimestamps;
    std::mutex m_StateMutex;
    std::chrono::steady_clock::time_point m_NextHotReimportCheck{};
};

[[nodiscard]] std::unique_ptr<IAssetDatabase> CreateAssetDatabase(
    const std::filesystem::path& projectRoot);
}
