#include "assets/EmbeddedAssetProvider.hpp"

#include "generated/EmbeddedAssets.hpp"

namespace fadix
{
std::optional<EmbeddedAssetView> FindEmbeddedAsset(const std::string_view path) noexcept
{
    const embedded::Asset* asset = embedded::Find(path);
    if (asset == nullptr)
    {
        return std::nullopt;
    }
    return EmbeddedAssetView{asset->Path, asset->Bytes};
}
}
