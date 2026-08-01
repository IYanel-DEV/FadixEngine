#pragma once

#include "engine/animation/Skeleton.hpp"

#include <glm/vec2.hpp>

#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fadix
{
struct GltfMeshAsset;  // forward-decl; full type in AnimationRuntime.hpp
struct AnimationGraph; // forward-decl for context

struct AnimGraphParameter
{
    enum class Type : std::uint8_t { Float, Bool, Int, Trigger };
    std::string Name;
    Type ParamType{Type::Float};
    float FloatValue{0.0F};
    bool BoolValue{false};
    int IntValue{0};
};

struct AnimGraphContext
{
    float DeltaTime{0.0F};
    const GltfMeshAsset* MeshAsset{nullptr};
    std::span<AnimGraphParameter> Parameters;
    std::unordered_map<std::string, SkeletonPose> SavedPoses;
    AnimationGraph* Graph{nullptr};  // back-ptr so child-index lookups work

    [[nodiscard]] float GetFloat(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name && p.ParamType == AnimGraphParameter::Type::Float)
                return p.FloatValue;
        return 0.0F;
    }
    [[nodiscard]] bool GetBool(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name &&
                (p.ParamType == AnimGraphParameter::Type::Bool ||
                 p.ParamType == AnimGraphParameter::Type::Trigger))
                return p.BoolValue;
        return false;
    }
    [[nodiscard]] int GetInt(std::string_view name) const noexcept
    {
        for (const AnimGraphParameter& p : Parameters)
            if (p.Name == name && p.ParamType == AnimGraphParameter::Type::Int)
                return p.IntValue;
        return 0;
    }
};

struct AnimGraphNode
{
    glm::vec2 EditorPosition{};

    virtual ~AnimGraphNode() = default;
    virtual SkeletonPose Evaluate(AnimGraphContext& ctx) = 0;
    [[nodiscard]] virtual std::string_view TypeName() const = 0;
    virtual void Serialize(std::ostream& out) const = 0;
    virtual void Deserialize(std::istream& in) = 0;
};

struct AnimationGraph
{
    std::string Name;
    std::vector<std::unique_ptr<AnimGraphNode>> Nodes;
    std::vector<AnimGraphParameter> Parameters;
    int OutputNodeIndex{-1};

    [[nodiscard]] SkeletonPose Evaluate(AnimGraphContext& ctx)
    {
        if (OutputNodeIndex < 0 ||
            OutputNodeIndex >= static_cast<int>(Nodes.size()) ||
            !Nodes[static_cast<std::size_t>(OutputNodeIndex)])
        {
            return {};
        }
        ctx.Graph = this;
        return Nodes[static_cast<std::size_t>(OutputNodeIndex)]->Evaluate(ctx);
    }

    // Clear trigger parameters after each Evaluate() call.
    void ConsumeTriggers()
    {
        for (AnimGraphParameter& p : Parameters)
            if (p.ParamType == AnimGraphParameter::Type::Trigger)
                p.BoolValue = false;
    }
};
}  // namespace fadix
