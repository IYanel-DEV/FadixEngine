#include "editor/dock/DockManager.hpp"

#include "project/ProjectJson.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace fadix::editor
{
namespace
{
constexpr std::array<const char*, 4> kPanelIds{
    "left-panel", "center", "inspector", "bottom-panel"};

constexpr std::array<std::pair<std::string_view, std::string_view>, 4> kHeaders{{
    {"dock-header-hierarchy", "left-panel"},
    {"dock-header-viewport", "center"},
    {"dock-header-inspector", "inspector"},
    {"dock-header-content", "bottom-panel"},
}};

constexpr const char* kDropOverlayRml =
    R"(<div id="dock-silhouette" class="dock-silhouette"/>
<div id="dock-compass" class="dock-compass">
    <div class="dock-chevron top">▲</div>
    <div class="dock-chevron left">◀</div>
    <div class="dock-center-diamond"/>
    <div class="dock-chevron right">▶</div>
    <div class="dock-chevron bottom">▼</div>
</div>)";

[[nodiscard]] bool PointInsideElement(Rml::Element* element, const float x, const float y)
{
    if (element == nullptr)
    {
        return false;
    }
    const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    return x >= position.x && y >= position.y && x < position.x + size.x && y < position.y + size.y;
}

[[nodiscard]] Rml::Element* FindSplitterRecursive(Rml::Element* element, const float x, const float y)
{
    if (element == nullptr)
    {
        return nullptr;
    }
    if (element->IsClassSet("dock-splitter") && PointInsideElement(element, x, y))
    {
        return element;
    }
    for (int index = 0; index < element->GetNumChildren(); ++index)
    {
        if (Rml::Element* hit = FindSplitterRecursive(element->GetChild(index), x, y))
        {
            return hit;
        }
    }
    return nullptr;
}

[[nodiscard]] Rml::Element* FindLeafRecursive(Rml::Element* element, const float x, const float y)
{
    if (element == nullptr)
    {
        return nullptr;
    }
    if (element->IsClassSet("dock-leaf") && PointInsideElement(element, x, y))
    {
        return element;
    }
    for (int index = 0; index < element->GetNumChildren(); ++index)
    {
        if (Rml::Element* hit = FindLeafRecursive(element->GetChild(index), x, y))
        {
            return hit;
        }
    }
    return nullptr;
}

[[nodiscard]] const char* RegionClass(const DropRegion region)
{
    switch (region)
    {
    case DropRegion::Left:
        return "left";
    case DropRegion::Right:
        return "right";
    case DropRegion::Top:
        return "top";
    case DropRegion::Bottom:
        return "bottom";
    case DropRegion::Center:
        return "center";
    case DropRegion::None:
    default:
        return "center";
    }
}
}

DockManager::DockManager(LogCallback log)
    : m_Log(std::move(log))
    , m_Layout(DockLayout::Default())
{
}

void DockManager::Bind(Rml::ElementDocument& document)
{
    Unbind();
    m_Document = &document;
    m_Root = document.GetElementById("dock-root");
    m_PanelBank = document.GetElementById("dock-panel-bank");
    m_FloatLayer = document.GetElementById("dock-float-layer");
    m_OverlayLayer = document.GetElementById("dock-overlay-layer");
    if (m_Root == nullptr || m_PanelBank == nullptr || m_FloatLayer == nullptr ||
        m_OverlayLayer == nullptr)
    {
        if (m_Log)
        {
            m_Log("DockManager bind failed: missing dock host elements", "error");
        }
        Unbind();
        return;
    }
    for (const char* panelId : kPanelIds)
    {
        if (document.GetElementById(panelId) == nullptr)
        {
            if (m_Log)
            {
                m_Log(std::string{"DockManager bind failed: missing panel "} + panelId, "error");
            }
            Unbind();
            return;
        }
    }
    RebuildDom();
}

void DockManager::Unbind() noexcept
{
    ClearDrag();
    m_Resize.reset();
    m_Document = nullptr;
    m_Root = nullptr;
    m_PanelBank = nullptr;
    m_FloatLayer = nullptr;
    m_OverlayLayer = nullptr;
}

void DockManager::SetLayout(DockLayout layout)
{
    m_Layout = std::move(layout);
    if (m_Document != nullptr)
    {
        RebuildDom();
    }
}

