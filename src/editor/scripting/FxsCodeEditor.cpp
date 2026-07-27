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
namespace
{
#ifdef _WIN32
constexpr wchar_t kHostClass[] = L"FadixFxsCodeHost";
constexpr int kMarginLine = 0;
constexpr int kMarginError = 1;
constexpr int kMarginFold = 2;
constexpr int kMarkerError = 1;
constexpr int kDiagIndic = 8;
constexpr int kFindIndic = 9;

COLORREF RgbHex(const int r, const int g, const int b)
{
    return RGB(r, g, b);
}

struct HostState
{
    FxsCodeEditor* Owner{nullptr};
};

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<HostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_SIZE:
        if (HWND sci = GetWindow(hwnd, GW_CHILD); sci != nullptr)
        {
            MoveWindow(sci, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;
    case WM_NOTIFY:
        if (state != nullptr && state->Owner != nullptr)
        {
            const auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->code == SCN_ZOOM)
            {
                state->Owner->PersistSettings();
            }
            else if (nmhdr->code == SCN_MARGINCLICK)
            {
                const auto* scn = reinterpret_cast<SCNotification*>(lParam);
                if (scn->margin == kMarginFold)
                {
                    const sptr_t line = SendMessageW(nmhdr->hwndFrom, SCI_LINEFROMPOSITION,
                        static_cast<WPARAM>(scn->position), 0);
                    SendMessageW(nmhdr->hwndFrom, SCI_TOGGLEFOLD, static_cast<WPARAM>(line), 0);
                }
            }
            else if (nmhdr->code == SCN_CHARADDED)
            {
                const auto* scn = reinterpret_cast<SCNotification*>(lParam);
                state->Owner->OnCharAdded(static_cast<int>(scn->ch));
                state->Owner->Tick();
            }
            else if (nmhdr->code == SCN_UPDATEUI)
            {
                state->Owner->Tick();
            }
            else if (nmhdr->code == SCN_AUTOCSELECTIONCHANGE)
            {
                state->Owner->OnAutocompleteSelectionChange();
            }
            else if (nmhdr->code == SCN_AUTOCCOMPLETED)
            {
                const auto* scn = reinterpret_cast<SCNotification*>(lParam);
                if (scn->text != nullptr)
                {
                    state->Owner->OnAutocompleteSelection(scn->text);
                }
            }
            else if (nmhdr->code == SCN_AUTOCCANCELLED)
            {
                state->Owner->CancelAutocomplete();
            }
            else if (nmhdr->code == SCN_DWELLSTART)
            {
                const auto* scn = reinterpret_cast<SCNotification*>(lParam);
                state->Owner->OnDwellStart(static_cast<std::size_t>(std::max<sptr_t>(0, scn->position)));
            }
            else if (nmhdr->code == SCN_DWELLEND)
            {
                state->Owner->OnDwellEnd();
            }
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SciSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR,
    DWORD_PTR refData)
{
    auto* owner = reinterpret_cast<FxsCodeEditor*>(refData);
    if (msg == WM_GETDLGCODE)
    {
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_HASSETSEL;
    }
    // Ctrl+letter also generates WM_CHAR (e.g. Ctrl+S → 0x13). Swallow those or
    // Scintilla inserts junk into the buffer even when KEYDOWN handled save.
    if (msg == WM_CHAR && owner != nullptr)
    {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (control && wParam < 0x20)
        {
            return 0;
        }
    }
    if (msg == WM_KEYDOWN && owner != nullptr)
    {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (control && wParam == VK_TAB)
        {
            owner->QueueUiCommand(shift ? FxsUiCommand::PrevDocument : FxsUiCommand::NextDocument);
            return 0;
        }
        if (control && (wParam == 'W' || wParam == 'w'))
        {
            owner->QueueUiCommand(FxsUiCommand::CloseDocument);
            return 0;
        }
        if (control && (wParam == 'S' || wParam == 's'))
        {
            owner->NotifySave();
            return 0;
        }
        if (control && shift && (wParam == 'F' || wParam == 'f'))
        {
            owner->QueueUiCommand(FxsUiCommand::FindInFiles);
            return 0;
        }
        if (control && !shift && (wParam == 'F' || wParam == 'f'))
        {
            owner->QueueUiCommand(FxsUiCommand::Find);
            return 0;
        }
        if (control && (wParam == 'H' || wParam == 'h'))
        {
            owner->QueueUiCommand(FxsUiCommand::Replace);
            return 0;
        }
        if (control && (wParam == 'G' || wParam == 'g'))
        {
            owner->QueueUiCommand(FxsUiCommand::GotoLine);
            return 0;
        }
        if (control && (wParam == 'P' || wParam == 'p'))
        {
            owner->QueueUiCommand(FxsUiCommand::QuickOpen);
            return 0;
        }
        if (wParam == VK_F3)
        {
            owner->QueueUiCommand(shift ? FxsUiCommand::FindPrev : FxsUiCommand::FindNext);
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            if (SendMessageW(hwnd, SCI_AUTOCACTIVE, 0, 0) != 0
                || SendMessageW(hwnd, SCI_CALLTIPACTIVE, 0, 0) != 0)
            {
                owner->CancelAutocomplete();
                SendMessageW(hwnd, SCI_CALLTIPCANCEL, 0, 0);
                return 0;
            }
            owner->QueueUiCommand(FxsUiCommand::Escape);
            return 0;
        }
        if (control && wParam == VK_SPACE)
        {
            owner->TriggerAutocomplete(true);
            return 0;
        }
        if (control && wParam == VK_OEM_2) // Ctrl+/ (US layout)
        {
            owner->ToggleLineComment();
            return 0;
        }
        if (control && !shift && (wParam == 'D' || wParam == 'd'))
        {
            owner->DuplicateSelectionOrLine();
            return 0;
        }
        if (control && shift && (wParam == 'K' || wParam == 'k'))
        {
            owner->DeleteLine();
            return 0;
        }
        if (control && shift && (wParam == 'W' || wParam == 'w'))
        {
            owner->TrimTrailingWhitespace();
            return 0;
        }
        if (!control && (GetKeyState(VK_MENU) & 0x8000) != 0 && wParam == VK_UP)
        {
            owner->MoveLines(true);
            return 0;
        }
        if (!control && (GetKeyState(VK_MENU) & 0x8000) != 0 && wParam == VK_DOWN)
        {
            owner->MoveLines(false);
            return 0;
        }
        if (wParam == VK_TAB)
        {
            if (SendMessageW(hwnd, SCI_AUTOCACTIVE, 0, 0) != 0)
            {
                return DefSubclassProc(hwnd, msg, wParam, lParam);
            }
            owner->IndentSelection(!shift);
            return 0;
        }
        if (control && (wParam == VK_OEM_PLUS || wParam == VK_ADD))
        {
            owner->ZoomIn();
            return 0;
        }
        if (control && (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT))
        {
            owner->ZoomOut();
            return 0;
        }
        if (control && wParam == '0')
        {
            owner->ZoomReset();
            return 0;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void RegisterHostClassOnce()
{
    static bool registered = false;
    if (registered)
    {
        return;
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHostClass;
    wc.hCursor = LoadCursor(nullptr, IDC_IBEAM);
    RegisterClassW(&wc);
    registered = true;
}

void RegisterScintillaOnce()
{
    static bool registered = false;
    if (registered)
    {
        return;
    }
    Scintilla_RegisterClasses(GetModuleHandleW(nullptr));
    registered = true;
}
#endif
}

FxsCodeEditor::FxsCodeEditor() = default;

FxsCodeEditor::~FxsCodeEditor()
{
    Destroy();
}

void FxsCodeEditor::AttachParent(void* parentHwnd)
{
    m_ParentHwnd = parentHwnd;
    EnsureCreated();
}

void FxsCodeEditor::Destroy()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr)
    {
        RemoveWindowSubclass(static_cast<HWND>(m_SciHwnd), SciSubclassProc, 1);
        DestroyWindow(static_cast<HWND>(m_SciHwnd));
        m_SciHwnd = nullptr;
    }
    if (m_HostHwnd != nullptr)
    {
        auto* state =
            reinterpret_cast<HostState*>(GetWindowLongPtrW(static_cast<HWND>(m_HostHwnd), GWLP_USERDATA));
        DestroyWindow(static_cast<HWND>(m_HostHwnd));
        m_HostHwnd = nullptr;
        delete state;
    }
    m_Dpi = 0;
    m_LayoutW = -1;
    m_LayoutH = -1;
#else
    static_cast<void>(m_ParentHwnd);
#endif
}

std::intptr_t FxsCodeEditor::Send(const unsigned int msg, const std::intptr_t w,
    const std::intptr_t l) const
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return 0;
    }
    return static_cast<std::intptr_t>(
        SendMessageW(static_cast<HWND>(m_SciHwnd), msg, static_cast<WPARAM>(w), static_cast<LPARAM>(l)));
#else
    static_cast<void>(msg);
    static_cast<void>(w);
    static_cast<void>(l);
    return 0;
#endif
}

void FxsCodeEditor::EnsureCreated()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr || m_ParentHwnd == nullptr)
    {
        return;
    }
    RegisterHostClassOnce();
    RegisterScintillaOnce();
    m_Settings = FxsEditorSettings::Load();

    auto* state = new HostState{};
    state->Owner = this;
    HWND parent = static_cast<HWND>(m_ParentHwnd);
    HWND host = CreateWindowExW(0, kHostClass, L"", WS_CHILD, 0, 0, 100, 100, parent, nullptr,
        GetModuleHandleW(nullptr), state);
    if (host == nullptr)
    {
        delete state;
        return;
    }
    m_HostHwnd = host;

    HWND sci = CreateWindowExW(0, L"Scintilla", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_CLIPCHILDREN, 0, 0, 100, 100, host, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (sci == nullptr)
    {
        DestroyWindow(host);
        m_HostHwnd = nullptr;
        delete state;
        return;
    }
    m_SciHwnd = sci;
    SetWindowSubclass(sci, SciSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(host, SW_HIDE);

    ApplyThemeAndChrome();
    ApplyDpiFont();
    ApplyLexer();
    ApplyIndentSettings();
    ApplyWrapAndWhitespace();
    ApplyZoom();
#endif
}

void FxsCodeEditor::SetSaveHandler(SaveHandler handler, void* user)
{
    m_SaveHandler = handler;
    m_SaveUser = user;
}

void FxsCodeEditor::SetVisible(const bool visible)
{
    m_WantVisible = visible;
#ifdef _WIN32
    EnsureCreated();
    if (m_HostHwnd == nullptr)
    {
        return;
    }
    if (!visible)
    {
        // Hidden HWND can keep Win32 focus and still eat WASD / typing during Play.
        ReleaseFocus();
    }
    const bool shown = IsWindowVisible(static_cast<HWND>(m_HostHwnd)) != FALSE;
    if (visible == shown)
    {
        return;
    }
    ShowWindow(static_cast<HWND>(m_HostHwnd), visible ? SW_SHOW : SW_HIDE);
#endif
}

void FxsCodeEditor::ReleaseFocus()
{
#ifdef _WIN32
    if (m_HostHwnd == nullptr && m_SciHwnd == nullptr)
    {
        return;
    }
    const HWND focus = GetFocus();
    const HWND host = static_cast<HWND>(m_HostHwnd);
    const HWND sci = static_cast<HWND>(m_SciHwnd);
    if (focus == nullptr)
    {
        return;
    }
    if (focus == sci || focus == host || (host != nullptr && IsChild(host, focus)))
    {
        if (m_ParentHwnd != nullptr)
        {
            SetFocus(static_cast<HWND>(m_ParentHwnd));
        }
        else
        {
            SetFocus(nullptr);
        }
    }
#endif
}

void FxsCodeEditor::Layout(const int clientX, const int clientY, const int width, const int height)
{
#ifdef _WIN32
    if (m_HostHwnd == nullptr)
    {
        return;
    }
    ApplyDpiFont();
    const int w = std::max(0, width);
    const int h = std::max(0, height);
    if (clientX == m_LayoutX && clientY == m_LayoutY && w == m_LayoutW && h == m_LayoutH)
    {
        return;
    }
    m_LayoutX = clientX;
    m_LayoutY = clientY;
    m_LayoutW = w;
    m_LayoutH = h;
    MoveWindow(static_cast<HWND>(m_HostHwnd), clientX, clientY, w, h, TRUE);
#else
    static_cast<void>(clientX);
    static_cast<void>(clientY);
    static_cast<void>(width);
    static_cast<void>(height);
#endif
}

void FxsCodeEditor::RequestFocus()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || !m_WantVisible)
    {
        return;
    }
    SetFocus(static_cast<HWND>(m_SciHwnd));
