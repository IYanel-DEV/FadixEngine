#include "editor/dock/DockLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <utility>

namespace fadix::editor
{
namespace
{
using project_json::Value;

std::unique_ptr<DockNode> Leaf(std::string id, std::string panel)
{
    auto node = std::make_unique<DockNode>();
    node->Id = std::move(id);
    node->PanelId = std::move(panel);
    return node;
}

std::unique_ptr<DockNode> Split(std::string id, const SplitAxis axis, const float ratio,
    std::unique_ptr<DockNode> first, std::unique_ptr<DockNode> second)
{
    auto node = std::make_unique<DockNode>();
    node->Id = std::move(id);
    node->Axis = axis;
    node->Ratio = std::clamp(ratio, MinSplitRatio, MaxSplitRatio);
    node->First = std::move(first);
    node->Second = std::move(second);
    return node;
}

DockNode* FindLeafRecursive(DockNode* node, const std::string_view panel)
{
    if (node == nullptr)
    {
        return nullptr;
    }
    if (node->IsLeaf())
    {
        return *node->PanelId == panel ? node : nullptr;
    }
    if (DockNode* found = FindLeafRecursive(node->First.get(), panel))
    {
        return found;
    }
    return FindLeafRecursive(node->Second.get(), panel);
}

DockNode* FindNodeRecursive(DockNode* node, const std::string_view id)
{
    if (node == nullptr || node->Id == id)
    {
        return node;
    }
    if (DockNode* found = FindNodeRecursive(node->First.get(), id))
    {
        return found;
    }
    return FindNodeRecursive(node->Second.get(), id);
}

std::unique_ptr<DockNode>* FindSlot(
    std::unique_ptr<DockNode>& node, const std::string_view panel)
{
    if (!node)
    {
        return nullptr;
    }
    if (node->IsLeaf())
    {
        return *node->PanelId == panel ? &node : nullptr;
    }
    if (auto* slot = FindSlot(node->First, panel))
    {
        return slot;
    }
    return FindSlot(node->Second, panel);
}

std::unique_ptr<DockNode> RemoveLeaf(
    std::unique_ptr<DockNode>& node, const std::string_view panel)
{
    if (!node)
    {
        return {};
    }
    if (node->IsLeaf())
    {
        return *node->PanelId == panel ? std::move(node) : nullptr;
    }
    // Collapse only for a direct child leaf; nested removals already rewrite in place.
    if (node->First && node->First->IsLeaf() && *node->First->PanelId == panel)
    {
        auto removed = std::move(node->First);
        node = std::move(node->Second);
        return removed;
    }
    if (node->Second && node->Second->IsLeaf() && *node->Second->PanelId == panel)
    {
        auto removed = std::move(node->Second);
        node = std::move(node->First);
        return removed;
    }
    if (auto removed = RemoveLeaf(node->First, panel))
    {
        return removed;
    }
    return RemoveLeaf(node->Second, panel);
}

std::size_t LeafCount(const DockNode* node)
{
    if (node == nullptr)
    {
        return 0;
    }
    if (node->IsLeaf())
    {
        return 1;
    }
    return LeafCount(node->First.get()) + LeafCount(node->Second.get());
}

std::string NextRuntimeId(const char* prefix, DockLayout& layout)
{
    static std::uint64_t counter = 0;
    std::string id;
    do
    {
        id = std::string{prefix} + std::to_string(++counter);
    } while (layout.FindNode(id) != nullptr);
    return id;
}

Value NodeToJson(const DockNode& node)
{
    Value json = Value::MakeObject();
    json["id"] = Value::MakeString(node.Id);
    if (node.IsLeaf())
    {
        json["type"] = Value::MakeString("leaf");
        json["panel"] = Value::MakeString(*node.PanelId);
        return json;
    }

    json["type"] = Value::MakeString("split");
    json["axis"] = Value::MakeString(
        node.Axis == SplitAxis::Horizontal ? "horizontal" : "vertical");
    json["ratio"] = Value::MakeNumber(node.Ratio);
    json["first"] = NodeToJson(*node.First);
    json["second"] = NodeToJson(*node.Second);
    return json;
}

bool ReadRequiredString(
    const Value& object, const std::string_view key, std::string& output, std::string& error)
{
    if (!object.Contains(key) || !object.at(key).IsString() || object.at(key).AsString().empty())
    {
        error = "dock JSON has a missing or invalid string";
        return false;
    }
    output = object.at(key).AsString();
    return true;
}

std::unique_ptr<DockNode> ParseNode(const Value& json, std::string& error)
{
    if (!json.IsObject())
    {
        error = "dock node must be an object";
        return {};
    }

    std::string type;
    std::string id;
    if (!ReadRequiredString(json, "type", type, error) ||
        !ReadRequiredString(json, "id", id, error))
    {
        return {};
    }
    if (type == "leaf")
    {
        std::string panel;
        if (!ReadRequiredString(json, "panel", panel, error))
        {
            return {};
        }
        return Leaf(std::move(id), std::move(panel));
    }
    if (type != "split")
    {
        error = "dock node has an unsupported type";
        return {};
    }

    std::string axis;
    if (!ReadRequiredString(json, "axis", axis, error) ||
        !json.Contains("ratio") ||
        json.at("ratio").GetType() != Value::Type::Number ||
        !json.Contains("first") || !json.Contains("second"))
    {
        error = "dock split is missing required fields";
        return {};
    }
    if (axis != "horizontal" && axis != "vertical")
    {
        error = "dock split has an invalid axis";
        return {};
    }
    const double ratio = json.at("ratio").AsNumber();
    if (!std::isfinite(ratio) || ratio < MinSplitRatio || ratio > MaxSplitRatio)
    {
        error = "dock split has an invalid ratio";
        return {};
    }

    auto first = ParseNode(json.at("first"), error);
    if (!first)
    {
        return {};
    }
    auto second = ParseNode(json.at("second"), error);
    if (!second)
    {
        return {};
    }

    auto node = std::make_unique<DockNode>();
    node->Id = std::move(id);
    node->Axis = axis == "horizontal" ? SplitAxis::Horizontal : SplitAxis::Vertical;
    node->Ratio = static_cast<float>(ratio);
    node->First = std::move(first);
    node->Second = std::move(second);
    return node;
}

bool ReadOptionalNumber(
    const Value& object, const std::string_view key, float& output, std::string& error)
{
    if (!object.Contains(key))
    {
        return true;
    }
    if (object.at(key).GetType() != Value::Type::Number ||
        !std::isfinite(object.at(key).AsNumber()))
    {
        error = "dock JSON has an invalid number";
        return false;
    }
    output = static_cast<float>(object.at(key).AsNumber());
    return true;
}

std::optional<DockLayout> MigrateV1(const Value& root, std::string& error)
{
    if (!root.Contains("zones") || !root.at("zones").IsObject())
    {
        error = "version-1 layout is missing zones";
        return std::nullopt;
    }
    const Value& zones = root.at("zones");
    std::string left;
    std::string center;
    std::string right;
    std::string bottom;
    if (!ReadRequiredString(zones, "left", left, error) ||
        !ReadRequiredString(zones, "center", center, error) ||
        !ReadRequiredString(zones, "right", right, error) ||
        !ReadRequiredString(zones, "bottom", bottom, error))
    {
        return std::nullopt;
    }

    float leftWidth = 232.0F;
    float rightWidth = 340.0F;
    float bottomHeight = 190.0F;
    if (root.Contains("dimensions"))
    {
        if (!root.at("dimensions").IsObject())
        {
            error = "version-1 dimensions must be an object";
            return std::nullopt;
        }
        const Value& dimensions = root.at("dimensions");
        if (!ReadOptionalNumber(dimensions, "leftWidth", leftWidth, error) ||
            !ReadOptionalNumber(dimensions, "rightWidth", rightWidth, error) ||
            !ReadOptionalNumber(dimensions, "bottomHeight", bottomHeight, error))
        {
            return std::nullopt;
        }
    }
    if (leftWidth < 0.0F || rightWidth < 0.0F || bottomHeight < 0.0F)
    {
        error = "version-1 dimensions must not be negative";
        return std::nullopt;
    }

    const float totalWidth = leftWidth + 900.0F + rightWidth;
    const float rootRatio =
        std::clamp(leftWidth / totalWidth, MinSplitRatio, MaxSplitRatio);
    const float topRatio = std::clamp(
        (totalWidth - leftWidth - rightWidth) / (totalWidth - leftWidth),
        MinSplitRatio, MaxSplitRatio);
    const float mainRatio = std::clamp(
        620.0F / (620.0F + bottomHeight), MinSplitRatio, MaxSplitRatio);

    DockLayout layout;
    layout.Root = Split("root", SplitAxis::Horizontal, rootRatio,
        Leaf("leaf-hierarchy", std::move(left)),
        Split("main", SplitAxis::Vertical, mainRatio,
            Split("top", SplitAxis::Horizontal, topRatio,
                Leaf("leaf-viewport", std::move(center)),
                Leaf("leaf-inspector", std::move(right))),
            Leaf("leaf-content", std::move(bottom))));
    if (!layout.Validate(error))
    {
        return std::nullopt;
    }
    return layout;
}
}

DockLayout DockLayout::Default()
{
    DockLayout layout;
    layout.Root = Split("root", SplitAxis::Horizontal, 0.145F,
        Leaf("leaf-hierarchy", "left-panel"),
        Split("main", SplitAxis::Vertical, 0.76F,
            Split("top", SplitAxis::Horizontal, 0.68F,
                Leaf("leaf-viewport", "center"),
                Leaf("leaf-inspector", "inspector")),
            Leaf("leaf-content", "bottom-panel")));
    return layout;
}

std::optional<DockLayout> DockLayout::FromJson(const Value& root, std::string& error)
{
    if (!root.IsObject())
    {
        error = "editor layout root must be an object";
        return std::nullopt;
    }

    if (!root.Contains("version"))
    {
        if (root.Contains("zones"))
        {
            return MigrateV1(root, error);
        }
        error = "unsupported editor layout version";
        return std::nullopt;
    }
    if (root.at("version").GetType() != Value::Type::Number)
    {
        error = "unsupported editor layout version";
        return std::nullopt;
    }

    const double version = root.at("version").AsNumber();
    if (version == 1.0)
    {
        return MigrateV1(root, error);
    }
    if (version != 2.0)
    {
        error = "unsupported editor layout version";
        return std::nullopt;
    }
    if (!root.Contains("dock") || !root.Contains("floating") ||
        !root.at("floating").IsArray())
    {
        error = "version-2 layout is missing dock or floating";
        return std::nullopt;
    }

    DockLayout layout;
    layout.Root = ParseNode(root.at("dock"), error);
    if (!layout.Root)
    {
        return std::nullopt;
    }
    for (const Value& json : root.at("floating").Array())
    {
        if (!json.IsObject())
        {
            error = "floating panel must be an object";
            return std::nullopt;
        }
        FloatingPanel panel;
        if (!ReadRequiredString(json, "panel", panel.PanelId, error) ||
            !json.Contains("x") || json.at("x").GetType() != Value::Type::Number ||
            !json.Contains("y") || json.at("y").GetType() != Value::Type::Number ||
            !json.Contains("width") || json.at("width").GetType() != Value::Type::Number ||
            !json.Contains("height") || json.at("height").GetType() != Value::Type::Number)
        {
            error = "floating panel has missing or invalid fields";
            return std::nullopt;
        }
        panel.X = static_cast<float>(json.at("x").AsNumber());
        panel.Y = static_cast<float>(json.at("y").AsNumber());
        panel.Width = static_cast<float>(json.at("width").AsNumber());
        panel.Height = static_cast<float>(json.at("height").AsNumber());
        if (!std::isfinite(panel.X) || !std::isfinite(panel.Y) ||
            !std::isfinite(panel.Width) || !std::isfinite(panel.Height))
        {
            error = "floating panel has non-finite bounds";
            return std::nullopt;
        }
        layout.Floating.push_back(std::move(panel));
    }
    if (!layout.Validate(error))
    {
        return std::nullopt;
    }
    return layout;
}

Value DockLayout::ToJson() const
{
    Value root = Value::MakeObject();
    root["version"] = Value::MakeNumber(2.0);
    root["dock"] = Root ? NodeToJson(*Root) : Value::MakeNull();
    Value floating = Value::MakeArray();
    for (const FloatingPanel& panel : Floating)
    {
        Value json = Value::MakeObject();
        json["panel"] = Value::MakeString(panel.PanelId);
        json["x"] = Value::MakeNumber(panel.X);
        json["y"] = Value::MakeNumber(panel.Y);
        json["width"] = Value::MakeNumber(panel.Width);
        json["height"] = Value::MakeNumber(panel.Height);
        floating.Push(std::move(json));
    }
    root["floating"] = std::move(floating);
    return root;
}

bool DockLayout::Validate(std::string& error) const
{
    std::unordered_set<std::string> nodeIds;
    std::unordered_set<std::string> panels;
    const std::unordered_set<std::string> allowed{
        "left-panel", "center", "inspector", "bottom-panel"};
    std::function<bool(const DockNode*)> visit = [&](const DockNode* node) {
        if (node == nullptr || node->Id.empty() || !nodeIds.insert(node->Id).second)
        {
            error = "dock node is null or has a duplicate id";
            return false;
        }
        if (node->IsLeaf())
        {
            if (node->First || node->Second || !allowed.contains(*node->PanelId) ||
                !panels.insert(*node->PanelId).second)
            {
                error = "dock leaf is malformed or duplicates a panel";
                return false;
            }
            return true;
        }
        if (!node->First || !node->Second || !std::isfinite(node->Ratio) ||
            node->Ratio < MinSplitRatio || node->Ratio > MaxSplitRatio)
        {
            error = "dock split has invalid children or ratio";
            return false;
        }
        return visit(node->First.get()) && visit(node->Second.get());
    };
    if (!visit(Root.get()))
    {
        return false;
    }
    for (const FloatingPanel& panel : Floating)
    {
        if (!allowed.contains(panel.PanelId) || !panels.insert(panel.PanelId).second ||
            !std::isfinite(panel.X) || !std::isfinite(panel.Y) ||
            !std::isfinite(panel.Width) || !std::isfinite(panel.Height) ||
            panel.Width < 180.0F || panel.Height < 120.0F)
        {
            error = "floating panel is invalid or duplicated";
            return false;
        }
    }
    if (panels.size() != allowed.size())
    {
        error = "layout does not contain every editor panel exactly once";
        return false;
    }
    error.clear();
    return true;
}

DockNode* DockLayout::FindLeaf(const std::string_view panelId) noexcept
{
    return FindLeafRecursive(Root.get(), panelId);
}

DockNode* DockLayout::FindNode(const std::string_view nodeId) noexcept
{
    return FindNodeRecursive(Root.get(), nodeId);
}

bool DockLayout::DockPanel(
    const std::string_view panelId, const std::string_view targetPanelId,
    const DropRegion region)
{
    if (region == DropRegion::None || panelId == targetPanelId)
    {
        return false;
    }

    DockNode* sourceLeaf = FindLeaf(panelId);
    DockNode* targetLeaf = FindLeaf(targetPanelId);
    const auto floatingIterator = std::find_if(Floating.begin(), Floating.end(),
        [panelId](const FloatingPanel& panel) { return panel.PanelId == panelId; });
    const bool sourceFloating = floatingIterator != Floating.end();
    if ((sourceLeaf == nullptr && !sourceFloating) || targetLeaf == nullptr)
    {
        return false;
    }

    if (region == DropRegion::Center)
    {
        if (sourceLeaf != nullptr)
        {
            std::swap(sourceLeaf->PanelId, targetLeaf->PanelId);
            return true;
        }
        FloatingPanel displacedBounds = *floatingIterator;
        Floating.erase(floatingIterator);
        displacedBounds.PanelId = *targetLeaf->PanelId;
        targetLeaf->PanelId = std::string{panelId};
        Floating.push_back(std::move(displacedBounds));
        return true;
    }

    const bool horizontal = region == DropRegion::Left || region == DropRegion::Right;
    const bool sourceFirst = region == DropRegion::Left || region == DropRegion::Top;
    std::unique_ptr<DockNode> source;
    if (sourceLeaf != nullptr)
    {
        source = RemoveLeaf(Root, panelId);
    }
    else
    {
        Floating.erase(floatingIterator);
        source = Leaf(NextRuntimeId("leaf-runtime-", *this), std::string{panelId});
    }
    if (!source)
    {
        return false;
    }

    auto* targetSlot = FindSlot(Root, targetPanelId);
    if (targetSlot == nullptr)
    {
        return false;
    }
    auto target = std::move(*targetSlot);
    const std::string splitId = NextRuntimeId("split-runtime-", *this);
    if (sourceFirst)
    {
        *targetSlot = Split(splitId,
            horizontal ? SplitAxis::Horizontal : SplitAxis::Vertical, 0.5F,
            std::move(source), std::move(target));
    }
    else
    {
        *targetSlot = Split(splitId,
            horizontal ? SplitAxis::Horizontal : SplitAxis::Vertical, 0.5F,
            std::move(target), std::move(source));
    }
    return true;
}

bool DockLayout::FloatPanel(const std::string_view panelId, FloatingPanel bounds)
{
    if (FindLeaf(panelId) == nullptr || LeafCount(Root.get()) <= 1)
    {
        return false;
    }
    auto removed = RemoveLeaf(Root, panelId);
    if (!removed)
    {
        return false;
    }
    bounds.PanelId = std::string{panelId};
    bounds.X = std::max(0.0F, bounds.X);
    bounds.Y = std::max(0.0F, bounds.Y);
    bounds.Width = std::max(180.0F, bounds.Width);
    bounds.Height = std::max(120.0F, bounds.Height);
    Floating.push_back(std::move(bounds));
    return true;
}

bool DockLayout::ResizeSplit(const std::string_view nodeId, const float ratio)
{
    DockNode* node = FindNode(nodeId);
    if (node == nullptr || node->IsLeaf() || !std::isfinite(ratio))
    {
        return false;
    }
    node->Ratio = std::clamp(ratio, MinSplitRatio, MaxSplitRatio);
    return true;
}
}
