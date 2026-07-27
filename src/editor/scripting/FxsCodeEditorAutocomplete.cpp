// Split out of FxsCodeEditor.cpp to reduce god-file size. Definitions only;
// all declarations remain in editor/scripting/FxsCodeEditor.hpp. Autocomplete,
// call-tip and completion-image plumbing. No behavior change.
#include "editor/scripting/FxsCodeEditor.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

extern "C" Scintilla::ILexer5* __stdcall CreateLexer(const char* name);
extern "C" int Scintilla_RegisterClasses(void* hInstance);

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#pragma comment(lib, "Comctl32.lib")
#endif


namespace fadix::editor
{

void FxsCodeEditor::Tick()
{
    MatchBraces();
    if (m_Language == ScriptLanguage::Lua && Send(SCI_CALLTIPACTIVE) != 0 && !m_CallTipFromCompletion)
    {
        UpdateCallTip();
    }
}

void FxsCodeEditor::CancelAutocomplete()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr)
    {
        Send(SCI_AUTOCCANCEL);
        if (m_CallTipFromCompletion)
        {
            Send(SCI_CALLTIPCANCEL);
            m_CallTipFromCompletion = false;
        }
    }
#endif
    m_Suggestions.clear();
    m_CompletionDocs.clear();
}

void FxsCodeEditor::RegisterCompletionImages()
{
#ifdef _WIN32
    // SCI_REGISTERIMAGE takes a char* that Scintilla XPM::Init treats as lines-form
    // (const char* const*) unless it starts with "/* XPM */". A single concatenated
    // string is NOT lines-form — atoi reads string bytes as a pointer and AVs.
    // Pass real line arrays (classic XPM lines form).
    static const char* kFunctionXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #DCDCAA",
        "x	c #1E1E22",
        "            ",
        "  ........  ",
        "  .xxxxxx.  ",
        "  .x....x.  ",
        "  .x.xx.x.  ",
        "  .x....x.  ",
        "  .x.xxxx.  ",
        "  .x....x.  ",
        "  .xxxxxx.  ",
        "  ........  ",
        "            ",
        "            ",
    };
    static const char* kPropertyXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #569CD6",
        "x	c #1E1E22",
        "            ",
        "   ......   ",
        "  ..xxxx..  ",
        "  .x....x.  ",
        "  .x....x.  ",
        "  .x....x.  ",
        "  .x....x.  ",
        "  ..xxxx..  ",
        "   ......   ",
        "            ",
        "            ",
        "            ",
    };
    static const char* kVariableXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #9CDCFE",
        "x	c #1E1E22",
        "            ",
        "  ..    ..  ",
        "  .x.  .x.  ",
        "  .x.  .x.  ",
        "  .x.  .x.  ",
        "  .x....x.  ",
        "  .xxxxxx.  ",
        "  .x....x.  ",
        "  .x    x.  ",
        "            ",
        "            ",
        "            ",
    };
    static const char* kKeywordXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #C586C0",
        "x	c #1E1E22",
        "            ",
        "  ........  ",
        "  .xxxxxx.  ",
        "  .x....x.  ",
        "  .x.xxx..  ",
        "  .x....x.  ",
        "  .x.xxx..  ",
        "  .x....x.  ",
        "  .xxxxxx.  ",
        "  ........  ",
        "            ",
        "            ",
    };
    static const char* kSnippetXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #CE9178",
        "x	c #1E1E22",
        "            ",
        " ........   ",
        " .xxxxxx.   ",
        " .x....x.   ",
        " .x....x..  ",
        " .x....xx.  ",
        " .x......x. ",
        " .xxxxxxx.  ",
        " ........   ",
        "            ",
        "            ",
        "            ",
    };
    static const char* kModuleXpm[] = {
        "12 12 3 1",
        " 	c None",
        ".	c #4EC9B0",
        "x	c #1E1E22",
        "            ",
        " .......... ",
        " .xxxxxxxx. ",
        " .x......x. ",
        " .x.xxxx.x. ",
        " .x.x..x.x. ",
        " .x.xxxx.x. ",
        " .x......x. ",
        " .xxxxxxxx. ",
        " .......... ",
        "            ",
        "            ",
    };
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Function),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kFunctionXpm)));
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Property),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kPropertyXpm)));
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Variable),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kVariableXpm)));
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Keyword),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kKeywordXpm)));
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Snippet),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kSnippetXpm)));
    Send(SCI_REGISTERIMAGE, static_cast<std::intptr_t>(FxsCompletionKind::Module),
        reinterpret_cast<std::intptr_t>(static_cast<const char* const*>(kModuleXpm)));
#endif
}

