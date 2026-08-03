#include "editor/imgui/panels/AnimGraphPanel.hpp"

#include "editor/scene/SceneEditor.hpp"
#include "engine/animation/AnimGraphNodes.hpp"
#include "runtime/Components.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace fadix::editor
{
namespace
{
constexpr ImVec2 kNodeSize{156.0F, 52.0F};

template <typename Callback>
void VisitInputs(AnimGraphNode& node, Callback callback)
{
    if (auto* outputNode = dynamic_cast<OutputNode*>(&node)) callback(outputNode->Child);
    else if (auto* floatBlend = dynamic_cast<BlendByFloatNode*>(&node))
    {
        for (BlendByFloatNode::Entry& entry : floatBlend->Entries) callback(entry.ChildIndex);
    }
    else if (auto* conditionBlend = dynamic_cast<BlendByConditionNode*>(&node))
    {
        callback(conditionBlend->TrueChild);
        callback(conditionBlend->FalseChild);
    }
    else if (auto* layeredBlend = dynamic_cast<LayeredBlendNode*>(&node))
    {
        callback(layeredBlend->BaseChild);
        callback(layeredBlend->LayerChild);
    }
    else if (auto* additive = dynamic_cast<AdditiveNode*>(&node))
    {
        callback(additive->BaseChild);
        callback(additive->AdditiveChild);
    }
    else if (auto* savedPose = dynamic_cast<SavedPoseNode*>(&node)) callback(savedPose->Child);
    else if (auto* stateMachine = dynamic_cast<StateMachineNode*>(&node))
    {
        for (auto& [name, child] : stateMachine->StateChildIndices)
        {
            static_cast<void>(name);
            callback(child);
        }
    }
}

bool InputString(const char* label, std::string& value)
{
    std::array<char, 160> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
    if (!ImGui::InputText(label, buffer.data(), buffer.size()))
    {
        return false;
    }
    value = buffer.data();
    return true;
}

bool DrawNodeIndex(const char* label, int& value, const AnimationGraph& graph)
{
    const char* preview = "None";
    if (value >= 0 && value < static_cast<int>(graph.Nodes.size()) && graph.Nodes[value])
    {
        preview = graph.Nodes[static_cast<std::size_t>(value)]->TypeName().data();
    }
    bool changed = false;
    if (ImGui::BeginCombo(label, preview))
    {
        if (ImGui::Selectable("None", value < 0))
        {
            value = -1;
            changed = true;
        }
        for (int i = 0; i < static_cast<int>(graph.Nodes.size()); ++i)
        {
            ImGui::PushID(i);
            const std::string nodeLabel = std::to_string(i) + ": " +
                std::string{graph.Nodes[static_cast<std::size_t>(i)]->TypeName()};
            if (ImGui::Selectable(nodeLabel.c_str(), value == i))
            {
                value = i;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

std::string UniqueParameterName(const AnimationGraph& graph)
{
    for (int suffix = 1;; ++suffix)
    {
        const std::string name = "Parameter" + std::to_string(suffix);
        const bool exists = std::any_of(graph.Parameters.begin(), graph.Parameters.end(),
            [&](const AnimGraphParameter& parameter) { return parameter.Name == name; });
        if (!exists) return name;
    }
}

void FixIndicesAfterErase(AnimationGraph& graph, const int removed)
{
    for (const std::unique_ptr<AnimGraphNode>& node : graph.Nodes)
    {
        VisitInputs(*node, [removed](int& input) {
            if (input == removed) input = -1;
            else if (input > removed) --input;
        });
    }
    if (graph.OutputNodeIndex == removed) graph.OutputNodeIndex = -1;
    else if (graph.OutputNodeIndex > removed) --graph.OutputNodeIndex;
}

template <typename Node>
void AddNode(AnimationGraph& graph, const glm::vec2 position, int& selected)
{
    auto node = std::make_unique<Node>();
    node->EditorPosition = position;
    graph.Nodes.push_back(std::move(node));
    selected = static_cast<int>(graph.Nodes.size()) - 1;
}
}

void AnimGraphPanel::Draw(SceneEditor& scene, AnimationGraph& graph, AnimatorComponent& animator)
{
    if (m_ActiveGraph != &graph)
    {
        m_ActiveGraph = &graph;
        m_SelectedNode = -1;
        m_SelectedParameter = -1;
        m_Pan = {40.0F, 40.0F};
    }

    bool changed = false;
    ImGui::BeginChild("##AnimGraphParameters", ImVec2{190.0F, 0.0F}, true);
    ImGui::TextUnformatted("Parameters");
    ImGui::SameLine();
    if (ImGui::SmallButton("+"))
    {
        graph.Parameters.push_back({UniqueParameterName(graph), AnimGraphParameter::Type::Float});
        m_SelectedParameter = static_cast<int>(graph.Parameters.size()) - 1;
        changed = true;
    }
    ImGui::Separator();
    for (int i = 0; i < static_cast<int>(graph.Parameters.size()); ++i)
    {
        ImGui::PushID(i);
        if (ImGui::Selectable(graph.Parameters[static_cast<std::size_t>(i)].Name.c_str(),
                m_SelectedParameter == i))
        {
            m_SelectedParameter = i;
        }
        ImGui::PopID();
    }
    if (m_SelectedParameter >= 0 &&
        m_SelectedParameter < static_cast<int>(graph.Parameters.size()))
    {
        ImGui::Separator();
        AnimGraphParameter& parameter =
            graph.Parameters[static_cast<std::size_t>(m_SelectedParameter)];
        changed |= InputString("Name", parameter.Name);
        int type = static_cast<int>(parameter.ParamType);
        constexpr std::array<const char*, 4> types{"Float", "Bool", "Int", "Trigger"};
        if (ImGui::Combo("Type", &type, types.data(), static_cast<int>(types.size())))
        {
            parameter.ParamType = static_cast<AnimGraphParameter::Type>(type);
            changed = true;
        }
        if (parameter.ParamType == AnimGraphParameter::Type::Float)
            changed |= ImGui::DragFloat("Default", &parameter.FloatValue, 0.02F);
        else if (parameter.ParamType == AnimGraphParameter::Type::Int)
            changed |= ImGui::DragInt("Default", &parameter.IntValue);
        else if (parameter.ParamType == AnimGraphParameter::Type::Bool)
            changed |= ImGui::Checkbox("Default", &parameter.BoolValue);
        if (ImGui::Button("Remove parameter"))
        {
            graph.Parameters.erase(graph.Parameters.begin() + m_SelectedParameter);
            m_SelectedParameter = -1;
            changed = true;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    const float inspectorWidth = 260.0F;
    const float canvasWidth = std::max(260.0F, ImGui::GetContentRegionAvail().x - inspectorWidth - 8.0F);
    ImGui::BeginChild("##AnimGraphCanvas", ImVec2{canvasWidth, 0.0F}, true,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const ImVec2 canvasMin = ImGui::GetWindowPos();
    const ImVec2 canvasSize = ImGui::GetWindowSize();
    const ImVec2 origin{canvasMin.x + m_Pan.x, canvasMin.y + m_Pan.y};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    constexpr float grid = 32.0F;
    for (float x = std::fmod(m_Pan.x, grid); x < canvasSize.x; x += grid)
        draw->AddLine({canvasMin.x + x, canvasMin.y},
            {canvasMin.x + x, canvasMin.y + canvasSize.y}, IM_COL32(54, 58, 68, 100));
    for (float y = std::fmod(m_Pan.y, grid); y < canvasSize.y; y += grid)
        draw->AddLine({canvasMin.x, canvasMin.y + y},
            {canvasMin.x + canvasSize.x, canvasMin.y + y}, IM_COL32(54, 58, 68, 100));

    ImGui::SetCursorScreenPos(canvasMin);
    ImGui::InvisibleButton("##AnimGraphCanvasInput", canvasSize,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
            ImGuiButtonFlags_MouseButtonMiddle);
    const bool canvasHovered = ImGui::IsItemHovered();
    if (canvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        m_Pan.x += ImGui::GetIO().MouseDelta.x;
        m_Pan.y += ImGui::GetIO().MouseDelta.y;
    }
    if (canvasHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    {
        const ImVec2 mouse = ImGui::GetMousePos();
        m_AddPosition = {mouse.x - origin.x, mouse.y - origin.y};
        ImGui::OpenPopup("Add animation node");
    }

    for (int nodeIndex = 0; nodeIndex < static_cast<int>(graph.Nodes.size()); ++nodeIndex)
    {
        AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(nodeIndex)];
        VisitInputs(node, [&](int& child) {
            if (child < 0 || child >= static_cast<int>(graph.Nodes.size())) return;
            const AnimGraphNode& source = *graph.Nodes[static_cast<std::size_t>(child)];
            const ImVec2 from{origin.x + source.EditorPosition.x + kNodeSize.x,
                origin.y + source.EditorPosition.y + kNodeSize.y * 0.5F};
            const ImVec2 to{origin.x + node.EditorPosition.x,
                origin.y + node.EditorPosition.y + kNodeSize.y * 0.5F};
            draw->AddBezierCubic(from, {from.x + 55.0F, from.y}, {to.x - 55.0F, to.y}, to,
                IM_COL32(102, 190, 255, 220), 2.0F);
        });
    }

    for (int nodeIndex = 0; nodeIndex < static_cast<int>(graph.Nodes.size()); ++nodeIndex)
    {
        AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(nodeIndex)];
        const ImVec2 position{origin.x + node.EditorPosition.x, origin.y + node.EditorPosition.y};
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID(nodeIndex);
        ImGui::InvisibleButton("node", kNodeSize, ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemClicked()) m_SelectedNode = nodeIndex;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            node.EditorPosition.x += ImGui::GetIO().MouseDelta.x;
            node.EditorPosition.y += ImGui::GetIO().MouseDelta.y;
            changed = true;
        }
        const bool selected = m_SelectedNode == nodeIndex;
        const ImU32 fill = selected ? IM_COL32(55, 91, 128, 255) : IM_COL32(45, 49, 59, 255);
        const ImU32 border = graph.OutputNodeIndex == nodeIndex
            ? IM_COL32(93, 220, 132, 255) : IM_COL32(105, 112, 128, 255);
        draw->AddRectFilled(position, {position.x + kNodeSize.x, position.y + kNodeSize.y},
            fill, 6.0F);
        draw->AddRect(position, {position.x + kNodeSize.x, position.y + kNodeSize.y},
            border, 6.0F, 0, selected ? 2.5F : 1.5F);
        draw->AddText({position.x + 10.0F, position.y + 9.0F}, IM_COL32_WHITE,
            node.TypeName().data());
        const std::string indexLabel = "#" + std::to_string(nodeIndex);
        draw->AddText({position.x + 10.0F, position.y + 29.0F}, IM_COL32(170, 178, 195, 255),
            indexLabel.c_str());
        ImGui::PopID();
    }

    if (ImGui::BeginPopup("Add animation node"))
    {
        const auto item = [&](const char* label, auto add) {
            if (ImGui::MenuItem(label))
            {
                add();
                changed = true;
            }
        };
        item("Clip", [&] {
            AddNode<ClipNode>(graph, m_AddPosition, m_SelectedNode);
            auto* clip = dynamic_cast<ClipNode*>(graph.Nodes.back().get());
            clip->ClipName = animator.ClipName;
        });
        item("Blend by Float", [&] { AddNode<BlendByFloatNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Blend by Condition", [&] { AddNode<BlendByConditionNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Layered Blend", [&] { AddNode<LayeredBlendNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Additive", [&] { AddNode<AdditiveNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Save Pose", [&] { AddNode<SavedPoseNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Use Saved Pose", [&] { AddNode<UseSavedPoseNode>(graph, m_AddPosition, m_SelectedNode); });
        item("Legacy State Machine", [&] {
            AddNode<StateMachineNode>(graph, m_AddPosition, m_SelectedNode);
            dynamic_cast<StateMachineNode*>(graph.Nodes.back().get())->Controller = animator.Controller;
        });
        item("Output", [&] {
            AddNode<OutputNode>(graph, m_AddPosition, m_SelectedNode);
            graph.OutputNodeIndex = m_SelectedNode;
        });
        ImGui::EndPopup();
    }
    ImGui::SetCursorScreenPos({canvasMin.x + 8.0F, canvasMin.y + canvasSize.y - 22.0F});
    ImGui::TextDisabled("Right-click: add node   Middle-drag: pan   Drag node: move");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##AnimGraphInspector", ImVec2{inspectorWidth, 0.0F}, true);
    ImGui::TextUnformatted("Node Inspector");
    ImGui::Separator();
    if (m_SelectedNode < 0 || m_SelectedNode >= static_cast<int>(graph.Nodes.size()))
    {
        ImGui::TextDisabled("Select a node.");
    }
    else
    {
        AnimGraphNode& node = *graph.Nodes[static_cast<std::size_t>(m_SelectedNode)];
        ImGui::Text("%d: %s", m_SelectedNode, node.TypeName().data());
        if (ImGui::Button("Set as output"))
        {
            graph.OutputNodeIndex = m_SelectedNode;
            changed = true;
        }
        ImGui::Separator();
        if (auto* outputNode = dynamic_cast<OutputNode*>(&node))
            changed |= DrawNodeIndex("Pose", outputNode->Child, graph);
        else if (auto* clipNode = dynamic_cast<ClipNode*>(&node))
        {
            changed |= InputString("Clip", clipNode->ClipName);
            changed |= ImGui::DragFloat("Speed", &clipNode->Speed, 0.02F);
            changed |= ImGui::Checkbox("Loop", &clipNode->Loop);
            changed |= ImGui::Checkbox("Mirror", &clipNode->Mirror);
        }
        else if (auto* floatBlend = dynamic_cast<BlendByFloatNode*>(&node))
        {
            changed |= InputString("Parameter", floatBlend->ParameterName);
            for (int i = 0; i < static_cast<int>(floatBlend->Entries.size()); ++i)
            {
                ImGui::PushID(i);
                changed |= ImGui::DragFloat("Threshold", &floatBlend->Entries[i].Threshold, 0.02F);
                changed |= DrawNodeIndex("Pose", floatBlend->Entries[i].ChildIndex, graph);
                ImGui::PopID();
            }
            if (ImGui::Button("Add blend point"))
            {
                floatBlend->Entries.push_back({});
                changed = true;
            }
            ImGui::SameLine();
            if (!floatBlend->Entries.empty() && ImGui::Button("Remove last"))
            {
                floatBlend->Entries.pop_back();
                changed = true;
            }
        }
        else if (auto* conditionBlend = dynamic_cast<BlendByConditionNode*>(&node))
        {
            changed |= InputString("Parameter", conditionBlend->ParameterName);
            changed |= DrawNodeIndex("True pose", conditionBlend->TrueChild, graph);
            changed |= DrawNodeIndex("False pose", conditionBlend->FalseChild, graph);
            changed |= ImGui::DragFloat("Blend seconds", &conditionBlend->BlendDuration, 0.01F, 0.0F);
        }
        else if (auto* layeredBlend = dynamic_cast<LayeredBlendNode*>(&node))
        {
            changed |= DrawNodeIndex("Base pose", layeredBlend->BaseChild, graph);
            changed |= DrawNodeIndex("Layer pose", layeredBlend->LayerChild, graph);
            changed |= ImGui::SliderFloat("Weight", &layeredBlend->Weight, 0.0F, 1.0F);
            changed |= InputString("Weight parameter", layeredBlend->WeightParameter);
            ImGui::InputInt("Bone index", &m_NewBoneIndex);
            if (ImGui::Button("Add bone"))
            {
                layeredBlend->BoneMask.push_back(std::max(0, m_NewBoneIndex));
                changed = true;
            }
            for (int i = 0; i < static_cast<int>(layeredBlend->BoneMask.size()); ++i)
            {
                ImGui::PushID(i);
                changed |= ImGui::InputInt("##Bone", &layeredBlend->BoneMask[i]);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    layeredBlend->BoneMask.erase(layeredBlend->BoneMask.begin() + i);
                    changed = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }
        else if (auto* additive = dynamic_cast<AdditiveNode*>(&node))
        {
            changed |= DrawNodeIndex("Base pose", additive->BaseChild, graph);
            changed |= DrawNodeIndex("Additive pose", additive->AdditiveChild, graph);
            changed |= ImGui::SliderFloat("Weight", &additive->Weight, 0.0F, 1.0F);
            changed |= InputString("Weight parameter", additive->WeightParameter);
        }
        else if (auto* savedPose = dynamic_cast<SavedPoseNode*>(&node))
        {
            changed |= InputString("Pose key", savedPose->PoseKey);
            changed |= DrawNodeIndex("Pose", savedPose->Child, graph);
        }
        else if (auto* usedPose = dynamic_cast<UseSavedPoseNode*>(&node))
            changed |= InputString("Pose key", usedPose->PoseKey);
        else if (auto* stateMachine = dynamic_cast<StateMachineNode*>(&node))
        {
            ImGui::TextWrapped("Uses the Animator's legacy state machine. Edit states and transitions in the controller above.");
            if (ImGui::Button("Refresh from Animator"))
            {
                stateMachine->Controller = animator.Controller;
                changed = true;
            }
            for (const AnimatorState& state : stateMachine->Controller.States)
            {
                const auto found = stateMachine->StateChildIndices.find(state.Name);
                int child = found != stateMachine->StateChildIndices.end() ? found->second : -1;
                if (DrawNodeIndex(state.Name.c_str(), child, graph))
                {
                    stateMachine->StateChildIndices[state.Name] = child;
                    changed = true;
                }
            }
        }
        ImGui::Separator();
        if (ImGui::Button("Delete node"))
        {
            const int removed = m_SelectedNode;
            graph.Nodes.erase(graph.Nodes.begin() + removed);
            FixIndicesAfterErase(graph, removed);
            m_SelectedNode = -1;
            changed = true;
        }
    }
    ImGui::EndChild();

    if (changed)
    {
        animator.RuntimeParameters = graph.Parameters;
        scene.MarkChanged();
    }
}
}
