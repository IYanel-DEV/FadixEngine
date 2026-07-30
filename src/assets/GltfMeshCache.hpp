#pragma once

#include "engine/assets/GltfMeshAsset.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/physics/IPhysicsWorld.hpp"
#include "engine/rhi/Device.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fadix
{
class GltfMeshCache final : public ICollisionMeshProvider
{
public:
    explicit GltfMeshCache(rhi::Device& device);

    // Load a .gltf/.glb file, parse geometry, upload to GPU. Returns the cached
    // asset (existing one if already loaded) or nullptr on failure.
    [[nodiscard]] const GltfMeshAsset* Load(const AssetHandle& handle,
        const std::string& gltfPath,
        std::function<void(const std::string&)> log = nullptr);

    [[nodiscard]] const GltfMeshAsset* Get(const AssetHandle& handle) const;
    // Mutable access for in-editor clip authoring (FDX Animation adds/edits clips).
    [[nodiscard]] GltfMeshAsset* GetMutable(const AssetHandle& handle);
    void SetAssetDatabase(class IAssetDatabase* database) noexcept { m_Database = database; }
    [[nodiscard]] std::optional<CollisionMeshView> CollisionMesh(
        const AssetHandle& handle) override;

    void Clear();

private:
    rhi::Device& m_Device;
    IAssetDatabase* m_Database{};
    std::unordered_map<AssetHandle, std::unique_ptr<GltfMeshAsset>> m_Meshes;
};
}
