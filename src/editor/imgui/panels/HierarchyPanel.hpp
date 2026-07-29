#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/Uuid.hpp"

#include <optional>
#include <vector>

namespace fadix::editor
{
class HierarchyPanel final
{
public:
    void Draw(SceneEditor& scene, EditorUiState& ui);

private:
    void DrawToolbar(SceneEditor& scene, EditorUiState& ui);
    void DrawTree(SceneEditor& scene, EditorUiState& ui);
    void DrawContextMenu(SceneEditor& scene, const Uuid& id, EditorUiState& ui);

    void SyncMultiFromPrimary(SceneEditor& scene);
    [[nodiscard]] bool IsMultiSelected(const Uuid& id) const;
    void SelectOnly(SceneEditor& scene, const Uuid& id, bool recordUndo);
    void ToggleMulti(SceneEditor& scene, const Uuid& id);
    void SelectRange(SceneEditor& scene, const std::vector<Uuid>& visibleOrder, const Uuid& id);
    bool DeleteMulti(SceneEditor& scene, EditorUiState& ui);
    bool DuplicateMulti(SceneEditor& scene, EditorUiState& ui);

    char m_FilterBuf[128]{};
    char m_RenameBuf[128]{};
    std::optional<Uuid> m_RenameTarget;
    bool m_FilterSynced{false};
    // Hide directional/point/spot lights from the tree (still exist in the scene).
    bool m_HideLights{true};
    std::vector<Uuid> m_Multi;
    std::optional<Uuid> m_RangeAnchor;
};
}
