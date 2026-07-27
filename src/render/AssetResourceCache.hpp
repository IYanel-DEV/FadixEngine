#pragma once

#include "engine/assets/AssetHandle.hpp"
#include "engine/assets/MaterialAsset.hpp"
#include "engine/assets/TextureAsset.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/rhi/Texture.hpp"
#include "engine/rhi/Sampler.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fadix
{
class IAssetDatabase;

class AssetResourceCache
{
public:
    AssetResourceCache(rhi::Device& device, IAssetDatabase& database);
    ~AssetResourceCache() = default;

    AssetResourceCache(const AssetResourceCache&) = delete;
    AssetResourceCache& operator=(const AssetResourceCache&) = delete;

    void Clear();
    void SetDatabase(IAssetDatabase& database);
    void Update();

    rhi::Texture* GetTexture(const AssetHandle& handle, bool isNormalMap = false);
    rhi::Sampler* GetSampler(const TextureImportSettings& settings);
    [[nodiscard]] TextureImportSettings GetTextureSettings(const AssetHandle& handle);
    MaterialAsset GetMaterial(const AssetHandle& handle);

    rhi::Texture* GetWhiteTexture() const { return m_WhiteTexture.get(); }
    rhi::Texture* GetNormalFallbackTexture() const { return m_NormalFallbackTexture.get(); }
    rhi::Texture* GetBlackTexture() const { return m_BlackTexture.get(); }
    rhi::Texture* GetMetallicRoughnessFallbackTexture() const
    {
        return m_MetallicRoughnessFallbackTexture.get();
    }

private:
    void LoadTexture(const AssetHandle& handle);
    void LoadMaterial(const AssetHandle& handle);
    void LoadTextureLocked(const AssetHandle& handle);
    void LoadMaterialLocked(const AssetHandle& handle);
    void CreateFallbacks();
    void WarnOnce(const AssetHandle& handle, const std::string& message);

    rhi::Device& m_Device;
    IAssetDatabase* m_Database;

    std::mutex m_Mutex;

    struct CachedTexture
    {
        std::unique_ptr<rhi::Texture> GpuTexture;
        TextureImportSettings Settings;
        std::filesystem::file_time_type LastModifiedTime;
    };

    struct CachedMaterial
    {
        MaterialAsset Asset;
        std::filesystem::file_time_type LastModifiedTime;
    };

    std::unordered_map<AssetHandle, CachedTexture> m_Textures;
    std::unordered_map<AssetHandle, CachedMaterial> m_Materials;
    std::unordered_map<uint32_t, std::unique_ptr<rhi::Sampler>> m_Samplers;
    std::unordered_set<AssetHandle> m_WarnedHandles;

    std::unique_ptr<rhi::Texture> m_WhiteTexture;
    std::unique_ptr<rhi::Texture> m_NormalFallbackTexture;
    std::unique_ptr<rhi::Texture> m_BlackTexture;
    std::unique_ptr<rhi::Texture> m_MetallicRoughnessFallbackTexture;

    std::chrono::steady_clock::time_point m_LastUpdateCheck{};
};
} // namespace fadix