const DockLayout& DockManager::Layout() const noexcept
{
    return m_Layout;
}

bool DockManager::HandleEvent(const SDL_Event& event)
{
    if (m_Document == nullptr || m_Root == nullptr)
    {
        return false;
    }

    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        m_Resize.reset();
        ClearDrag();
        return false;
    }

    const bool pointerEvent = event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!pointerEvent)
    {
        return false;
    }

    const float mouseX = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
    const float mouseY = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
    {
        Rml::Element* splitter = HitSplitter(mouseX, mouseY);
        if (splitter != nullptr)
        {
            const Rml::String nodeId = splitter->GetAttribute("data-dock-split", Rml::String{});
            DockNode* node = m_Layout.FindNode(std::string{nodeId});
            Rml::Element* parent = splitter->GetParentNode();
            if (node == nullptr || node->IsLeaf() || parent == nullptr)
            {
                return false;
            }
            const Rml::Vector2f size = parent->GetBox().GetSize(Rml::BoxArea::Border);
            const float extent = node->Axis == SplitAxis::Horizontal ? size.x : size.y;
            if (!(extent > 1.0F))
            {
                return false;
            }
            m_Resize = ResizeState{
                std::string{nodeId},
                node->Axis,
                node->Axis == SplitAxis::Horizontal ? mouseX : mouseY,
                node->Ratio,
                extent};
            return true;
        }

        for (const auto& [headerId, panelId] : kHeaders)
        {
            Rml::Element* header = m_Document->GetElementById(std::string{headerId});
            if (!PointInsideElement(header, mouseX, mouseY))
            {
                continue;
            }
            m_Drag = DragState{
                std::string{panelId},
                mouseX,
                mouseY,
                false,
                IsFloating(panelId),
                {},
                DropRegion::None};
            return false;
        }
        return false;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION && m_Resize)
    {
        const float pointer = m_Resize->Axis == SplitAxis::Horizontal
            ? event.motion.x
            : event.motion.y;
        const float delta = (pointer - m_Resize->Origin) / m_Resize->Extent;
        if (m_Layout.ResizeSplit(m_Resize->NodeId, m_Resize->StartRatio + delta))
        {
            RebuildDom();
        }
        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION && m_Drag)
    {
        if (!m_Drag->Active)
        {
            if (std::hypot(mouseX - m_Drag->StartX, mouseY - m_Drag->StartY) < 5.0F)
            {
                return false;
            }
            m_Drag->Active = true;
            if (Rml::Element* panel = m_Document->GetElementById(m_Drag->PanelId))
            {
                panel->SetClass("dragging", true);
            }
        }

        UpdateDropOverlay(mouseX, mouseY);

        if (m_Drag->WasFloating)
        {
            if (m_Drag->TargetPanelId.empty())
            {
                if (FloatingPanel* floating = FindFloating(m_Drag->PanelId))
                {
                    floating->X += mouseX - m_Drag->StartX;
                    floating->Y += mouseY - m_Drag->StartY;
                    ClampFloatingPanel(*floating);
                    ApplyFloaterTransform(*floating);
                }
            }
            m_Drag->StartX = mouseX;
            m_Drag->StartY = mouseY;
        }

        return true;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
    {
        if (m_Resize)
        {
            m_Resize.reset();
            return true;
        }
        if (m_Drag)
        {
            const bool wasActive = m_Drag->Active;
            bool changed = false;
            if (wasActive && !m_Drag->TargetPanelId.empty() &&
                m_Drag->Region != DropRegion::None)
            {
                changed = m_Layout.DockPanel(
                    m_Drag->PanelId, m_Drag->TargetPanelId, m_Drag->Region);
            }
            else if (wasActive && m_Drag->Region == DropRegion::None && !m_Drag->WasFloating)
            {
                FloatingPanel bounds;
                bounds.PanelId = m_Drag->PanelId;
                bounds.Width = 360.0F;
                bounds.Height = 260.0F;
                if (Rml::Element* workspace = m_Document->GetElementById("workspace"))
                {
                    const Rml::Vector2f workspacePosition =
                        workspace->GetAbsoluteOffset(Rml::BoxArea::Border);
                    bounds.X = std::max(0.0F, mouseX - workspacePosition.x - 160.0F);
                    bounds.Y = std::max(0.0F, mouseY - workspacePosition.y - 14.0F);
                }
                else
                {
                    bounds.X = std::max(0.0F, mouseX - 160.0F);
                    bounds.Y = std::max(0.0F, mouseY - 14.0F);
                }
                ClampFloatingPanel(bounds);
                changed = m_Layout.FloatPanel(m_Drag->PanelId, bounds);
            }
            ClearDrag();
            if (changed)
            {
                RebuildDom();
            }
            return wasActive;
        }
    }

    return false;
}