#endif
}

bool FxsCodeEditor::HasFocus() const noexcept
{
#ifdef _WIN32
    return m_SciHwnd != nullptr && GetFocus() == static_cast<HWND>(m_SciHwnd);
#else
    return false;
#endif
}

bool FxsCodeEditor::IsCreated() const noexcept
{
    return m_SciHwnd != nullptr;
}

void FxsCodeEditor::SetText(const std::string_view text)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_SETREADONLY, 0);
    Send(SCI_CLEARALL);
    const std::string copy{text};
    Send(SCI_SETTEXT, 0, reinterpret_cast<std::intptr_t>(copy.c_str()));
    Send(SCI_EMPTYUNDOBUFFER);
    Send(SCI_SETSAVEPOINT);
    Send(SCI_GOTOPOS, 0);
#else
    static_cast<void>(text);
#endif
}

std::string FxsCodeEditor::GetText() const
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return {};
    }
    const auto len = static_cast<int>(Send(SCI_GETLENGTH));
    std::string out(static_cast<std::size_t>(len) + 1U, '\0');
    Send(SCI_GETTEXT, static_cast<std::uintptr_t>(len + 1), reinterpret_cast<std::intptr_t>(out.data()));
    out.resize(static_cast<std::size_t>(len));
    // Normalize CRLF → LF for engine assets.
    std::string lf;
    lf.reserve(out.size());
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        if (out[i] == '\r')
        {
            lf.push_back('\n');
            if (i + 1 < out.size() && out[i + 1] == '\n')
            {
                ++i;
            }
        }
        else
        {
            lf.push_back(out[i]);
        }
    }
    return lf;
