#include "editor/assets/AssetThumbnailCache.hpp"

#include "assets/MaterialAssetJson.hpp"
#include "assets/TextureProcessor.hpp"
#include "engine/assets/IAssetDatabase.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

namespace fadix
{
namespace
{
constexpr std::uint32_t kThumbSize = 32;

void WriteErrorTga(const std::filesystem::path& path)
{
    // Magenta/black checker so failures are obvious.
    std::vector<std::byte> pixels(kThumbSize * kThumbSize * 4);
    for (std::uint32_t y = 0; y < kThumbSize; ++y)
    {
        for (std::uint32_t x = 0; x < kThumbSize; ++x)
        {
            const bool dark = ((x / 4) + (y / 4)) % 2 == 0;
            const std::size_t i = (static_cast<std::size_t>(y) * kThumbSize + x) * 4;
            pixels[i + 0] = dark ? std::byte{40} : std::byte{200};
            pixels[i + 1] = dark ? std::byte{0} : std::byte{40};
            pixels[i + 2] = dark ? std::byte{40} : std::byte{200};
            pixels[i + 3] = std::byte{255};
        }
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return;
    }
    unsigned char header[18]{};
    header[2] = 2; // uncompressed true-color
    header[12] = static_cast<unsigned char>(kThumbSize & 0xFF);
    header[13] = static_cast<unsigned char>((kThumbSize >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(kThumbSize & 0xFF);
    header[15] = static_cast<unsigned char>((kThumbSize >> 8) & 0xFF);
    header[16] = 32;
    header[17] = 0x28; // top-left origin, 8 alpha bits
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    // TGA BGRA
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        const char bgra[4] = {
            static_cast<char>(pixels[i + 2]),
            static_cast<char>(pixels[i + 1]),
            static_cast<char>(pixels[i + 0]),
            static_cast<char>(pixels[i + 3])};
        out.write(bgra, 4);
    }
}
} // namespace

AssetThumbnailCache::AssetThumbnailCache(IAssetDatabase& database)
    : m_Database(database)
{
}

void AssetThumbnailCache::SetProjectRoot(std::filesystem::path projectRoot)
{
    m_ThumbRoot = std::move(projectRoot) / ".fadix" / "thumbnails";
    Clear();
}

void AssetThumbnailCache::Clear()
{
    m_TexturePaths.clear();
    m_MaterialSwatches.clear();
    m_Errors.clear();
}

std::filesystem::path AssetThumbnailCache::ThumbPath(const AssetHandle& handle) const
{
    return m_ThumbRoot / (handle.ToString() + ".tga");
}

bool AssetThumbnailCache::WriteTgaRgba(
    const std::filesystem::path& path,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::byte* rgba,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const std::uint32_t sourceStride) const
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return false;
    }
    unsigned char header[18]{};
    header[2] = 2;
    header[12] = static_cast<unsigned char>(width & 0xFF);
    header[13] = static_cast<unsigned char>((width >> 8) & 0xFF);
    header[14] = static_cast<unsigned char>(height & 0xFF);
    header[15] = static_cast<unsigned char>((height >> 8) & 0xFF);
    header[16] = 32;
    header[17] = 0x28;
    out.write(reinterpret_cast<const char*>(header), sizeof(header));

    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint32_t srcY =
            sourceHeight <= 1 ? 0 : (y * (sourceHeight - 1)) / (height - 1);
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint32_t srcX =
                sourceWidth <= 1 ? 0 : (x * (sourceWidth - 1)) / (width - 1);
            const std::size_t index =
                static_cast<std::size_t>(srcY) * sourceStride + static_cast<std::size_t>(srcX) * 4U;
            const char bgra[4] = {
                static_cast<char>(rgba[index + 2]),
                static_cast<char>(rgba[index + 1]),
                static_cast<char>(rgba[index + 0]),
                static_cast<char>(rgba[index + 3])};
            out.write(bgra, 4);
        }
    }
    return static_cast<bool>(out);
}

bool AssetThumbnailCache::EnsureTextureThumb(const AssetHandle& handle)
{
    if (const auto it = m_TexturePaths.find(handle); it != m_TexturePaths.end())
    {
        return !m_Errors[handle];
    }

    const AssetMetadata* meta = m_Database.Meta(handle);
    if (meta == nullptr || meta->Type != "Texture")
    {
        m_Errors[handle] = true;
        return false;
    }

    const std::filesystem::path path = ThumbPath(handle);
    std::error_code error;
    const auto importedTime = std::filesystem::last_write_time(meta->ImportedPath, error);
    if (!error && std::filesystem::exists(path, error))
    {
        error.clear();
        const auto thumbTime = std::filesystem::last_write_time(path, error);
        if (!error && thumbTime >= importedTime)
        {
            m_TexturePaths[handle] = path;
            m_Errors[handle] = false;
            return true;
        }
    }

    Result<ProcessedTextureData> processed = ReadProcessedTexture(meta->ImportedPath);
    if (!processed || processed.Value().Pixels.empty())
    {
        std::filesystem::create_directories(m_ThumbRoot, error);
        WriteErrorTga(path);
        m_TexturePaths[handle] = path;
        m_Errors[handle] = true;
        return false;
    }

    const ProcessedTextureData& data = processed.Value();
    if (!WriteTgaRgba(
            path,
            kThumbSize,
            kThumbSize,
            data.Pixels.data(),
            data.Header.Width,
            data.Header.Height,
            data.Header.Width * 4U))
    {
        WriteErrorTga(path);
        m_TexturePaths[handle] = path;
        m_Errors[handle] = true;
        return false;
    }

    m_TexturePaths[handle] = path;
    m_Errors[handle] = false;
    return true;
}

std::optional<std::filesystem::path> AssetThumbnailCache::TextureThumbnailPath(
    const AssetHandle& handle)
{
    if (!handle.IsValid())
    {
        return std::nullopt;
    }
    static_cast<void>(EnsureTextureThumb(handle));
    const auto it = m_TexturePaths.find(handle);
    if (it == m_TexturePaths.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> AssetThumbnailCache::MaterialSwatchRgb(const AssetHandle& handle)
{
    if (!handle.IsValid())
    {
        return std::nullopt;
    }
    if (const auto it = m_MaterialSwatches.find(handle); it != m_MaterialSwatches.end())
    {
        return it->second;
    }

    const AssetMetadata* meta = m_Database.Meta(handle);
    if (meta == nullptr || meta->Type != "Material")
    {
        m_Errors[handle] = true;
        return std::nullopt;
    }

    Result<MaterialAsset> material = LoadMaterialAsset(meta->SourcePath, handle);
    if (!material)
    {
        m_Errors[handle] = true;
        return std::nullopt;
    }

    const glm::vec4 color = material.Value().BaseColor;
    const int r = static_cast<int>(std::clamp(color.x, 0.0F, 1.0F) * 255.0F);
    const int g = static_cast<int>(std::clamp(color.y, 0.0F, 1.0F) * 255.0F);
    const int b = static_cast<int>(std::clamp(color.z, 0.0F, 1.0F) * 255.0F);
    std::string rgb = std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b);
    m_MaterialSwatches[handle] = rgb;
    m_Errors[handle] = false;
    return rgb;
}

bool AssetThumbnailCache::IsError(const AssetHandle& handle) const
{
    const auto it = m_Errors.find(handle);
    return it != m_Errors.end() && it->second;
}

} // namespace fadix
