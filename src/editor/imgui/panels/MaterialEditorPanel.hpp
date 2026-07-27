#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/material/MaterialEditorController.hpp"

namespace fadix::editor
{
class MaterialEditorPanel final
{
public:
    void Bind(MaterialEditorController& editor);
    void Draw(EditorUiState& ui);

private:
    void DrawPreview();
    void DrawFields(EditorUiState& ui);
    void DrawTextureSlot(const char* label, const char* slot, EditorUiState& ui);

    MaterialEditorController* m_Editor{nullptr};
};
}