#else
    return {};
#endif
}

void FxsCodeEditor::Clear()
{
    SetText("");
}

void FxsCodeEditor::SetLanguage(const ScriptLanguage language)
{
    m_Language = language;
    ApplyLexer();
}

void FxsCodeEditor::InsertText(const std::string_view text)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || text.empty())
    {
        return;
    }
    const std::string copy{text};
    Send(SCI_REPLACESEL, 0, reinterpret_cast<std::intptr_t>(copy.c_str()));
#else
    static_cast<void>(text);
#endif
}

void FxsCodeEditor::SelectAll()
{
    Send(SCI_SELECTALL);
}

void FxsCodeEditor::GotoLineColumn(const std::size_t line, const std::size_t column)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const auto lineIndex = static_cast<std::intptr_t>(line > 0 ? line - 1 : 0);
    const auto pos = Send(SCI_POSITIONFROMLINE, lineIndex);
    const auto lineEnd = Send(SCI_GETLINEENDPOSITION, lineIndex);
    const auto caret = std::min(pos + static_cast<std::intptr_t>(column > 0 ? column - 1 : 0), lineEnd);
    Send(SCI_SETSEL, caret, caret);
    Send(SCI_SCROLLCARET);
#else
    static_cast<void>(line);
    static_cast<void>(column);
#endif
}

void FxsCodeEditor::SetSelection(const std::size_t anchor, const std::size_t caret)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_SETSEL, static_cast<std::intptr_t>(anchor), static_cast<std::intptr_t>(caret));
#else
    static_cast<void>(anchor);
    static_cast<void>(caret);
#endif
}

void FxsCodeEditor::EnsureRangeVisible(const std::size_t start, const std::size_t end)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_SCROLLRANGE, static_cast<std::intptr_t>(start), static_cast<std::intptr_t>(end));
    Send(SCI_SCROLLCARET);
#else
    static_cast<void>(start);
    static_cast<void>(end);
#endif
}

bool FxsCodeEditor::TakeModified()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return false;
    }
    if (Send(SCI_GETMODIFY) == 0)
    {
        return false;
    }
    Send(SCI_SETSAVEPOINT);
    return true;
#else
    return false;
#endif
}

std::size_t FxsCodeEditor::Cursor() const
{
    return static_cast<std::size_t>(std::max<std::intptr_t>(0, Send(SCI_GETCURRENTPOS)));
}

