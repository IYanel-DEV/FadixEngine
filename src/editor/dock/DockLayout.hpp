#pragma once

#include "project/ProjectJson.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fadix::editor
{
inline constexpr float MinSplitRatio = 0.1F;
inline constexpr float MaxSplitRatio = 0.9F;

enum class SplitAxis
{
    Horizontal,
    Vertical
};

enum class DropRegion
{
    None,
    Center,
    Left,
    Right,
    Top,
    Bottom
};

struct DockNode
{
    std::string Id;
    std::optional<std::string> PanelId;
    SplitAxis Axis{SplitAxis::Horizontal};
    float Ratio{0.5F};
    std::unique_ptr<DockNode> First;
    std::unique_ptr<DockNode> Second;

    [[nodiscard]] bool IsLeaf() const noexcept { return PanelId.has_value(); }
};

struct FloatingPanel
{
    std::string PanelId;
    float X{80.0F};
    float Y{80.0F};
    float Width{360.0F};
    float Height{260.0F};
};

struct DockLayout
{
    std::unique_ptr<DockNode> Root;
    std::vector<FloatingPanel> Floating;

    [[nodiscard]] static DockLayout Default();
    [[nodiscard]] static std::optional<DockLayout> FromJson(
        const project_json::Value& root, std::string& error);
    [[nodiscard]] project_json::Value ToJson() const;
    [[nodiscard]] bool Validate(std::string& error) const;
    [[nodiscard]] DockNode* FindLeaf(std::string_view panelId) noexcept;
    [[nodiscard]] DockNode* FindNode(std::string_view nodeId) noexcept;
    [[nodiscard]] bool DockPanel(
        std::string_view panelId, std::string_view targetPanelId, DropRegion region);
    [[nodiscard]] bool FloatPanel(std::string_view panelId, FloatingPanel bounds);
    [[nodiscard]] bool ResizeSplit(std::string_view nodeId, float ratio);
};
}
