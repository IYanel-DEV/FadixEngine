#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "editor/scripting/ScriptEditorController.hpp"

#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;

namespace fadix
{
class ScriptDatabase;
}

namespace fadix::editor
{
/// ImGui shell around ScriptEditorController; Scintilla HWND overlays the code area.
class ScriptEditorPanel final
{
public:
    void Bind(ScriptEditorController& editor, ScriptDatabase* scripts, SDL_Window* window);
    void Draw(EditorUiState& ui, const std::filesystem::path& createFolder);

private:
    void DrawToolbar(EditorUiState& ui, const std::filesystem::path& createFolder);
    void DrawTabs(EditorUiState& ui);
    void DrawFindBar(EditorUiState& ui);
    void DrawFindInFiles(EditorUiState& ui);
    void DrawList();
    void LayoutOverlay(bool suspend);
    void HandleCommands(EditorUiState& ui);
    void OpenFind(bool withReplace);
    void CloseFind();
    void OpenGoto();
    void OpenQuickOpen();
    void OpenFindInFiles();
    void DrawClosePrompt(EditorUiState& ui);
    void DrawExternalPrompt(EditorUiState& ui);

    ScriptEditorController* m_Editor{nullptr};
    ScriptDatabase* m_Scripts{nullptr};
    SDL_Window* m_Window{nullptr};
    char m_SearchBuf[128]{};
    char m_NameBuf[128]{};
    char m_FindBuf[256]{};
    char m_ReplaceBuf[256]{};
    char m_GotoBuf[64]{};
    char m_QuickOpenBuf[128]{};
    char m_ProjectFindBuf[256]{};
    char m_ProjectReplaceBuf[256]{};
    bool m_PendingLua{false};
    bool m_PendingCpp{false};
    bool m_ShowFind{false};
    bool m_ShowReplace{false};
    bool m_FocusFind{false};
    bool m_FocusReplace{false};
    bool m_ShowGoto{false};
    bool m_FocusGoto{false};
    bool m_ShowQuickOpen{false};
    bool m_FocusQuickOpen{false};
    bool m_ConfirmReplaceAll{false};
    bool m_ShowFindInFiles{false};
    bool m_FocusProjectFind{false};
    bool m_ConfirmProjectReplace{false};
    std::size_t m_PendingReplaceCount{0};
    std::vector<FxsProjectHit> m_ProjectHits;
};
}