std::size_t FxsCodeEditor::SelectionAnchor() const
{
    return static_cast<std::size_t>(std::max<std::intptr_t>(0, Send(SCI_GETANCHOR)));
}

std::size_t FxsCodeEditor::SelectionLength() const
{
    const auto a = static_cast<std::intptr_t>(SelectionAnchor());
    const auto c = static_cast<std::intptr_t>(Cursor());
    return static_cast<std::size_t>(a > c ? a - c : c - a);
}

int FxsCodeEditor::ZoomLevel() const
{
    return static_cast<int>(Send(SCI_GETZOOM));
}

std::size_t FxsCodeEditor::Line() const
{
    const auto pos = Send(SCI_GETCURRENTPOS);
    return static_cast<std::size_t>(Send(SCI_LINEFROMPOSITION, static_cast<std::uintptr_t>(pos))) + 1U;
}

std::size_t FxsCodeEditor::Column() const
{
    const auto pos = Send(SCI_GETCURRENTPOS);
    return static_cast<std::size_t>(Send(SCI_GETCOLUMN, static_cast<std::uintptr_t>(pos))) + 1U;
}

void FxsCodeEditor::SetWordWrap(const bool enabled)
{
    m_Settings.WordWrap = enabled;
    ApplyWrapAndWhitespace();
    PersistSettings();
}

void FxsCodeEditor::SetViewWhitespace(const bool enabled)
{
    m_Settings.ViewWhitespace = enabled;
    ApplyWrapAndWhitespace();
    PersistSettings();
}

void FxsCodeEditor::ToggleWordWrap()
{
    SetWordWrap(!m_Settings.WordWrap);
}

void FxsCodeEditor::ToggleViewWhitespace()
{
    SetViewWhitespace(!m_Settings.ViewWhitespace);
}

void FxsCodeEditor::ZoomIn()
{
    Send(SCI_ZOOMIN);
    m_Settings.Zoom = static_cast<int>(Send(SCI_GETZOOM));
    PersistSettings();
}

void FxsCodeEditor::ZoomOut()
{
    Send(SCI_ZOOMOUT);
    m_Settings.Zoom = static_cast<int>(Send(SCI_GETZOOM));
    PersistSettings();
}

void FxsCodeEditor::ZoomReset()
{
    m_Settings.Zoom = 0;
    ApplyZoom();
    PersistSettings();
}

void FxsCodeEditor::ApplySettings(const FxsEditorSettings& settings)
{
    m_Settings = settings;
    ApplyIndentSettings();
    ApplyWrapAndWhitespace();
    ApplyZoom();
}

void FxsCodeEditor::PersistSettings()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr)
    {
        m_Settings.Zoom = static_cast<int>(Send(SCI_GETZOOM));
    }
#endif
    m_Settings.Save();
}

void FxsCodeEditor::NotifySave()
{
    if (m_SaveHandler != nullptr)
    {
        m_SaveHandler(m_SaveUser);
    }
}

void FxsCodeEditor::QueueUiCommand(const FxsUiCommand command)
{
    m_PendingUi = command;
}

FxsUiCommand FxsCodeEditor::TakeUiCommand()
{
    const FxsUiCommand cmd = m_PendingUi;
    m_PendingUi = FxsUiCommand::None;
    return cmd;
}

void FxsCodeEditor::ClearMatchHighlights()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    constexpr int kFindIndic = 9;
    Send(SCI_SETINDICATORCURRENT, kFindIndic);
    Send(SCI_INDICATORCLEARRANGE, 0, Send(SCI_GETLENGTH));
#endif
}

void FxsCodeEditor::HighlightMatches(const std::vector<FxsSearchMatch>& matches)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_INDICSETSTYLE, kFindIndic, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, kFindIndic, RGB(0x56, 0x9c, 0xd6));
    Send(SCI_INDICSETALPHA, kFindIndic, 60);
    Send(SCI_INDICSETUNDER, kFindIndic, 1);
    Send(SCI_SETINDICATORCURRENT, kFindIndic);
    Send(SCI_INDICATORCLEARRANGE, 0, Send(SCI_GETLENGTH));
    for (const FxsSearchMatch& m : matches)
    {
        if (m.End > m.Start)
        {
            Send(SCI_INDICATORFILLRANGE, static_cast<std::intptr_t>(m.Start),
                static_cast<std::intptr_t>(m.End - m.Start));
        }
    }
#else
    static_cast<void>(matches);
#endif
}

void FxsCodeEditor::ClearDiagnostics()
{
    m_Diagnostics.clear();
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_SETINDICATORCURRENT, kDiagIndic);
    Send(SCI_INDICATORCLEARRANGE, 0, Send(SCI_GETLENGTH));
    Send(SCI_MARKERDELETEALL, kMarkerError);
    OnDwellEnd();
#endif
}

void* FxsCodeEditor::CreateDocument()
{
#ifdef _WIN32
    EnsureCreated();
    if (m_SciHwnd == nullptr)
    {
        return nullptr;
    }
    // bytes=0 → default document size; returns doc pointer with refcount 1.
    return reinterpret_cast<void*>(Send(SCI_CREATEDOCUMENT, 0, 0));
#else
    return nullptr;
#endif
}

void FxsCodeEditor::AddRefDocument(void* doc)
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr && doc != nullptr)
    {
        Send(SCI_ADDREFDOCUMENT, 0, reinterpret_cast<std::intptr_t>(doc));
    }
#else
    static_cast<void>(doc);
#endif
}

void FxsCodeEditor::ReleaseDocument(void* doc)
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr && doc != nullptr)
    {
        Send(SCI_RELEASEDOCUMENT, 0, reinterpret_cast<std::intptr_t>(doc));
    }
