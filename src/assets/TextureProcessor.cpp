#include "assets/TextureProcessor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <SDL3/SDL_surface.h>

#ifdef FADIX_HAVE_STB_HDR
// Declarations only: the stb_image implementation is already compiled into the
// tinygltf library we link against, so defining it here would duplicate symbols.
#include <stb_image.h>
#endif

namespace fadix
{
namespace
{
// IEEE-754 float32 -> float16 (round-to-nearest-even not required for HDR store;
// truncation of the low mantissa bits is acceptable and keeps this dependency-free).
[[nodiscard]] std::uint16_t FloatToHalf(const float value) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    std::int32_t exponent = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent <= 0)
    {
        return static_cast<std::uint16_t>(sign); // underflow -> signed zero
    }
    if (exponent >= 0x1F)
    {
        return static_cast<std::uint16_t>(sign | 0x7C00U); // overflow -> inf
    }
    return static_cast<std::uint16_t>(
        sign | (static_cast<std::uint32_t>(exponent) << 10U) | (mantissa >> 13U));
}

std::uint32_t PackSettingsFlags(const TextureImportSettings& settings)
{
    std::uint32_t flags = 0;
    flags |= settings.sRGB ? 1U : 0U;
    flags |= settings.GenerateMipmaps ? 2U : 0U;
    flags |= settings.NormalMap ? 4U : 0U;
    flags |= (static_cast<std::uint32_t>(settings.Filter) & 1U) << 3;
    flags |= (static_cast<std::uint32_t>(settings.WrapU) & 3U) << 4;
    flags |= (static_cast<std::uint32_t>(settings.WrapV) & 3U) << 6;
    return flags;
}

TextureImportSettings UnpackSettingsFlags(const std::uint32_t flags, const std::uint32_t maxSize)
{
    TextureImportSettings settings;
    settings.sRGB = (flags & 1U) != 0U;
    settings.GenerateMipmaps = (flags & 2U) != 0U;
    settings.NormalMap = (flags & 4U) != 0U;
    settings.Filter = (flags & (1U << 3)) != 0U ? TextureFilter::Linear : TextureFilter::Nearest;
    settings.WrapU = static_cast<TextureWrap>((flags >> 4) & 3U);
    settings.WrapV = static_cast<TextureWrap>((flags >> 6) & 3U);
    settings.MaxSize = maxSize == 0U ? 4096U : maxSize;
    return settings;
}

void DownscaleHalf(
    const std::span<const std::byte> source,
    const std::uint32_t width,
    const std::uint32_t height,
    std::vector<std::byte>& destination)
{
    const std::uint32_t newWidth = std::max(width / 2U, 1U);
    const std::uint32_t newHeight = std::max(height / 2U, 1U);
    destination.resize(static_cast<std::size_t>(newWidth) * static_cast<std::size_t>(newHeight) * 4U);
    for (std::uint32_t y = 0; y < newHeight; ++y)
    {
        for (std::uint32_t x = 0; x < newWidth; ++x)
        {
            const std::uint32_t sourceX = x * 2U;
            const std::uint32_t sourceY = y * 2U;
            std::array<float, 4> accum{0.0F, 0.0F, 0.0F, 0.0F};
            int count = 0;
            for (std::uint32_t offsetY = 0; offsetY < 2U && sourceY + offsetY < height; ++offsetY)
            {
                for (std::uint32_t offsetX = 0; offsetX < 2U && sourceX + offsetX < width; ++offsetX)
                {
                    const std::size_t index =
                        (static_cast<std::size_t>(sourceY + offsetY) * width + sourceX + offsetX) * 4U;
                    for (int channel = 0; channel < 4; ++channel)
                    {
                        accum[static_cast<std::size_t>(channel)] +=
                            static_cast<float>(std::to_integer<unsigned char>(source[index + channel]));
                    }
                    ++count;
                }
            }
            const std::size_t destinationIndex =
                (static_cast<std::size_t>(y) * newWidth + x) * 4U;
            for (int channel = 0; channel < 4; ++channel)
            {
                const float value = accum[static_cast<std::size_t>(channel)] / static_cast<float>(count);
                destination[destinationIndex + static_cast<std::size_t>(channel)] =
                    static_cast<std::byte>(std::clamp(static_cast<int>(value + 0.5F), 0, 255));
            }
        }
    }
}

