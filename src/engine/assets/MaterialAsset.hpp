#pragma once

#include "engine/assets/AssetHandle.hpp"
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <string>

namespace fadix
{
enum class AlphaMode : std::uint8_t
{
    Opaque,
    Mask,
    Blend
};

struct MaterialAsset
{
    AssetHandle Handle{InvalidAssetHandle};
    std::string Name{"Material"};

    glm::vec4 BaseColor{1.0F, 1.0F, 1.0F, 1.0F};
    AssetHandle BaseColorTexture{InvalidAssetHandle};

    AssetHandle NormalTexture{InvalidAssetHandle};

    float Metallic{0.0F};
    float Roughness{0.7F};
    AssetHandle MetallicRoughnessTexture{InvalidAssetHandle};

    glm::vec3 EmissiveColor{0.0F, 0.0F, 0.0F};
    float EmissiveIntensity{0.0F};
    AssetHandle EmissiveTexture{InvalidAssetHandle};

    // Ambient occlusion. Occlusion lives in the red channel; a combined glTF ORM
    // map (occlusion=R, roughness=G, metallic=B) can be assigned to both this and
    // MetallicRoughnessTexture. Occlusion only attenuates indirect/ambient (IBL),
    // never direct light. Linear data texture (never sRGB).
    AssetHandle OcclusionTexture{InvalidAssetHandle};
    float OcclusionStrength{1.0F};
    // Tangent-space normal XY scale (glTF normalScale). 1 = as authored.
    float NormalScale{1.0F};

    AlphaMode AlphaMode{fadix::AlphaMode::Opaque};
    float AlphaCutoff{0.5F};
    bool DoubleSided{false};

    glm::vec2 UVScale{1.0F, 1.0F};
    glm::vec2 UVOffset{0.0F, 0.0F};
};
} // namespace fadix