#else
    static_cast<void>(doc);
#endif
}

void* FxsCodeEditor::CurrentDocument() const
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(Send(SCI_GETDOCPOINTER));
#else
    return nullptr;
#endif
}

void FxsCodeEditor::SetDocument(void* doc)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || doc == nullptr)
    {
        return;
    }
    // SCI_SETDOCPOINTER releases the current doc before AddRef on the new one.
    // Passing the already-current pointer frees it then AddRefs freed memory.
    if (doc == CurrentDocument())
    {
        return;
    }
    Send(SCI_SETDOCPOINTER, 0, reinterpret_cast<std::intptr_t>(doc));
#else
    static_cast<void>(doc);
#endif
}

void FxsCodeEditor::CaptureViewState(FxsViewState& out) const
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    out.FirstVisibleLine = static_cast<std::size_t>(std::max<std::intptr_t>(0, Send(SCI_GETFIRSTVISIBLELINE)));
    out.XOffset = static_cast<std::size_t>(std::max<std::intptr_t>(0, Send(SCI_GETXOFFSET)));
    out.Anchor = SelectionAnchor();
    out.Caret = Cursor();
    out.Zoom = ZoomLevel();
#else
    static_cast<void>(out);
#endif
}

void FxsCodeEditor::ApplyViewState(const FxsViewState& state)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_SETZOOM, state.Zoom);
    SetSelection(state.Anchor, state.Caret);
    Send(SCI_SETFIRSTVISIBLELINE, static_cast<std::intptr_t>(state.FirstVisibleLine));
    Send(SCI_SETXOFFSET, static_cast<std::intptr_t>(state.XOffset));
#else
    static_cast<void>(state);
#endif
}

void FxsCodeEditor::SetDiagnostics(std::vector<FxsEditorDiagnostic> diagnostics)
{
    ClearDiagnostics();
    m_Diagnostics = std::move(diagnostics);
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(SCI_INDICSETSTYLE, kDiagIndic, INDIC_SQUIGGLE);
    Send(SCI_INDICSETFORE, kDiagIndic, RgbHex(0xf4, 0x47, 0x47));
    Send(SCI_INDICSETUNDER, kDiagIndic, 1);
    Send(SCI_SETINDICATORCURRENT, kDiagIndic);
    for (const auto& d : m_Diagnostics)
    {
        if (d.End > d.Start)
        {
            Send(SCI_INDICATORFILLRANGE, static_cast<std::intptr_t>(d.Start),
                static_cast<std::intptr_t>(d.End - d.Start));
        }
        if (d.Line > 0)
        {
            Send(SCI_MARKERADD, static_cast<std::intptr_t>(d.Line - 1), kMarkerError);
        }
    }
#endif
}

void FxsCodeEditor::OnDwellStart(const std::size_t position)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || m_Diagnostics.empty())
    {
        return;
    }
    for (const auto& d : m_Diagnostics)
    {
        if (position >= d.Start && position < d.End)
        {
            m_DwellTip = d.Message;
            Send(SCI_CALLTIPSHOW, static_cast<std::intptr_t>(position),
                reinterpret_cast<std::intptr_t>(m_DwellTip.c_str()));
            m_CallTipFromCompletion = false;
            return;
        }
    }
#else
    static_cast<void>(position);
#endif
}

void FxsCodeEditor::OnDwellEnd()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr && !m_CallTipFromCompletion)
    {
        // Keep param call tips; only cancel dwell tips (no open '(' tip active check).
        if (Send(SCI_CALLTIPACTIVE) != 0 && !m_DwellTip.empty())
        {
            Send(SCI_CALLTIPCANCEL);
        }
    }
#endif
    m_DwellTip.clear();
}

void FxsCodeEditor::ToggleLineComment()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const std::string text = GetText();
    const auto toggled = ToggleLineComments(text, SelectionAnchor(), Cursor(),
        m_Language == ScriptLanguage::Cpp ? "//" : "--");
    SetText(toggled.Text);
    SetSelection(toggled.Anchor, toggled.Caret);
#endif
}

void FxsCodeEditor::DuplicateSelectionOrLine()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    if (Send(SCI_GETSELECTIONEMPTY) != 0)
    {
        Send(SCI_LINEDUPLICATE);
    }
    else
    {
        Send(SCI_SELECTIONDUPLICATE);
    }
#endif
}

void FxsCodeEditor::MoveLines(const bool up)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(up ? SCI_MOVESELECTEDLINESUP : SCI_MOVESELECTEDLINESDOWN);
#endif
}

void FxsCodeEditor::DeleteLine()
{
#ifdef _WIN32
    if (m_SciHwnd != nullptr)
    {
        Send(SCI_LINEDELETE);
    }
#endif
}

void FxsCodeEditor::TrimTrailingWhitespace()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const auto caret = Cursor();
    const std::string trimmed = fadix::editor::TrimTrailingWhitespace(GetText());
    SetText(trimmed);
    const auto pos = static_cast<std::intptr_t>(std::min(caret, trimmed.size()));
    Send(SCI_GOTOPOS, pos);
#endif
}

void FxsCodeEditor::IndentSelection(const bool forward)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    Send(forward ? SCI_TAB : SCI_BACKTAB);
#endif
}

void FxsCodeEditor::AutoIndentAfterNewline()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const auto pos = Send(SCI_GETCURRENTPOS);
    const auto line = Send(SCI_LINEFROMPOSITION, pos);
    if (line <= 0)
    {
        return;
    }
    const auto prevIndent = Send(SCI_GETLINEINDENTATION, line - 1);
    Send(SCI_SETLINEINDENTATION, line, prevIndent);
    const auto indented = Send(SCI_GETLINEINDENTPOSITION, line);
    Send(SCI_GOTOPOS, indented);
