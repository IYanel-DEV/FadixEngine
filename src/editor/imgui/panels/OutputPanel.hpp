#pragma once

#include "editor/EditorLog.hpp"
#include "editor/imgui/EditorUiState.hpp"

#include <imgui.h>

#include <functional>
#include <string>

namespace fadix::editor
{
class OutputPanel final
{
public:
    void Bind(EditorLog& log);
    void SetOpenDiagnostic(std::function<void(const OutputEntry&)> openDiagnostic);

    void Draw(EditorUiState& ui);

private:
    EditorLog* m_Log{nullptr};
    std::function<void(const OutputEntry&)> m_OpenDiagnostic;
    ImGuiTextFilter m_TextFilter;
    bool m_ShowInfo{true};
    bool m_ShowWarn{true};
    bool m_ShowError{true};
    bool m_AutoScroll{true};
};
}
