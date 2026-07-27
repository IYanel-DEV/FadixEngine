#include "assets/GltfMeshCache.hpp"

#include "engine/animation/Skeleton.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/rhi/Types.hpp"

#include <glm/common.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <unordered_map>
#include <vector>

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include <tiny_gltf.h>

namespace fadix
{
namespace
{
struct Vertex
{
    glm::vec3 Position;
    glm::vec3 Normal;
    glm::vec4 Tangent;
    glm::vec2 UV;
    glm::vec4 Color;
    glm::vec4 JointIndices;
    glm::vec4 JointWeights;
};
static_assert(sizeof(Vertex) == 96);

struct AccessorView
{
    const unsigned char* Data{nullptr};
    std::size_t Stride{0};
    std::size_t Count{0};
    int ComponentType{0};
    int Type{0};
};

[[nodiscard]] AccessorView ViewAccessor(const tinygltf::Model& model, const int accessorIndex)
{
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size()))
    {
        return {};
    }
    const tinygltf::Accessor& accessor = model.accessors[static_cast<std::size_t>(accessorIndex)];
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
    {
        return {};
    }
    const tinygltf::BufferView& view = model.bufferViews[static_cast<std::size_t>(accessor.bufferView)];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
    {
        return {};
    }
    const tinygltf::Buffer& buffer = model.buffers[static_cast<std::size_t>(view.buffer)];
    const std::size_t offset = view.byteOffset + accessor.byteOffset;
    if (offset > buffer.data.size())
    {
        return {};
    }
    return {buffer.data.data() + offset,
        static_cast<std::size_t>(accessor.ByteStride(view)),
        accessor.count,
        accessor.componentType,
        accessor.type};
}

[[nodiscard]] glm::vec3 ReadVec3(const AccessorView& view, const std::size_t index)
{
    const auto* p = reinterpret_cast<const float*>(view.Data + index * view.Stride);
    return {p[0], p[1], p[2]};
}

[[nodiscard]] glm::vec2 ReadVec2(const AccessorView& view, const std::size_t index)
{
    const auto* p = reinterpret_cast<const float*>(view.Data + index * view.Stride);
    return {p[0], p[1]};
}

[[nodiscard]] glm::vec4 ReadVec4(const AccessorView& view, const std::size_t index)
{
    const auto* p = reinterpret_cast<const float*>(view.Data + index * view.Stride);
    return {p[0], p[1], p[2], p[3]};
}

[[nodiscard]] glm::vec4 ReadJointIndices(const AccessorView& view, const std::size_t index)
{
    const unsigned char* element = view.Data + index * view.Stride;
    glm::vec4 result{0.0F};
    for (int c = 0; c < 4; ++c)
    {
        switch (view.ComponentType)
        {
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            result[c] = static_cast<float>(element[c]);
            break;
        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        {
            std::uint16_t value = 0;
            std::memcpy(&value, element + c * sizeof(std::uint16_t), sizeof(value));
            result[c] = static_cast<float>(value);
            break;
        }
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
        {
            float value = 0.0F;
            std::memcpy(&value, element + c * sizeof(float), sizeof(value));
            result[c] = value;
            break;
        }
        default:
            break;
        }
    }
    return result;
}

[[nodiscard]] glm::mat4 NodeLocalMatrix(const tinygltf::Node& node)
{
    if (node.matrix.size() == 16)
    {
        return glm::make_mat4(node.matrix.data());
    }
    const glm::vec3 translation = node.translation.size() == 3
        ? glm::vec3{
              static_cast<float>(node.translation[0]),
              static_cast<float>(node.translation[1]),
              static_cast<float>(node.translation[2])}
        : glm::vec3{0.0F};
    glm::quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    if (node.rotation.size() == 4)
    {
        rotation = glm::normalize(glm::quat{
            static_cast<float>(node.rotation[3]),
            static_cast<float>(node.rotation[0]),
            static_cast<float>(node.rotation[1]),
            static_cast<float>(node.rotation[2])});
    }
    const glm::vec3 scale = node.scale.size() == 3
        ? glm::vec3{
              static_cast<float>(node.scale[0]),
              static_cast<float>(node.scale[1]),
              static_cast<float>(node.scale[2])}
        : glm::vec3{1.0F};
    return glm::translate(glm::mat4{1.0F}, translation) * glm::mat4_cast(rotation) *
        glm::scale(glm::mat4{1.0F}, scale);
}