#endif
}

void FxsCodeEditor::MaybeAutoClose(const int ch)
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    char open = 0;
    char close = 0;
    switch (ch)
    {
    case '(': open = '('; close = ')'; break;
    case '[': open = '['; close = ']'; break;
    case '{': open = '{'; close = '}'; break;
    case '"': open = '"'; close = '"'; break;
    case '\'': open = '\''; close = '\''; break;
    default:
        // Skip over existing closer when typing it.
        if (ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'')
        {
            const auto pos = Send(SCI_GETCURRENTPOS);
            if (pos > 0 && Send(SCI_GETCHARAT, pos) == ch)
            {
                Send(SCI_DELETERANGE, pos - 1, 1);
                Send(SCI_GOTOPOS, pos);
            }
        }
        return;
    }
    const std::string text = GetText();
    const auto caret = Cursor();
    if (!ShouldAutoClose(text, caret, open, close))
    {
        return;
    }
    m_SuppressAutoClose = true;
    const char pair[2] = {close, '\0'};
    Send(SCI_INSERTTEXT, static_cast<std::intptr_t>(caret),
        reinterpret_cast<std::intptr_t>(pair));
    m_SuppressAutoClose = false;
#else
    static_cast<void>(ch);
#endif
}

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

void FxsCodeEditor::ApplyMarginStyles()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    // Match editor chrome — STYLECLEARALL / lexer swaps otherwise leave the
    // number margin on the default (often bright) background.
    const COLORREF bg = RgbHex(0x1e, 0x1e, 0x22);
    const COLORREF marginBg = RgbHex(0x1e, 0x1e, 0x22);
    const COLORREF marginFg = RgbHex(0x85, 0x85, 0x85);
    const COLORREF foldFg = RgbHex(0x6e, 0x6e, 0x6e);
    Send(SCI_STYLESETFORE, STYLE_LINENUMBER, marginFg);
    Send(SCI_STYLESETBACK, STYLE_LINENUMBER, marginBg);
    Send(SCI_SETFOLDMARGINCOLOUR, 1, marginBg);
    Send(SCI_SETFOLDMARGINHICOLOUR, 1, marginBg);
    for (int m = SC_MARKNUM_FOLDEREND; m <= SC_MARKNUM_FOLDEROPEN; ++m)
    {
        Send(SCI_MARKERSETFORE, static_cast<std::uintptr_t>(m), foldFg);
        Send(SCI_MARKERSETBACK, static_cast<std::uintptr_t>(m), marginBg);
    }
    Send(SCI_STYLESETBACK, STYLE_DEFAULT, bg);
#else
#endif
}