void DockManager::Reset()
{
    SetLayout(DockLayout::Default());
}

project_json::Value DockManager::Serialize() const
{
    return m_Layout.ToJson();
}

bool DockManager::Deserialize(const project_json::Value& root, std::string& error)
{
    auto layout = DockLayout::FromJson(root, error);
    if (!layout)
    {
        return false;
    }
    SetLayout(std::move(*layout));
    return true;
}

bool DockManager::Load(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        if (m_Log)
        {
            m_Log("Could not open dock layout " + path.generic_string(), "warn");
        }
        return false;
    }
    std::ostringstream text;
    text << input.rdbuf();
    const auto parsed = project_json::Parse(text.str());
    if (!parsed)
    {
        if (m_Log)
        {
            m_Log("Ignoring invalid dock layout JSON " + path.generic_string(), "warn");
        }
        return false;
    }
    std::string error;
    if (!Deserialize(*parsed, error))
    {
        if (m_Log)
        {
            m_Log("Ignoring invalid dock layout " + path.generic_string() + ": " + error, "warn");
        }
        return false;
    }
    return true;
}

bool DockManager::Save(const std::filesystem::path& path) const
{
    std::error_code error;
    const std::filesystem::path folder = path.parent_path();
    if (!folder.empty())
    {
        std::filesystem::create_directories(folder, error);
        if (error)
        {
            if (m_Log)
            {
                m_Log("Could not create dock layout folder: " + error.message(), "error");
            }
            return false;
        }
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            if (m_Log)
            {
                m_Log("Could not write dock layout " + temporary.generic_string(), "error");
            }
            return false;
        }
        output << project_json::Stringify(Serialize()) << '\n';
        if (!output)
        {
            if (m_Log)
            {
                m_Log("Could not finish dock layout " + temporary.generic_string(), "error");
            }
            return false;
        }
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        if (m_Log)
        {
            m_Log("Could not replace dock layout " + path.generic_string() + ": " + error.message(),
                "error");
        }
        return false;
    }
    return true;
}

void DockManager::ClearHostChildren(Rml::Element* host)
{
    if (host == nullptr)
    {
        return;
    }
    while (Rml::Element* child = host->GetFirstChild())
    {
        static_cast<void>(host->RemoveChild(child));
    }
}

void DockManager::ApplySplitRatio(
    const DockNode& node, Rml::Element& first, Rml::Element& second)
{
    // RmlUi does not resolve percentage width/height against flex-sized ancestors.
    // Percentage + flex:0 collapses the first pane; the flex:1 sibling (often
    // bottom-panel / Assets) then fills the whole dock-root after reset/load.
    static_cast<void>(node.Axis);
    const int firstGrow =
        std::max(1, static_cast<int>(std::round(node.Ratio * 1000.0F)));
    const int secondGrow = std::max(1, 1000 - firstGrow);
    first.SetProperty("flex", std::to_string(firstGrow));
    second.SetProperty("flex", std::to_string(secondGrow));
}

Rml::ElementPtr DockManager::BuildNode(const DockNode& node)
{
    if (m_Document == nullptr)
    {
        return {};
    }

    if (node.IsLeaf())
    {
        Rml::ElementPtr leaf = m_Document->CreateElement("div");
        leaf->SetClass("dock-leaf", true);
        leaf->SetAttribute("data-dock-node", node.Id);
        leaf->SetProperty("flex", "1");
        return leaf;
    }

    Rml::ElementPtr container = m_Document->CreateElement("div");
    container->SetClass("dock-node", true);
    container->SetClass(
        node.Axis == SplitAxis::Horizontal ? "horizontal" : "vertical", true);
    container->SetAttribute("data-dock-node", node.Id);

    Rml::ElementPtr firstWrap = m_Document->CreateElement("div");
    firstWrap->SetClass("dock-child", true);
    Rml::ElementPtr secondWrap = m_Document->CreateElement("div");
    secondWrap->SetClass("dock-child", true);

    if (node.First)
    {
        firstWrap->AppendChild(BuildNode(*node.First));
    }
    if (node.Second)
    {
        secondWrap->AppendChild(BuildNode(*node.Second));
    }

    Rml::ElementPtr splitter = m_Document->CreateElement("div");
    splitter->SetClass("dock-splitter", true);
    splitter->SetClass(
        node.Axis == SplitAxis::Horizontal ? "vertical" : "horizontal", true);
    splitter->SetAttribute("data-dock-split", node.Id);

    ApplySplitRatio(node, *firstWrap, *secondWrap);
    container->AppendChild(std::move(firstWrap));
    container->AppendChild(std::move(splitter));
    container->AppendChild(std::move(secondWrap));
    return container;
}

