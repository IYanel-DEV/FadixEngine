#pragma once

#include "engine/Result.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/assets/MaterialAsset.hpp"

#include <filesystem>
#include <string>

namespace fadix
{
inline constexpr std::uint32_t MaterialFileVersion = 1;

[[nodiscard]] MaterialAsset DefaultMaterialAsset(
    AssetHandle handle,
    std::string name = "Material");

[[nodiscard]] Result<MaterialAsset> LoadMaterialAsset(
    const std::filesystem::path& path,
    AssetHandle handle);

[[nodiscard]] Result<void> SaveMaterialAsset(
    const std::filesystem::path& path,
    const MaterialAsset& asset);

} // namespace fadix