void FxsCodeEditor::ApplyThemeAndChrome()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    // Fadix dark palette (#1e1e22 bg, VS-like accents).
    const COLORREF bg = RgbHex(0x1e, 0x1e, 0x22);
    const COLORREF fg = RgbHex(0xd4, 0xd4, 0xd4);
    const COLORREF caretLine = RgbHex(0x2a, 0x2a, 0x2e);
    const COLORREF sel = RgbHex(0x26, 0x4f, 0x78);

    Send(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
    Send(SCI_SETCODEPAGE, SC_CP_UTF8);
    Send(SCI_SETIMEINTERACTION, SC_IME_WINDOWED);
    Send(SCI_SETSCROLLWIDTH, 1);
    Send(SCI_SETSCROLLWIDTHTRACKING, 1);
    Send(SCI_SETPHASESDRAW, SC_PHASES_MULTIPLE);
    Send(SCI_SETBUFFEREDDRAW, 1);
    Send(SCI_SETHSCROLLBAR, 1);
    Send(SCI_SETVSCROLLBAR, 1);
    Send(SCI_SETENDATLASTLINE, 0);
    Send(SCI_SETMULTIPLESELECTION, 1);
    Send(SCI_SETADDITIONALSELECTIONTYPING, 1);
    Send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    Send(SCI_SETVIRTUALSPACEOPTIONS, SCVS_RECTANGULARSELECTION);
    Send(SCI_SETMOUSEWHEELCAPTURES, 1);
    Send(SCI_SETCARETLINEVISIBLE, 1);
    Send(SCI_SETCARETLINEBACK, caretLine);
    Send(SCI_SETSELBACK, 1, sel);
    Send(SCI_SETSELFORE, 0, 0);
    Send(SCI_SETINDENTATIONGUIDES, SC_IV_LOOKBOTH);
    Send(SCI_STYLESETBACK, STYLE_DEFAULT, bg);
    Send(SCI_STYLESETFORE, STYLE_DEFAULT, fg);
    Send(SCI_STYLECLEARALL);

    Send(SCI_SETMARGINTYPEN, kMarginLine, SC_MARGIN_NUMBER);
    Send(SCI_SETMARGINWIDTHN, kMarginLine, 48);

    Send(SCI_SETMARGINTYPEN, kMarginError, SC_MARGIN_SYMBOL);
    Send(SCI_SETMARGINWIDTHN, kMarginError, 14);
    Send(SCI_SETMARGINMASKN, kMarginError, 1 << kMarkerError);
    Send(SCI_SETMARGINSENSITIVEN, kMarginError, 0);
    Send(SCI_MARKERDEFINE, kMarkerError, SC_MARK_SHORTARROW);
    Send(SCI_MARKERSETFORE, kMarkerError, RgbHex(0xf4, 0x47, 0x47));
    Send(SCI_MARKERSETBACK, kMarkerError, RgbHex(0xf4, 0x47, 0x47));

    Send(SCI_SETPROPERTY, reinterpret_cast<std::intptr_t>("fold"),
        reinterpret_cast<std::intptr_t>("1"));
    Send(SCI_SETPROPERTY, reinterpret_cast<std::intptr_t>("fold.compact"),
        reinterpret_cast<std::intptr_t>("1"));
    Send(SCI_SETMARGINTYPEN, kMarginFold, SC_MARGIN_SYMBOL);
    Send(SCI_SETMARGINMASKN, kMarginFold, SC_MASK_FOLDERS);
    Send(SCI_SETMARGINWIDTHN, kMarginFold, 16);
    Send(SCI_SETMARGINSENSITIVEN, kMarginFold, 1);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPEN, SC_MARK_BOXMINUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDER, SC_MARK_BOXPLUS);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERSUB, SC_MARK_VLINE);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERTAIL, SC_MARK_LCORNER);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEREND, SC_MARK_BOXPLUSCONNECTED);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED);
    Send(SCI_MARKERDEFINE, SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER);
    Send(SCI_SETFOLDFLAGS, SC_FOLDFLAG_LINEAFTER_CONTRACTED);
    ApplyMarginStyles();

    Send(SCI_STYLESETFORE, STYLE_BRACELIGHT, RgbHex(0x56, 0x9c, 0xd6));
    Send(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
    Send(SCI_STYLESETFORE, STYLE_BRACEBAD, RgbHex(0xf4, 0x47, 0x47));
    Send(SCI_SETMOUSEDWELLTIME, 400);
    constexpr int kFindIndic = 9;
    Send(SCI_INDICSETSTYLE, kFindIndic, INDIC_ROUNDBOX);
    Send(SCI_INDICSETFORE, kFindIndic, RgbHex(0x56, 0x9c, 0xd6));
    Send(SCI_INDICSETALPHA, kFindIndic, 60);
    Send(SCI_INDICSETUNDER, kFindIndic, 1);

    Send(SCI_INDICSETSTYLE, kDiagIndic, INDIC_SQUIGGLE);
    Send(SCI_INDICSETFORE, kDiagIndic, RgbHex(0xf4, 0x47, 0x47));
    Send(SCI_INDICSETUNDER, kDiagIndic, 1);

    Send(SCI_AUTOCSETSEPARATOR, ' ');
    Send(SCI_AUTOCSETTYPESEPARATOR, '?');
    Send(SCI_AUTOCSETIGNORECASE, 1);
    Send(SCI_AUTOCSETCASEINSENSITIVEBEHAVIOUR, SC_CASEINSENSITIVEBEHAVIOUR_RESPECTCASE);
    Send(SCI_AUTOCSETORDER, SC_ORDER_CUSTOM);
    Send(SCI_AUTOCSETCHOOSESINGLE, 0);
    Send(SCI_AUTOCSETCANCELATSTART, 0);
    Send(SCI_AUTOCSETAUTOHIDE, 1);
    Send(SCI_AUTOCSETMAXHEIGHT, 12);
    Send(SCI_CALLTIPSETBACK, RgbHex(0x25, 0x25, 0x2a));
    Send(SCI_CALLTIPSETFORE, fg);
    Send(SCI_CALLTIPSETFOREHLT, RgbHex(0x56, 0x9c, 0xd6));
    RegisterCompletionImages();
#endif
}

void FxsCodeEditor::ApplyDpiFont()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr || m_ParentHwnd == nullptr)
    {
        return;
    }
    const UINT dpi = GetDpiForWindow(static_cast<HWND>(m_ParentHwnd));
    if (dpi == 0 || dpi == m_Dpi)
    {
        return;
    }
    m_Dpi = dpi;
    // 10pt Consolas at 96 DPI; size tracks HWND DPI only (zoom is separate).
    const int points = std::max(1, MulDiv(10, static_cast<int>(dpi), 96));
    Send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<std::intptr_t>("Consolas"));
    Send(SCI_STYLESETSIZE, STYLE_DEFAULT, points);
    Send(SCI_STYLECLEARALL);
    ApplyLexer();
    ApplyMarginStyles();
    // Line-number margin width scales with DPI.
    Send(SCI_SETMARGINWIDTHN, kMarginLine, MulDiv(48, static_cast<int>(dpi), 96));
    Send(SCI_SETMARGINWIDTHN, kMarginFold, MulDiv(16, static_cast<int>(dpi), 96));
#endif
}