void DecomposeRest(const glm::mat4& local, Joint& joint)
{
    joint.RestTranslation = glm::vec3{local[3]};
    joint.RestScale = glm::vec3{
        glm::length(glm::vec3{local[0]}),
        glm::length(glm::vec3{local[1]}),
        glm::length(glm::vec3{local[2]})};
    glm::mat3 rotationMatrix{local};
    if (joint.RestScale.x > 1.0e-6F)
    {
        rotationMatrix[0] /= joint.RestScale.x;
    }
    if (joint.RestScale.y > 1.0e-6F)
    {
        rotationMatrix[1] /= joint.RestScale.y;
    }
    if (joint.RestScale.z > 1.0e-6F)
    {
        rotationMatrix[2] /= joint.RestScale.z;
    }
    joint.RestRotation = glm::normalize(glm::quat_cast(rotationMatrix));
    joint.LocalTransform = local;
}

[[nodiscard]] bool AppendPrimitive(const tinygltf::Model& model,
    const tinygltf::Primitive& primitive,
    const glm::mat4& transform,
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& indices,
    glm::vec3& boxMin,
    glm::vec3& boxMax)
{
    // glTF defaults an omitted primitive.mode to TRIANGLES.
    if (primitive.mode != -1 && primitive.mode != TINYGLTF_MODE_TRIANGLES)
    {
        return false;
    }
    const auto positionIt = primitive.attributes.find("POSITION");
    if (positionIt == primitive.attributes.end())
    {
        return false;
    }
    const AccessorView positions = ViewAccessor(model, positionIt->second);
    if (positions.Data == nullptr || positions.Count == 0)
    {
        return false;
    }

    AccessorView normals;
    if (const auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end())
    {
        normals = ViewAccessor(model, it->second);
    }
    AccessorView uvs;
    if (const auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end())
    {
        uvs = ViewAccessor(model, it->second);
    }
    AccessorView tangents;
    if (const auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end())
    {
        tangents = ViewAccessor(model, it->second);
    }
    AccessorView joints;
    if (const auto it = primitive.attributes.find("JOINTS_0"); it != primitive.attributes.end())
    {
        joints = ViewAccessor(model, it->second);
    }
    AccessorView weights;
    if (const auto it = primitive.attributes.find("WEIGHTS_0"); it != primitive.attributes.end())
    {
        weights = ViewAccessor(model, it->second);
    }

    const auto vertexBase = static_cast<std::uint32_t>(vertices.size());
    const auto indexBase = indices.size();
    const glm::mat3 normalTransform = glm::inverseTranspose(glm::mat3{transform});
    const glm::mat3 directionTransform{transform};
    const float orientation = glm::determinant(directionTransform) < 0.0F ? -1.0F : 1.0F;
    for (std::size_t i = 0; i < positions.Count; ++i)
    {
        Vertex vertex{};
        vertex.Position = glm::vec3{transform * glm::vec4{ReadVec3(positions, i), 1.0F}};
        const glm::vec3 sourceNormal =
            normals.Data != nullptr ? ReadVec3(normals, i) : glm::vec3{0.0F, 1.0F, 0.0F};
        vertex.Normal = glm::normalize(normalTransform * sourceNormal);
        if (tangents.Data != nullptr)
        {
            const glm::vec4 sourceTangent = ReadVec4(tangents, i);
            vertex.Tangent = glm::vec4{
                glm::normalize(directionTransform * glm::vec3{sourceTangent}),
                sourceTangent.w * orientation};
        }
        vertex.UV = uvs.Data != nullptr ? ReadVec2(uvs, i) : glm::vec2{0.0F};
        vertex.Color = glm::vec4{1.0F};
        vertex.JointIndices = joints.Data != nullptr ? ReadJointIndices(joints, i) : glm::vec4{0.0F};
        vertex.JointWeights = weights.Data != nullptr ? ReadVec4(weights, i) : glm::vec4{0.0F};
        vertices.push_back(vertex);

        boxMin = glm::min(boxMin, vertex.Position);
        boxMax = glm::max(boxMax, vertex.Position);
    }

    if (primitive.indices >= 0)
    {
        const tinygltf::Accessor& accessor =
            model.accessors[static_cast<std::size_t>(primitive.indices)];
        const AccessorView indexView = ViewAccessor(model, primitive.indices);
        if (indexView.Data == nullptr)
        {
            return false;
        }
        for (std::size_t i = 0; i < indexView.Count; ++i)
        {
            const unsigned char* element = indexView.Data + i * indexView.Stride;
            std::uint32_t value = 0;
            switch (accessor.componentType)
            {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                value = *element;
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            {
                std::uint16_t v = 0;
                std::memcpy(&v, element, sizeof(v));
                value = v;
                break;
            }
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                std::memcpy(&value, element, sizeof(value));
                break;
            default:
                return false;
            }
            indices.push_back(vertexBase + value);
        }
    }
    else
    {
        for (std::size_t i = 0; i < positions.Count; ++i)
        {
            indices.push_back(vertexBase + static_cast<std::uint32_t>(i));
        }
    }
    if (orientation < 0.0F)
    {
        for (std::size_t i = indexBase; i + 2 < indices.size(); i += 3)
        {
            std::swap(indices[i + 1], indices[i + 2]);
        }
    }
    return true;
}

