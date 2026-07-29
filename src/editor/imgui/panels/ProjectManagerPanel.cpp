#include "editor/imgui/panels/ProjectManagerPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "engine/Version.hpp"
#include "engine/audio/AudioEngine.hpp"
#include "project/ProjectService.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string_view>
#include <vector>

namespace fadix::editor
{
namespace
{
constexpr char SelectSoundId[] = "editor.project.select";
constexpr char LaunchSoundId[] = "editor.project.launch";

struct LauncherUpdate
{
    const char* Tag;
    const char* Title;
    const char* Detail;
};

constexpr std::array<LauncherUpdate, 2> News{{
    {"CURRENT RELEASE", "Fadix 0.9.126 is here",
        "Professional animation timelines, events and Animator Controller state machines."},
    {"EDITOR UPDATE", "Build worlds with less friction",
        "New collision tools, character physics, improved lighting and the advanced FXS Editor."},
}};

constexpr std::array<LauncherUpdate, 10> DevLog{{
    {"0.8.291", "First editor foundation",
        "Project creation, a basic scene viewport and the first entity workflow."},
    {"0.8.404", "Projects and scene saving",
        "Recent projects, persistent scenes, hierarchy editing and inspector components."},
    {"0.8.612", "Professional workspace",
        "Dockable ImGui panels, project layouts, content browsing and editor commands."},
    {"0.8.790", "FXS scripting",
        "Runtime scripts, drag-to-attach workflow and the first integrated code editor."},
    {"0.8.918", "Physics foundation",
        "Jolt bodies, shape colliders, mass, friction and stable play-mode simulation."},
    {"0.9.012", "Rendering upgrade",
        "PBR materials, improved lighting, shadows, quality presets and post-processing."},
    {"0.9.047", "Characters and collisions",
        "Dedicated character controller, mesh collision and editor collision visualization."},
    {"0.9.083", "Asset and scene workflow",
        "FBX, GLB and glTF importing, external file drops and multiple scene assets."},
    {"0.9.104", "World and editor polish",
        "Sun and moon cycle, persistent settings, hierarchy icons and improved gizmos."},
    {"0.9.126  CURRENT", "Animation workflow update",
        "Animation timelines, events, crossfades and visual Animator Controller state machines."},
}};

[[nodiscard]] std::vector<std::byte> MakeToneWave(
    const float startHz, const float endHz, const float seconds)
{
    constexpr std::uint32_t sampleRate = 22'050;
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const auto sampleCount = static_cast<std::uint32_t>(
        std::max(1L, std::lround(seconds * static_cast<float>(sampleRate))));
    const std::uint32_t dataBytes = sampleCount * sizeof(std::int16_t);
    std::vector<std::byte> wave;
    wave.reserve(44U + dataBytes);
    const auto text = [&wave](const std::string_view value) {
        for (const unsigned char ch : value)
        {
            wave.push_back(static_cast<std::byte>(ch));
        }
    };
    const auto word = [&wave](const std::uint16_t value) {
        wave.push_back(static_cast<std::byte>(value & 0xffU));
        wave.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    };
    const auto dword = [&wave](const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
        {
            wave.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    };

    text("RIFF");
    dword(36U + dataBytes);
    text("WAVEfmt ");
    dword(16U);
    word(1U);
    word(channels);
    dword(sampleRate);
    dword(sampleRate * channels * bits / 8U);
    word(channels * bits / 8U);
    word(bits);
    text("data");
    dword(dataBytes);

    float phase = 0.0F;
    for (std::uint32_t index = 0; index < sampleCount; ++index)
    {
        const float progress = static_cast<float>(index) / static_cast<float>(sampleCount);
        const float frequency = std::lerp(startHz, endHz, progress);
        phase += 2.0F * std::numbers::pi_v<float> * frequency /
            static_cast<float>(sampleRate);
        const float attack = std::min(progress / 0.12F, 1.0F);
        const float release = std::clamp((1.0F - progress) / 0.35F, 0.0F, 1.0F);
        const auto sample = static_cast<std::int16_t>(
            std::sin(phase) * attack * release * 7'500.0F);
        word(static_cast<std::uint16_t>(sample));
    }
#ifndef NDEBUG
    assert(wave.size() == 44U + dataBytes);
#endif
    return wave;
}

void DisabledButton(const char* label, const char* reason)
{
    ImGui::BeginDisabled();
    ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", reason);
    }
}

[[nodiscard]] bool IsCancelMessage(const std::string& message)
{
    return message.find("cancelled") != std::string::npos ||
        message.find("canceled") != std::string::npos;
}
}

void ProjectManagerPanel::Reset(EditorSession& session)
{
    static_cast<void>(session);
    m_SelectedRecent.reset();
    m_Status = "Select or create a project.";
    m_Template = ProjectTemplate::Empty3D;
    std::memset(m_CreateName, 0, sizeof(m_CreateName));
    std::memset(m_OpenPath, 0, sizeof(m_OpenPath));
    std::memset(m_RenameName, 0, sizeof(m_RenameName));
    const std::string parent = fadix::ProjectService::DefaultProjectsDirectory().string();
    std::snprintf(m_CreatePath, sizeof(m_CreatePath), "%s", parent.c_str());
    m_Initialized = true;
}

fadix::ProjectService* ProjectManagerPanel::Service(EditorSession& session) noexcept
{
    return dynamic_cast<fadix::ProjectService*>(&session.Projects());
}

void ProjectManagerPanel::Draw(
    EditorSession& session,
    EditorUiState& ui,
    const EditorTheme& theme,
    AudioEngine* audio,
    SDL_Window* window)
{
    static_cast<void>(window);
    if (!m_Initialized)
    {
        Reset(session);
    }
    EnsureSounds(audio);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = ui.MaximizedPadT + ui.TitleBarHeight;
    const float bottom = ui.MaximizedPadB;
    ImGui::SetNextWindowPos(ImVec2{viewport->Pos.x + ui.MaximizedPadL, viewport->Pos.y + top});
    ImGui::SetNextWindowSize(ImVec2{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR,
        viewport->Size.y - top - bottom});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.MainBackground);
    ImGui::Begin(
        "##ProjectManager",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##launcher_hero", ImVec2{0.0F, 82.0F}, true);
    if (theme.HasLogo())
    {
        ImGui::Image(theme.Logo(),
            ImVec2{54.0F, 40.0F},
            ImVec2{102.0F / 408.0F, 67.0F / 408.0F},
            ImVec2{300.0F / 408.0F, 211.0F / 408.0F});
        ImGui::SameLine(0.0F, 14.0F);
    }
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("FADIX ENGINE");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    ImGui::TextUnformatted("Create, continue and follow the engine's development.");
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    const std::string version = "VERSION " + std::string{EngineVersion} + "  /  CURRENT";
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(
        ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(version.c_str()).x));
    ImGui::PushStyleColor(ImGuiCol_Text, theme.Accent);
    ImGui::TextUnformatted(version.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const float contentHeight = std::max(ImGui::GetContentRegionAvail().y - 34.0F, 340.0F);
    if (ImGui::BeginTable("##pm_cols",
            3,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
            ImVec2{0.0F, contentHeight}))
    {
        ImGui::TableSetupColumn("projects", ImGuiTableColumnFlags_WidthStretch, 0.36F);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthStretch, 0.34F);
        ImGui::TableSetupColumn("updates", ImGuiTableColumnFlags_WidthStretch, 0.30F);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        DrawRecents(session, ui, theme);
        ImGui::TableNextColumn();
        DrawCreate(session, ui, theme);
        ImGui::Spacing();
        DrawOpen(session, ui, theme);
        ImGui::Spacing();
        DrawSelectedActions(session, ui, theme);
        ImGui::TableNextColumn();
        DrawUpdates(theme);
        ImGui::EndTable();
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    ImGui::TextUnformatted(m_Status.c_str());
    ImGui::PopStyleColor();
    ui.StatusText = m_Status;
    ImGui::End();
}

void ProjectManagerPanel::DrawRecents(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    static_cast<void>(ui);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##recents", ImVec2{0.0F, 0.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted(FADIX_ICON_FOLDER "  YOUR PROJECTS");
    ImGui::PopStyleColor();
    ImGui::Separator();

    const auto recents = session.Projects().Recents();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    ImGui::Text("%zu recent project%s", recents.size(), recents.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (recents.empty())
    {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextUnformatted("No recent projects");
        ImGui::TextWrapped("Create a project or open an existing project.fadix file.");
        ImGui::PopStyleColor();
    }
    else
    {
        for (std::size_t index = 0; index < recents.size(); ++index)
        {
            const RecentProject& entry = recents[index];
            const char* templ =
                entry.Project.Template == ProjectTemplate::Empty2D
                    ? "Empty 2D"
                    : (entry.Project.Template == ProjectTemplate::TinyGame ? "Tiny Game"
                                                                          : "Empty 3D");
            const bool selected = m_SelectedRecent == index;
            const std::string label = std::string{FADIX_ICON_CUBE "  "} + entry.Project.Name +
                "\n     " + templ;
            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_Header, theme.Selection);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.Hover);
            if (ImGui::Selectable(
                    label.c_str(),
                    selected,
                    ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2{0.0F, 54.0F}))
            {
                m_SelectedRecent = index;
                PlaySelectSound();
                std::snprintf(
                    m_RenameName,
                    sizeof(m_RenameName),
                    "%s",
                    entry.Project.Name.c_str());
                std::snprintf(
                    m_OpenPath,
                    sizeof(m_OpenPath),
                    "%s",
                    entry.Project.ProjectFile.string().c_str());
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    OpenSelected(session, ui);
                }
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", entry.Project.RootPath.string().c_str());
            }
            ImGui::PopID();
        }
        if (m_SelectedRecent && *m_SelectedRecent >= recents.size())
        {
            m_SelectedRecent.reset();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ProjectManagerPanel::DrawCreate(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##create", ImVec2{0.0F, 224.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted(FADIX_ICON_PLUS "  CREATE PROJECT");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::TextUnformatted("Project name");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##create_name", "My Game", m_CreateName, sizeof(m_CreateName));
    ImGui::TextUnformatted("Location");
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 76.0F, 80.0F));
    ImGui::InputText("##create_path", m_CreatePath, sizeof(m_CreatePath));
    ImGui::SameLine();
    if (Service(session) != nullptr)
    {
        if (ImGui::Button("Browse##parent", ImVec2{68.0F, 0.0F}))
        {
            BrowseParent(session, ui);
        }
    }
    else
    {
        DisabledButton("Browse##parent", "Folder browsing requires ProjectService");
    }

