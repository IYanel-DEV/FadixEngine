#pragma once

#include "assets/ScriptAsset.hpp"
#include "editor/scripting/FxsApiCatalog.hpp"
#include "editor/scripting/FxsDiagnostics.hpp"
#include "editor/scripting/FxsEditorSettings.hpp"
#include "editor/scripting/FxsTextSearch.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fadix::editor
{
enum class FxsUiCommand : std::uint8_t
{
    None = 0,
    Find,
    Replace,
    FindNext,
    FindPrev,
    GotoLine,
    QuickOpen,
    Escape,
    NextDocument,
    PrevDocument,
    CloseDocument,
    FindInFiles,
};

struct FxsViewState
{
    std::size_t FirstVisibleLine{0};
    std::size_t XOffset{0};
    std::size_t Anchor{0};
    std::size_t Caret{0};
    int Zoom{0};
};

struct FxsEditorDiagnostic
{
    std::size_t Line{1};
    std::size_t Column{1};
    std::size_t Start{0};
    std::size_t End{0};
    std::string Message;
};

/// Owns the Scintilla HWND + Lexilla lexer setup for the FXS Editor.
class FxsCodeEditor
{
public:
    FxsCodeEditor();
    ~FxsCodeEditor();

    FxsCodeEditor(const FxsCodeEditor&) = delete;
    FxsCodeEditor& operator=(const FxsCodeEditor&) = delete;

    void AttachParent(void* parentHwnd);
    void Destroy();

    void SetVisible(bool visible);
    void Layout(int clientX, int clientY, int width, int height);
    void RequestFocus();
    /// Give keyboard focus back to the SDL/parent HWND (call when hiding or leaving Play).
    void ReleaseFocus();
    [[nodiscard]] bool HasFocus() const noexcept;
    [[nodiscard]] bool IsCreated() const noexcept;

    void SetText(std::string_view text);
    [[nodiscard]] std::string GetText() const;
    void Clear();

    void SetLanguage(ScriptLanguage language);
    void InsertText(std::string_view text);
    void SelectAll();
    void GotoLineColumn(std::size_t line, std::size_t column);
    void SetSelection(std::size_t anchor, std::size_t caret);
    void EnsureRangeVisible(std::size_t start, std::size_t end);

    [[nodiscard]] bool TakeModified();
    [[nodiscard]] std::size_t Cursor() const;
    [[nodiscard]] std::size_t SelectionAnchor() const;
    [[nodiscard]] std::size_t SelectionLength() const;
    [[nodiscard]] std::size_t Line() const;
    [[nodiscard]] std::size_t Column() const;
    [[nodiscard]] int ZoomLevel() const;

    void SetWordWrap(bool enabled);
    void SetViewWhitespace(bool enabled);
    void ToggleWordWrap();
    void ToggleViewWhitespace();
    [[nodiscard]] bool WordWrap() const noexcept { return m_Settings.WordWrap; }
    [[nodiscard]] bool ViewWhitespace() const noexcept { return m_Settings.ViewWhitespace; }

    void ZoomIn();
    void ZoomOut();
    void ZoomReset();
    void ApplySettings(const FxsEditorSettings& settings);
    [[nodiscard]] const FxsEditorSettings& Settings() const noexcept { return m_Settings; }
    void PersistSettings();
    void NotifySave();
    void QueueUiCommand(FxsUiCommand command);
    [[nodiscard]] FxsUiCommand TakeUiCommand();

    void ClearMatchHighlights();
    void HighlightMatches(const std::vector<FxsSearchMatch>& matches);

    void SetDiagnostics(std::vector<FxsEditorDiagnostic> diagnostics);
    void ClearDiagnostics();
    [[nodiscard]] const std::vector<FxsEditorDiagnostic>& Diagnostics() const noexcept
    {
        return m_Diagnostics;
    }

    [[nodiscard]] void* CreateDocument();
    void AddRefDocument(void* doc);
    void ReleaseDocument(void* doc);
    [[nodiscard]] void* CurrentDocument() const;
    void SetDocument(void* doc);
    void CaptureViewState(FxsViewState& out) const;
    void ApplyViewState(const FxsViewState& state);

    void ToggleLineComment();
    void DuplicateSelectionOrLine();
    void MoveLines(bool up);
    void DeleteLine();
    void TrimTrailingWhitespace();
    void IndentSelection(bool forward);

    void TriggerAutocomplete(bool force);
    void CancelAutocomplete();
    [[nodiscard]] const std::string& CompletionDocumentation() const noexcept
    {
        return m_CompletionDocs;
    }

    using SaveHandler = void (*)(void* user);
    void SetSaveHandler(SaveHandler handler, void* user);

    void Tick();
    void OnCharAdded(int ch);
    void OnDwellStart(std::size_t position);
    void OnDwellEnd();
    void OnAutocompleteSelectionChange();
    void OnAutocompleteSelection(std::string_view selected);

private:
    void EnsureCreated();
    void ApplyThemeAndChrome();
    void ApplyMarginStyles();
    void ApplyDpiFont();
    void ApplyLexer();
    void ApplyIndentSettings();
    void ApplyWrapAndWhitespace();
    void ApplyZoom();
    void MatchBraces();
    void RegisterCompletionImages();
    void UpdateCallTip();
    void ShowSuggestions(const std::vector<FxsSuggestion>& suggestions, std::size_t prefixLen);
    void AutoIndentAfterNewline();
    void MaybeAutoClose(int ch);
    [[nodiscard]] std::intptr_t Send(unsigned int msg, std::intptr_t w = 0, std::intptr_t l = 0) const;

    void* m_ParentHwnd{nullptr};
    void* m_HostHwnd{nullptr};
    void* m_SciHwnd{nullptr};
    ScriptLanguage m_Language{ScriptLanguage::Lua};
    FxsEditorSettings m_Settings{};
    std::uint32_t m_Dpi{0};
    int m_LayoutX{0};
    int m_LayoutY{0};
    int m_LayoutW{-1};
    int m_LayoutH{-1};
    bool m_WantVisible{false};
    SaveHandler m_SaveHandler{nullptr};
    void* m_SaveUser{nullptr};
    FxsUiCommand m_PendingUi{FxsUiCommand::None};
    std::vector<FxsSuggestion> m_Suggestions;
    std::string m_CompletionDocs;
    bool m_CallTipFromCompletion{false};
    std::vector<FxsEditorDiagnostic> m_Diagnostics;
    std::string m_DwellTip;
    bool m_SuppressAutoClose{false};
};
}