void AppendNodeGeometry(const tinygltf::Model& model,
    const int nodeIndex,
    const glm::mat4& parentTransform,
    std::vector<Vertex>& vertices,
    std::vector<std::uint32_t>& indices,
    glm::vec3& boxMin,
    glm::vec3& boxMax,
    std::vector<GltfPrimitiveRange>& primitives)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
    {
        return;
    }
    const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
    const glm::mat4 transform = parentTransform * NodeLocalMatrix(node);
    if (node.mesh >= 0 && node.mesh < static_cast<int>(model.meshes.size()))
    {
        const tinygltf::Mesh& mesh = model.meshes[static_cast<std::size_t>(node.mesh)];
        for (const tinygltf::Primitive& primitive : mesh.primitives)
        {
            const auto firstIndex = static_cast<std::uint32_t>(indices.size());
            if (!AppendPrimitive(
                    model, primitive, transform, vertices, indices, boxMin, boxMax))
            {
                continue;
            }
            primitives.push_back({firstIndex,
                static_cast<std::uint32_t>(indices.size()) - firstIndex,
                static_cast<std::int32_t>(primitive.material)});
        }
    }
    for (const int child : node.children)
    {
        AppendNodeGeometry(
            model, child, transform, vertices, indices, boxMin, boxMax, primitives);
    }
}

void LoadSkeleton(const tinygltf::Model& model, GltfMeshAsset& asset)
{
    if (model.skins.empty())
    {
        return;
    }
    const tinygltf::Skin& skin = model.skins.front();
    if (skin.joints.empty())
    {
        return;
    }

    const int jointCount =
        std::min(static_cast<int>(skin.joints.size()), kMaxSkinJoints);
    asset.Skeleton.Joints.resize(static_cast<std::size_t>(jointCount));
    std::unordered_map<int, int> nodeToJoint;
    nodeToJoint.reserve(static_cast<std::size_t>(jointCount));
    for (int i = 0; i < jointCount; ++i)
    {
        const int nodeIndex = skin.joints[static_cast<std::size_t>(i)];
        nodeToJoint[nodeIndex] = i;
        Joint& joint = asset.Skeleton.Joints[static_cast<std::size_t>(i)];
        joint.Parent = -1;
        if (nodeIndex >= 0 && nodeIndex < static_cast<int>(model.nodes.size()))
        {
            const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
            joint.Name = node.name;
            DecomposeRest(NodeLocalMatrix(node), joint);
        }
    }

    if (skin.inverseBindMatrices >= 0)
    {
        const AccessorView ibm = ViewAccessor(model, skin.inverseBindMatrices);
        if (ibm.Data != nullptr)
        {
            for (int i = 0; i < jointCount && static_cast<std::size_t>(i) < ibm.Count; ++i)
            {
                const auto* m = reinterpret_cast<const float*>(ibm.Data + static_cast<std::size_t>(i) * ibm.Stride);
                asset.Skeleton.Joints[static_cast<std::size_t>(i)].InverseBindMatrix = glm::make_mat4(m);
            }
        }
    }

    for (int i = 0; i < jointCount; ++i)
    {
        const int nodeIndex = skin.joints[static_cast<std::size_t>(i)];
        if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model.nodes.size()))
        {
            continue;
        }
        const tinygltf::Node& node = model.nodes[static_cast<std::size_t>(nodeIndex)];
        for (const int child : node.children)
        {
            const auto it = nodeToJoint.find(child);
            if (it != nodeToJoint.end())
            {
                asset.Skeleton.Joints[static_cast<std::size_t>(it->second)].Parent = i;
            }
        }
    }

    asset.Skeleton.ResetToRestPose();
    asset.Skeleton.ComputeGlobalTransforms();
    asset.HasSkeleton = true;
}

