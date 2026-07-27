#include "editor/imgui/panels/ScriptEditorPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "assets/ScriptDatabase.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
#endif

namespace fadix::editor
{
namespace
{
void ImGuiRectToClientPixels(const ImVec2& min, const ImVec2& size, int& x, int& y, int& w, int& h)
{
    const ImVec2 fb = ImGui::GetIO().DisplayFramebufferScale;
    x = static_cast<int>(std::lround(min.x * fb.x));
    y = static_cast<int>(std::lround(min.y * fb.y));
    w = static_cast<int>(std::lround(size.x * fb.x));
    h = static_cast<int>(std::lround(size.y * fb.y));
}

void FocusSdlWindow(SDL_Window* window)
{
#ifdef _WIN32
    if (window == nullptr)
    {
        return;
    }
    if (void* hwnd = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
    {
        SetFocus(static_cast<HWND>(hwnd));
    }
#else
    static_cast<void>(window);
#endif
}

bool ParseGoto(const char* text, std::size_t& line, std::size_t& column)
{
    line = 1;
    column = 1;
    if (text == nullptr || text[0] == '\0')
    {
        return false;
    }
    int l = 0;
    int c = 1;
    if (std::sscanf(text, "%d:%d", &l, &c) == 2 || std::sscanf(text, "%d,%d", &l, &c) == 2)
    {
        line = static_cast<std::size_t>(std::max(1, l));
        column = static_cast<std::size_t>(std::max(1, c));
        return true;
    }
    if (std::sscanf(text, "%d", &l) == 1)
    {
        line = static_cast<std::size_t>(std::max(1, l));
        column = 1;
        return true;
    }
    return false;
}
}

void ScriptEditorPanel::Bind(
    ScriptEditorController& editor, ScriptDatabase* scripts, SDL_Window* window)
{
    m_Editor = &editor;
    m_Scripts = scripts;
    m_Window = window;
#ifdef _WIN32
    if (window != nullptr)
    {
        if (void* hwnd = SDL_GetPointerProperty(
                SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
        {
            editor.AttachNativeHost(hwnd);
        }
    }
#endif
    std::snprintf(m_SearchBuf, sizeof(m_SearchBuf), "%s", editor.Filter().c_str());
}

void ScriptEditorPanel::OpenFind(const bool withReplace)
{
    m_ShowFind = true;
    m_ShowReplace = withReplace;
    m_FocusFind = true;
    FocusSdlWindow(m_Window);
    if (m_Editor != nullptr)
    {
        std::snprintf(m_FindBuf, sizeof(m_FindBuf), "%s", m_Editor->FindQuery().c_str());
        std::snprintf(m_ReplaceBuf, sizeof(m_ReplaceBuf), "%s", m_Editor->ReplaceText().c_str());
        m_Editor->SetFindQuery(m_FindBuf);
    }
}

void ScriptEditorPanel::CloseFind()
{
    m_ShowFind = false;
    m_ShowReplace = false;
    m_ConfirmReplaceAll = false;
    if (m_Editor != nullptr)
    {
        m_Editor->ClearFindHighlights();
        m_Editor->RequestNativeFocus();
    }
}

void ScriptEditorPanel::OpenGoto()
{
    m_ShowGoto = true;
    m_FocusGoto = true;
    m_GotoBuf[0] = '\0';
    FocusSdlWindow(m_Window);
    ImGui::OpenPopup("##fxs_goto");
}

void ScriptEditorPanel::OpenQuickOpen()
{
    m_ShowQuickOpen = true;
    m_FocusQuickOpen = true;
    m_QuickOpenBuf[0] = '\0';
    FocusSdlWindow(m_Window);
    ImGui::OpenPopup("##fxs_quick_open");
}

void ScriptEditorPanel::OpenFindInFiles()
{
    m_ShowFindInFiles = true;
    m_FocusProjectFind = true;
    std::snprintf(m_ProjectFindBuf, sizeof(m_ProjectFindBuf), "%s", m_Editor->FindQuery().c_str());
}

void ScriptEditorPanel::HandleCommands(EditorUiState& ui)
{
    if (m_Editor == nullptr)
    {
        return;
    }
    const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const ImGuiIO& io = ImGui::GetIO();
    if (panelFocused && !io.WantTextInput)
    {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F))
        {
            OpenFindInFiles();
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F))
        {
            OpenFind(false);
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_H))
        {
            OpenFind(true);
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
        {
            OpenGoto();
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
        {
            OpenQuickOpen();
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_W))
        {
            if (m_Editor->RequestCloseActive() == FxsCloseDecision::NeedsSavePrompt)
            {
                ImGui::OpenPopup("##fxs_close_dirty");
            }
        }
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Tab))
        {
            m_Editor->NextDocument(!io.KeyShift);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_F3))
        {
            m_Editor->FindNext(!io.KeyShift);
        }
    }

    switch (m_Editor->TakeUiCommand())
    {
    case FxsUiCommand::Find:
        OpenFind(false);
        break;
    case FxsUiCommand::Replace:
        OpenFind(true);
        break;
    case FxsUiCommand::FindNext:
        m_Editor->FindNext(true);
        break;
    case FxsUiCommand::FindPrev:
        m_Editor->FindNext(false);
        break;
    case FxsUiCommand::GotoLine:
        OpenGoto();
        break;
    case FxsUiCommand::QuickOpen:
        OpenQuickOpen();
        break;
    case FxsUiCommand::FindInFiles:
        OpenFindInFiles();
        break;
    case FxsUiCommand::NextDocument:
        m_Editor->NextDocument(true);
        break;
    case FxsUiCommand::PrevDocument:
        m_Editor->NextDocument(false);
        break;
    case FxsUiCommand::CloseDocument:
        if (m_Editor->RequestCloseActive() == FxsCloseDecision::NeedsSavePrompt)
        {
            ImGui::OpenPopup("##fxs_close_dirty");
        }
        break;
    case FxsUiCommand::Escape:
        if (m_ShowFindInFiles)
        {
            m_Editor->ProjectSearch().Cancel();
            m_ShowFindInFiles = false;
        }
        else if (m_ShowFind)
        {
            CloseFind();
        }
        break;
    case FxsUiCommand::None:
        break;
    }
    static_cast<void>(ui);
}

