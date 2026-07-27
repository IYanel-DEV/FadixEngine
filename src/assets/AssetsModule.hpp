#pragma once

#include <filesystem>
#include <memory>

namespace fadix
{
class AssetBrowserController;
class IAssetDatabase;
class IProjectService;

struct AssetsModule
{
    std::unique_ptr<IAssetDatabase> Database;
    std::unique_ptr<AssetBrowserController> Browser;

    AssetsModule();
    ~AssetsModule();
    AssetsModule(AssetsModule&&) noexcept;
    AssetsModule& operator=(AssetsModule&&) noexcept;
    AssetsModule(const AssetsModule&) = delete;
    AssetsModule& operator=(const AssetsModule&) = delete;
};

[[nodiscard]] AssetsModule RegisterAssetsModule(const std::filesystem::path& projectRoot);
[[nodiscard]] AssetsModule RegisterAssetsModule(IProjectService& projects);
}
