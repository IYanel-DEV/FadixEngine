#pragma once

#include "engine/assets/AssetHandle.hpp"
#include "engine/rhi/Types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace fadix
{
enum class TextureFilter : std::uint8_t
{
    Nearest,
    Linear
};

enum class TextureWrap : std::uint8_t
{
    Repeat,
    Clamp,
    Mirror
};

struct TextureImportSettings
{
    bool sRGB{true};
    bool GenerateMipmaps{true};
    TextureFilter Filter{TextureFilter::Linear};
    TextureWrap WrapU{TextureWrap::Repeat};
    TextureWrap WrapV{TextureWrap::Repeat};
    bool NormalMap{false};
    std::uint32_t MaxSize{4096};
};

struct TextureAsset
{
    AssetHandle Handle{InvalidAssetHandle};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
    rhi::Format Format{rhi::Format::Unknown};
    bool sRGB{true};
    std::uint32_t MipCount{1};
    TextureImportSettings ImportSettings;
    std::vector<std::byte> Pixels;
};

// Processed texture file magic header "FTEX"
inline constexpr std::uint32_t ProcessedTextureMagic = 0x58455446;
inline constexpr std::uint32_t ProcessedTextureVersion = 1;

struct ProcessedTextureHeader
{
    std::uint32_t Magic{ProcessedTextureMagic};
    std::uint32_t Version{ProcessedTextureVersion};
    std::uint32_t Width{0};
    std::uint32_t Height{0};
    rhi::Format Format{rhi::Format::Unknown};
    std::uint32_t sRGB{1};
    std::uint32_t MipCount{1};
    std::uint32_t DataSize{0};
    // Version >= 2
    std::uint32_t SettingsFlags{0};
    std::uint32_t MaxSize{4096};
};

} // namespace fadix
