#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

namespace fadix
{
struct EmbeddedAssetView
{
    std::string_view Path;
    std::span<const std::byte> Bytes;
};

[[nodiscard]] std::optional<EmbeddedAssetView> FindEmbeddedAsset(
    std::string_view path) noexcept;

// Returns a real directory containing the assets required by the editor. When
// the source asset folder is unavailable (as in the one-file release), the
// embedded files are materialized into the user's local cache.
[[nodiscard]] const std::filesystem::path& RuntimeAssetRoot();
}