void LoadAnimations(
    const tinygltf::Model& model,
    const std::unordered_map<int, int>& nodeToJoint,
    GltfMeshAsset& asset)
{
    for (const tinygltf::Animation& animation : model.animations)
    {
        AnimationClipAsset clip;
        clip.Name = animation.name.empty()
            ? ("Clip" + std::to_string(asset.Animations.size()))
            : animation.name;

        for (const tinygltf::AnimationChannel& channel : animation.channels)
        {
            if (channel.sampler < 0 ||
                channel.sampler >= static_cast<int>(animation.samplers.size()))
            {
                continue;
            }
            const auto jointIt = nodeToJoint.find(channel.target_node);
            if (jointIt == nodeToJoint.end())
            {
                continue;
            }

            AnimationChannel out;
            out.JointIndex = jointIt->second;
            if (channel.target_path == "translation")
            {
                out.Target = AnimationChannel::Property::Translation;
            }
            else if (channel.target_path == "rotation")
            {
                out.Target = AnimationChannel::Property::Rotation;
            }
            else if (channel.target_path == "scale")
            {
                out.Target = AnimationChannel::Property::Scale;
            }
            else
            {
                continue;
            }

            const tinygltf::AnimationSampler& sampler =
                animation.samplers[static_cast<std::size_t>(channel.sampler)];
            const AccessorView times = ViewAccessor(model, sampler.input);
            const AccessorView values = ViewAccessor(model, sampler.output);
            if (times.Data == nullptr || values.Data == nullptr || times.Count == 0)
            {
                continue;
            }

            out.Keyframes.resize(times.Count);
            for (std::size_t i = 0; i < times.Count; ++i)
            {
                float time = 0.0F;
                std::memcpy(&time, times.Data + i * times.Stride, sizeof(time));
                out.Keyframes[i].Time = time;
                clip.Duration = std::max(clip.Duration, time);

                if (out.Target == AnimationChannel::Property::Rotation)
                {
                    const glm::vec4 q = ReadVec4(values, i);
                    out.Keyframes[i].Value = q;
                }
                else
                {
                    const glm::vec3 v = ReadVec3(values, i);
                    out.Keyframes[i].Value = glm::vec4{v, 0.0F};
                }
            }
            clip.Channels.push_back(std::move(out));
        }

        if (!clip.Channels.empty())
        {
            asset.Animations.push_back(std::move(clip));
        }
    }
}
}

GltfMeshCache::GltfMeshCache(rhi::Device& device) : m_Device(device) {}

const GltfMeshAsset* GltfMeshCache::Get(const AssetHandle& handle) const
{
    const auto it = m_Meshes.find(handle);
    return it != m_Meshes.end() ? it->second.get() : nullptr;
}

std::optional<CollisionMeshView> GltfMeshCache::CollisionMesh(const AssetHandle& handle)
{
    const GltfMeshAsset* mesh = Get(handle);
    if (mesh == nullptr && m_Database != nullptr)
    {
        if (const AssetMetadata* metadata = m_Database->Meta(handle))
        {
            const std::filesystem::path& path = !metadata->SourcePath.empty()
                ? metadata->SourcePath
                : metadata->ImportedPath;
            mesh = Load(handle, path.string());
        }
    }
    if (mesh == nullptr || mesh->CollisionVertices.empty() || mesh->CollisionIndices.empty())
    {
        return std::nullopt;
    }
    return CollisionMeshView{
        mesh->CollisionVertices, mesh->CollisionIndices, mesh->BoundingBoxMin, mesh->BoundingBoxMax};
}

void GltfMeshCache::Clear()
{
    m_Meshes.clear();
}

