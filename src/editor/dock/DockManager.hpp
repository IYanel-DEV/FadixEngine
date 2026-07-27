#pragma once

#include "editor/dock/DockLayout.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL_events.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace fadix::editor
{
class DockManager
{
public:
    using LogCallback = std::function<void(std::string, const char*)>;

    explicit DockManager(LogCallback log);
    void Bind(Rml::ElementDocument& document);
    void Unbind() noexcept;
    void SetLayout(DockLayout layout);
    [[nodiscard]] const DockLayout& Layout() const noexcept;
    [[nodiscard]] bool HandleEvent(const SDL_Event& event);
    void Reset();
    [[nodiscard]] project_json::Value Serialize() const;
    [[nodiscard]] bool Deserialize(const project_json::Value& root, std::string& error);
    [[nodiscard]] bool Load(const std::filesystem::path& path);
    [[nodiscard]] bool Save(const std::filesystem::path& path) const;

private:
    struct ResizeState
    {
        std::string NodeId;
        SplitAxis Axis{SplitAxis::Horizontal};
        float Origin{0.0F};
        float StartRatio{0.5F};
        float Extent{1.0F};
    };

    struct DragState
    {
        std::string PanelId;
        float StartX{0.0F};
        float StartY{0.0F};
        bool Active{false};
        bool WasFloating{false};
        std::string TargetPanelId;
        DropRegion Region{DropRegion::None};
    };

    void RebuildDom();
    Rml::ElementPtr BuildNode(const DockNode& node);
    void ApplySplitRatio(const DockNode& node, Rml::Element& first, Rml::Element& second);
    void ProjectFloatingPanels();
    void ClampFloatingPanel(FloatingPanel& floating) const;
    void ApplyFloaterTransform(const FloatingPanel& floating);
    [[nodiscard]] FloatingPanel* FindFloating(std::string_view panelId) noexcept;
    [[nodiscard]] bool IsFloating(std::string_view panelId) const noexcept;
    void ClearHostChildren(Rml::Element* host);
    [[nodiscard]] Rml::Element* HitSplitter(float x, float y) const;
    [[nodiscard]] std::string HitLeaf(float x, float y) const;
    [[nodiscard]] DropRegion HitDropRegion(Rml::Element& leaf, float x, float y) const;
    void UpdateDropOverlay(float x, float y);
    void HideDropOverlay() noexcept;
    void ClearDrag() noexcept;

    LogCallback m_Log;
    DockLayout m_Layout;
    Rml::ElementDocument* m_Document{nullptr};
    Rml::Element* m_Root{nullptr};
    Rml::Element* m_PanelBank{nullptr};
    Rml::Element* m_FloatLayer{nullptr};
    Rml::Element* m_OverlayLayer{nullptr};
    std::optional<ResizeState> m_Resize;
    std::optional<DragState> m_Drag;
};
}