void FxsCodeEditor::ApplyLexer()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const char* lexerName = m_Language == ScriptLanguage::Cpp ? "cpp" : "lua";
    Scintilla::ILexer5* lexer = CreateLexer(lexerName);
    Send(SCI_SETILEXER, 0, reinterpret_cast<std::intptr_t>(lexer));

    const COLORREF keyword = RgbHex(0x56, 0x9c, 0xd6);
    const COLORREF string = RgbHex(0xce, 0x91, 0x78);
    const COLORREF number = RgbHex(0xb5, 0xce, 0xa8);
    const COLORREF comment = RgbHex(0x6a, 0x99, 0x55);
    const COLORREF func = RgbHex(0xdc, 0xdc, 0xaa);
    const COLORREF fg = RgbHex(0xd4, 0xd4, 0xd4);
    const COLORREF bg = RgbHex(0x1e, 0x1e, 0x22);

    if (m_Language == ScriptLanguage::Cpp)
    {
        Send(SCI_STYLESETBACK, SCE_C_DEFAULT, bg);
        Send(SCI_STYLESETFORE, SCE_C_DEFAULT, fg);
        Send(SCI_STYLESETFORE, SCE_C_COMMENT, comment);
        Send(SCI_STYLESETFORE, SCE_C_COMMENTLINE, comment);
        Send(SCI_STYLESETFORE, SCE_C_COMMENTDOC, comment);
        Send(SCI_STYLESETFORE, SCE_C_NUMBER, number);
        Send(SCI_STYLESETFORE, SCE_C_WORD, keyword);
        Send(SCI_STYLESETFORE, SCE_C_WORD2, keyword);
        Send(SCI_STYLESETFORE, SCE_C_STRING, string);
        Send(SCI_STYLESETFORE, SCE_C_CHARACTER, string);
        Send(SCI_STYLESETFORE, SCE_C_PREPROCESSOR, keyword);
        Send(SCI_STYLESETFORE, SCE_C_OPERATOR, fg);
        Send(SCI_STYLESETFORE, SCE_C_IDENTIFIER, fg);
        Send(SCI_STYLESETFORE, SCE_C_VERBATIM, string);
        Send(SCI_SETKEYWORDS, 0,
            reinterpret_cast<std::intptr_t>(
                "alignas alignof and and_eq asm auto bitand bitor bool break case catch char "
                "char8_t char16_t char32_t class compl concept const consteval constexpr "
                "constinit const_cast continue co_await co_return co_yield decltype default "
                "delete do double dynamic_cast else enum explicit export extern false final "
                "float for friend goto if inline int long mutable namespace new noexcept not "
                "not_eq nullptr operator or or_eq override private protected public register "
                "reinterpret_cast requires return short signed sizeof static static_assert "
                "static_cast struct switch template this thread_local throw true try typedef "
                "typeid typename union unsigned using virtual void volatile wchar_t while "
                "xor xor_eq"));
        Send(SCI_SETKEYWORDS, 1,
            reinterpret_cast<std::intptr_t>(
                "std string vector map unordered_map optional unique_ptr shared_ptr size_t "
                "int32_t uint32_t int64_t uint64_t"));
    }
    else
    {
        Send(SCI_STYLESETBACK, SCE_LUA_DEFAULT, bg);
        Send(SCI_STYLESETFORE, SCE_LUA_DEFAULT, fg);
        Send(SCI_STYLESETFORE, SCE_LUA_COMMENT, comment);
        Send(SCI_STYLESETFORE, SCE_LUA_COMMENTLINE, comment);
        Send(SCI_STYLESETFORE, SCE_LUA_COMMENTDOC, comment);
        Send(SCI_STYLESETFORE, SCE_LUA_NUMBER, number);
        Send(SCI_STYLESETFORE, SCE_LUA_WORD, keyword);
        Send(SCI_STYLESETFORE, SCE_LUA_WORD2, keyword);
        Send(SCI_STYLESETFORE, SCE_LUA_WORD3, func);
        Send(SCI_STYLESETFORE, SCE_LUA_STRING, string);
        Send(SCI_STYLESETFORE, SCE_LUA_CHARACTER, string);
        Send(SCI_STYLESETFORE, SCE_LUA_LITERALSTRING, string);
        Send(SCI_STYLESETFORE, SCE_LUA_OPERATOR, fg);
        Send(SCI_STYLESETFORE, SCE_LUA_IDENTIFIER, fg);
        Send(SCI_STYLESETFORE, SCE_LUA_LABEL, func);
        Send(SCI_SETKEYWORDS, 0,
            reinterpret_cast<std::intptr_t>(
                "and break do else elseif end false for function goto if in local nil not or "
                "repeat return then true until while"));
        Send(SCI_SETKEYWORDS, 1,
            reinterpret_cast<std::intptr_t>(
                "self entity OnUpdate OnStart OnDestroy"));
        Send(SCI_SETKEYWORDS, 2,
            reinterpret_cast<std::intptr_t>(
                "print pairs ipairs type tostring tonumber require"));
    }
    ApplyMarginStyles();
    Send(SCI_COLOURISE, 0, -1);
#endif
}

void FxsCodeEditor::ApplyIndentSettings()
{
    Send(SCI_SETTABWIDTH, static_cast<std::uintptr_t>(std::max(1, m_Settings.TabWidth)));
    Send(SCI_SETUSETABS, m_Settings.UseTabs ? 1 : 0);
    Send(SCI_SETINDENT, static_cast<std::uintptr_t>(std::max(1, m_Settings.TabWidth)));
}

void FxsCodeEditor::ApplyWrapAndWhitespace()
{
    Send(SCI_SETWRAPMODE, m_Settings.WordWrap ? SC_WRAP_WORD : SC_WRAP_NONE);
    Send(SCI_SETVIEWWS, m_Settings.ViewWhitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
}

void FxsCodeEditor::ApplyZoom()
{
    Send(SCI_SETZOOM, m_Settings.Zoom);
}

void FxsCodeEditor::MatchBraces()
{
#ifdef _WIN32
    if (m_SciHwnd == nullptr)
    {
        return;
    }
    const auto pos = Send(SCI_GETCURRENTPOS);
    auto check = [this](const std::intptr_t at) -> bool {
        if (at < 0)
        {
            return false;
        }
        const int ch = static_cast<int>(Send(SCI_GETCHARAT, at));
        if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}')
        {
            const auto match = Send(SCI_BRACEMATCH, at, 0);
            if (match != -1)
            {
                Send(SCI_BRACEHIGHLIGHT, at, match);
            }
            else
            {
                Send(SCI_BRACEBADLIGHT, at);
            }
            return true;
        }
        return false;
    };
    if (!check(pos) && !check(pos - 1))
    {
        Send(SCI_BRACEHIGHLIGHT, -1, -1);
    }
#endif
}
}
