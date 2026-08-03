#pragma once

#include "engine/animation/AnimGraphNodeRegistry.hpp"
#include "engine/animation/AnimationGraph.hpp"

#include <glm/vec2.hpp>

#include <istream>
#include <iomanip>
#include <ostream>
#include <string>

namespace fadix
{
// Format (line-oriented, same style as AnimatorControllerIO):
//
// graph <Name>
// params <N>
//   param <Name> <Type(0-3)> <FloatValue> <BoolValue(0/1)> <IntValue>
// nodes <N>
//   node <TypeName> <editorX> <editorY>
//     <node-specific key-value lines>
//   endnode
// output <OutputNodeIndex>

inline void WriteAnimationGraph(std::ostream& out, const AnimationGraph& graph)
{
    out << "graph " << std::quoted(graph.Name) << '\n';
    out << "params " << graph.Parameters.size() << '\n';
    for (const AnimGraphParameter& p : graph.Parameters)
    {
        out << "param " << std::quoted(p.Name) << ' '
            << static_cast<int>(p.ParamType) << ' '
            << p.FloatValue << ' '
            << (p.BoolValue ? 1 : 0) << ' '
            << p.IntValue << '\n';
    }
    out << "nodes " << graph.Nodes.size() << '\n';
    for (const auto& node : graph.Nodes)
    {
        out << "node " << node->TypeName() << ' '
            << node->EditorPosition.x << ' ' << node->EditorPosition.y << '\n';
        node->Serialize(out);
        out << "endnode\n";
    }
    out << "output " << graph.OutputNodeIndex << '\n';
}

[[nodiscard]] inline bool ReadAnimationGraph(std::istream& in, AnimationGraph& graph)
{
    std::string token;
    if (!(in >> token) || token != "graph") return false;
    if (!(in >> std::quoted(graph.Name))) return false;

    std::size_t paramCount = 0;
    if (!(in >> token) || token != "params") return false;
    in >> paramCount;
    graph.Parameters.resize(paramCount);
    for (AnimGraphParameter& p : graph.Parameters)
    {
        int typeInt = 0, boolInt = 0;
        in >> token >> std::quoted(p.Name) >> typeInt >> p.FloatValue >> boolInt >> p.IntValue;
        p.ParamType = static_cast<AnimGraphParameter::Type>(typeInt);
        p.BoolValue = boolInt != 0;
    }

    std::size_t nodeCount = 0;
    if (!(in >> token) || token != "nodes") return false;
    in >> nodeCount;
    graph.Nodes.resize(nodeCount);
    for (auto& nodePtr : graph.Nodes)
    {
        std::string typeName;
        float ex = 0.0F, ey = 0.0F;
        if (!(in >> token) || token != "node") return false;
        in >> typeName >> ex >> ey;
        nodePtr = AnimGraphNodeRegistry::Instance().Create(typeName);
        if (!nodePtr) return false;
        nodePtr->EditorPosition = {ex, ey};
        nodePtr->Deserialize(in);
        if (!in || !(in >> token) || token != "endnode") return false;
    }
    if (!(in >> token) || token != "output") return false;
    in >> graph.OutputNodeIndex;
    return true;
}
}  // namespace fadix
