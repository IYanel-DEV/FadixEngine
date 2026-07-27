#include "editor/imgui/panels/OutputPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"

#include <imgui.h>

namespace fadix::editor
{
void OutputPanel::Bind(EditorLog& log)
{
    m_Log = &log;
}

void OutputPanel::SetOpenDiagnostic(std::function<void(const OutputEntry&)> openDiagnostic)
{
    m_OpenDiagnostic = std::move(openDiagnostic);
}

void OutputPanel::Draw(EditorUiState& ui)
{
    if (!ui.ShowOutput || m_Log == nullptr)
    {
        return;
    }
    if (!ImGui::Begin(FADIX_ICON_TERMINAL " Output###Output", &ui.ShowOutput))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Clear"))
    {
        m_Log->Clear();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Info", &m_ShowInfo);
    ImGui::SameLine();
    ImGui::Checkbox("Warn", &m_ShowWarn);
    ImGui::SameLine();
    ImGui::Checkbox("Error", &m_ShowError);
    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &m_AutoScroll);

    ImGui::Separator();
    if (ImGui::BeginChild("##output_list", ImVec2{0.0F, 0.0F}, ImGuiChildFlags_Borders))
    {
        const auto entries = m_Log->Entries();
        bool any = false;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const OutputEntry& entry = entries[index];
            const bool visible = (entry.Severity == "info" && m_ShowInfo) ||
                (entry.Severity == "warn" && m_ShowWarn) ||
                (entry.Severity == "error" && m_ShowError);
            if (!visible)
            {
                continue;
            }
            any = true;
            ImGui::PushID(static_cast<int>(index));
            const char* sev = entry.Severity == "warn"    ? "WARN"
                : entry.Severity == "error"               ? "ERROR"
                                                          : "INFO";
            ImGui::TextDisabled("%s", entry.Time.c_str());
            ImGui::SameLine();
            if (entry.Severity == "error")
            {
                ImGui::TextColored(ImVec4{0.95F, 0.35F, 0.35F, 1.0F}, "%s", sev);
            }
            else if (entry.Severity == "warn")
            {
                ImGui::TextColored(ImVec4{0.95F, 0.75F, 0.25F, 1.0F}, "%s", sev);
            }
            else
            {
                ImGui::TextUnformatted(sev);
            }
            ImGui::SameLine();
            if (!entry.ScriptName.empty())
            {
                if (ImGui::Selectable(entry.Text.c_str(), false))
                {
                    if (m_OpenDiagnostic)
                    {
                        m_OpenDiagnostic(entry);
                    }
                    else
                    {
                        ui.ShowScriptEditor = true;
                        ui.StatusText = "Open " + entry.ScriptName + ":" +
                            std::to_string(entry.Line) + " (script editor pending)";
                    }
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Open %s:%zu:%zu", entry.ScriptName.c_str(), entry.Line,
                        entry.Column);
                }
            }
            else
            {
                ImGui::TextWrapped("%s", entry.Text.c_str());
            }
            ImGui::PopID();
        }
        if (!any)
        {
            ImGui::TextDisabled("No messages match the active filters");
        }
        if (m_AutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0F)
        {
            ImGui::SetScrollHereY(1.0F);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
}
