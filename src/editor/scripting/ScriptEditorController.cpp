#include "editor/scripting/ScriptEditorController.hpp"

#include "assets/ScriptDatabase.hpp"
#include "editor/scripting/FxsDiagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace fadix::editor
{
namespace
{
std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::filesystem::file_time_type ReadWriteTime(const std::filesystem::path& path, bool& ok)
{
    std::error_code error;
    const auto t = std::filesystem::last_write_time(path, error);
    ok = !error;
    return t;
}
}

ScriptEditorController::ScriptEditorController()
    : m_Code{std::make_unique<FxsCodeEditor>()}
{
    m_Code->SetSaveHandler(&ScriptEditorController::OnNativeSave, this);
}

ScriptEditorController::~ScriptEditorController()
{
    for (auto& doc : m_Docs)
    {
        DestroyDocument(doc);
    }
}

void ScriptEditorController::OnNativeSave(void* user)
{
    if (auto* self = static_cast<ScriptEditorController*>(user))
    {
        self->SaveSelected();
    }
}

void ScriptEditorController::Bind(ScriptDatabase& database)
{
    m_Database = &database;
    RefreshScriptList();
    m_StatusInfo = "Ready";
}

void ScriptEditorController::SetValidator(Validator validator, ValidationReporter reporter)
{
    m_Validator = std::move(validator);
    m_ValidationReporter = std::move(reporter);
}

void ScriptEditorController::SetUiNotify(UiNotify notify)
{
    m_UiNotify = std::move(notify);
}

void ScriptEditorController::SetSessionPath(std::filesystem::path path)
{
    m_SessionPath = std::move(path);
}

void ScriptEditorController::NotifyUi()
{
    if (m_UiNotify)
    {
        m_UiNotify();
    }
}

void ScriptEditorController::AttachNativeHost(void* parentHwnd)
{
    if (m_Code)
    {
        m_Code->AttachParent(parentHwnd);
        if (ActiveDoc() != nullptr)
        {
            EnsureSciDocument(*ActiveDoc());
            ShowDocument(m_Active);
        }
    }
}

void ScriptEditorController::SetNativeVisible(const bool visible)
{
    m_WantNativeVisible = visible;
    SyncNativeVisibility();
}

void ScriptEditorController::SuspendNative(const bool suspend)
{
    m_NativeSuspended = suspend;
    SyncNativeVisibility();
    if (suspend && m_Code)
    {
        m_Code->ReleaseFocus();
    }
}

void ScriptEditorController::SyncNativeVisibility()
{
    if (!m_Code)
    {
        return;
    }
    m_Code->SetVisible(m_WantNativeVisible && HasScript() && !m_NativeSuspended);
}

void ScriptEditorController::LayoutNative(
    const int clientX, const int clientY, const int width, const int height)
{
    if (m_Code)
    {
        m_Code->Layout(clientX, clientY, width, height);
    }
}

void ScriptEditorController::RequestNativeFocus()
{
    if (m_Code)
    {
        m_Code->RequestFocus();
    }
}

FxsUiCommand ScriptEditorController::TakeUiCommand()
{
    return m_Code ? m_Code->TakeUiCommand() : FxsUiCommand::None;
}

void ScriptEditorController::QueueUiCommand(const FxsUiCommand command)
{
    if (m_Code)
    {
        m_Code->QueueUiCommand(command);
    }
}

FxsOpenDocument* ScriptEditorController::ActiveDoc()
{
    return m_Active < m_Docs.size() ? &m_Docs[m_Active] : nullptr;
}

const FxsOpenDocument* ScriptEditorController::ActiveDoc() const
{
    return m_Active < m_Docs.size() ? &m_Docs[m_Active] : nullptr;
}

int ScriptEditorController::FindOpenIndex(const std::string& name) const
{
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
    {
        if (m_Docs[i].Name == name)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ScriptEditorController::DestroyDocument(FxsOpenDocument& doc)
{
    if (m_Code && doc.SciDoc != nullptr)
    {
        m_Code->ReleaseDocument(doc.SciDoc);
    }
    doc.SciDoc = nullptr;
}

void ScriptEditorController::EnsureSciDocument(FxsOpenDocument& doc)
{
    if (!m_Code || !m_Code->IsCreated() || doc.SciDoc != nullptr)
    {
        return;
    }
    doc.SciDoc = m_Code->CreateDocument();
    if (doc.SciDoc == nullptr)
    {
        return;
    }
    // Do not stash/restore the previous doc pointer. SCI_SETDOCPOINTER releases the
    // view's document; the anonymous startup doc often has refcount 1, so restoring
    // that pointer is a use-after-free (AV in Editor::SetDocPointer AddRef).
    // Leave the new document current — ShowDocument will no-op SetDocument if same.
    m_Code->SetDocument(doc.SciDoc);
    m_Code->SetLanguage(doc.Language);
    m_Code->SetText(doc.Buffer);
    m_Code->ClearDiagnostics();
}

void ScriptEditorController::StashActiveView()
{
    auto* doc = ActiveDoc();
    if (doc == nullptr)
    {
        return;
    }
    PullActiveFromNative();
    if (m_Code && m_Code->IsCreated() && doc->SciDoc != nullptr)
    {
        m_Code->CaptureViewState(doc->View);
        doc->Diagnostics = m_Code->Diagnostics();
    }
}

void ScriptEditorController::ShowDocument(const std::size_t index)
{
    if (index >= m_Docs.size())
    {
        return;
    }
    m_Active = index;
    auto& doc = m_Docs[index];
    EnsureSciDocument(doc);
    if (m_Code && m_Code->IsCreated() && doc.SciDoc != nullptr)
    {
        m_Code->SetDocument(doc.SciDoc);
        m_Code->SetLanguage(doc.Language);
        m_Code->ApplyViewState(doc.View);
        m_Code->SetDiagnostics(doc.Diagnostics);
    }
    SyncNativeVisibility();
    UpdateStatusBar();
    SetHeader();
    NotifyUi();
}

void ScriptEditorController::PullActiveFromNative()
{
    auto* doc = ActiveDoc();
    if (doc == nullptr)
    {
        return;
    }
    if (m_Code && m_Code->IsCreated() && doc->SciDoc != nullptr
        && m_Code->CurrentDocument() == doc->SciDoc)
    {
        doc->Buffer = m_Code->GetText();
    }
    m_Source = doc->Buffer;
}

void ScriptEditorController::CommitActiveToAsset()
{
    PullActiveFromNative();
    auto* doc = ActiveDoc();
    if (doc == nullptr || m_Database == nullptr)
    {
        return;
    }
    if (auto asset = m_Database->Get(doc->Name))
    {
        ScriptAsset& script = asset->get();
        script.SourceCode.assign(doc->Buffer.begin(), doc->Buffer.end());
        script.Loaded = true;
        script.Dirty = doc->Dirty;
    }
}

void ScriptEditorController::TickNative()
{
    if (m_Code && HasScript())
    {
        m_Code->Tick();
        if (m_Code->TakeModified())
        {
            PullActiveFromNative();
            if (auto* doc = ActiveDoc())
            {
                doc->Dirty = true;
                if (m_Database != nullptr)
                {
                    if (auto asset = m_Database->Get(doc->Name))
                    {
                        asset->get().SourceCode.assign(doc->Buffer.begin(), doc->Buffer.end());
                        asset->get().Loaded = true;
                        asset->get().Dirty = true;
                    }
                }
            }
            SetHeader();
            ScheduleValidation();
        }
        UpdateStatusBar();
    }
    MaybeRunScheduledValidation();
    if (!m_Saving)
    {
        CheckExternalChanges();
    }
}

void ScriptEditorController::RefreshScriptList()
{
    m_VisibleNames.clear();
    if (m_Database == nullptr)
    {
        NotifyUi();
        return;
    }
    const std::string filter = Lower(m_Filter);
    for (const auto& asset : m_Database->All())
    {
        if (filter.empty() || Lower(asset->Name).find(filter) != std::string::npos)
        {
            m_VisibleNames.push_back(asset->Name);
        }
    }
    std::sort(m_VisibleNames.begin(), m_VisibleNames.end(),
        [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    NotifyUi();
}

void ScriptEditorController::SetFilter(const std::string& filter)
{
    m_Filter = filter;
    RefreshScriptList();
}

void ScriptEditorController::RememberDiskTime(FxsOpenDocument& doc)
{
    bool ok = false;
    doc.DiskWriteTime = ReadWriteTime(doc.Path, ok);
    doc.DiskWriteKnown = ok;
    doc.ExternalChanged = false;
}

void ScriptEditorController::OpenScript(const std::string& name)
{
    if (m_Database == nullptr)
    {
        return;
    }
    if (const int existing = FindOpenIndex(name); existing >= 0)
    {
        if (static_cast<std::size_t>(existing) != m_Active)
        {
            StashActiveView();
            ShowDocument(static_cast<std::size_t>(existing));
        }
        RequestNativeFocus();
        return;
    }
    auto asset = m_Database->Get(name);
    if (!asset)
    {
        return;
    }
    ScriptAsset& script = asset->get();
    if (!script.Loaded && !m_Database->LoadSource(script))
    {
        return;
    }

    StashActiveView();
    FxsOpenDocument doc;
    doc.Name = name;
    doc.Path = script.SourcePath;
    doc.Language = script.Language;
    doc.Buffer.assign(script.SourceCode.begin(), script.SourceCode.end());
    doc.Buffer.erase(std::remove(doc.Buffer.begin(), doc.Buffer.end(), '\r'), doc.Buffer.end());
    doc.Dirty = script.Dirty;
    RememberDiskTime(doc);
    m_Docs.push_back(std::move(doc));
    ShowDocument(m_Docs.size() - 1);
    RequestNativeFocus();
    RefreshScriptList();
    ScheduleValidation();
}

void ScriptEditorController::SelectScript(const std::string& name)
{
    OpenScript(name);
}

void ScriptEditorController::SelectScriptAt(
    const std::string& name, const std::size_t line, const std::size_t column)
{
    OpenScript(name);
    GotoLineColumnInDocument(line, column);
}

void ScriptEditorController::ActivateTab(const std::size_t index)
{
    if (index >= m_Docs.size() || index == m_Active)
    {
        return;
    }
    StashActiveView();
    ShowDocument(index);
    RequestNativeFocus();
}

void ScriptEditorController::NextDocument(const bool forward)
{
    if (m_Docs.size() < 2)
    {
        return;
    }
    const std::size_t next = forward ? (m_Active + 1) % m_Docs.size()
                                     : (m_Active + m_Docs.size() - 1) % m_Docs.size();
    ActivateTab(next);
}

bool ScriptEditorController::CreateNew(const ScriptLanguage language,
    const std::filesystem::path& folder,
    const std::string& preferredName)
{
    if (m_Database == nullptr)
    {
        return false;
    }
    std::string name = preferredName.empty() ? "NewScript" : preferredName;
    if (!preferredName.empty() && m_Database->Get(name))
    {
        return false;
    }
    for (int suffix = 1; preferredName.empty() && m_Database->Get(name); ++suffix)
    {
        name = "NewScript" + std::to_string(suffix);
    }
    if (m_Database->CreateNew(name, language, folder) == nullptr)
    {
        return false;
    }
    RefreshScriptList();
    OpenScript(name);
    return true;
}

bool ScriptEditorController::WriteDocumentToDisk(FxsOpenDocument& doc)
{
    if (m_Database == nullptr)
    {
        return false;
    }
    if (auto asset = m_Database->Get(doc.Name))
    {
        asset->get().SourceCode.assign(doc.Buffer.begin(), doc.Buffer.end());
        asset->get().Loaded = true;
        asset->get().Dirty = true;
    }
    m_Saving = true;
    const bool ok = m_Database->Save(doc.Name);
    m_Saving = false;
    if (ok)
    {
        doc.Dirty = false;
        RememberDiskTime(doc);
        if (auto asset = m_Database->Get(doc.Name))
        {
            asset->get().Dirty = false;
        }
    }
    return ok;
}

void ScriptEditorController::SaveSelected()
{
    auto* doc = ActiveDoc();
    if (doc == nullptr)
    {
        return;
    }
    if (doc->ExternalChanged)
    {
        m_ExternalIndex = m_Active;
        m_CompareDisk = doc->DiskSnapshot;
        m_CompareEditor = doc->Buffer;
        m_StatusInfo = "File changed on disk — choose Reload / Keep / Compare";
        NotifyUi();
        return;
    }
    CommitActiveToAsset();
    if (WriteDocumentToDisk(*doc))
    {
        ValidateActive();
    }
    SetHeader();
    UpdateStatusBar();
    RefreshScriptList();
}

void ScriptEditorController::SaveAll()
{
    StashActiveView();
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
    {
        auto& doc = m_Docs[i];
        if (!doc.Dirty)
        {
            continue;
        }
        if (doc.ExternalChanged)
        {
            continue; // never silently overwrite
        }
        if (m_Code && m_Code->IsCreated() && doc.SciDoc != nullptr)
        {
            m_Code->SetDocument(doc.SciDoc);
            doc.Buffer = m_Code->GetText();
        }
        if (m_Database != nullptr)
        {
            if (auto asset = m_Database->Get(doc.Name))
            {
                asset->get().SourceCode.assign(doc.Buffer.begin(), doc.Buffer.end());
                asset->get().Loaded = true;
            }
        }
        static_cast<void>(WriteDocumentToDisk(doc));
    }
    if (ActiveDoc() != nullptr)
    {
        ShowDocument(m_Active);
    }
    SetHeader();
    UpdateStatusBar();
    RefreshScriptList();
}

FxsCloseDecision ScriptEditorController::RequestCloseActive()
{
    return RequestCloseAt(m_Active);
}

FxsCloseDecision ScriptEditorController::RequestCloseAt(const std::size_t index)
{
    if (index >= m_Docs.size())
    {
        return FxsCloseDecision::Closed;
    }
    if (m_Docs[index].Dirty)
    {
        m_PendingCloseIndex = index;
        return FxsCloseDecision::NeedsSavePrompt;
    }
    FinishClose(index, false);
    return FxsCloseDecision::Closed;
}

void ScriptEditorController::ConfirmCloseSave()
{
    if (!m_PendingCloseIndex)
    {
        return;
    }
    const std::size_t index = *m_PendingCloseIndex;
    m_PendingCloseIndex.reset();
    FinishClose(index, true);
}

void ScriptEditorController::ConfirmCloseDiscard()
{
    if (!m_PendingCloseIndex)
    {
        return;
    }
    const std::size_t index = *m_PendingCloseIndex;
    m_PendingCloseIndex.reset();
    FinishClose(index, false);
}

void ScriptEditorController::CancelClose()
{
    m_PendingCloseIndex.reset();
}

std::string ScriptEditorController::PendingCloseName() const
{
    if (!m_PendingCloseIndex || *m_PendingCloseIndex >= m_Docs.size())
    {
        return {};
    }
    return m_Docs[*m_PendingCloseIndex].Name;
}

void ScriptEditorController::FinishClose(const std::size_t index, const bool save)
{
    if (index >= m_Docs.size())
    {
        return;
    }
    if (index == m_Active)
    {
        StashActiveView();
    }
    else if (m_Code && m_Code->IsCreated() && m_Docs[index].SciDoc != nullptr)
    {
        // pull buffer from that doc
        void* prev = m_Code->CurrentDocument();
        m_Code->SetDocument(m_Docs[index].SciDoc);
        m_Docs[index].Buffer = m_Code->GetText();
        if (prev != nullptr)
        {
            m_Code->SetDocument(prev);
        }
    }
    if (save)
    {
        if (m_Docs[index].ExternalChanged)
        {
            m_PendingCloseIndex = index; // still blocked
            m_ExternalIndex = index;
            return;
        }
        static_cast<void>(WriteDocumentToDisk(m_Docs[index]));
    }
    else if (m_Database != nullptr)
    {
        if (auto asset = m_Database->Get(m_Docs[index].Name))
        {
            // discard: reload from disk into asset
            m_Database->LoadSource(asset->get());
        }
    }
    DestroyDocument(m_Docs[index]);
    m_Docs.erase(m_Docs.begin() + static_cast<std::ptrdiff_t>(index));
    if (m_Docs.empty())
    {
        m_Active = 0;
        m_Source.clear();
        if (m_Code)
        {
            m_Code->Clear();
            m_Code->ClearDiagnostics();
        }
        SyncNativeVisibility();
        SetHeader();
        UpdateStatusBar();
        return;
    }
    if (m_Active > index)
    {
        --m_Active;
    }
    else if (m_Active >= m_Docs.size())
    {
        m_Active = m_Docs.size() - 1;
    }
    ShowDocument(m_Active);
}

const std::string& ScriptEditorController::SelectedScript() const
{
    if (const auto* doc = ActiveDoc())
    {
        return doc->Name;
    }
    return m_Empty;
}

const std::string& ScriptEditorController::Source() const
{
    const_cast<ScriptEditorController*>(this)->PullActiveFromNative();
    if (const auto* doc = ActiveDoc())
    {
        return doc->Buffer;
    }
    return m_Empty;
}

bool ScriptEditorController::HasScript() const noexcept
{
    return ActiveDoc() != nullptr;
}

bool ScriptEditorController::Editing() const noexcept
{
    return HasScript() && m_WantNativeVisible;
}

bool ScriptEditorController::Dirty() const noexcept
{
    const auto* doc = ActiveDoc();
    return doc != nullptr && doc->Dirty;
}

bool ScriptEditorController::NativeFocused() const noexcept
{
    return m_Code && m_Code->HasFocus();
}

std::size_t ScriptEditorController::Cursor() const
{
    if (m_Code && m_Code->IsCreated())
    {
        return m_Code->Cursor();
    }
    return ActiveDoc() != nullptr ? ActiveDoc()->View.Caret : 0;
}

std::size_t ScriptEditorController::SelectionAnchor() const
{
    if (m_Code && m_Code->IsCreated())
    {
        return m_Code->SelectionAnchor();
    }
    return ActiveDoc() != nullptr ? ActiveDoc()->View.Anchor : 0;
}

std::size_t ScriptEditorController::SelectionLength() const
{
    if (m_Code && m_Code->IsCreated())
    {
        return m_Code->SelectionLength();
    }
    const auto a = SelectionAnchor();
    const auto c = Cursor();
    return a > c ? a - c : c - a;
}

std::span<const std::string> ScriptEditorController::VisibleNames() const noexcept
{
    return m_VisibleNames;
}

std::span<const FxsOpenDocument> ScriptEditorController::OpenDocuments() const noexcept
{
    return m_Docs;
    }

std::vector<std::string> ScriptEditorController::AllScriptNames() const
{
    std::vector<std::string> names;
    if (m_Database == nullptr)
    {
        return names;
    }
    for (const auto& asset : m_Database->All())
    {
        names.push_back(asset->Name);
    }
    std::sort(names.begin(), names.end(),
        [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return names;
}

bool ScriptEditorController::NameDirty(const std::string& name) const
{
    if (const int i = FindOpenIndex(name); i >= 0)
    {
        return m_Docs[static_cast<std::size_t>(i)].Dirty;
    }
    if (m_Database != nullptr)
    {
    if (auto asset = m_Database->Get(name))
    {
        return asset->get().Dirty;
        }
    }
    return false;
}

ScriptLanguage ScriptEditorController::LanguageOf(const std::string& name) const
{
    if (const int i = FindOpenIndex(name); i >= 0)
    {
        return m_Docs[static_cast<std::size_t>(i)].Language;
    }
    if (m_Database != nullptr)
    {
        if (auto asset = m_Database->Get(name))
        {
            return asset->get().Language;
        }
    }
    return ScriptLanguage::Lua;
}

const char* ScriptEditorController::LanguageLabel() const noexcept
{
    if (const auto* doc = ActiveDoc())
    {
        return doc->Language == ScriptLanguage::Cpp ? "C++" : "FXS";
    }
    return "FXS";
}

std::string ScriptEditorController::IndentModeLabel() const
{
    const FxsEditorSettings& s = m_Code ? m_Code->Settings() : FxsEditorSettings{};
    if (s.UseTabs)
    {
        return "Tabs: " + std::to_string(std::max(1, s.TabWidth));
    }
    return "Spaces: " + std::to_string(std::max(1, s.TabWidth));
}

int ScriptEditorController::ZoomPercent() const
{
    return 100 + (m_Code ? m_Code->ZoomLevel() : 0) * 10;
}

const std::optional<ScriptValidationResult>& ScriptEditorController::LastValidation() const noexcept
{
    static const std::optional<ScriptValidationResult> kNone;
    if (const auto* doc = ActiveDoc())
    {
        return doc->Validation;
    }
    return kNone;
}

const std::vector<FxsEditorDiagnostic>& ScriptEditorController::Diagnostics() const
{
    static const std::vector<FxsEditorDiagnostic> kEmpty;
    if (m_Code && ActiveDoc() != nullptr)
    {
        return m_Code->Diagnostics();
    }
    if (const auto* doc = ActiveDoc())
    {
        return doc->Diagnostics;
    }
    return kEmpty;
}

void ScriptEditorController::InsertText(const std::string_view text)
{
    if (!HasScript())
    {
        return;
    }
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->InsertText(text);
        PullActiveFromNative();
    }
    else if (auto* doc = ActiveDoc())
    {
        const std::size_t begin = std::min(doc->View.Anchor, doc->View.Caret);
        const std::size_t end = std::max(doc->View.Anchor, doc->View.Caret);
        doc->Buffer.erase(begin, end - begin);
        doc->Buffer.insert(begin, text);
        doc->View.Caret = begin + text.size();
        doc->View.Anchor = doc->View.Caret;
        m_Source = doc->Buffer;
    }
    MarkDirty();
}

void ScriptEditorController::SelectAll()
{
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->SelectAll();
        return;
    }
    if (auto* doc = ActiveDoc())
    {
        doc->View.Anchor = 0;
        doc->View.Caret = doc->Buffer.size();
    }
}

void ScriptEditorController::ToggleWordWrap()
{
    if (m_Code)
    {
        m_Code->ToggleWordWrap();
    }
}

void ScriptEditorController::ToggleViewWhitespace()
{
    if (m_Code)
    {
        m_Code->ToggleViewWhitespace();
    }
}

bool ScriptEditorController::WordWrap() const noexcept
{
    return m_Code && m_Code->WordWrap();
}

bool ScriptEditorController::ViewWhitespace() const noexcept
{
    return m_Code && m_Code->ViewWhitespace();
}

void ScriptEditorController::SetFindQuery(std::string query)
{
    m_FindQuery = std::move(query);
    RefreshFindMatches();
}

void ScriptEditorController::SetReplaceText(std::string text)
{
    m_ReplaceText = std::move(text);
}

void ScriptEditorController::SetFindFlags(const FxsSearchFlags flags)
{
    m_FindFlags = flags;
    RefreshFindMatches();
}

void ScriptEditorController::RefreshFindMatches()
{
    PullActiveFromNative();
    m_FindScan = ScanDocument(Source(), m_FindQuery, m_FindFlags);
    m_FindIndex = static_cast<std::size_t>(-1);
    SyncFindHighlights();
}

void ScriptEditorController::SyncFindHighlights()
{
    if (!m_Code)
    {
        return;
    }
    if (!m_FindScan.Ok || m_FindQuery.empty())
    {
        m_Code->ClearMatchHighlights();
        return;
    }
    m_Code->HighlightMatches(m_FindScan.Matches);
}

void ScriptEditorController::ClearFindHighlights()
{
    if (m_Code)
    {
        m_Code->ClearMatchHighlights();
    }
}

void ScriptEditorController::ApplyFindSelection()
{
    if (m_FindIndex >= m_FindScan.Matches.size())
    {
        return;
    }
    const auto& m = m_FindScan.Matches[m_FindIndex];
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->SetSelection(m.Start, m.End);
        m_Code->EnsureRangeVisible(m.Start, m.End);
    }
    if (auto* doc = ActiveDoc())
    {
        doc->View.Anchor = m.Start;
        doc->View.Caret = m.End;
    }
    UpdateStatusBar();
}

bool ScriptEditorController::FindNext(const bool forward)
{
    RefreshFindMatches();
    if (m_FindScan.Matches.empty())
    {
        return false;
    }
    m_FindIndex = NextMatchIndex(m_FindScan.Matches, Cursor(), forward);
    ApplyFindSelection();
    return true;
}

bool ScriptEditorController::ReplaceCurrent()
{
    if (m_FindIndex >= m_FindScan.Matches.size())
    {
        if (!FindNext(true))
        {
            return false;
        }
    }
    const auto m = m_FindScan.Matches[m_FindIndex];
    PullActiveFromNative();
    if (auto* doc = ActiveDoc())
    {
        doc->Buffer.replace(m.Start, m.End - m.Start, m_ReplaceText);
        if (m_Code && m_Code->IsCreated())
        {
            m_Code->SetText(doc->Buffer);
            m_Code->SetSelection(m.Start, m.Start + m_ReplaceText.size());
        }
        MarkDirty();
    }
    RefreshFindMatches();
    return FindNext(true);
}

FxsReplaceAllResult ScriptEditorController::ReplaceAll()
{
    PullActiveFromNative();
    auto result = ReplaceAllInDocument(Source(), m_FindQuery, m_ReplaceText, m_FindFlags);
    if (result.Ok && result.Count > 0)
    {
        if (auto* doc = ActiveDoc())
        {
            doc->Buffer = result.Text;
            if (m_Code && m_Code->IsCreated())
            {
                m_Code->SetText(doc->Buffer);
            }
            MarkDirty();
        }
    }
    RefreshFindMatches();
    return result;
}

std::string ScriptEditorController::FindStatusLabel() const
{
    if (m_FindQuery.empty())
    {
        return {};
    }
    if (!m_FindScan.Ok)
    {
        return m_FindScan.Error;
    }
    if (m_FindScan.Matches.empty())
    {
        return "No matches";
    }
    const std::size_t n = m_FindIndex == static_cast<std::size_t>(-1) ? 0 : m_FindIndex + 1;
    return std::to_string(n) + " of " + std::to_string(m_FindScan.Matches.size());
}

void ScriptEditorController::GotoLineColumnInDocument(const std::size_t line, const std::size_t column)
{
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->GotoLineColumn(line, column);
        RequestNativeFocus();
        TickNative();
        return;
    }
    PullActiveFromNative();
    const std::string& src = Source();
    std::size_t offset = 0;
    std::size_t atLine = 1;
    while (offset < src.size() && atLine < line)
    {
        if (src[offset] == '\n')
        {
            ++atLine;
        }
        ++offset;
    }
    const std::size_t lineStart = offset;
    while (offset < src.size() && src[offset] != '\n' && offset - lineStart + 1 < column)
    {
        ++offset;
    }
    if (auto* doc = ActiveDoc())
    {
        doc->View.Caret = offset;
        doc->View.Anchor = offset;
    }
    UpdateStatusBar();
}

void ScriptEditorController::TrimTrailingWhitespace()
{
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->TrimTrailingWhitespace();
        PullActiveFromNative();
    }
    else if (auto* doc = ActiveDoc())
    {
        doc->Buffer = fadix::editor::TrimTrailingWhitespace(doc->Buffer);
    }
    MarkDirty();
}

void ScriptEditorController::ToggleLineComment()
{
    if (m_Code && m_Code->IsCreated())
    {
        m_Code->ToggleLineComment();
        PullActiveFromNative();
    }
    else if (auto* doc = ActiveDoc())
    {
        const auto toggled = ToggleLineComments(doc->Buffer, doc->View.Anchor, doc->View.Caret,
            doc->Language == ScriptLanguage::Cpp ? "//" : "--");
        doc->Buffer = toggled.Text;
        doc->View.Anchor = toggled.Anchor;
        doc->View.Caret = toggled.Caret;
    }
    MarkDirty();
}

void ScriptEditorController::SetValidationDebounceMs(const int ms)
{
    m_ValidationDebounceMs = std::max(0, ms);
}

void ScriptEditorController::ScheduleValidation()
    {
    m_ValidationPending = true;
    m_ValidationDue = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(m_ValidationDebounceMs);
    }

void ScriptEditorController::MaybeRunScheduledValidation()
    {
    if (!m_ValidationPending || std::chrono::steady_clock::now() < m_ValidationDue)
    {
        return;
    }
    FlushPendingValidation();
}

void ScriptEditorController::FlushPendingValidation()
{
    m_ValidationPending = false;
    ValidateActive();
}

void ScriptEditorController::ValidateActive()
{
    auto* doc = ActiveDoc();
    if (doc == nullptr || !m_Validator || m_Database == nullptr)
    {
        return;
    }
    CommitActiveToAsset();
    if (auto asset = m_Database->Get(doc->Name))
    {
        ScriptValidationResult result = m_Validator(asset->get());
        if (!result.Success)
        {
            const auto parsed = ParseLuaCompileError(result.Message);
            if (parsed.Ok)
            {
                if (!parsed.Message.empty())
                {
                    result.Message = parsed.Message;
                }
                if (parsed.HasLocation)
                {
                    result.Line = parsed.Line;
                    result.Column = parsed.Column;
                }
            }
        }
        ApplyValidationResult(*doc, result);
    }
}

void ScriptEditorController::ApplyValidationResult(
    FxsOpenDocument& doc, const ScriptValidationResult& result)
{
    doc.Validation = result;
    if (result.Success)
    {
        doc.Diagnostics.clear();
        if (m_Code && ActiveDoc() == &doc)
        {
            m_Code->ClearDiagnostics();
        }
    }
    else
    {
        FxsEditorDiagnostic diag;
        diag.Line = result.Line;
        diag.Column = result.Column;
        diag.Message = result.Message;
        const auto range = SquiggleRangeForLineColumn(doc.Buffer, result.Line, result.Column);
        diag.Start = range.Start;
        diag.End = range.End;
        doc.Diagnostics = {diag};
        if (m_Code && ActiveDoc() == &doc)
        {
            m_Code->SetDiagnostics(doc.Diagnostics);
        }
    }
    if (m_ValidationReporter && m_Database != nullptr)
    {
        if (auto asset = m_Database->Get(doc.Name))
        {
            m_ValidationReporter(asset->get(), result);
        }
    }
    UpdateStatusBar();
}

void ScriptEditorController::MarkDirty()
{
    if (auto* doc = ActiveDoc())
    {
        doc->Dirty = true;
        CommitActiveToAsset();
    }
    SetHeader();
    ScheduleValidation();
    UpdateStatusBar();
    RefreshScriptList();
}

void ScriptEditorController::UpdateStatusBar()
{
    if (m_Code && m_Code->IsCreated() && HasScript())
    {
        m_StatusLine = m_Code->Line();
        m_StatusColumn = m_Code->Column();
    }
    else if (const auto* doc = ActiveDoc())
    {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < doc->View.Caret && i < doc->Buffer.size(); ++i)
        {
            if (doc->Buffer[i] == '\n')
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }
    m_StatusLine = line;
    m_StatusColumn = column;
    }
    else
    {
        m_StatusLine = 1;
        m_StatusColumn = 1;
    }

    if (!HasScript())
    {
        m_StatusInfo = "Ready";
    }
    else if (ActiveDoc()->ExternalChanged)
    {
        m_StatusInfo = "External change detected";
    }
    else if (ActiveDoc()->Validation && !ActiveDoc()->Validation->Success)
    {
        m_StatusInfo = ActiveDoc()->Validation->Message;
    }
    else
    {
        m_StatusInfo = ActiveDoc()->Dirty ? "Unsaved changes" : "Saved";
    }
    NotifyUi();
}

void ScriptEditorController::SetHeader()
{
    if (!HasScript())
    {
        m_HeaderText = "No script selected";
    }
    else
    {
        m_HeaderText = ActiveDoc()->Name + (ActiveDoc()->Dirty ? " *" : "");
        if (ActiveDoc()->ExternalChanged)
        {
            m_HeaderText += " !";
        }
    }
    NotifyUi();
}

bool ScriptEditorController::SaveSession() const
{
    if (m_SessionPath.empty())
    {
        return false;
    }
    FxsEditorSession session;
    for (const auto& doc : m_Docs)
    {
        session.Open.push_back(FxsSessionDoc{doc.Name});
    }
    if (const auto* doc = ActiveDoc())
    {
        session.Active = doc->Name;
    }
    std::error_code error;
    std::filesystem::create_directories(m_SessionPath.parent_path(), error);
    std::ofstream out{m_SessionPath, std::ios::binary | std::ios::trunc};
    if (!out)
    {
        return false;
    }
    out << SerializeFxsSession(session);
    return static_cast<bool>(out);
}

bool ScriptEditorController::LoadSession()
{
    if (m_SessionPath.empty())
    {
        return false;
    }
    std::ifstream in{m_SessionPath, std::ios::binary};
    if (!in)
    {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    FxsEditorSession session;
    if (!ParseFxsSession(buffer.str(), session))
    {
        return false;
    }
    for (const auto& item : session.Open)
    {
        OpenScript(item.Name);
    }
    if (!session.Active.empty())
    {
        OpenScript(session.Active);
    }
    return true;
}

void ScriptEditorController::CheckExternalChanges()
{
    for (std::size_t i = 0; i < m_Docs.size(); ++i)
    {
        auto& doc = m_Docs[i];
        if (!doc.DiskWriteKnown || doc.Path.empty())
        {
            continue;
        }
        bool ok = false;
        const auto now = ReadWriteTime(doc.Path, ok);
        if (!ok || now == doc.DiskWriteTime)
        {
            continue;
        }
        std::ifstream in{doc.Path, std::ios::binary};
        if (!in)
        {
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string disk = buffer.str();
        disk.erase(std::remove(disk.begin(), disk.end(), '\r'), disk.end());
        PullActiveFromNative();
        if (i == m_Active)
        {
            PullActiveFromNative();
        }
        else if (m_Code && m_Code->IsCreated() && doc.SciDoc != nullptr)
        {
            void* prev = m_Code->CurrentDocument();
            m_Code->SetDocument(doc.SciDoc);
            doc.Buffer = m_Code->GetText();
            if (prev)
            {
                m_Code->SetDocument(prev);
            }
        }
        if (disk == doc.Buffer)
        {
            doc.DiskWriteTime = now;
            continue;
        }
        doc.ExternalChanged = true;
        doc.DiskSnapshot = std::move(disk);
        doc.DiskWriteTime = now;
        if (!m_ExternalIndex)
        {
            m_ExternalIndex = i;
            m_CompareDisk = doc.DiskSnapshot;
            m_CompareEditor = doc.Buffer;
        }
        SetHeader();
    UpdateStatusBar();
    }
}

bool ScriptEditorController::HasExternalConflict() const
{
    return m_ExternalIndex.has_value();
}

std::string ScriptEditorController::ExternalConflictName() const
{
    if (!m_ExternalIndex || *m_ExternalIndex >= m_Docs.size())
    {
        return {};
    }
    return m_Docs[*m_ExternalIndex].Name;
}

void ScriptEditorController::ResolveExternal(const FxsExternalDecision decision)
{
    if (!m_ExternalIndex || *m_ExternalIndex >= m_Docs.size())
    {
        return;
    }
    const std::size_t index = *m_ExternalIndex;
    auto& doc = m_Docs[index];
    if (decision == FxsExternalDecision::Compare)
    {
        m_CompareDisk = doc.DiskSnapshot;
        m_CompareEditor = doc.Buffer;
        // keep prompt open for Keep/Reload after compare
        return;
    }
    if (decision == FxsExternalDecision::Reload)
    {
        doc.Buffer = doc.DiskSnapshot;
        doc.Dirty = false;
        doc.ExternalChanged = false;
        RememberDiskTime(doc);
        if (m_Database != nullptr)
        {
            if (auto asset = m_Database->Get(doc.Name))
            {
                asset->get().SourceCode.assign(doc.Buffer.begin(), doc.Buffer.end());
                asset->get().Dirty = false;
                asset->get().Loaded = true;
            }
        }
        if (index == m_Active)
        {
            if (m_Code && m_Code->IsCreated())
            {
                EnsureSciDocument(doc);
                m_Code->SetDocument(doc.SciDoc);
                m_Code->SetText(doc.Buffer);
            }
            ScheduleValidation();
        }
        else if (doc.SciDoc != nullptr && m_Code && m_Code->IsCreated())
        {
            void* prev = m_Code->CurrentDocument();
            m_Code->SetDocument(doc.SciDoc);
            m_Code->SetText(doc.Buffer);
            if (prev)
            {
                m_Code->SetDocument(prev);
            }
        }
    }
    else if (decision == FxsExternalDecision::KeepEditor)
    {
        doc.ExternalChanged = false;
        // next Save will overwrite disk intentionally
        RememberDiskTime(doc);
    }
    m_ExternalIndex.reset();
    SetHeader();
    UpdateStatusBar();
    RefreshScriptList();
}

void ScriptEditorController::DebugMarkExternalChange(const std::string& name, std::string diskText)
{
    const int i = FindOpenIndex(name);
    if (i < 0)
    {
        return;
    }
    auto& doc = m_Docs[static_cast<std::size_t>(i)];
    doc.ExternalChanged = true;
    doc.DiskSnapshot = std::move(diskText);
    doc.DiskWriteKnown = true;
    m_ExternalIndex = static_cast<std::size_t>(i);
    m_CompareDisk = doc.DiskSnapshot;
    m_CompareEditor = doc.Buffer;
    SetHeader();
}

void ScriptEditorController::StartFindInFiles(std::string query, const FxsSearchFlags flags)
{
    m_ProjectReplaceQuery = query;
    FxsProjectSearchRequest req;
    req.Query = std::move(query);
    req.Flags = flags;
    req.ScriptsOnly = true;
    if (m_Database != nullptr)
    {
        req.Roots.push_back(m_Database->ScriptsFolder());
    }
    m_ProjectSearch.Start(std::move(req));
}

std::size_t ScriptEditorController::PreviewReplaceInFiles(const std::string_view replacement)
{
    m_ReplacePreview = FxsProjectSearch::PreviewReplace(m_ProjectSearch.SnapshotHits(), replacement);
    return m_ReplacePreview.size();
}

std::size_t ScriptEditorController::ConfirmReplaceInFiles(const std::string_view replacement)
{
    const auto hits = m_ProjectSearch.SnapshotHits();
    const std::size_t files = FxsProjectSearch::ApplyReplaceInFiles(
        hits, m_ProjectReplaceQuery, replacement, {});
    // Reload any open docs that were on disk-only changes carefully
    for (auto& doc : m_Docs)
    {
        if (doc.Dirty)
        {
            continue;
        }
        bool ok = false;
        const auto t = ReadWriteTime(doc.Path, ok);
        if (ok && doc.DiskWriteKnown && t != doc.DiskWriteTime)
        {
            std::ifstream in{doc.Path, std::ios::binary};
            if (in)
            {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                doc.Buffer = buffer.str();
                doc.Buffer.erase(std::remove(doc.Buffer.begin(), doc.Buffer.end(), '\r'), doc.Buffer.end());
                RememberDiskTime(doc);
            }
        }
    }
    if (ActiveDoc() != nullptr)
    {
        ShowDocument(m_Active);
    }
    return files;
}
}
