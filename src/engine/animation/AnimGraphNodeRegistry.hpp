#pragma once

#include "engine/animation/AnimGraphNodes.hpp"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace fadix
{
class AnimGraphNodeRegistry
{
public:
    using Factory = std::function<std::unique_ptr<AnimGraphNode>()>;

    static AnimGraphNodeRegistry& Instance()
    {
        static AnimGraphNodeRegistry s;
        return s;
    }

    template <typename T>
    void Register()
    {
        T proto;
        m_Factories[std::string{proto.TypeName()}] = []{ return std::make_unique<T>(); };
    }

    [[nodiscard]] std::unique_ptr<AnimGraphNode> Create(const std::string& typeName) const
    {
        auto it = m_Factories.find(typeName);
        return it != m_Factories.end() ? it->second() : nullptr;
    }

    // Call once at startup — registers all built-in node types.
    static void RegisterBuiltins()
    {
        auto& reg = Instance();
        reg.Register<ClipNode>();
        reg.Register<BlendByFloatNode>();
        reg.Register<BlendByConditionNode>();
        reg.Register<LayeredBlendNode>();
        reg.Register<AdditiveNode>();
        reg.Register<SavedPoseNode>();
        reg.Register<UseSavedPoseNode>();
        reg.Register<StateMachineNode>();
        reg.Register<OutputNode>();
    }

private:
    std::unordered_map<std::string, Factory> m_Factories;
};
}  // namespace fadix
