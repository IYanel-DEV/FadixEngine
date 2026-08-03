#pragma once

#include "engine/animation/AnimationGraph.hpp"

#include <glm/vec2.hpp>

namespace fadix
{
struct AnimatorComponent;
class SceneEditor;
}

namespace fadix::editor
{
class AnimGraphPanel final
{
public:
    void Draw(SceneEditor& scene, AnimationGraph& graph, AnimatorComponent& animator);

private:
    AnimationGraph* m_ActiveGraph{nullptr};
    int m_SelectedNode{-1};
    int m_SelectedParameter{-1};
    int m_NewBoneIndex{0};
    glm::vec2 m_Pan{40.0F, 40.0F};
    glm::vec2 m_AddPosition{};
};
}