void ScriptEditorPanel::DrawTabs(EditorUiState& ui)
{
    const auto docs = m_Editor->OpenDocuments();
    if (docs.empty())
    {
        return;
    }
    for (std::size_t i = 0; i < docs.size(); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        std::string label = docs[i].Name;
        if (docs[i].Dirty)
        {
            label += " *";
        }
        if (docs[i].ExternalChanged)
        {
            label += " !";
        }
        const bool selected = i == m_Editor->ActiveTabIndex();
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_None, ImVec2{0, 0}))
        {
            m_Editor->ActivateTab(i);
        }
        if (ImGui::BeginPopupContextItem("##tabmenu"))
        {
            if (ImGui::MenuItem("Close"))
            {
                if (m_Editor->RequestCloseAt(i) == FxsCloseDecision::NeedsSavePrompt)
                {
                    ImGui::OpenPopup("##fxs_close_dirty");
                }
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    static_cast<void>(ui);
}

void ScriptEditorPanel::DrawClosePrompt(EditorUiState& ui)
{
    if (!m_Editor->HasPendingClose())
    {
        return;
    }
    ImGui::OpenPopup("##fxs_close_dirty");
    if (ImGui::BeginPopupModal("##fxs_close_dirty", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Save changes to %s?", m_Editor->PendingCloseName().c_str());
        if (ImGui::Button("Save"))
        {
            m_Editor->ConfirmCloseSave();
            ImGui::CloseCurrentPopup();
            ui.StatusText = "Saved and closed";
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save"))
        {
            m_Editor->ConfirmCloseDiscard();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_Editor->CancelClose();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ScriptEditorPanel::DrawExternalPrompt(EditorUiState& ui)
{
    if (!m_Editor->HasExternalConflict())
    {
        return;
    }
    ImGui::OpenPopup("##fxs_external");
    if (ImGui::BeginPopupModal("##fxs_external", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("%s changed on disk.", m_Editor->ExternalConflictName().c_str());
        ImGui::TextUnformatted("Reload discards editor edits. Keep Editor overwrites disk on Save.");
        if (ImGui::Button("Reload"))
        {
            m_Editor->ResolveExternal(FxsExternalDecision::Reload);
            ImGui::CloseCurrentPopup();
            ui.StatusText = "Reloaded from disk";
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep Editor Version"))
        {
            m_Editor->ResolveExternal(FxsExternalDecision::KeepEditor);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Compare"))
        {
            m_Editor->ResolveExternal(FxsExternalDecision::Compare);
        }
        if (ImGui::CollapsingHeader("Compare", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginChild("##disk", ImVec2{280, 160}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Disk");
            ImGui::Separator();
            ImGui::TextUnformatted(m_Editor->ExternalDiskText().c_str());
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("##editor", ImVec2{280, 160}, ImGuiChildFlags_Borders);
            ImGui::TextUnformatted("Editor");
            ImGui::Separator();
            ImGui::TextUnformatted(m_Editor->ExternalEditorText().c_str());
            ImGui::EndChild();
        }
        ImGui::EndPopup();
    }
}

void ScriptEditorPanel::DrawFindInFiles(EditorUiState& ui)
{
    if (!m_ShowFindInFiles)
    {
        return;
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Find in Files");
    if (m_FocusProjectFind)
    {
        ImGui::SetKeyboardFocusHere();
        m_FocusProjectFind = false;
    }
    ImGui::SetNextItemWidth(220.0F);
    const bool go = ImGui::InputText("##proj_find", m_ProjectFindBuf, sizeof(m_ProjectFindBuf),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || go)
    {
        m_ProjectHits.clear();
        m_Editor->StartFindInFiles(m_ProjectFindBuf);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel Search"))
    {
        m_Editor->ProjectSearch().Cancel();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close##fif"))
    {
        m_ShowFindInFiles = false;
    }
    ImGui::TextDisabled("%s", m_Editor->ProjectSearch().Status().c_str());

    for (auto& hit : m_Editor->ProjectSearch().TakeNewHits())
    {
        m_ProjectHits.push_back(std::move(hit));
    }

    ImGui::BeginChild("##fif_results", ImVec2{0, 140}, ImGuiChildFlags_Borders);
    for (std::size_t i = 0; i < m_ProjectHits.size(); ++i)
    {
        const auto& h = m_ProjectHits[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label = h.Path.filename().string() + ':' + std::to_string(h.Line) + "  "
            + h.Snippet;
        if (ImGui::Selectable(label.c_str()))
        {
            ui.ShowScriptEditor = true;
            m_Editor->SelectScriptAt(h.ScriptName, h.Line, h.Column);
            m_Editor->RequestNativeFocus();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(180.0F);
    ImGui::InputText("Replace with", m_ProjectReplaceBuf, sizeof(m_ProjectReplaceBuf));
    ImGui::SameLine();
    if (ImGui::Button("Preview Replace"))
    {
        m_Editor->PreviewReplaceInFiles(m_ProjectReplaceBuf);
        m_ConfirmProjectReplace = true;
        ImGui::OpenPopup("##fxs_replace_files");
    }
    if (ImGui::BeginPopupModal("##fxs_replace_files", &m_ConfirmProjectReplace,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Replace in %zu matches?", m_Editor->ProjectSearch().HitCount());
        for (const auto& line : m_Editor->ReplacePreview())
        {
            ImGui::TextUnformatted(line.c_str());
        }
        if (ImGui::Button("Confirm Replace"))
        {
            const std::size_t n = m_Editor->ConfirmReplaceInFiles(m_ProjectReplaceBuf);
            ui.StatusText = "Replaced in " + std::to_string(n) + " files";
            m_ConfirmProjectReplace = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_ConfirmProjectReplace = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ScriptEditorPanel::DrawToolbar(
    EditorUiState& ui, const std::filesystem::path& createFolder)
{
    auto tip = [](const char* text) {
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s", text);
        }
    };
    auto toggle = [&](const char* label, const char* tipText, const bool active, auto&& onClick) {
        if (active)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        }
        if (ImGui::SmallButton(label))
        {
            onClick();
        }
        tip(tipText);
        if (active)
        {
            ImGui::PopStyleColor();
        }
        ImGui::SameLine();
    };

    if (ImGui::SmallButton(FADIX_ICON_PLUS " Lua"))
    {
        m_PendingLua = true;
        std::snprintf(m_NameBuf, sizeof(m_NameBuf), "NewScript");
    }
    tip("New FXS / Lua script");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_PLUS " C++"))
    {
        m_PendingCpp = true;
        std::snprintf(m_NameBuf, sizeof(m_NameBuf), "NewScript");
    }
    tip("New native C++ script");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::InputTextWithHint(
            "##script_search", FADIX_ICON_SEARCH " Filter…", m_SearchBuf, sizeof(m_SearchBuf)))
    {
        m_Editor->SetFilter(m_SearchBuf);
    }
    tip("Filter script list. Ctrl+F finds in the open file.");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_SAVE " Save"))
    {
        m_Editor->SaveSelected();
        ui.StatusText = "Saved script";
    }
    tip("Save (Ctrl+S)");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_SAVE " All"))
    {
        m_Editor->SaveAll();
        ui.StatusText = "Saved all scripts";
    }
    tip("Save all dirty scripts");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    toggle(FADIX_ICON_TEXT_WIDTH " Wrap", "Toggle word wrap", m_Editor->WordWrap(),
        [&] { m_Editor->ToggleWordWrap(); });
    toggle(FADIX_ICON_PARAGRAPH " Spaces", "Show whitespace", m_Editor->ViewWhitespace(),
        [&] { m_Editor->ToggleViewWhitespace(); });
    if (ImGui::SmallButton(FADIX_ICON_COMMENT))
    {
        m_Editor->ToggleLineComment();
    }
    tip("Toggle line comment (Ctrl+/)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Trim"))
    {
        m_Editor->TrimTrailingWhitespace();
        ui.StatusText = "Trimmed trailing whitespace";
    }
    tip("Trim trailing whitespace (Ctrl+Shift+W)");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_SEARCH " Find in Files"))
    {
        OpenFindInFiles();
    }
    tip("Find in project scripts (Ctrl+Shift+F)");
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::SmallButton(FADIX_ICON_XMARK " Close"))
    {
        if (m_Editor->RequestCloseActive() == FxsCloseDecision::NeedsSavePrompt)
        {
            ImGui::OpenPopup("##fxs_close_dirty");
        }
    }
    tip("Close active document (Ctrl+W)");

    if (m_PendingLua || m_PendingCpp)
    {
        ImGui::OpenPopup("##new_script");
    }
    if (ImGui::BeginPopupModal("##new_script", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", m_NameBuf, sizeof(m_NameBuf));
        if (ImGui::Button("Create"))
        {
            const ScriptLanguage lang =
                m_PendingCpp ? ScriptLanguage::Cpp : ScriptLanguage::Lua;
            if (m_Editor->CreateNew(lang, createFolder, m_NameBuf))
            {
                ui.StatusText = std::string{"Created "} + m_NameBuf;
            }
            else
            {
                ui.StatusText = "Could not create script";
            }
            m_PendingLua = false;
            m_PendingCpp = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_PendingLua = false;
            m_PendingCpp = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    static_cast<void>(createFolder);
}

void ScriptEditorPanel::DrawFindBar(EditorUiState& ui)
{
    if (!m_ShowFind || m_Editor == nullptr)
    {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    if (ImGui::BeginChild("##fxs_find_bar", ImVec2{0.0F, m_ShowReplace ? 72.0F : 40.0F},
            ImGuiChildFlags_Borders))
    {
        FxsSearchFlags flags = m_Editor->FindFlags();
        ImGui::SetNextItemWidth(220.0F);
        if (m_FocusFind)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusFind = false;
        }
        const bool findChanged = ImGui::InputTextWithHint("##fxs_find", "Find in document",
            m_FindBuf, sizeof(m_FindBuf),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (findChanged || (ImGui::IsItemDeactivatedAfterEdit()))
        {
            m_Editor->SetFindQuery(m_FindBuf);
        }
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter))
        {
            m_Editor->SetFindQuery(m_FindBuf);
            m_Editor->FindNext(!ImGui::GetIO().KeyShift);
        }
        if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            CloseFind();
        }

        ImGui::SameLine();
        if (ImGui::Checkbox("Aa", &flags.MatchCase))
        {
            m_Editor->SetFindFlags(flags);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Match case");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("W", &flags.WholeWord))
        {
            m_Editor->SetFindFlags(flags);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Whole word");
        }
        ImGui::SameLine();
        if (ImGui::Checkbox(".*", &flags.Regex))
        {
            m_Editor->SetFindFlags(flags);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Regular expression");
        }
        ImGui::SameLine();
        if (ImGui::Button("Next"))
        {
            m_Editor->SetFindQuery(m_FindBuf);
            m_Editor->FindNext(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Prev"))
        {
            m_Editor->SetFindQuery(m_FindBuf);
            m_Editor->FindNext(false);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(m_Editor->FindStatusLabel().c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
        {
            CloseFind();
        }

        if (m_ShowReplace)
        {
            ImGui::SetNextItemWidth(220.0F);
            if (m_FocusReplace)
            {
                ImGui::SetKeyboardFocusHere();
                m_FocusReplace = false;
            }
            if (ImGui::InputTextWithHint(
                    "##fxs_replace", "Replace with", m_ReplaceBuf, sizeof(m_ReplaceBuf)))
            {
                m_Editor->SetReplaceText(m_ReplaceBuf);
            }
            ImGui::SameLine();
            if (ImGui::Button("Replace"))
            {
                m_Editor->SetFindQuery(m_FindBuf);
                m_Editor->SetReplaceText(m_ReplaceBuf);
                if (m_Editor->ReplaceCurrent())
                {
                    ui.StatusText = "Replaced match";
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Replace All"))
            {
                m_Editor->SetFindQuery(m_FindBuf);
                m_Editor->SetReplaceText(m_ReplaceBuf);
                m_Editor->RefreshFindMatches();
                m_PendingReplaceCount = m_Editor->FindScan().Matches.size();
                if (!m_Editor->FindScan().Ok)
                {
                    ui.StatusText = "Invalid regex";
                }
                else if (m_PendingReplaceCount == 0)
                {
                    ui.StatusText = "No matches to replace";
                }
                else
                {
                    m_ConfirmReplaceAll = true;
                    ImGui::OpenPopup("##fxs_replace_all");
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (m_ConfirmReplaceAll &&
        ImGui::BeginPopupModal("##fxs_replace_all", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Replace %zu occurrence%s?", m_PendingReplaceCount,
            m_PendingReplaceCount == 1 ? "" : "s");
        if (ImGui::Button("Replace All"))
        {
            const FxsReplaceAllResult result = m_Editor->ReplaceAll();
            if (!result.Ok)
            {
                ui.StatusText = "Invalid regex";
            }
            else
            {
                ui.StatusText = "Replaced " + std::to_string(result.Count) + " matches";
            }
            m_ConfirmReplaceAll = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_ConfirmReplaceAll = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void ScriptEditorPanel::DrawList()
{
    if (ImGui::BeginChild("##script_list", ImVec2{180.0F, 0.0F}, ImGuiChildFlags_Borders))
    {
        const auto names = m_Editor->VisibleNames();
        for (const std::string& name : names)
        {
            const bool selected = name == m_Editor->SelectedScript();
            const std::string label =
                name + (m_Editor->NameDirty(name) ? " *" : "") + " [" +
                (m_Editor->LanguageOf(name) == ScriptLanguage::Cpp ? "C++" : "FXS") + "]";
            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_Editor->SelectScript(name);
            }
        }
        if (names.empty())
        {
            ImGui::TextDisabled("No scripts");
        }
    }
    ImGui::EndChild();
}

void ScriptEditorPanel::LayoutOverlay(const bool suspend)
{
    if (m_Editor == nullptr)
    {
        return;
    }
    m_Editor->SuspendNative(suspend);
    if (suspend)
    {
        return;
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 size = ImGui::GetItemRectSize();
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    ImGuiRectToClientPixels(min, size, x, y, w, h);
    m_Editor->LayoutNative(x, y, w, h);
    m_Editor->TickNative();
}

void ScriptEditorPanel::Draw(EditorUiState& ui, const std::filesystem::path& createFolder)
{
    if (!ui.ShowScriptEditor || m_Editor == nullptr)
    {
        if (m_Editor != nullptr)
        {
            m_Editor->SetNativeVisible(false);
        }
        return;
    }

    if (!ImGui::Begin(FADIX_ICON_CODE " FXS Editor###Script Editor", &ui.ShowScriptEditor))
    {
        m_Editor->SetNativeVisible(false);
        ImGui::End();
        return;
    }

    HandleCommands(ui);
    DrawToolbar(ui, createFolder);
    DrawTabs(ui);
    ImGui::TextUnformatted(m_Editor->HeaderText().c_str());
    DrawFindBar(ui);
    DrawFindInFiles(ui);
    ImGui::Separator();

    DrawList();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::BeginChild(
        "##script_code", ImVec2{0.0F, -ImGui::GetFrameHeightWithSpacing()}, ImGuiChildFlags_Borders);
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::IsItemClicked())
    {
        m_Editor->RequestNativeFocus();
    }

    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool dockDragging = g.MovingWindow != nullptr ||
        (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsWindowHovered(
            ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    const bool findCapturing = (m_ShowFind || m_ShowFindInFiles)
        && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    // Docked-but-unfocused / Play: Scintilla must not keep Win32 focus or WASD
    // types into the script instead of driving the game.
    const bool panelFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool playing =
        ui.PlayModeLabel == "Playing" || ui.PlayModeLabel == "Paused";
    const bool suspend = dockDragging || !ui.ShowScriptEditor || findCapturing
        || m_Editor->HasPendingClose() || m_Editor->HasExternalConflict()
        || !panelFocused || playing;
    LayoutOverlay(suspend);
    ImGui::EndChild();

    const std::string indentMode = m_Editor->IndentModeLabel();
    ImGui::TextDisabled(
        "Ln %zu, Col %zu | Sel %zu | %s | %s | %s | Zoom %d%%",
        m_Editor->StatusLine(),
        m_Editor->StatusColumn(),
        m_Editor->SelectionLength(),
        m_Editor->LanguageLabel(),
        indentMode.c_str(),
        m_Editor->EncodingLabel(),
        m_Editor->ZoomPercent());
    if (const auto& validation = m_Editor->LastValidation();
        validation && !validation->Success)
    {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4{0.95F, 0.35F, 0.35F, 1.0F}, "| %s", validation->Message.c_str());
    }
    else if (!m_Editor->StatusInfo().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", m_Editor->StatusInfo().c_str());
    }
    ImGui::EndGroup();

    if (ImGui::CollapsingHeader("Problems"))
    {
        const auto& diags = m_Editor->Diagnostics();
        if (diags.empty())
        {
            ImGui::TextDisabled("No problems");
        }
        for (std::size_t i = 0; i < diags.size(); ++i)
        {
            const auto& d = diags[i];
            ImGui::PushID(static_cast<int>(i));
            const std::string label = m_Editor->SelectedScript() + ':' + std::to_string(d.Line)
                + ':' + std::to_string(d.Column) + ": " + d.Message;
            if (ImGui::Selectable(label.c_str()))
            {
                m_Editor->SelectScriptAt(m_Editor->SelectedScript(), d.Line, d.Column);
                m_Editor->RequestNativeFocus();
                ui.ShowOutput = true;
            }
            ImGui::PopID();
        }
    }

    if (m_ShowGoto)
    {
        ImGui::OpenPopup("##fxs_goto");
    }
    if (ImGui::BeginPopupModal("##fxs_goto", &m_ShowGoto, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_FocusGoto)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusGoto = false;
        }
        ImGui::TextUnformatted("Go to line[:column]");
        if (ImGui::InputText("##goto", m_GotoBuf, sizeof(m_GotoBuf),
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            std::size_t line = 1;
            std::size_t column = 1;
            if (ParseGoto(m_GotoBuf, line, column))
            {
                m_Editor->GotoLineColumnInDocument(line, column);
                m_Editor->RequestNativeFocus();
            }
            m_ShowGoto = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_ShowGoto = false;
            ImGui::CloseCurrentPopup();
            m_Editor->RequestNativeFocus();
        }
        if (ImGui::Button("Go"))
        {
            std::size_t line = 1;
            std::size_t column = 1;
            if (ParseGoto(m_GotoBuf, line, column))
            {
                m_Editor->GotoLineColumnInDocument(line, column);
                m_Editor->RequestNativeFocus();
            }
            m_ShowGoto = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            m_ShowGoto = false;
            ImGui::CloseCurrentPopup();
            m_Editor->RequestNativeFocus();
        }
        ImGui::EndPopup();
    }

    if (m_ShowQuickOpen)
    {
        ImGui::OpenPopup("##fxs_quick_open");
    }
    if (ImGui::BeginPopupModal("##fxs_quick_open", &m_ShowQuickOpen, ImGuiWindowFlags_AlwaysAutoResize))
    {
        if (m_FocusQuickOpen)
        {
            ImGui::SetKeyboardFocusHere();
            m_FocusQuickOpen = false;
        }
        ImGui::SetNextItemWidth(280.0F);
        ImGui::InputTextWithHint("##qo", "Quick-open script", m_QuickOpenBuf, sizeof(m_QuickOpenBuf));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            m_ShowQuickOpen = false;
            ImGui::CloseCurrentPopup();
            m_Editor->RequestNativeFocus();
        }
        const std::string filterLower = [&] {
            std::string v = m_QuickOpenBuf;
            std::transform(v.begin(), v.end(), v.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return v;
        }();
        if (ImGui::BeginChild("##qo_list", ImVec2{280.0F, 200.0F}, ImGuiChildFlags_Borders))
        {
            const std::vector<std::string> names = m_Editor->AllScriptNames();
            for (const std::string& name : names)
            {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!filterLower.empty() && lower.find(filterLower) == std::string::npos)
                {
                    continue;
                }
                if (ImGui::Selectable(name.c_str()) ||
                    (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)))
                {
                    m_Editor->SelectScript(name);
                    m_ShowQuickOpen = false;
                    ImGui::CloseCurrentPopup();
                    break;
                }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }

    DrawClosePrompt(ui);
    DrawExternalPrompt(ui);

    m_Editor->SetNativeVisible(ui.ShowScriptEditor && m_Editor->HasScript() && !findCapturing
        && !m_Editor->HasPendingClose() && !m_Editor->HasExternalConflict());
    ImGui::End();
}
}
