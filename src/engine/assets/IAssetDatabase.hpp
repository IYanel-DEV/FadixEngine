#pragma once

#include "engine/Result.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <string>

namespace fadix
{
struct AssetMetadata
{
    AssetHandle Handle;
    std::filesystem::path SourcePath;
    std::filesystem::path ImportedPath;
    std::string Type;
};

class IAssetDatabase
{
public:
    virtual ~IAssetDatabase() = default;
    [[nodiscard]] virtual std::future<Result<AssetHandle>> ImportAsync(
        const std::filesystem::path& sourcePath) = 0;
    [[nodiscard]] virtual std::optional<AssetHandle> FindByPath(
        const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual const AssetMetadata* Meta(const AssetHandle& handle) const noexcept = 0;
    virtual void PollImports() = 0;
    [[nodiscard]] virtual std::span<const AssetMetadata> List() const noexcept = 0;
};
}
