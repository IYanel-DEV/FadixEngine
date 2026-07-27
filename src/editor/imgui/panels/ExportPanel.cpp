#include "editor/imgui/panels/ExportPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "editor/EditorSession.hpp"
#include "project/ProjectService.hpp"

#include <imgui.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace fadix::editor
{
namespace
{
void SetDefaultDestination(ExportPanel& /*self*/, char* dest, const std::size_t destSize, const EditorSession& session)
{
    if (dest[0] != '\0')
    {
        return;
    }
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const std::filesystem::path folder =
        session.ActiveProject().RootPath / "Saved" / "Exports" /
        (session.ActiveProject().Name + "_export_" + std::to_string(stamp));
    std::snprintf(dest, destSize, "%s", folder.string().c_str());
}
}

void ExportPanel::Draw(EditorSession& session, EditorUiState& ui, SDL_Window* window)
{
    if (!ui.ShowExport)
    {
        return;
    }
    if (!ImGui::Begin(FADIX_ICON_EXPORT " Export###Export", &ui.ShowExport))
    {
        ImGui::End();
        return;
    }

    if (!m_Initialized)
    {
        m_Initialized = true;
#ifdef FADIX_SRC_DIR
        // Prefer sibling Debug/Release player next to the editor binary.
#endif
        if (const char* base = SDL_GetBasePath())
        {
            m_PlayerSource = std::filesystem::path{base} / "fadix_player.exe";
            SDL_free(const_cast<char*>(base));
        }
        if (!std::filesystem::exists(m_PlayerSource))
        {
            m_PlayerSource = std::filesystem::path{"fadix_player.exe"};
        }
        std::snprintf(
            m_ExecutableName,
            sizeof(m_ExecutableName),
            "%s.exe",
            session.ActiveProject().Name.c_str());
        if (session.ActiveProject().DefaultScene.empty())
        {
            std::snprintf(m_BootScene, sizeof(m_BootScene), "Scenes/Main.scene");
        }
        else
        {
            std::snprintf(
                m_BootScene,
                sizeof(m_BootScene),
                "%s",
                session.ActiveProject().DefaultScene.c_str());
        }
        SetDefaultDestination(*this, m_Destination, sizeof(m_Destination), session);
    }

    ImGui::InputText("Executable Name", m_ExecutableName, sizeof(m_ExecutableName));
    ImGui::InputText("Destination Folder", m_Destination, sizeof(m_Destination));
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
    {
        if (auto* service = dynamic_cast<ProjectService*>(session.projectService.get()))
        {
            const std::filesystem::path initial = session.ActiveProject().RootPath / "Saved";
            if (auto picked = service->BrowseForFolder(initial))
            {
                const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
                const std::filesystem::path dest =
                    picked.Value() /
                    (session.ActiveProject().Name + "_export_" + std::to_string(stamp));
                std::snprintf(m_Destination, sizeof(m_Destination), "%s", dest.string().c_str());
            }
        }
    }

    ImGui::InputText("Boot Scene", m_BootScene, sizeof(m_BootScene));
    ImGui::Checkbox("Fullscreen", &m_Fullscreen);
    ImGui::SameLine();
    ImGui::Checkbox("VSync", &m_VSync);
    ImGui::InputInt("Width", &m_Width);
    ImGui::InputInt("Height", &m_Height);

    ImGui::Separator();
    ImGui::TextWrapped("%s", m_Summary.empty() ? "Ready to export." : m_Summary.c_str());
    ImGui::ProgressBar(m_Progress, ImVec2{-1.0F, 0.0F}, m_ProgressStage.c_str());

    if (!m_LastResult.Messages.empty())
    {
        if (ImGui::BeginChild("export_log", ImVec2{0.0F, 120.0F}, ImGuiChildFlags_Borders))
        {
            for (const ExportMessage& message : m_LastResult.Messages)
            {
                const char* tag = message.Severity == ExportSeverity::Error
                    ? "[error] "
                    : (message.Severity == ExportSeverity::Warning ? "[warn] " : "[info] ");
                ImGui::TextUnformatted(tag);
                ImGui::SameLine();
                ImGui::TextWrapped("%s", message.Text.c_str());
            }
        }
        ImGui::EndChild();
    }

    const bool canExport = session.ActiveProject().ProjectFile.empty() == false;
    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button("Export"))
    {
        RunExport(session, ui);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_LastResult.StagedRoot.empty());
    if (ImGui::Button("Open Output Folder"))
    {
        OpenOutputFolder();
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void ExportPanel::RunExport(EditorSession& session, EditorUiState& ui)
{
    m_Progress = 0.0F;
    m_ProgressStage = "Starting";
    m_Summary.clear();
    m_LastResult = {};

    ExportOptions options;
    options.ProjectFile = session.ActiveProject().ProjectFile;
    options.DestinationDirectory = m_Destination;
    options.PlayerExecutableSource = m_PlayerSource;
    options.ExecutableName = m_ExecutableName;
    options.BootScene = m_BootScene;
    options.Fullscreen = m_Fullscreen;
    options.Width = m_Width;
    options.Height = m_Height;
    options.VSync = m_VSync;
    options.SaveAll = [&]() -> Result<void> {
        if (session.Document().Path.empty())
        {
            return Result<void>::Ok();
        }
        return session.SaveScene();
    };

    m_LastResult = ExportProject(options, [this](const ExportProgress& progress) {
        m_Progress = progress.Fraction;
        m_ProgressStage = progress.Stage;
    });

    if (m_LastResult.Ok)
    {
        std::ostringstream summary;
        summary << "Export OK → " << m_LastResult.StagedRoot.string() << " ("
                << m_LastResult.StagedAssets.size() << " files)";
        m_Summary = summary.str();
        ui.StatusText = m_Summary;
    }
    else
    {
        m_Summary = "Export failed";
        for (const auto& message : m_LastResult.Messages)
        {
            if (message.Severity == ExportSeverity::Error)
            {
                m_Summary = message.Text;
                break;
            }
        }
        ui.StatusText = m_Summary;
    }
}

void ExportPanel::OpenOutputFolder() const
{
    if (m_LastResult.StagedRoot.empty())
    {
        return;
    }
#ifdef _WIN32
    ShellExecuteW(
        nullptr,
        L"open",
        m_LastResult.StagedRoot.wstring().c_str(),
        nullptr,
        nullptr,
        SW_SHOWNORMAL);
#else
    static_cast<void>(SDL_OpenURL(
        (std::string{"file://"} + m_LastResult.StagedRoot.string()).c_str()));
#endif
}
}