void DockManager::ClampFloatingPanel(FloatingPanel& floating) const
{
    floating.Width = std::max(180.0F, floating.Width);
    floating.Height = std::max(120.0F, floating.Height);

    float workspaceWidth = 0.0F;
    float workspaceHeight = 0.0F;
    if (m_Document != nullptr)
    {
        if (Rml::Element* workspace = m_Document->GetElementById("workspace"))
        {
            const Rml::Vector2f size = workspace->GetBox().GetSize(Rml::BoxArea::Border);
            workspaceWidth = size.x;
            workspaceHeight = size.y;
        }
    }

    floating.X = std::clamp(floating.X, 0.0F, std::max(0.0F, workspaceWidth - 80.0F));
    floating.Y = std::clamp(floating.Y, 0.0F, std::max(0.0F, workspaceHeight - 29.0F));
}

void DockManager::ApplyFloaterTransform(const FloatingPanel& floating)
{
    if (m_FloatLayer == nullptr)
    {
        return;
    }

    for (int index = 0; index < m_FloatLayer->GetNumChildren(); ++index)
    {
        Rml::Element* child = m_FloatLayer->GetChild(index);
        if (child == nullptr)
        {
            continue;
        }
        const Rml::String panelId =
            child->GetAttribute("data-floating-panel", Rml::String{});
        if (panelId != floating.PanelId)
        {
            continue;
        }
        child->SetProperty("left", std::to_string(floating.X) + "px");
        child->SetProperty("top", std::to_string(floating.Y) + "px");
        child->SetProperty("width", std::to_string(floating.Width) + "px");
        child->SetProperty("height", std::to_string(floating.Height) + "px");
        return;
    }
}

FloatingPanel* DockManager::FindFloating(const std::string_view panelId) noexcept
{
    const auto iterator = std::find_if(m_Layout.Floating.begin(), m_Layout.Floating.end(),
        [panelId](const FloatingPanel& panel) { return panel.PanelId == panelId; });
    return iterator != m_Layout.Floating.end() ? &(*iterator) : nullptr;
}

bool DockManager::IsFloating(const std::string_view panelId) const noexcept
{
    return std::any_of(m_Layout.Floating.begin(), m_Layout.Floating.end(),
        [panelId](const FloatingPanel& panel) { return panel.PanelId == panelId; });
}

void DockManager::ProjectFloatingPanels()
{
    if (m_Document == nullptr || m_FloatLayer == nullptr)
    {
        return;
    }

    for (FloatingPanel& floating : m_Layout.Floating)
    {
        ClampFloatingPanel(floating);

        Rml::ElementPtr floater = m_Document->CreateElement("div");
        floater->SetClass("dock-floater", true);
        floater->SetAttribute("data-floating-panel", floating.PanelId);
        floater->SetProperty("left", std::to_string(floating.X) + "px");
        floater->SetProperty("top", std::to_string(floating.Y) + "px");
        floater->SetProperty("width", std::to_string(floating.Width) + "px");
        floater->SetProperty("height", std::to_string(floating.Height) + "px");
        floater->SetProperty("pointer-events", "auto");

        Rml::ElementPtr content = m_Document->CreateElement("div");
        content->SetClass("dock-floater-content", true);

        if (Rml::Element* panel = m_Document->GetElementById(floating.PanelId))
        {
            if (Rml::Element* parent = panel->GetParentNode())
            {
                content->AppendChild(parent->RemoveChild(panel));
            }
        }

        floater->AppendChild(std::move(content));
        m_FloatLayer->AppendChild(std::move(floater));
    }
}

