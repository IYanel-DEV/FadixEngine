#include "AssetResourceCache.hpp"

#include "assets/MaterialAssetJson.hpp"
#include "assets/TextureProcessor.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/rhi/Buffer.hpp"
#include "engine/rhi/CommandList.hpp"

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace fadix
{
namespace
{
uint32_t HashTextureImportSettings(const TextureImportSettings& settings)
{
    uint32_t hash = 0;
    hash |= (static_cast<uint32_t>(settings.Filter) & 0x1) << 0;
    hash |= (static_cast<uint32_t>(settings.WrapU) & 0x3) << 1;
    hash |= (static_cast<uint32_t>(settings.WrapV) & 0x3) << 3;
    hash |= settings.sRGB ? (1U << 5) : 0U;
    hash |= settings.GenerateMipmaps ? (1U << 6) : 0U;
    hash |= settings.NormalMap ? (1U << 7) : 0U;
    return hash;
}
} // namespace

AssetResourceCache::AssetResourceCache(rhi::Device& device, IAssetDatabase& database)
    : m_Device(device), m_Database(&database)
{
    CreateFallbacks();
}

void AssetResourceCache::Clear()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Textures.clear();
    m_Materials.clear();
    m_WarnedHandles.clear();
}

void AssetResourceCache::SetDatabase(IAssetDatabase& database)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Database = &database;
    m_Textures.clear();
    m_Materials.clear();
    m_WarnedHandles.clear();
}

void AssetResourceCache::Update()
{
    if (m_Database == nullptr)
    {
        return;
    }

    // Hot-reload polling only — never decode/import on the render path.
    // Throttle disk stats; a full List()+stat every frame tanks viewport FPS.
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    constexpr auto kMinInterval = std::chrono::milliseconds{500};
    if (m_LastUpdateCheck.time_since_epoch().count() != 0 && now - m_LastUpdateCheck < kMinInterval)
    {
        return;
    }
    m_LastUpdateCheck = now;

    std::lock_guard<std::mutex> lock(m_Mutex);

    // Only touch assets already resident in the GPU cache.
    std::vector<AssetHandle> dirtyTextures;
    dirtyTextures.reserve(m_Textures.size());
    for (auto& [handle, cached] : m_Textures)
    {
        const AssetMetadata* meta = m_Database->Meta(handle);
        if (meta == nullptr || meta->Type != "Texture")
        {
            continue;
        }
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(meta->ImportedPath, ec);
        if (!ec && time > cached.LastModifiedTime)
        {
            dirtyTextures.push_back(handle);
        }
    }
    for (const AssetHandle& handle : dirtyTextures)
    {
        LoadTextureLocked(handle);
    }

    std::vector<AssetHandle> dirtyMaterials;
    dirtyMaterials.reserve(m_Materials.size());
    for (auto& [handle, cached] : m_Materials)
    {
        const AssetMetadata* meta = m_Database->Meta(handle);
        if (meta == nullptr || meta->Type != "Material")
        {
            continue;
        }
        std::error_code ec;
        const auto time = std::filesystem::last_write_time(meta->ImportedPath, ec);
        if (!ec && time > cached.LastModifiedTime)
        {
            dirtyMaterials.push_back(handle);
        }
    }
    for (const AssetHandle& handle : dirtyMaterials)
    {
        LoadMaterialLocked(handle);
    }
}

