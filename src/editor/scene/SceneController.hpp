#pragma once

#include "editor/scene/SceneEditor.hpp"
#include "engine/assets/AssetHandle.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Rml
{
class Element;
class ElementDocument;
class Event;
class EventListener;
}

namespace fadix
{
struct AssetDragPayload;

class SceneController final
{
public:
    SceneController(IWorld& world, UndoStack& history);
    ~SceneController();

    void Bind(Rml::ElementDocument& document);
    void Refresh();
    [[nodiscard]] std::optional<Uuid> Selection() const noexcept;
    void SetSelection(std::optional<Uuid> selection);
    [[nodiscard]] std::optional<Uuid> SceneRootId() const;
    [[nodiscard]] bool IsSceneRoot(const Uuid& id) const;
    void SetFilter(std::string filter);
    void SetChangedCallback(std::function<void()> callback);
    void SetAssetDatabase(AssetDatabase* database);
    void SetGltfMeshCache(GltfMeshCache* cache);
    void SetSelectedMaterialProvider(std::function<std::optional<AssetHandle>()> provider);
    void SetSelectedMeshProvider(std::function<std::optional<AssetHandle>()> provider);
    void SetStatusReporter(std::function<void(std::string_view)> reporter);

    // Returns true if the drop was handled (compatible or rejected with status).
    [[nodiscard]] bool TryApplyInspectorDrop(
        const AssetDragPayload& payload,
        float mouseX,
        float mouseY);
    // Hierarchy entity → Scripts Target (or other entity-ref slots later).
    [[nodiscard]] bool TryApplyEntityDrop(const Uuid& entityId, float mouseX, float mouseY);

    [[nodiscard]] bool BeginNumericScrub(float mouseX, float mouseY);
    [[nodiscard]] bool UpdateNumericScrub(float mouseX);
    [[nodiscard]] bool EndNumericScrub();
    [[nodiscard]] bool NumericScrubActive() const noexcept;
    [[nodiscard]] bool ComponentPickerOpen() const noexcept { return m_AddComponentOpen; }
    void CloseComponentPicker();
    // RmlUi hit-tests break inside overflow:auto popups; pick/toggle by geometry instead.
    [[nodiscard]] bool TryPickComponentOption(float mouseX, float mouseY);
    // Returns true when the pointer is over the Add Component button (toggles picker).
    [[nodiscard]] bool TryToggleComponentPicker(float mouseX, float mouseY);

    // Exposed so ImGui panels can use the same mutation layer without Rml.
    SceneEditor& Editor() noexcept { return m_Editor; }

private:
    class Listener;
    void Handle(Rml::Event& event);
    void Listen(const char* id, bool changeEvent = false);
    void RebuildTree();
    void RebuildInspector();
    void ToggleComponentPicker(Rml::Element& anchor);
    void RebuildComponentPickerList();
    [[nodiscard]] bool ApplyMaterialFieldEdit(std::string_view id, Rml::Element* source);

    struct NumericDragState
    {
        std::string Id;
        float StartMouseX{0.0F};
        float StartValue{0.0F};
        EntitySnapshot Before;
        bool Dragging{false};
        bool Changed{false};
    };

    SceneEditor m_Editor;
    Rml::ElementDocument* m_Document{nullptr};
    std::string m_ComponentFilter;
    std::vector<std::string> m_ComponentPickIds;
    bool m_AddComponentOpen{false};
    std::optional<NumericDragState> m_NumericDrag;
    std::vector<std::unique_ptr<Rml::EventListener>> m_Listeners;
};
} // namespace fadix
