#include "editor/scripting/ScriptEditorRmlAdapter.hpp"

#include "assets/ScriptDatabase.hpp"

#include <RmlUi/Core.h>

#include <sstream>

namespace fadix::editor
{
namespace
{
std::string EscapeRml(const std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}
}

void ScriptEditorRmlAdapter::Bind(
    Rml::ElementDocument& document, ScriptEditorController& editor)
{
    m_Document = &document;
    m_Editor = &editor;
    editor.SetUiNotify([this]() { Sync(); });
    if (Rml::Element* view = m_Document->GetElementById("script-code-view"))
    {
        view->SetInnerRML("");
    }
    Sync();
}

void ScriptEditorRmlAdapter::Sync()
{
    if (m_Document == nullptr || m_Editor == nullptr)
    {
        return;
    }

    if (Rml::Element* header = m_Document->GetElementById("script-editor-filename"))
    {
        header->SetInnerRML(EscapeRml(m_Editor->HeaderText()));
    }
    if (Rml::Element* pos = m_Document->GetElementById("script-status-pos"))
    {
        pos->SetInnerRML(
            "Ln " + std::to_string(m_Editor->StatusLine()) + ", Col " +
            std::to_string(m_Editor->StatusColumn()));
    }
    if (Rml::Element* lang = m_Document->GetElementById("script-status-lang"))
    {
        lang->SetInnerRML(m_Editor->LanguageLabel());
    }
    if (Rml::Element* info = m_Document->GetElementById("script-status-info"))
    {
        info->SetInnerRML(EscapeRml(m_Editor->StatusInfo()));
    }

    Rml::Element* list = m_Document->GetElementById("script-list");
    if (list == nullptr)
    {
        return;
    }
    const auto names = m_Editor->VisibleNames();
    if (names.empty())
    {
        list->SetInnerRML("<div class=\"script-empty\">No scripts yet. Create one above.</div>");
        return;
    }
    std::ostringstream html;
    std::size_t index = 0;
    for (const std::string& name : names)
    {
        const char* lang =
            m_Editor->LanguageOf(name) == ScriptLanguage::Cpp ? "C++" : "Lua";
        const bool dirty = m_Editor->NameDirty(name);
        html << "<div class=\"script-row"
             << (name == m_Editor->SelectedScript() ? " selected" : "")
             << "\" data-script-index=\"" << index << "\">"
             << "<span class=\"label\">" << EscapeRml(name) << (dirty ? " *" : "") << "</span>"
             << "<span class=\"lang-badge\">" << lang << "</span></div>";
        ++index;
    }
    list->SetInnerRML(html.str());
}

void ScriptEditorRmlAdapter::HandleListClick(Rml::Event& event)
{
    if (m_Editor == nullptr)
    {
        return;
    }
    Rml::Element* target = event.GetTargetElement();
    while (target != nullptr)
    {
        if (target->HasAttribute("data-script-index"))
        {
            const int index =
                std::stoi(target->GetAttribute("data-script-index")->Get<Rml::String>());
            const auto names = m_Editor->VisibleNames();
            if (index >= 0 && static_cast<std::size_t>(index) < names.size())
            {
                m_Editor->SelectScript(names[static_cast<std::size_t>(index)]);
            }
            return;
        }
        target = target->GetParentNode();
    }
}
}