SDL_Surface* ResizeSurface(SDL_Surface* source, const int targetWidth, const int targetHeight)
{
    SDL_Surface* converted = SDL_ConvertSurface(source, SDL_PIXELFORMAT_RGBA32);
    if (converted == nullptr)
    {
        return nullptr;
    }
    SDL_Surface* resized = SDL_CreateSurface(targetWidth, targetHeight, SDL_PIXELFORMAT_RGBA32);
    if (resized == nullptr)
    {
        SDL_DestroySurface(converted);
        return nullptr;
    }
    if (!SDL_BlitSurfaceScaled(converted, nullptr, resized, nullptr, SDL_SCALEMODE_LINEAR))
    {
        SDL_DestroySurface(converted);
        SDL_DestroySurface(resized);
        return nullptr;
    }
    SDL_DestroySurface(converted);
    return resized;
}
} // namespace

TextureImportSettings DefaultTextureImportSettings() noexcept
{
    return TextureImportSettings{};
}

TextureImportSettings ReadTextureImportSettings(const std::filesystem::path& metaPath)
{
    TextureImportSettings settings = DefaultTextureImportSettings();
    std::ifstream stream{metaPath};
    std::string line;
    while (std::getline(stream, line))
    {
        constexpr std::string_view srgbPrefix{"srgb="};
        constexpr std::string_view mipsPrefix{"mipmaps="};
        constexpr std::string_view filterPrefix{"filter="};
        constexpr std::string_view wrapUPrefix{"wrapu="};
        constexpr std::string_view wrapVPrefix{"wrapv="};
        constexpr std::string_view normalPrefix{"normalmap="};
        constexpr std::string_view maxSizePrefix{"maxsize="};
        if (line.starts_with(srgbPrefix))
        {
            settings.sRGB = line.substr(srgbPrefix.size()) != "0";
        }
        else if (line.starts_with(mipsPrefix))
        {
            settings.GenerateMipmaps = line.substr(mipsPrefix.size()) != "0";
        }
        else if (line.starts_with(filterPrefix))
        {
            settings.Filter = line.substr(filterPrefix.size()) == "nearest"
                ? TextureFilter::Nearest
                : TextureFilter::Linear;
        }
        else if (line.starts_with(wrapUPrefix))
        {
            const std::string value = line.substr(wrapUPrefix.size());
            settings.WrapU = value == "clamp" ? TextureWrap::Clamp
                : value == "mirror"                ? TextureWrap::Mirror
                                                   : TextureWrap::Repeat;
        }
        else if (line.starts_with(wrapVPrefix))
        {
            const std::string value = line.substr(wrapVPrefix.size());
            settings.WrapV = value == "clamp" ? TextureWrap::Clamp
                : value == "mirror"                ? TextureWrap::Mirror
                                                   : TextureWrap::Repeat;
        }
        else if (line.starts_with(normalPrefix))
        {
            settings.NormalMap = line.substr(normalPrefix.size()) != "0";
        }
        else if (line.starts_with(maxSizePrefix))
        {
            settings.MaxSize = static_cast<std::uint32_t>(
                std::stoul(line.substr(maxSizePrefix.size())));
        }
    }
    return settings;
}

Result<void> WriteTextureImportSettings(
    const std::filesystem::path& metaPath,
    const TextureImportSettings& settings)
{
    std::ifstream existing{metaPath};
    std::ostringstream preserved;
    preserved << existing.rdbuf();
    std::string content = preserved.str();

    const auto upsert = [&content](const std::string& key, const std::string& value) {
        const std::string prefix = key + '=';
        std::istringstream input{content};
        std::ostringstream output;
        bool replaced = false;
        std::string line;
        while (std::getline(input, line))
        {
            if (line.starts_with(prefix))
            {
                output << prefix << value << '\n';
                replaced = true;
            }
            else if (!line.empty())
            {
                output << line << '\n';
            }
        }
        if (!replaced)
        {
            output << prefix << value << '\n';
        }
        content = output.str();
    };

    upsert("srgb", settings.sRGB ? "1" : "0");
    upsert("mipmaps", settings.GenerateMipmaps ? "1" : "0");
    upsert("filter", settings.Filter == TextureFilter::Nearest ? "nearest" : "linear");
    upsert("wrapu", settings.WrapU == TextureWrap::Clamp ? "clamp"
        : settings.WrapU == TextureWrap::Mirror           ? "mirror"
                                                          : "repeat");
    upsert("wrapv", settings.WrapV == TextureWrap::Clamp ? "clamp"
        : settings.WrapV == TextureWrap::Mirror           ? "mirror"
                                                          : "repeat");
    upsert("normalmap", settings.NormalMap ? "1" : "0");
    upsert("maxsize", std::to_string(settings.MaxSize));

    std::ofstream stream{metaPath, std::ios::trunc};
    if (!stream)
    {
        return Result<void>::Error("Could not write texture import settings");
    }
    stream << content;
    return Result<void>::Ok();
}

