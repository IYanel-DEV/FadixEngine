#pragma once

#include "engine/assets/AssetHandle.hpp"
#include "engine/assets/MaterialAsset.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace fadix
{
class IAssetDatabase;

// Lightweight thumbnail cache for the Asset Browser.
// Textures: downscaled TGA written under <project>/.fadix/thumbnails/
// Materials: CSS color swatch (no GPU sphere).
class AssetThumbnailCache
{
public:
    explicit AssetThumbnailCache(IAssetDatabase& database);

    void SetProjectRoot(std::filesystem::path projectRoot);
    void Clear();

    // Absolute path to a cached TGA, or empty if unavailable / error placeholder needed.
    [[nodiscard]] std::optional<std::filesystem::path> TextureThumbnailPath(
        const AssetHandle& handle);

    // Returns "r,g,b" 0-255 for a material swatch, or nullopt if material missing.
    [[nodiscard]] std::optional<std::string> MaterialSwatchRgb(const AssetHandle& handle);

    [[nodiscard]] bool IsError(const AssetHandle& handle) const;

private:
    [[nodiscard]] std::filesystem::path ThumbPath(const AssetHandle& handle) const;
    [[nodiscard]] bool EnsureTextureThumb(const AssetHandle& handle);
    [[nodiscard]] bool WriteTgaRgba(
        const std::filesystem::path& path,
        std::uint32_t width,
        std::uint32_t height,
        const std::byte* rgba,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t sourceStride) const;

    IAssetDatabase& m_Database;
    std::filesystem::path m_ThumbRoot;
    std::unordered_map<AssetHandle, std::filesystem::path> m_TexturePaths;
    std::unordered_map<AssetHandle, std::string> m_MaterialSwatches;
    std::unordered_map<AssetHandle, bool> m_Errors;
};
} // namespace fadix
