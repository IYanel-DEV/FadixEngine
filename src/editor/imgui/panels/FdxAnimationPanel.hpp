#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/Uuid.hpp"

#include <filesystem>
#include <optional>

namespace fadix::editor
{
// FDX Animation: dockable timeline/keyframe editor for skeletal clips imported on
// a skinned glTF model. Edits clip data in place (the AnimationPlayer evaluates it)
// and round-trips authored clips to project Animations/*.fdxanim.
class FdxAnimationPanel final
{
public:
    void Draw(SceneEditor& scene, EditorUiState& ui, const std::filesystem::path& projectRoot);

private:
    std::optional<Uuid> m_Pinned;
    int m_SelectedChannel{-1};
    int m_SelectedKey{-1};
    int m_TSelectedChannel{-1}; // transform-clip channel/key selection (separate pool)
    int m_TSelectedKey{-1};
    char m_SaveName[128]{};
};
}