rhi::Texture* AssetResourceCache::GetTexture(const AssetHandle& handle, const bool isNormalMap)
{
    if (handle == InvalidAssetHandle)
    {
        return isNormalMap ? m_NormalFallbackTexture.get() : m_WhiteTexture.get();
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(handle);
    if (it != m_Textures.end())
    {
        return it->second.GpuTexture ? it->second.GpuTexture.get()
                                    : (isNormalMap ? m_NormalFallbackTexture.get()
                                                   : m_WhiteTexture.get());
    }

    LoadTextureLocked(handle);
    it = m_Textures.find(handle);
    if (it != m_Textures.end() && it->second.GpuTexture)
    {
        return it->second.GpuTexture.get();
    }

    WarnOnce(handle, "Texture asset could not be loaded; using fallback");
    return isNormalMap ? m_NormalFallbackTexture.get() : m_WhiteTexture.get();
}

rhi::Sampler* AssetResourceCache::GetSampler(const TextureImportSettings& settings)
{
    const uint32_t hash = HashTextureImportSettings(settings);
    auto it = m_Samplers.find(hash);
    if (it != m_Samplers.end())
    {
        return it->second.get();
    }

    rhi::SamplerDesc desc;
    desc.MinFilter =
        settings.Filter == TextureFilter::Linear ? rhi::FilterMode::Linear : rhi::FilterMode::Nearest;
    desc.MagFilter = desc.MinFilter;
    desc.MipFilter = desc.MinFilter;
    desc.AddressU = settings.WrapU == TextureWrap::Clamp ? rhi::WrapMode::Clamp
        : settings.WrapU == TextureWrap::Mirror                     ? rhi::WrapMode::Mirror
                                                                    : rhi::WrapMode::Repeat;
    desc.AddressV = settings.WrapV == TextureWrap::Clamp ? rhi::WrapMode::Clamp
        : settings.WrapV == TextureWrap::Mirror                     ? rhi::WrapMode::Mirror
                                                                    : rhi::WrapMode::Repeat;
    desc.AddressW = desc.AddressU;

    auto samplerResult = m_Device.CreateSampler(desc);
    if (samplerResult)
    {
        rhi::Sampler* ptr = samplerResult.Value().get();
        m_Samplers[hash] = std::move(samplerResult).Value();
        return ptr;
    }
    return nullptr;
}

TextureImportSettings AssetResourceCache::GetTextureSettings(const AssetHandle& handle)
{
    if (handle == InvalidAssetHandle)
    {
        return TextureImportSettings{};
    }
    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Textures.find(handle);
    if (it == m_Textures.end())
    {
        LoadTextureLocked(handle);
        it = m_Textures.find(handle);
    }
    if (it != m_Textures.end())
    {
        return it->second.Settings;
    }
    return TextureImportSettings{};
}

MaterialAsset AssetResourceCache::GetMaterial(const AssetHandle& handle)
{
    if (handle == InvalidAssetHandle)
    {
        return MaterialAsset{};
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    auto it = m_Materials.find(handle);
    if (it != m_Materials.end())
    {
        return it->second.Asset;
    }

    LoadMaterialLocked(handle);
    it = m_Materials.find(handle);
    if (it != m_Materials.end())
    {
        return it->second.Asset;
    }

    WarnOnce(handle, "Material asset could not be loaded; using defaults");
    return MaterialAsset{};
}

void AssetResourceCache::LoadTexture(const AssetHandle& handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    LoadTextureLocked(handle);
}

void AssetResourceCache::LoadTextureLocked(const AssetHandle& handle)
{
    if (m_Database == nullptr)
    {
        return;
    }
    const AssetMetadata* meta = m_Database->Meta(handle);
    if (!meta || meta->Type != "Texture")
    {
        return;
    }

    std::error_code ec;
    const auto lastModified = std::filesystem::last_write_time(meta->ImportedPath, ec);
    if (ec)
    {
        return;
    }

    Result<ProcessedTextureData> processed = ReadProcessedTexture(meta->ImportedPath);
    if (!processed)
    {
        return;
    }

    const ProcessedTextureData& data = processed.Value();
    rhi::TextureDesc desc;
    desc.Extent.Width = data.Header.Width;
    desc.Extent.Height = data.Header.Height;
    // Color data (base color, emissive) is authored in sRGB and must be decoded
    // to linear on sample; the importer clears the sRGB flag for normal and
    // data textures (metallic/roughness) so those stay linear UNORM. All
    // lighting then happens in linear space with exactly one decode.
    desc.PixelFormat = (data.Header.Format == rhi::Format::R8G8B8A8Unorm && data.Header.sRGB != 0U)
        ? rhi::Format::R8G8B8A8UnormSrgb
        : data.Header.Format;
    desc.Usage = rhi::TextureUsage::Sampled;
    desc.MipLevels = data.Header.MipCount;

    auto textureResult = m_Device.CreateTexture(desc, data.Pixels);
    if (textureResult)
    {
        CachedTexture cached;
        cached.GpuTexture = std::move(textureResult).Value();
        cached.Settings = data.Settings;
        cached.LastModifiedTime = lastModified;
        m_Textures[handle] = std::move(cached);
    }
}

void AssetResourceCache::LoadMaterial(const AssetHandle& handle)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    LoadMaterialLocked(handle);
}

void AssetResourceCache::LoadMaterialLocked(const AssetHandle& handle)
{
    if (m_Database == nullptr)
    {
        return;
    }
    const AssetMetadata* meta = m_Database->Meta(handle);
    if (!meta || meta->Type != "Material")
    {
        return;
    }

    std::error_code ec;
    const auto lastModified = std::filesystem::last_write_time(meta->ImportedPath, ec);
    if (ec)
    {
        return;
    }

    Result<MaterialAsset> material = LoadMaterialAsset(meta->SourcePath, handle);
    if (!material)
    {
        return;
    }

    CachedMaterial cached;
    cached.LastModifiedTime = lastModified;
    cached.Asset = std::move(material).Value();
    m_Materials[handle] = std::move(cached);
}

void AssetResourceCache::CreateFallbacks()
{
    rhi::TextureDesc desc;
    desc.Extent.Width = 1;
    desc.Extent.Height = 1;
    desc.PixelFormat = rhi::Format::R8G8B8A8Unorm;
    desc.Usage = rhi::TextureUsage::Sampled;
    desc.MipLevels = 1;

    const std::vector<std::byte> white{
        std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    m_WhiteTexture = m_Device.CreateTexture(desc, white).Value();

    const std::vector<std::byte> black{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{255}};
    m_BlackTexture = m_Device.CreateTexture(desc, black).Value();

    const std::vector<std::byte> normal{
        std::byte{128}, std::byte{128}, std::byte{255}, std::byte{255}};
    m_NormalFallbackTexture = m_Device.CreateTexture(desc, normal).Value();

    // glTF metallic-roughness defaults: roughness=1 (green), metallic=0 (blue)
    const std::vector<std::byte> metallicRoughness{
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    m_MetallicRoughnessFallbackTexture = m_Device.CreateTexture(desc, metallicRoughness).Value();
}

void AssetResourceCache::WarnOnce(const AssetHandle& handle, const std::string& message)
{
    // A missing asset is noisy enough without reporting it again every frame.
    if (m_WarnedHandles.insert(handle).second)
    {
        std::cerr << "[Fadix] " << message << " (" << handle.ToString() << ")\n";
    }
}

} // namespace fadix