void FxsCodeEditor::ShowSuggestions(
    const std::vector<FxsSuggestion>& suggestions, const std::size_t prefixLen)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    m_Suggestions = suggestions;
    if (m_Suggestions.empty())
    {
        Send(SCI_AUTOCCANCEL);
        m_CompletionDocs.clear();
        return;
    }
    std::string list;
    for (std::size_t i = 0; i < m_Suggestions.size(); ++i)
    {
        if (i != 0)
        {
            list.push_back(' ');
        }
        list += m_Suggestions[i].Label;
        list.push_back('?');
        list += std::to_string(static_cast<int>(m_Suggestions[i].Kind));
    }
    Send(SCI_AUTOCSHOW, static_cast<std::intptr_t>(prefixLen),
        reinterpret_cast<std::intptr_t>(list.c_str()));
    OnAutocompleteSelectionChange();
#else
    static_cast<void>(suggestions);
    static_cast<void>(prefixLen);
#endif
}

void FxsCodeEditor::TriggerAutocomplete(const bool force)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || m_Language != ScriptLanguage::Lua)
    {
        return;
    }
    const std::string doc = GetText();
    const std::size_t cursor = Cursor();
    const auto ctx = FxsApiCatalog::AnalyzeContext(doc, cursor);
    if (ctx.InStringOrComment)
    {
        CancelAutocomplete();
        return;
    }
    if (!force && !ctx.MemberAccess && ctx.Prefix.empty())
    {
        return;
    }
    FxsSuggestQuery query;
    query.Document = doc;
    query.Cursor = cursor;
    query.Force = force;
    const auto suggestions = FxsApiCatalog::Suggest(query);
    ShowSuggestions(suggestions, ctx.Prefix.size());
#else
    static_cast<void>(force);
#endif
}

void FxsCodeEditor::OnCharAdded(const int ch)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    if (ch == '\n' || ch == '\r')
    {
        AutoIndentAfterNewline();
    }
    else if (!m_SuppressAutoClose)
    {
        MaybeAutoClose(ch);
    }
    if (m_Language == ScriptLanguage::Lua)
    {
        if (ch == '(')
        {
            m_CallTipFromCompletion = false;
            UpdateCallTip();
            return;
        }
        if (ch == ')')
        {
            Send(SCI_CALLTIPCANCEL);
            m_CallTipFromCompletion = false;
            return;
        }
        const bool ident = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
        if (ident || ch == '.' || ch == ':')
        {
            TriggerAutocomplete(false);
        }
    }
#else
    static_cast<void>(ch);
#endif
}

void FxsCodeEditor::OnAutocompleteSelectionChange()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || m_Suggestions.empty())
    {
        return;
    }
    const auto idx = static_cast<std::size_t>(Send(SCI_AUTOCGETCURRENT));
    if (idx >= m_Suggestions.size())
    {
        return;
    }
    const FxsSuggestion& s = m_Suggestions[idx];
    m_CompletionDocs = s.Signature;
    if (!s.Documentation.empty())
    {
        if (!m_CompletionDocs.empty())
        {
            m_CompletionDocs.push_back('\n');
        }
        m_CompletionDocs += s.Documentation;
    }
    if (!m_CompletionDocs.empty())
    {
        const auto pos = Send(SCI_GETCURRENTPOS);
        Send(SCI_CALLTIPSHOW, pos, reinterpret_cast<std::intptr_t>(m_CompletionDocs.c_str()));
        m_CallTipFromCompletion = true;
    }
#endif
}

void FxsCodeEditor::OnAutocompleteSelection(const std::string_view selected)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const FxsSuggestion* hit = nullptr;
    for (const auto& s : m_Suggestions)
    {
        if (s.Label == selected)
        {
            hit = &s;
            break;
        }
    }
    if (hit != nullptr && hit->InsertText != selected)
    {
        const auto pos = Send(SCI_GETCURRENTPOS);
        const auto start = pos - static_cast<std::intptr_t>(selected.size());
        if (start >= 0)
        {
            Send(SCI_SETTARGETRANGE, start, pos);
            Send(SCI_REPLACETARGET, static_cast<std::intptr_t>(hit->InsertText.size()),
                reinterpret_cast<std::intptr_t>(hit->InsertText.c_str()));
        }
    }
    m_Suggestions.clear();
    if (m_CallTipFromCompletion)
    {
        Send(SCI_CALLTIPCANCEL);
        m_CallTipFromCompletion = false;
    }
    m_CompletionDocs.clear();
#else
    static_cast<void>(selected);
#endif
}

void FxsCodeEditor::UpdateCallTip()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || m_Language != ScriptLanguage::Lua)
    {
        return;
    }
    const std::string doc = GetText();
    const auto tip = FxsApiCatalog::BuildCallTip(doc, Cursor(), {});
    if (!tip.Active)
    {
        if (!m_CallTipFromCompletion)
        {
            Send(SCI_CALLTIPCANCEL);
        }
        return;
    }
    const auto pos = Send(SCI_GETCURRENTPOS);
    Send(SCI_CALLTIPSHOW, pos, reinterpret_cast<std::intptr_t>(tip.Text.c_str()));
    if (tip.HighlightEnd > tip.HighlightStart)
    {
        Send(SCI_CALLTIPSETHLT, tip.HighlightStart, tip.HighlightEnd);
    }
    m_CallTipFromCompletion = false;
#endif
}

} // namespace fadix::editor