const GltfMeshAsset* GltfMeshCache::Load(const AssetHandle& handle,
    const std::string& gltfPath,
    std::function<void(const std::string&)> log)
{
    if (const GltfMeshAsset* existing = Get(handle))
    {
        return existing;
    }
    const auto report = [&](const std::string& message) {
        if (log)
        {
            log(message);
        }
    };

    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string error;
    std::string warning;

    std::string extension = std::filesystem::path{gltfPath}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const bool isBinary = extension == ".glb";
    const bool loaded = isBinary
        ? loader.LoadBinaryFromFile(&model, &error, &warning, gltfPath)
        : loader.LoadASCIIFromFile(&model, &error, &warning, gltfPath);
    if (!warning.empty())
    {
        report("glTF warning (" + gltfPath + "): " + warning);
    }
    if (!loaded || !error.empty())
    {
        report("glTF load failed (" + gltfPath + "): " + error);
        return nullptr;
    }

    auto asset = std::make_unique<GltfMeshAsset>();
    asset->Id = handle;
    asset->DebugName = gltfPath;

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    glm::vec3 boxMin{std::numeric_limits<float>::max()};
    glm::vec3 boxMax{std::numeric_limits<float>::lowest()};

    // Static glTF node transforms are part of the authored mesh layout. Bake
    // them into the uploaded vertices; otherwise multi-part models collapse at
    // the origin. Skinned meshes keep their existing bind-space path.
    if (model.skins.empty() && !model.scenes.empty())
    {
        const int sceneIndex = model.defaultScene >= 0 &&
                model.defaultScene < static_cast<int>(model.scenes.size())
            ? model.defaultScene
            : 0;
        for (const int root : model.scenes[static_cast<std::size_t>(sceneIndex)].nodes)
        {
            AppendNodeGeometry(model,
                root,
                glm::mat4{1.0F},
                vertices,
                indices,
                boxMin,
                boxMax,
                asset->Primitives);
        }
    }
    else
    {
        for (const tinygltf::Mesh& mesh : model.meshes)
        {
            for (const tinygltf::Primitive& primitive : mesh.primitives)
            {
                const auto firstIndex = static_cast<std::uint32_t>(indices.size());
                if (!AppendPrimitive(model,
                        primitive,
                        glm::mat4{1.0F},
                        vertices,
                        indices,
                        boxMin,
                        boxMax))
                {
                    continue;
                }
                asset->Primitives.push_back({firstIndex,
                    static_cast<std::uint32_t>(indices.size()) - firstIndex,
                    static_cast<std::int32_t>(primitive.material)});
            }
        }
    }

    if (vertices.empty() || indices.empty())
    {
        report("glTF has no drawable triangle geometry: " + gltfPath);
        return nullptr;
    }

    asset->BoundingBoxMin = boxMin;
    asset->BoundingBoxMax = boxMax;
    asset->CollisionVertices.reserve(vertices.size());
    for (const Vertex& vertex : vertices)
    {
        asset->CollisionVertices.push_back(vertex.Position);
    }
    asset->CollisionIndices = indices;

    LoadSkeleton(model, *asset);
    if (asset->HasSkeleton)
    {
        std::unordered_map<int, int> nodeToJoint;
        const tinygltf::Skin& skin = model.skins.front();
        const int jointCount =
            std::min(static_cast<int>(skin.joints.size()), kMaxSkinJoints);
        for (int i = 0; i < jointCount; ++i)
        {
            nodeToJoint[skin.joints[static_cast<std::size_t>(i)]] = i;
        }
        LoadAnimations(model, nodeToJoint, *asset);
    }

    auto vertexResult = m_Device.CreateBuffer(
        {vertices.size() * sizeof(Vertex), rhi::BufferUsage::Vertex, "GltfVertices"});
    auto indexResult = m_Device.CreateBuffer(
        {indices.size() * sizeof(std::uint32_t), rhi::BufferUsage::Index, "GltfIndices"});
    if (!vertexResult || !indexResult)
    {
        report("glTF buffer creation failed: " +
            (!vertexResult ? vertexResult.ErrorMessage() : indexResult.ErrorMessage()));
        return nullptr;
    }
    asset->VertexBuffer = std::move(vertexResult).Value();
    asset->IndexBuffer = std::move(indexResult).Value();
    asset->VertexBuffer->Upload(std::as_bytes(std::span{vertices}));
    asset->IndexBuffer->Upload(std::as_bytes(std::span{indices}));

    const GltfMeshAsset* stored = asset.get();
    m_Meshes.emplace(handle, std::move(asset));
    return stored;
}
}