void DockManager::RebuildDom()
{
    if (m_Document == nullptr || m_Root == nullptr || m_PanelBank == nullptr)
    {
        return;
    }

    std::unordered_map<std::string, Rml::ElementPtr> ownedPanels;
    for (const char* panelId : kPanelIds)
    {
        Rml::Element* panel = m_Document->GetElementById(panelId);
        Rml::Element* parent = panel != nullptr ? panel->GetParentNode() : nullptr;
        if (parent != nullptr)
        {
            ownedPanels.emplace(panelId, parent->RemoveChild(panel));
        }
    }

    ClearHostChildren(m_Root);
    ClearHostChildren(m_FloatLayer);
    ClearHostChildren(m_OverlayLayer);

    if (m_Layout.Root)
    {
        m_Root->AppendChild(BuildNode(*m_Layout.Root));
    }

    std::function<void(Rml::Element*)> attachLeaves = [&](Rml::Element* element) {
        if (element == nullptr)
        {
            return;
        }
        if (element->IsClassSet("dock-leaf"))
        {
            const Rml::String nodeId = element->GetAttribute("data-dock-node", Rml::String{});
            DockNode* node = m_Layout.FindNode(std::string{nodeId});
            if (node != nullptr && node->PanelId)
            {
                auto panel = ownedPanels.find(*node->PanelId);
                if (panel != ownedPanels.end())
                {
                    element->AppendChild(std::move(panel->second));
                    ownedPanels.erase(panel);
                }
                else if (m_Log)
                {
                    m_Log("Dock rebuild missing panel " + *node->PanelId, "error");
                }
            }
            return;
        }
        for (int index = 0; index < element->GetNumChildren(); ++index)
        {
            attachLeaves(element->GetChild(index));
        }
    };
    attachLeaves(m_Root);

    for (auto& [panelId, panel] : ownedPanels)
    {
        if (m_Log)
        {
            m_Log("Dock rebuild parked unmatched panel " + panelId + " in panel bank", "warn");
        }
        m_PanelBank->AppendChild(std::move(panel));
    }

    ProjectFloatingPanels();
}

Rml::Element* DockManager::HitSplitter(const float x, const float y) const
{
    return FindSplitterRecursive(m_Root, x, y);
}

std::string DockManager::HitLeaf(const float x, const float y) const
{
    Rml::Element* leaf = FindLeafRecursive(m_Root, x, y);
    if (leaf == nullptr)
    {
        return {};
    }
    for (int index = 0; index < leaf->GetNumChildren(); ++index)
    {
        Rml::Element* child = leaf->GetChild(index);
        if (child == nullptr)
        {
            continue;
        }
        const Rml::String id = child->GetId();
        for (const char* panelId : kPanelIds)
        {
            if (id == panelId)
            {
                return panelId;
            }
        }
    }
    return {};
}

DropRegion DockManager::HitDropRegion(Rml::Element& leaf, const float x, const float y) const
{
    const Rml::Vector2f position = leaf.GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = leaf.GetBox().GetSize(Rml::BoxArea::Border);
    const float width = size.x;
    const float height = size.y;
    if (!(width > 0.0F) || !(height > 0.0F))
    {
        return DropRegion::None;
    }
    const float localX = x - position.x;
    const float localY = y - position.y;
    if (std::abs(localX - width * 0.5F) <= 18.0F &&
        std::abs(localY - height * 0.5F) <= 18.0F)
    {
        return DropRegion::Center;
    }
    if (localX < width * 0.25F)
    {
        return DropRegion::Left;
    }
    if (localX > width * 0.75F)
    {
        return DropRegion::Right;
    }
    if (localY < height * 0.25F)
    {
        return DropRegion::Top;
    }
    if (localY > height * 0.75F)
    {
        return DropRegion::Bottom;
    }
    return DropRegion::Center;
}

void DockManager::HideDropOverlay() noexcept
{
    ClearHostChildren(m_OverlayLayer);
    if (m_OverlayLayer != nullptr)
    {
        m_OverlayLayer->SetClass("hidden", true);
    }
}

