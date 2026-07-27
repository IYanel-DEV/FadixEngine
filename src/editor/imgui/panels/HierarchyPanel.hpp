#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/Uuid.hpp"

#include <optional>

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

    char m_FilterBuf[128]{};
    char m_RenameBuf[128]{};
    std::optional<Uuid> m_RenameTarget;
    bool m_FilterSynced{false};
};
}
