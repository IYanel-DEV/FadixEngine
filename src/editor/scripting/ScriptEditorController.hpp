#pragma once

#include "assets/ScriptAsset.hpp"
#include "editor/scripting/FxsCodeEditor.hpp"
#include "editor/scripting/FxsEditorSession.hpp"
#include "editor/scripting/FxsProjectSearch.hpp"
#include "editor/scripting/FxsTextSearch.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fadix
{
class ScriptDatabase;
}

namespace fadix::editor
{
struct ScriptValidationResult
{
    bool Success{true};
    std::string Message;
    std::size_t Line{1};
    std::size_t Column{1};
};

enum class FxsCloseDecision : std::uint8_t
{
    None = 0,
    NeedsSavePrompt,
    Closed,
};

enum class FxsExternalDecision : std::uint8_t
{
    None = 0,
    Reload,
    KeepEditor,
    Compare,
};

struct FxsOpenDocument
{
    std::string Name;
    std::filesystem::path Path;
    ScriptLanguage Language{ScriptLanguage::Lua};
    void* SciDoc{nullptr};
    std::string Buffer; // used when Scintilla HWND is not created (smoke)
    bool Dirty{false};
    FxsViewState View{};
    std::optional<ScriptValidationResult> Validation;
    std::vector<FxsEditorDiagnostic> Diagnostics;
    std::filesystem::file_time_type DiskWriteTime{};
    bool DiskWriteKnown{false};
    bool ExternalChanged{false};
    std::string DiskSnapshot; // for Compare
};

/// Multi-document FXS workspace: tabs + one Scintilla view (doc pointers).
class ScriptEditorController
{
public:
    using Validator = std::function<ScriptValidationResult(const ScriptAsset&)>;
    using ValidationReporter =
        std::function<void(const ScriptAsset&, const ScriptValidationResult&)>;
    using UiNotify = std::function<void()>;

    ScriptEditorController();
    ~ScriptEditorController();

    ScriptEditorController(const ScriptEditorController&) = delete;
    ScriptEditorController& operator=(const ScriptEditorController&) = delete;

    void Bind(ScriptDatabase& database);
    void SetValidator(Validator validator, ValidationReporter reporter);
    void SetUiNotify(UiNotify notify);
    void SetSessionPath(std::filesystem::path path);

    void AttachNativeHost(void* parentHwnd);
    void SetNativeVisible(bool visible);
    void SuspendNative(bool suspend);
    void LayoutNative(int clientX, int clientY, int width, int height);
    void TickNative();
    void RequestNativeFocus();
    [[nodiscard]] FxsUiCommand TakeUiCommand();
    void QueueUiCommand(FxsUiCommand command);

    void RefreshScriptList();
    void OpenScript(const std::string& name);
    void SelectScript(const std::string& name);
    void SelectScriptAt(const std::string& name, std::size_t line, std::size_t column = 1);
    void ActivateTab(std::size_t index);
    void NextDocument(bool forward);
    bool CreateNew(
        ScriptLanguage language,
        const std::filesystem::path& folder = {},
        const std::string& preferredName = {});

    void SaveSelected();
    void SaveAll();
    /// Returns NeedsSavePrompt when dirty; call ConfirmCloseSave/Discard/CancelClose.
    [[nodiscard]] FxsCloseDecision RequestCloseActive();
    [[nodiscard]] FxsCloseDecision RequestCloseAt(std::size_t index);
    void ConfirmCloseSave();
    void ConfirmCloseDiscard();
    void CancelClose();
    [[nodiscard]] bool HasPendingClose() const noexcept { return m_PendingCloseIndex.has_value(); }
    [[nodiscard]] std::string PendingCloseName() const;

    void SetFilter(const std::string& filter);

    void InsertText(std::string_view text);
    void SelectAll();
    void ToggleWordWrap();
    void ToggleViewWhitespace();
    [[nodiscard]] bool WordWrap() const noexcept;
    [[nodiscard]] bool ViewWhitespace() const noexcept;

    void SetFindQuery(std::string query);
    void SetReplaceText(std::string text);
    void SetFindFlags(FxsSearchFlags flags);
    void RefreshFindMatches();
    [[nodiscard]] bool FindNext(bool forward);
    [[nodiscard]] bool ReplaceCurrent();
    [[nodiscard]] FxsReplaceAllResult ReplaceAll();
    void ClearFindHighlights();
    void GotoLineColumnInDocument(std::size_t line, std::size_t column = 1);

    void TrimTrailingWhitespace();
    void ToggleLineComment();

    void SetValidationDebounceMs(int ms);
    void FlushPendingValidation();
    void TickValidation();
    [[nodiscard]] bool IsCompiling() const noexcept;
    [[nodiscard]] std::string_view CompilingScript() const noexcept;

    [[nodiscard]] bool SaveSession() const;
    [[nodiscard]] bool LoadSession();

    void CheckExternalChanges();
    [[nodiscard]] bool HasExternalConflict() const;
    [[nodiscard]] std::string ExternalConflictName() const;
    [[nodiscard]] const std::string& ExternalDiskText() const noexcept { return m_CompareDisk; }
    [[nodiscard]] const std::string& ExternalEditorText() const noexcept { return m_CompareEditor; }
    void ResolveExternal(FxsExternalDecision decision);

    FxsProjectSearch& ProjectSearch() noexcept { return m_ProjectSearch; }
    void StartFindInFiles(std::string query, FxsSearchFlags flags = {});
    [[nodiscard]] std::size_t PreviewReplaceInFiles(std::string_view replacement);
    [[nodiscard]] std::size_t ConfirmReplaceInFiles(std::string_view replacement);
    [[nodiscard]] const std::vector<std::string>& ReplacePreview() const noexcept
    {
        return m_ReplacePreview;
    }

