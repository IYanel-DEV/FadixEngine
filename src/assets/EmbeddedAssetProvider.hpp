#pragma once

#include <cstddef>
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
}
