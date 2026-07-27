#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "engine/Uuid.hpp"

#include <optional>

namespace fadix::editor
{
class InspectorPanel final
{
public:
    void Draw(SceneEditor& scene, EditorUiState& ui);

private:
    char m_NameBuf[128]{};
    std::optional<Uuid> m_NameBound;
};
}