    [[nodiscard]] const std::string& FindQuery() const noexcept { return m_FindQuery; }
    [[nodiscard]] const std::string& ReplaceText() const noexcept { return m_ReplaceText; }
    [[nodiscard]] FxsSearchFlags FindFlags() const noexcept { return m_FindFlags; }
    [[nodiscard]] const FxsSearchScan& FindScan() const noexcept { return m_FindScan; }
    [[nodiscard]] std::size_t FindMatchIndex() const noexcept { return m_FindIndex; }
    [[nodiscard]] std::string FindStatusLabel() const;

    [[nodiscard]] const std::string& SelectedScript() const;
    [[nodiscard]] const std::string& Source() const;
    [[nodiscard]] bool HasScript() const noexcept;
    [[nodiscard]] bool Editing() const noexcept;
    [[nodiscard]] bool Dirty() const noexcept;
    [[nodiscard]] bool NativeFocused() const noexcept;
    [[nodiscard]] std::size_t Cursor() const;
    [[nodiscard]] std::size_t SelectionAnchor() const;
    [[nodiscard]] std::size_t SelectionLength() const;
    [[nodiscard]] std::span<const std::string> VisibleNames() const noexcept;
    [[nodiscard]] std::span<const FxsOpenDocument> OpenDocuments() const noexcept;
    [[nodiscard]] std::size_t ActiveTabIndex() const noexcept { return m_Active; }
    [[nodiscard]] std::vector<std::string> AllScriptNames() const;
    [[nodiscard]] const std::string& Filter() const noexcept { return m_Filter; }
    [[nodiscard]] std::size_t StatusLine() const noexcept { return m_StatusLine; }
    [[nodiscard]] std::size_t StatusColumn() const noexcept { return m_StatusColumn; }
    [[nodiscard]] const std::string& StatusInfo() const noexcept { return m_StatusInfo; }
    [[nodiscard]] const std::string& HeaderText() const noexcept { return m_HeaderText; }
    [[nodiscard]] const char* LanguageLabel() const noexcept;
    [[nodiscard]] std::string IndentModeLabel() const;
    [[nodiscard]] const char* EncodingLabel() const noexcept { return "UTF-8"; }
    [[nodiscard]] int ZoomPercent() const;
    [[nodiscard]] bool NameDirty(const std::string& name) const;
    [[nodiscard]] ScriptLanguage LanguageOf(const std::string& name) const;
    [[nodiscard]] const std::optional<ScriptValidationResult>& LastValidation() const noexcept;
    [[nodiscard]] const std::vector<FxsEditorDiagnostic>& Diagnostics() const;

    /// Test helper: mark disk mtime older and inject external text without writing.
    void DebugMarkExternalChange(const std::string& name, std::string diskText);

private:
    [[nodiscard]] FxsOpenDocument* ActiveDoc();
    [[nodiscard]] const FxsOpenDocument* ActiveDoc() const;
    [[nodiscard]] int FindOpenIndex(const std::string& name) const;
    void StashActiveView();
    void ShowDocument(std::size_t index);
    void EnsureSciDocument(FxsOpenDocument& doc);
    void DestroyDocument(FxsOpenDocument& doc);
    void CommitActiveToAsset();
    void PullActiveFromNative();
    void UpdateStatusBar();
    void SetHeader();
    void ValidateActive();
    void StartAsyncValidation();
    void ScheduleValidation();
    void MaybeRunScheduledValidation();
    void ApplyValidationResult(FxsOpenDocument& doc, const ScriptValidationResult& result);
    void MarkDirty();
    void SyncNativeVisibility();
    void NotifyUi();
    void ApplyFindSelection();
    void SyncFindHighlights();
    void RememberDiskTime(FxsOpenDocument& doc);
    [[nodiscard]] bool WriteDocumentToDisk(FxsOpenDocument& doc);
    void FinishClose(std::size_t index, bool save);
    static void OnNativeSave(void* user);

    ScriptDatabase* m_Database{nullptr};
    std::vector<FxsOpenDocument> m_Docs;
    std::size_t m_Active{0};
    std::string m_Filter;
    mutable std::string m_Source;
    std::string m_Empty;
    std::vector<std::string> m_VisibleNames;
    Validator m_Validator;
    ValidationReporter m_ValidationReporter;
    UiNotify m_UiNotify;
    std::filesystem::path m_SessionPath;
    std::size_t m_StatusLine{1};
    std::size_t m_StatusColumn{1};
    std::string m_StatusInfo{"Ready"};
    std::string m_HeaderText{"No script selected"};
    bool m_WantNativeVisible{false};
    bool m_NativeSuspended{false};
    int m_ValidationDebounceMs{400};
    bool m_ValidationPending{false};
    std::chrono::steady_clock::time_point m_ValidationDue{};
    struct AsyncValidationResult
    {
        std::string ScriptName;
        std::uint64_t Generation{};
        ScriptValidationResult Validation;
    };
    std::future<AsyncValidationResult> m_ValidationJob;
    std::string m_CompilingScript;
    std::uint64_t m_ValidationGeneration{};
    std::optional<std::size_t> m_PendingCloseIndex;
    std::optional<std::size_t> m_ExternalIndex;
    std::string m_CompareDisk;
    std::string m_CompareEditor;
    bool m_Saving{false};

    std::string m_FindQuery;
    std::string m_ReplaceText;
    FxsSearchFlags m_FindFlags{};
    FxsSearchScan m_FindScan{};
    std::size_t m_FindIndex{static_cast<std::size_t>(-1)};
    FxsProjectSearch m_ProjectSearch;
    std::vector<std::string> m_ReplacePreview;
    std::string m_ProjectReplaceQuery;

    std::unique_ptr<FxsCodeEditor> m_Code;
};
}
