#include "engine/animation/AnimationGraphIO.hpp"

#include <sstream>
#include <utility>

namespace fadix
{
std::unique_ptr<AnimationGraph> CloneAnimationGraph(const AnimationGraph& source)
{
    AnimGraphNodeRegistry::RegisterBuiltins();
    std::ostringstream graphText;
    WriteAnimationGraph(graphText, source);
    auto graph = std::make_unique<AnimationGraph>();
    std::istringstream graphInput{graphText.str()};
    return ReadAnimationGraph(graphInput, *graph) ? std::move(graph) : nullptr;
}
}
