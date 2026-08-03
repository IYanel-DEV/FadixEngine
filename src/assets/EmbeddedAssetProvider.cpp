#include "assets/EmbeddedAssetProvider.hpp"

#include "engine/Version.hpp"
#include "generated/EmbeddedAssets.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifndef FADIX_ASSET_ROOT
#define FADIX_ASSET_ROOT "assets"
#endif

namespace fadix
{
namespace
{
[[nodiscard]] bool IsSafeRelativePath(const std::filesystem::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_path())
    {
        return false;
    }
    for (const auto& part : path)
    {
        if (part == "..")
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::filesystem::path CacheRoot()
{
#ifdef _WIN32
    char* local = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&local, &length, "LOCALAPPDATA") == 0 && local != nullptr && local[0] != '\0')
    {
        const std::filesystem::path root = std::filesystem::path{local} / "FadixEngine" / "Cache" /
            std::string{EngineVersion} / "assets";
        std::free(local);
        return root;
    }
    std::free(local);
#endif
    std::error_code error;
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
    if (error)
    {
        throw std::runtime_error("Could not locate a writable Fadix asset cache");
    }
    return temporary / "FadixEngine" / std::string{EngineVersion} / "assets";
}

[[nodiscard]] bool FileMatches(
    const std::filesystem::path& path, const std::span<const std::byte> bytes)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) != bytes.size() || error)
    {
        return false;
    }
    std::ifstream stream{path, std::ios::binary};
    std::vector<std::byte> existing(bytes.size());
    return stream.read(reinterpret_cast<char*>(existing.data()),
               static_cast<std::streamsize>(existing.size())) &&
        std::equal(existing.begin(), existing.end(), bytes.begin(), bytes.end());
}

[[nodiscard]] std::filesystem::path MaterializeAssets()
{
    // Embedded does not mean imaginary: shader and template loaders still need real files.
    const std::filesystem::path root = CacheRoot();
    for (const embedded::Asset& asset : embedded::Assets)
    {
        const std::filesystem::path relative{asset.Path};
        if (!IsSafeRelativePath(relative))
        {
            throw std::runtime_error("Invalid embedded asset path: " + std::string{asset.Path});
        }

        const std::filesystem::path destination = root / relative;
        if (FileMatches(destination, asset.Bytes))
        {
            continue;
        }

        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error)
        {
            throw std::runtime_error(
                "Could not create the Fadix asset cache: " + error.message());
        }
        std::ofstream stream{destination, std::ios::binary | std::ios::trunc};
        if (!stream.write(reinterpret_cast<const char*>(asset.Bytes.data()),
                static_cast<std::streamsize>(asset.Bytes.size())))
        {
            throw std::runtime_error("Could not write embedded asset: " + destination.string());
        }
    }
    return root;
}
}

std::optional<EmbeddedAssetView> FindEmbeddedAsset(const std::string_view path) noexcept
{
    const embedded::Asset* asset = embedded::Find(path);
    if (asset == nullptr)
    {
        return std::nullopt;
    }
    return EmbeddedAssetView{asset->Path, asset->Bytes};
}

const std::filesystem::path& RuntimeAssetRoot()
{
    static const std::filesystem::path root = [] {
#ifndef FADIX_PORTABLE_BUILD
        const std::filesystem::path sourceRoot{FADIX_ASSET_ROOT};
        std::error_code error;
        if (std::filesystem::is_directory(sourceRoot, error) && !error)
        {
            return sourceRoot;
        }
#endif
        return MaterializeAssets();
    }();
    return root;
}
}