bool ValidateProcessedHeader(const ProcessedTextureHeader& header, const std::size_t fileSize) noexcept
{
    if (header.Magic != ProcessedTextureMagic)
    {
        return false;
    }
    if (header.Version != ProcessedTextureVersion && header.Version != 2U)
    {
        return false;
    }
    if (header.Width == 0U || header.Height == 0U || header.MipCount == 0U)
    {
        return false;
    }
    if (header.Width > 16384U || header.Height > 16384U)
    {
        return false;
    }
    const std::size_t headerSize =
        header.Version >= 2U ? sizeof(ProcessedTextureHeader) : sizeof(ProcessedTextureHeader) - 8U;
    if (fileSize < headerSize + header.DataSize)
    {
        return false;
    }
    const std::size_t expectedMinimum =
        static_cast<std::size_t>(header.Width) * static_cast<std::size_t>(header.Height) * 4U;
    if (header.DataSize < expectedMinimum)
    {
        return false;
    }
    return true;
}

Result<ProcessedTextureData> ProcessTextureSurface(
    SDL_Surface* surface,
    const TextureImportSettings& settings)
{
    if (surface == nullptr)
    {
        return Result<ProcessedTextureData>::Error("Texture surface is null");
    }

    TextureImportSettings effective = settings;
    if (effective.NormalMap)
    {
        effective.sRGB = false;
    }

    SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
    if (rgba == nullptr)
    {
        return Result<ProcessedTextureData>::Error("Failed to convert texture to RGBA32");
    }

    int width = rgba->w;
    int height = rgba->h;
    if (effective.MaxSize > 0U)
    {
        const int maxDimension = static_cast<int>(effective.MaxSize);
        if (width > maxDimension || height > maxDimension)
        {
            const float scale = static_cast<float>(maxDimension) /
                static_cast<float>(std::max(width, height));
            const int targetWidth = std::max(1, static_cast<int>(std::lround(width * scale)));
            const int targetHeight = std::max(1, static_cast<int>(std::lround(height * scale)));
            SDL_Surface* resized = ResizeSurface(rgba, targetWidth, targetHeight);
            SDL_DestroySurface(rgba);
            if (resized == nullptr)
            {
                return Result<ProcessedTextureData>::Error("Failed to resize texture");
            }
            rgba = resized;
            width = rgba->w;
            height = rgba->h;
        }
    }

    ProcessedTextureData result;
    result.Settings = effective;
    result.Header.Magic = ProcessedTextureMagic;
    result.Header.Version = 2U;
    result.Header.Width = static_cast<std::uint32_t>(width);
    result.Header.Height = static_cast<std::uint32_t>(height);
    result.Header.Format = rhi::Format::R8G8B8A8Unorm;
    result.Header.sRGB = effective.sRGB ? 1U : 0U;
    result.Header.SettingsFlags = PackSettingsFlags(effective);
    result.Header.MaxSize = effective.MaxSize;

    const std::size_t baseSize =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    result.Pixels.resize(baseSize);
    std::memcpy(result.Pixels.data(), rgba->pixels, baseSize);

    if (effective.GenerateMipmaps)
    {
        std::vector<std::byte> currentLevel = result.Pixels;
        std::uint32_t levelWidth = result.Header.Width;
        std::uint32_t levelHeight = result.Header.Height;
        result.Header.MipCount = 1U;
        while (levelWidth > 1U || levelHeight > 1U)
        {
            std::vector<std::byte> nextLevel;
            DownscaleHalf(currentLevel, levelWidth, levelHeight, nextLevel);
            result.Pixels.insert(result.Pixels.end(), nextLevel.begin(), nextLevel.end());
            currentLevel = std::move(nextLevel);
            levelWidth = std::max(levelWidth / 2U, 1U);
            levelHeight = std::max(levelHeight / 2U, 1U);
            ++result.Header.MipCount;
        }
    }
    else
    {
        result.Header.MipCount = 1U;
    }

    result.Header.DataSize = static_cast<std::uint32_t>(result.Pixels.size());
    SDL_DestroySurface(rgba);
    return Result<ProcessedTextureData>::Ok(std::move(result));
}