void DockManager::ClearDrag() noexcept
{
    if (m_Document != nullptr && m_Drag)
    {
        if (Rml::Element* panel = m_Document->GetElementById(m_Drag->PanelId))
        {
            panel->SetClass("dragging", false);
        }
    }
    HideDropOverlay();
    m_Drag.reset();
}

void DockManager::UpdateDropOverlay(const float x, const float y)
{
    if (m_Document == nullptr || m_OverlayLayer == nullptr || !m_Drag || !m_Drag->Active)
    {
        return;
    }

    const std::string targetPanelId = HitLeaf(x, y);
    if (targetPanelId.empty())
    {
        m_Drag->TargetPanelId.clear();
        m_Drag->Region = DropRegion::None;
        HideDropOverlay();
        return;
    }

    Rml::Element* leafElement = nullptr;
    std::function<void(Rml::Element*)> findLeaf = [&](Rml::Element* element) {
        if (element == nullptr || leafElement != nullptr)
        {
            return;
        }
        if (element->IsClassSet("dock-leaf"))
        {
            const Rml::String nodeId = element->GetAttribute("data-dock-node", Rml::String{});
            const DockNode* node = m_Layout.FindNode(std::string{nodeId});
            if (node != nullptr && node->PanelId && *node->PanelId == targetPanelId)
            {
                leafElement = element;
                return;
            }
        }
        for (int index = 0; index < element->GetNumChildren(); ++index)
        {
            findLeaf(element->GetChild(index));
        }
    };
    findLeaf(m_Root);
    if (leafElement == nullptr)
    {
        m_Drag->TargetPanelId.clear();
        m_Drag->Region = DropRegion::None;
        HideDropOverlay();
        return;
    }

    const DropRegion region = HitDropRegion(*leafElement, x, y);
    m_Drag->TargetPanelId = targetPanelId;
    m_Drag->Region = region;

    if (m_OverlayLayer->GetNumChildren() == 0)
    {
        m_OverlayLayer->SetInnerRML(kDropOverlayRml);
    }

    Rml::Element* silhouette = m_Document->GetElementById("dock-silhouette");
    Rml::Element* compass = m_Document->GetElementById("dock-compass");
    if (silhouette == nullptr || compass == nullptr)
    {
        m_OverlayLayer->SetInnerRML(kDropOverlayRml);
        silhouette = m_Document->GetElementById("dock-silhouette");
        compass = m_Document->GetElementById("dock-compass");
    }
    if (silhouette == nullptr || compass == nullptr)
    {
        return;
    }

    m_OverlayLayer->SetClass("hidden", false);

    const Rml::Vector2f overlayOrigin =
        m_OverlayLayer->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f leafPos = leafElement->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f leafSize = leafElement->GetBox().GetSize(Rml::BoxArea::Border);

    float silX = leafPos.x - overlayOrigin.x;
    float silY = leafPos.y - overlayOrigin.y;
    float silW = leafSize.x;
    float silH = leafSize.y;
    switch (region)
    {
    case DropRegion::Left:
        silW = leafSize.x * 0.5F;
        break;
    case DropRegion::Right:
        silX += leafSize.x * 0.5F;
        silW = leafSize.x * 0.5F;
        break;
    case DropRegion::Top:
        silH = leafSize.y * 0.5F;
        break;
    case DropRegion::Bottom:
        silY += leafSize.y * 0.5F;
        silH = leafSize.y * 0.5F;
        break;
    case DropRegion::Center:
    case DropRegion::None:
    default:
        break;
    }

    for (const char* name : {"center", "left", "right", "top", "bottom"})
    {
        silhouette->SetClass(name, false);
    }
    silhouette->SetClass(RegionClass(region), true);
    silhouette->SetProperty("left", std::to_string(silX) + "px");
    silhouette->SetProperty("top", std::to_string(silY) + "px");
    silhouette->SetProperty("width", std::to_string(silW) + "px");
    silhouette->SetProperty("height", std::to_string(silH) + "px");

    constexpr float kCompassSize = 112.0F;
    const float compassX =
        leafPos.x - overlayOrigin.x + leafSize.x * 0.5F - kCompassSize * 0.5F;
    const float compassY =
        leafPos.y - overlayOrigin.y + leafSize.y * 0.5F - kCompassSize * 0.5F;
    compass->SetProperty("left", std::to_string(compassX) + "px");
    compass->SetProperty("top", std::to_string(compassY) + "px");
}
}
