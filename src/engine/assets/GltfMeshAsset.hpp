#pragma once

#include "engine/animation/AnimationClip.hpp"
#include "engine/animation/Skeleton.hpp"
#include "engine/assets/AssetHandle.hpp"
#include "engine/rhi/Buffer.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fadix
{
struct GltfPrimitiveRange
{
    std::uint32_t FirstIndex{0};
    std::uint32_t IndexCount{0};
    std::int32_t MaterialIndex{-1};
};

struct GltfMeshAsset
{
    AssetHandle Id{};
    std::string DebugName;

    // GPU buffers — vertex layout matches ViewportRenderer Vertex:
    // Position Normal Tangent UV Color JointIndices JointWeights = 96 bytes
    std::unique_ptr<rhi::Buffer> VertexBuffer;
    std::unique_ptr<rhi::Buffer> IndexBuffer;

    std::vector<GltfPrimitiveRange> Primitives;

    glm::vec3 BoundingBoxMin{0.0F};
    glm::vec3 BoundingBoxMax{0.0F};

    // CPU geometry is retained for fitted Jolt mesh collision and editor
    // collision wireframes. Positions already include static glTF node transforms.
    std::vector<glm::vec3> CollisionVertices;
    std::vector<std::uint32_t> CollisionIndices;

    SkeletonAsset Skeleton{};
    bool HasSkeleton{false};
    std::vector<AnimationClipAsset> Animations;

    [[nodiscard]] bool IsValid() const noexcept
    {
        return VertexBuffer != nullptr && IndexBuffer != nullptr;
    }
};
}