    if (ImGui::RadioButton("Empty 3D", m_Template == ProjectTemplate::Empty3D))
    {
        m_Template = ProjectTemplate::Empty3D;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Empty 2D", m_Template == ProjectTemplate::Empty2D))
    {
        m_Template = ProjectTemplate::Empty2D;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Tiny Game", m_Template == ProjectTemplate::TinyGame))
    {
        m_Template = ProjectTemplate::TinyGame;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
    if (ImGui::Button(FADIX_ICON_PLUS "  Create Project", ImVec2{-1.0F, 0.0F}))
    {
        CreateProject(session, ui);
    }
    ImGui::PopStyleColor(2);
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ProjectManagerPanel::DrawOpen(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##open", ImVec2{0.0F, 132.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted(FADIX_ICON_FOLDER_OPEN "  OPEN PROJECT");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 76.0F, 80.0F));
    ImGui::InputTextWithHint(
        "##open_path", "Choose project.fadix", m_OpenPath, sizeof(m_OpenPath));
    ImGui::SameLine();
    if (Service(session) != nullptr)
    {
        if (ImGui::Button("Browse##open", ImVec2{68.0F, 0.0F}))
        {
            BrowseOpen(session, ui);
        }
    }
    else
    {
        DisabledButton("Browse##open", "File browsing requires ProjectService");
    }
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Open Project", ImVec2{-1.0F, 0.0F}))
    {
        OpenPath(session, ui, m_OpenPath);
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ProjectManagerPanel::DrawSelectedActions(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    const bool hasSelection = m_SelectedRecent.has_value();
    const bool hasService = Service(session) != nullptr;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##selected_project", ImVec2{0.0F, 0.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("SELECTED PROJECT");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (hasSelection)
    {
        const auto recents = session.Projects().Recents();
        if (*m_SelectedRecent < recents.size())
        {
            ImGui::TextUnformatted(recents[*m_SelectedRecent].Project.Name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
            ImGui::TextWrapped("%s", recents[*m_SelectedRecent].Project.RootPath.string().c_str());
            ImGui::PopStyleColor();
        }
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextWrapped("Select a recent project to open, rename or reveal it.");
        ImGui::PopStyleColor();
    }
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##rename_project", "Rename project", m_RenameName, sizeof(m_RenameName));

    if (hasSelection)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
        if (ImGui::Button(FADIX_ICON_PLAY "  Launch Selected", ImVec2{-1.0F, 0.0F}))
        {
            OpenSelected(session, ui);
        }
        ImGui::PopStyleColor();
    }
    else
    {
        DisabledButton("Launch Selected", "Select a recent project first");
    }
    ImGui::SameLine();
    if (hasSelection && hasService)
    {
        if (ImGui::Button("Rename"))
        {
            RenameSelected(session, ui);
        }
    }
    else
    {
        DisabledButton(
            "Rename",
            hasService ? "Select a recent project first" : "Project service extras unavailable");
    }
    ImGui::SameLine();
    if (hasSelection && hasService)
    {
        if (ImGui::Button("Reveal"))
        {
            RevealSelected(session, ui);
        }
    }
    else
    {
        DisabledButton(
            "Reveal",
            hasService ? "Select a recent project first" : "Project service extras unavailable");
    }
    ImGui::SameLine();
    if (hasSelection && hasService)
    {
        if (ImGui::Button("Remove"))
        {
            RemoveSelected(session, ui);
        }
    }
    else
    {
        DisabledButton(
            "Remove",
            hasService ? "Select a recent project first" : "Project service extras unavailable");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ProjectManagerPanel::DrawUpdates(const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##launcher_updates", ImVec2{0.0F, 0.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted(FADIX_ICON_GLOBE "  LATEST NEWS");
    ImGui::PopStyleColor();
    ImGui::Separator();

    for (std::size_t index = 0; index < News.size(); ++index)
    {
        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Header);
        ImGui::BeginChild("##news", ImVec2{0.0F, 82.0F}, true);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.Accent);
        ImGui::TextUnformatted(News[index].Tag);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
        ImGui::TextUnformatted(News[index].Title);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextWrapped("%s", News[index].Detail);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted(FADIX_ICON_LIST "  DEVELOPMENT LOG");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    ImGui::TextUnformatted("From the first public editor build to today.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::BeginChild("##dev_log", ImVec2{0.0F, 0.0F}, false);
    for (const LauncherUpdate& entry : DevLog)
    {
        const bool current = std::string_view{entry.Tag}.find("CURRENT") != std::string_view::npos;
        ImGui::PushStyleColor(ImGuiCol_Text, current ? theme.Accent : theme.TextMuted);
        ImGui::TextUnformatted(entry.Tag);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
        ImGui::TextUnformatted(entry.Title);
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextWrapped("%s", entry.Detail);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void ProjectManagerPanel::EnsureSounds(AudioEngine* audio)
{
    m_Audio = audio;
    if (m_SoundsInitialized)
    {
        return;
    }
    m_SoundsInitialized = true;
    if (audio == nullptr || !audio->IsInitialized())
    {
        return;
    }
    const std::vector<std::byte> select = MakeToneWave(760.0F, 540.0F, 0.055F);
    const std::vector<std::byte> launch = MakeToneWave(330.0F, 760.0F, 0.18F);
    m_SoundsReady = audio->LoadMemory(SelectSoundId, select) &&
        audio->LoadMemory(LaunchSoundId, launch);
}

void ProjectManagerPanel::PlaySelectSound()
{
    if (m_SoundsReady && m_Audio != nullptr)
    {
        static_cast<void>(m_Audio->Play(SelectSoundId, 0, 0.55F));
    }
}

void ProjectManagerPanel::PlayLaunchSound()
{
    if (m_SoundsReady && m_Audio != nullptr)
    {
        static_cast<void>(m_Audio->Play(LaunchSoundId, 0, 0.72F));
    }
}

void ProjectManagerPanel::CreateProject(EditorSession& session, EditorUiState& ui)
{
    m_Status = "Creating project...";
    auto result = session.Projects().Create(m_CreateName, m_CreatePath, m_Template);
    if (!result)
    {
        m_Status = result.ErrorMessage();
        return;
    }
    m_Status = "Created " + result.Value().Name;
    PlayLaunchSound();
    ui.PendingProject = result.Value();
    ui.RequestEnterWorkbench = true;
}

void ProjectManagerPanel::OpenPath(
    EditorSession& session, EditorUiState& ui, const std::string& path)
{
    if (path.empty())
    {
        m_Status = "Choose a project.fadix file";
        return;
    }
    m_Status = "Opening project...";
    auto result = session.Projects().Open(path);
    if (!result)
    {
        m_Status = result.ErrorMessage();
        return;
    }
    m_Status = "Opened " + result.Value().Name;
    PlayLaunchSound();
    ui.PendingProject = result.Value();
    ui.RequestEnterWorkbench = true;
}

void ProjectManagerPanel::OpenSelected(EditorSession& session, EditorUiState& ui)
{
    if (!m_SelectedRecent || *m_SelectedRecent >= session.Projects().Recents().size())
    {
        m_Status = "Select a recent project first";
        return;
    }
    OpenPath(
        session,
        ui,
        session.Projects().Recents()[*m_SelectedRecent].Project.ProjectFile.string());
}

void ProjectManagerPanel::RemoveSelected(EditorSession& session, EditorUiState& ui)
{
    static_cast<void>(ui);
    fadix::ProjectService* service = Service(session);
    if (!m_SelectedRecent || service == nullptr)
    {
        m_Status = "Select a recent project first";
        return;
    }
    const auto path = session.Projects().Recents()[*m_SelectedRecent].Project.ProjectFile;
    if (auto result = service->RemoveRecent(path); !result)
    {
        m_Status = result.ErrorMessage();
        return;
    }
    m_SelectedRecent.reset();
    m_Status = "Removed from recent list";
}

void ProjectManagerPanel::RenameSelected(EditorSession& session, EditorUiState& ui)
{
    static_cast<void>(ui);
    fadix::ProjectService* service = Service(session);
    if (!m_SelectedRecent || service == nullptr)
    {
        m_Status = "Select a recent project first";
        return;
    }
    const auto path = session.Projects().Recents()[*m_SelectedRecent].Project.ProjectFile;
    auto result = service->Rename(path, m_RenameName);
    if (!result)
    {
        m_Status = result.ErrorMessage();
        return;
    }
    std::snprintf(m_RenameName, sizeof(m_RenameName), "%s", result.Value().Name.c_str());
    m_Status = "Renamed to " + result.Value().Name;
}

void ProjectManagerPanel::RevealSelected(EditorSession& session, EditorUiState& ui)
{
    static_cast<void>(ui);
    fadix::ProjectService* service = Service(session);
    if (!m_SelectedRecent || service == nullptr)
    {
        m_Status = "Select a recent project first";
        return;
    }
    const auto& project = session.Projects().Recents()[*m_SelectedRecent].Project;
    if (auto result = service->RevealInFileManager(project.ProjectFile); !result)
    {
        m_Status = result.ErrorMessage();
        return;
    }
    m_Status = "Revealed in file manager";
}

void ProjectManagerPanel::BrowseParent(EditorSession& session, EditorUiState& ui)
{
    static_cast<void>(ui);
    fadix::ProjectService* service = Service(session);
    if (service == nullptr)
    {
        m_Status = "Folder browsing unavailable";
        return;
    }
    auto result = service->BrowseForFolder(m_CreatePath);
    if (!result)
    {
        m_Status = IsCancelMessage(result.ErrorMessage()) ? "Browse cancelled"
                                                          : result.ErrorMessage();
        return;
    }
    std::snprintf(m_CreatePath, sizeof(m_CreatePath), "%s", result.Value().string().c_str());
    m_Status = "Parent folder selected";
}

void ProjectManagerPanel::BrowseOpen(EditorSession& session, EditorUiState& ui)
{
    static_cast<void>(ui);
    fadix::ProjectService* service = Service(session);
    if (service == nullptr)
    {
        m_Status = "File browsing unavailable";
        return;
    }
    auto result =
        service->BrowseForProjectFile(fadix::ProjectService::DefaultProjectsDirectory());
    if (!result)
    {
        m_Status = IsCancelMessage(result.ErrorMessage()) ? "Browse cancelled"
                                                          : result.ErrorMessage();
        return;
    }
    std::snprintf(m_OpenPath, sizeof(m_OpenPath), "%s", result.Value().string().c_str());
    m_Status = "Project file selected";
}
}