Result<ProcessedTextureData> ProcessHdrFile(
    const std::filesystem::path& path,
    const TextureImportSettings& settings)
{
#ifdef FADIX_HAVE_STB_HDR
    int width = 0;
    int height = 0;
    int channels = 0;
    float* pixels = stbi_loadf(path.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr)
    {
        return Result<ProcessedTextureData>::Error(
            std::string{"Failed to decode HDR image: "} + stbi_failure_reason());
    }
    if (width <= 0 || height <= 0)
    {
        stbi_image_free(pixels);
        return Result<ProcessedTextureData>::Error("HDR image has invalid dimensions");
    }

    // Optional decimation so an enormous environment can't blow past MaxSize.
    int stride = 1;
    const int maxDim = settings.MaxSize > 0U ? static_cast<int>(settings.MaxSize) : 4096;
    while ((width / stride) > maxDim || (height / stride) > maxDim)
    {
        stride *= 2;
    }
    const int outWidth = std::max(1, width / stride);
    const int outHeight = std::max(1, height / stride);

    ProcessedTextureData result;
    TextureImportSettings effective = settings;
    effective.sRGB = false; // HDR is scene-linear radiance
    effective.NormalMap = false;
    result.Settings = effective;
    result.Header.Magic = ProcessedTextureMagic;
    result.Header.Version = 2U;
    result.Header.Width = static_cast<std::uint32_t>(outWidth);
    result.Header.Height = static_cast<std::uint32_t>(outHeight);
    result.Header.Format = rhi::Format::R16G16B16A16Float;
    result.Header.sRGB = 0U;
    result.Header.MipCount = 1U;
    result.Header.SettingsFlags = PackSettingsFlags(effective);
    result.Header.MaxSize = effective.MaxSize;

    // 4 half-float channels per texel.
    result.Pixels.resize(static_cast<std::size_t>(outWidth) * outHeight * 4U * sizeof(std::uint16_t));
    auto* out = reinterpret_cast<std::uint16_t*>(result.Pixels.data());
    for (int y = 0; y < outHeight; ++y)
    {
        for (int x = 0; x < outWidth; ++x)
        {
            const float* src = pixels + (static_cast<std::size_t>(y * stride) * width + x * stride) * 4U;
            const std::size_t dst = (static_cast<std::size_t>(y) * outWidth + x) * 4U;
            out[dst + 0] = FloatToHalf(src[0]);
            out[dst + 1] = FloatToHalf(src[1]);
            out[dst + 2] = FloatToHalf(src[2]);
            out[dst + 3] = FloatToHalf(1.0F);
        }
    }
    stbi_image_free(pixels);
    result.Header.DataSize = static_cast<std::uint32_t>(result.Pixels.size());
    return Result<ProcessedTextureData>::Ok(std::move(result));
#else
    static_cast<void>(path);
    static_cast<void>(settings);
    return Result<ProcessedTextureData>::Error(
        "HDR import requires an stb-enabled build (FADIX_HAVE_STB_HDR)");
#endif
}

Result<void> WriteProcessedTexture(
    const std::filesystem::path& path,
    const ProcessedTextureData& data)
{
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return Result<void>::Error("Could not open processed texture for writing");
    }
    out.write(reinterpret_cast<const char*>(&data.Header), sizeof(data.Header));
    out.write(reinterpret_cast<const char*>(data.Pixels.data()), data.Pixels.size());
    if (!out)
    {
        return Result<void>::Error("Could not finish writing processed texture");
    }
    return Result<void>::Ok();
}

Result<ProcessedTextureData> ReadProcessedTexture(const std::filesystem::path& path)
{
    std::error_code error;
    const auto fileSize = std::filesystem::file_size(path, error);
    if (error)
    {
        return Result<ProcessedTextureData>::Error("Could not stat processed texture");
    }

    std::ifstream in{path, std::ios::binary};
    if (!in)
    {
        return Result<ProcessedTextureData>::Error("Could not open processed texture");
    }

    ProcessedTextureHeader header{};
    constexpr std::size_t baseHeaderSize = sizeof(ProcessedTextureHeader) - 8U;
    if (!in.read(reinterpret_cast<char*>(&header), static_cast<std::streamsize>(baseHeaderSize)))
    {
        return Result<ProcessedTextureData>::Error("Processed texture header is truncated");
    }
    if (header.Version >= 2U)
    {
        if (!in.read(
                reinterpret_cast<char*>(&header.SettingsFlags),
                static_cast<std::streamsize>(sizeof(ProcessedTextureHeader) - baseHeaderSize)))
        {
            return Result<ProcessedTextureData>::Error("Processed texture header is truncated");
        }
    }
    if (!ValidateProcessedHeader(header, fileSize))
    {
        return Result<ProcessedTextureData>::Error("Processed texture header is invalid");
    }

    ProcessedTextureData result;
    result.Header = header;
    result.Settings = header.Version >= 2U
        ? UnpackSettingsFlags(header.SettingsFlags, header.MaxSize)
        : DefaultTextureImportSettings();
    result.Pixels.resize(header.DataSize);
    if (!in.read(reinterpret_cast<char*>(result.Pixels.data()), header.DataSize))
    {
        return Result<ProcessedTextureData>::Error("Processed texture payload is truncated");
    }
    return Result<ProcessedTextureData>::Ok(std::move(result));
}

} // namespace fadix
