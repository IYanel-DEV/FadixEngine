#pragma once

#include "engine/Result.hpp"
#include "engine/assets/TextureAsset.hpp"

#include <filesystem>
#include <span>
#include <vector>

struct SDL_Surface;

namespace fadix
{
struct ProcessedTextureData
{
    ProcessedTextureHeader Header;
    TextureImportSettings Settings;
    std::vector<std::byte> Pixels;
};

[[nodiscard]] Result<ProcessedTextureData> ProcessTextureSurface(
    SDL_Surface* surface,
    const TextureImportSettings& settings);

// Decodes a Radiance .hdr equirectangular image to a linear R16G16B16A16Float
// processed texture (sRGB flag cleared). Reuses the stb_image float decoder that
// ships with the tinygltf dependency; returns an error in builds compiled
// without FADIX_HAVE_STB_HDR.
[[nodiscard]] Result<ProcessedTextureData> ProcessHdrFile(
    const std::filesystem::path& path,
    const TextureImportSettings& settings);

[[nodiscard]] Result<void> WriteProcessedTexture(
    const std::filesystem::path& path,
    const ProcessedTextureData& data);

[[nodiscard]] Result<ProcessedTextureData> ReadProcessedTexture(
    const std::filesystem::path& path);

[[nodiscard]] bool ValidateProcessedHeader(
    const ProcessedTextureHeader& header,
    std::size_t fileSize) noexcept;

[[nodiscard]] TextureImportSettings DefaultTextureImportSettings() noexcept;

[[nodiscard]] TextureImportSettings ReadTextureImportSettings(
    const std::filesystem::path& metaPath);

[[nodiscard]] Result<void> WriteTextureImportSettings(
    const std::filesystem::path& metaPath,
    const TextureImportSettings& settings);

} // namespace fadix
