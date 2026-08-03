#include "editor/imgui/panels/ProjectManagerPanel.hpp"

#include "editor/imgui/EditorIcons.hpp"
#include "engine/Version.hpp"
#include "engine/audio/AudioEngine.hpp"
#include "project/ProjectJson.hpp"
#include "project/ProjectService.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <numbers>
#include <system_error>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace fadix::editor
{
namespace
{
constexpr char SelectSoundId[] = "editor.project.select";
constexpr char LaunchSoundId[] = "editor.project.launch";
constexpr char ReleasesApiUrl[] =
    "https://api.github.com/repos/IYanel-DEV/FadixEngine/releases?per_page=20";
constexpr char ReleasesPageUrl[] = "https://github.com/IYanel-DEV/FadixEngine/releases";

struct LauncherUpdate
{
    const char* Tag;
    const char* Title;
    const char* Detail;
};

constexpr std::array<LauncherUpdate, 2> News{{
    {"CURRENT RELEASE", "Fadix 0.9.136 is available",
        "Adds animation graphs, stronger scripting workflows and low-spec build defaults."},
    {"EDITOR UPDATE", "Build worlds with less friction",
        "New collision tools, character physics, improved lighting and the advanced FXS Editor."},
}};

constexpr std::array<LauncherUpdate, 11> DevLog{{
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
    {"0.9.129", "Legacy ImGui compatibility",
        "Bypasses converted ImGui shader blobs on older Intel and NVIDIA D3D12 drivers."},
    {"0.9.130", "Direct3D 11 compatibility",
        "Automatically falls back from unreliable D3D12 drivers and keeps the editor usable."},
}};

struct HttpResult
{
    bool Success{false};
    std::string Data;
    std::string Error;
};

[[nodiscard]] std::filesystem::path VersionsDirectory()
{
#ifdef _WIN32
    wchar_t buffer[32'768]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer,
        static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer))
    {
        return std::filesystem::path{buffer} / L"FadixEngine" / L"Versions";
    }
#endif
    return std::filesystem::temp_directory_path() / "FadixEngine" / "Versions";
}

[[nodiscard]] std::string SafePathSegment(std::string value)
{
    for (char& ch : value)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) == 0 && ch != '.' && ch != '-' && ch != '_')
        {
            ch = '_';
        }
    }
    return value.empty() ? "unknown" : value;
}

[[nodiscard]] std::filesystem::path InstalledVersionPath(
    const std::string_view tag, const std::string_view assetName)
{
    return VersionsDirectory() / SafePathSegment(std::string{tag}) /
        SafePathSegment(std::string{assetName});
}

#ifdef _WIN32
struct InternetHandle
{
    HINTERNET Value{nullptr};
    ~InternetHandle()
    {
        if (Value != nullptr)
        {
            WinHttpCloseHandle(Value);
        }
    }
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : Value(value) {}
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

[[nodiscard]] std::wstring ToWide(const std::string_view text)
{
    if (text.empty())
    {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), required);
    return result;
}

[[nodiscard]] std::string WinHttpError(const char* operation)
{
    return std::string{operation} + " failed (Windows error " +
        std::to_string(GetLastError()) + ")";
}

[[nodiscard]] HttpResult HttpGet(
    const std::string_view url, const std::filesystem::path* outputPath = nullptr)
{
    const std::wstring wideUrl = ToWide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (wideUrl.empty() || !WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts))
    {
        return {false, {}, WinHttpError("Invalid download URL")};
    }

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0)
    {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    InternetHandle session{WinHttpOpen(L"FadixEngine-VersionManager/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.Value == nullptr)
    {
        return {false, {}, WinHttpError("WinHttpOpen")};
    }
    WinHttpSetTimeouts(session.Value, 5'000, 5'000, 15'000, 30'000);

    InternetHandle connection{WinHttpConnect(session.Value, host.c_str(), parts.nPort, 0)};
    if (connection.Value == nullptr)
    {
        return {false, {}, WinHttpError("WinHttpConnect")};
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request{WinHttpOpenRequest(connection.Value, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (request.Value == nullptr)
    {
        return {false, {}, WinHttpError("WinHttpOpenRequest")};
    }
    constexpr wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.Value, headers, static_cast<DWORD>(-1L),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.Value, nullptr))
    {
        return {false, {}, WinHttpError("GitHub request")};
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.Value,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300)
    {
        return {false, {}, "GitHub returned HTTP " + std::to_string(status)};
    }

    std::ofstream output;
    if (outputPath != nullptr)
    {
        output.open(*outputPath, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return {false, {}, "Could not create " + outputPath->string()};
        }
    }
    HttpResult result;
    std::array<char, 64 * 1024> buffer{};
    for (;;)
    {
        DWORD read = 0;
        if (!WinHttpReadData(request.Value, buffer.data(),
                static_cast<DWORD>(buffer.size()), &read))
        {
            return {false, {}, WinHttpError("Download")};
        }
        if (read == 0)
        {
            break;
        }
        if (outputPath != nullptr)
        {
            output.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!output)
            {
                return {false, {}, "Could not write " + outputPath->string()};
            }
        }
        else
        {
            result.Data.append(buffer.data(), read);
        }
    }
    result.Success = true;
    return result;
}
#else
[[nodiscard]] HttpResult HttpGet(
    const std::string_view url, const std::filesystem::path* outputPath = nullptr)
{
    static_cast<void>(url);
    static_cast<void>(outputPath);
    return {false, {}, "Editor version downloads are currently supported on Windows only"};
}
#endif

[[nodiscard]] HttpResult DownloadVersion(
    const std::string_view url, const std::filesystem::path& destination)
{
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error)
    {
        return {false, {}, "Could not create versions folder: " + error.message()};
    }
    std::filesystem::path partial = destination;
    partial += ".part";
    HttpResult result = HttpGet(url, &partial);
    if (!result.Success)
    {
        std::filesystem::remove(partial, error);
        return result;
    }
    std::filesystem::rename(partial, destination, error);
    if (error)
    {
        std::filesystem::remove(partial, error);
        return {false, {}, "Could not finish download: " + error.message()};
    }
    return result;
}

void OpenExternal(const std::filesystem::path& target)
{
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    static_cast<void>(target);
#endif
}

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

[[nodiscard]] const char* TemplateName(const ProjectTemplate value)
{
    switch (value)
    {
    case ProjectTemplate::Empty2D: return "Empty 2D";
    case ProjectTemplate::TinyGame: return "Tiny Game";
    case ProjectTemplate::Empty3D: break;
    }
    return "Empty 3D";
}

[[nodiscard]] bool ContainsInsensitive(
    const std::string_view text, const std::string_view query)
{
    if (query.empty())
    {
        return true;
    }
    return std::search(text.begin(), text.end(), query.begin(), query.end(),
        [](const char left, const char right) {
            return std::tolower(static_cast<unsigned char>(left)) ==
                std::tolower(static_cast<unsigned char>(right));
        }) != text.end();
}

[[nodiscard]] bool EqualsInsensitive(
    const std::string_view left, const std::string_view right)
{
    return left.size() == right.size() &&
        std::equal(left.begin(), left.end(), right.begin(),
            [](const char lhs, const char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs)) ==
                    std::tolower(static_cast<unsigned char>(rhs));
            });
}

[[nodiscard]] std::string Trimmed(std::string value)
{
    const auto notSpace = [](const unsigned char ch) { return std::isspace(ch) == 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

[[nodiscard]] bool SidebarItem(const char* label, const bool selected)
{
    return ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2{0.0F, 32.0F});
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
    std::memset(m_Search, 0, sizeof(m_Search));
    m_Page = 0;
    m_ShowCreate = false;
    m_ShowOpen = false;
    m_ShowProjectActions = false;
    m_LatestVersion.clear();
    m_VersionStatus = "Checking GitHub releases...";
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
    PollVersionTasks();
    if (!m_ReleasesLoading && !m_ReleaseFetch.valid() && m_Releases.empty() &&
        m_VersionStatus == "Checking GitHub releases...")
    {
        StartReleaseRefresh();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float top = ui.MaximizedPadT + ui.TitleBarHeight;
    const float bottom = ui.MaximizedPadB;
    ImGui::SetNextWindowPos(ImVec2{viewport->Pos.x + ui.MaximizedPadL, viewport->Pos.y + top});
    ImGui::SetNextWindowSize(ImVec2{
        viewport->Size.x - ui.MaximizedPadL - ui.MaximizedPadR,
        viewport->Size.y - top - bottom});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.MainBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
    ImGui::Begin(
        "##ProjectManager",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    if (ImGui::BeginTable("##launcher_layout", 2,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
            ImVec2{0.0F, 0.0F}))
    {
        ImGui::TableSetupColumn("sidebar", ImGuiTableColumnFlags_WidthFixed, 192.0F);
        ImGui::TableSetupColumn("content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Brand);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0F, 16.0F});
        ImGui::BeginChild("##launcher_sidebar", ImVec2{0.0F, 0.0F},
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        if (SidebarItem(FADIX_ICON_FOLDER "  Projects", m_Page == 0)) m_Page = 0;
        if (SidebarItem(FADIX_ICON_CUBE "  Versions", m_Page == 1)) m_Page = 1;
        if (SidebarItem(FADIX_ICON_GRID "  Templates", m_Page == 2)) m_Page = 2;
        if (SidebarItem(FADIX_ICON_GLOBE "  News", m_Page == 3)) m_Page = 3;

        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - 132.0F));
        ImGui::Separator();
        if (SidebarItem(FADIX_ICON_GEAR "  Settings", m_Page == 4)) m_Page = 4;
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        ImGui::TextUnformatted("FADIX ENGINE");
        ImGui::Text("Build %s", std::string{EngineVersion}.c_str());
        if (!m_LatestVersion.empty())
        {
            ImGui::Text("Latest %s", m_LatestVersion.c_str());
        }
        if (m_Status != "Select or create a project.")
        {
            ImGui::TextWrapped("%s", m_Status.c_str());
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.MainBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{28.0F, 20.0F});
        ImGui::BeginChild("##launcher_content", ImVec2{0.0F, 0.0F},
            ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
        if (m_Page == 0)
        {
            DrawRecents(session, ui, theme);
        }
        else if (m_Page == 1)
        {
            DrawVersions(theme);
        }
        else if (m_Page == 2)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
            ImGui::TextUnformatted("TEMPLATES");
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Choose a starting point for a new project");
            ImGui::Spacing();
            const std::array<std::pair<ProjectTemplate, const char*>, 3> templates{{
                {ProjectTemplate::Empty3D, "Empty 3D  -  a clean three-dimensional scene"},
                {ProjectTemplate::Empty2D, "Empty 2D  -  a clean two-dimensional scene"},
                {ProjectTemplate::TinyGame, "Tiny Game  -  a playable example project"},
            }};
            for (const auto& [value, description] : templates)
            {
                ImGui::PushID(static_cast<int>(value));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
                ImGui::BeginChild("##template", ImVec2{0.0F, 68.0F}, true);
                ImGui::TextUnformatted(description);
                ImGui::SameLine(ImGui::GetWindowWidth() - 126.0F);
                if (ImGui::Button("Use Template", ImVec2{104.0F, 0.0F}))
                {
                    m_Template = value;
                    m_ShowCreate = true;
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::PopID();
            }
        }
        else if (m_Page == 3)
        {
            DrawUpdates(theme);
        }
        else
        {
            DrawSettings(theme);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndTable();
    }

    if (m_ShowCreate)
    {
        ImGui::OpenPopup("New Project");
        m_ShowCreate = false;
    }
    if (m_ShowOpen)
    {
        ImGui::OpenPopup("Import Project");
        m_ShowOpen = false;
    }
    if (m_ShowProjectActions)
    {
        ImGui::OpenPopup("Project Actions");
        m_ShowProjectActions = false;
    }
    DrawCreate(session, ui, theme);
    DrawOpen(session, ui, theme);
    DrawSelectedActions(session, ui, theme);

    ui.StatusText = m_Status;
    ImGui::End();
}

void ProjectManagerPanel::DrawRecents(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    const auto recents = session.Projects().Recents();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("PROJECTS");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
    ImGui::Text("%zu project%s", recents.size(), recents.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();

    const float toolbarWidth = 236.0F;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - toolbarWidth));
    ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        ImVec4{std::min(theme.Accent.x + 0.08F, 1.0F),
            std::min(theme.Accent.y + 0.08F, 1.0F),
            std::min(theme.Accent.z + 0.08F, 1.0F), 1.0F});
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0F, 1.0F, 1.0F, 1.0F});
    if (ImGui::Button(FADIX_ICON_PLUS "  New Project", ImVec2{116.0F, 28.0F}))
    {
        m_ShowCreate = true;
    }
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Import", ImVec2{96.0F, 28.0F}))
    {
        m_ShowOpen = true;
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint("##project_search",
        FADIX_ICON_SEARCH "  Search projects by name or path...",
        m_Search, sizeof(m_Search));
    ImGui::Spacing();

    ImGui::BeginChild("##project_library", ImVec2{0.0F, 0.0F}, false);
    if (recents.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
        ImGui::BeginChild("##empty_projects", ImVec2{0.0F, 174.0F}, true);
        ImGui::SetCursorPosY(50.0F);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextMuted);
        const char* title = "No projects here yet";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(title).x) * 0.5F);
        ImGui::TextUnformatted(title);
        const char* detail = "Create a new project or import an existing project.fadix file.";
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(detail).x) * 0.5F);
        ImGui::TextUnformatted(detail);
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    else
    {
        bool anyVisible = false;
        for (std::size_t index = 0; index < recents.size(); ++index)
        {
            const RecentProject& entry = recents[index];
            if (!ContainsInsensitive(entry.Project.Name, m_Search) &&
                !ContainsInsensitive(entry.Project.RootPath.string(), m_Search))
            {
                continue;
            }
            anyVisible = true;
            const bool selected = m_SelectedRecent == index;
            ImGui::PushID(static_cast<int>(index));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, selected ? theme.Selection : theme.Panel);
            ImGui::BeginChild("##project_row", ImVec2{0.0F, 82.0F},
                ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const float actionWidth = 160.0F;
            const float rowWidth = std::max(ImGui::GetContentRegionAvail().x - actionWidth, 160.0F);
            const std::string label = std::string{FADIX_ICON_CUBE "  "} + entry.Project.Name +
                "\n     " + entry.Project.RootPath.string() + "\n     " +
                TemplateName(entry.Project.Template) + "  /  Fadix " +
                std::string{EngineVersion};
            ImGui::PushStyleColor(ImGuiCol_Header, theme.Selection);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme.Hover);
            if (ImGui::Selectable(
                    label.c_str(),
                    selected,
                    ImGuiSelectableFlags_AllowDoubleClick,
                    ImVec2{rowWidth, 62.0F}))
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
            ImGui::SameLine();
            ImGui::SetCursorPosY(20.0F);
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0F, 1.0F, 1.0F, 1.0F});
            if (ImGui::Button(FADIX_ICON_PLAY "  Open", ImVec2{94.0F, 30.0F}))
            {
                m_SelectedRecent = index;
                OpenSelected(session, ui);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("...", ImVec2{34.0F, 30.0F}))
            {
                m_SelectedRecent = index;
                std::snprintf(m_RenameName, sizeof(m_RenameName), "%s",
                    entry.Project.Name.c_str());
                m_ProjectActionsX = ImGui::GetItemRectMax().x;
                m_ProjectActionsBelowY = ImGui::GetItemRectMax().y + 4.0F;
                m_ProjectActionsAboveY = ImGui::GetItemRectMin().y - 4.0F;
                m_ShowProjectActions = true;
            }
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopID();
            ImGui::Spacing();
        }
        if (!anyVisible)
        {
            ImGui::TextDisabled("No projects match '%s'.", m_Search);
        }
        if (m_SelectedRecent && *m_SelectedRecent >= recents.size())
        {
            m_SelectedRecent.reset();
        }
    }
    ImGui::EndChild();
}

void ProjectManagerPanel::DrawVersions(const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("VERSIONS");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Install and manage official Windows editor releases from GitHub");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - 212.0F));
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Downloads", ImVec2{104.0F, 28.0F}))
    {
        std::error_code error;
        std::filesystem::create_directories(VersionsDirectory(), error);
        OpenExternal(VersionsDirectory());
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_ReleasesLoading);
    if (ImGui::Button(FADIX_ICON_REFRESH "  Refresh", ImVec2{96.0F, 28.0F}))
    {
        StartReleaseRefresh();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##current_editor", ImVec2{0.0F, 76.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::Text("FX  Fadix %s", std::string{EngineVersion}.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.Play);
    ImGui::TextUnformatted("RUNNING");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("This editor is currently open and cannot be removed here.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!m_VersionStatus.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
            m_VersionStatus.find("failed") == std::string::npos ? theme.TextMuted : theme.Error);
        ImGui::TextWrapped("%s", m_VersionStatus.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    if (m_Releases.empty())
    {
        if (m_ReleasesLoading)
        {
            ImGui::TextDisabled("Contacting GitHub...");
        }
        else
        {
            ImGui::TextDisabled("No downloadable editor releases were returned.");
        }
        return;
    }

    ImGui::TextDisabled("AVAILABLE RELEASES");
    ImGui::Spacing();
    for (std::size_t index = 0; index < m_Releases.size(); ++index)
    {
        const EditorRelease& release = m_Releases[index];
        const std::filesystem::path installed =
            InstalledVersionPath(release.Tag, release.AssetName);
        std::error_code error;
        const bool isInstalled = std::filesystem::is_regular_file(installed, error) && !error;
        const bool busy = !m_ActiveVersionTask.empty();
        const bool thisDownloading = m_ActiveVersionTask == release.Tag;

        ImGui::PushID(static_cast<int>(index));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
        ImGui::BeginChild("##release", ImVec2{0.0F, 72.0F},
            ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
        ImGui::Text("FX  %s%s", release.Tag.c_str(), release.Prerelease ? "  PRE-RELEASE" : "");
        ImGui::PopStyleColor();
        const double megabytes = static_cast<double>(release.SizeBytes) / (1024.0 * 1024.0);
        if (isInstalled)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, theme.Play);
            ImGui::Text("Installed  |  %.1f MB", megabytes);
            ImGui::PopStyleColor();
        }
        else
        {
            ImGui::TextDisabled("Official Windows x64 editor  |  %.1f MB", megabytes);
        }

        const float right = ImGui::GetWindowWidth() - 14.0F;
        if (isInstalled)
        {
            ImGui::SameLine(right - 184.0F);
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
            if (ImGui::Button(FADIX_ICON_PLAY "  Launch", ImVec2{94.0F, 28.0F}))
            {
                LaunchVersion(release);
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(FADIX_ICON_TRASH "  Delete", ImVec2{82.0F, 28.0F}))
            {
                DeleteVersion(release);
            }
            ImGui::EndDisabled();
        }
        else
        {
            ImGui::SameLine(right - 112.0F);
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
            if (ImGui::Button(thisDownloading ? "Downloading..." : "Download",
                    ImVec2{104.0F, 28.0F}))
            {
                StartVersionDownload(release);
            }
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void ProjectManagerPanel::StartReleaseRefresh()
{
    if (m_ReleasesLoading)
    {
        return;
    }
    m_ReleasesLoading = true;
    m_VersionStatus = "Checking GitHub releases...";
    m_ReleaseFetch = std::async(std::launch::async, []() -> ReleaseFetchResult {
        const HttpResult response = HttpGet(ReleasesApiUrl);
        if (!response.Success)
        {
            return {{}, response.Error};
        }
        const auto json = project_json::Parse(response.Data);
        if (!json || !json->IsArray())
        {
            return {{}, "GitHub returned an unreadable releases list"};
        }

        ReleaseFetchResult result;
        for (const project_json::Value& item : json->Array())
        {
            if (!item.IsObject() || !item.at("tag_name").IsString())
            {
                continue;
            }
            if (item.at("draft").GetType() == project_json::Value::Type::Bool &&
                item.at("draft").AsBool())
            {
                continue;
            }
            if (!item.at("assets").IsArray())
            {
                continue;
            }
            for (const project_json::Value& asset : item.at("assets").Array())
            {
                if (!asset.IsObject() || !asset.at("name").IsString() ||
                    !asset.at("browser_download_url").IsString())
                {
                    continue;
                }
                const std::string& name = asset.at("name").AsString();
                // Releases also contain players and checksums. This picker only invites
                // executables it can actually launch.
                if (!name.starts_with("FadixEngine-") || !name.ends_with("Windows-x64.exe") ||
                    name.find("Player") != std::string::npos)
                {
                    continue;
                }
                EditorRelease release;
                release.Tag = item.at("tag_name").AsString();
                release.AssetName = name;
                release.DownloadUrl = asset.at("browser_download_url").AsString();
                if (asset.at("size").GetType() == project_json::Value::Type::Number)
                {
                    release.SizeBytes = static_cast<std::uint64_t>(asset.at("size").AsNumber());
                }
                release.Prerelease =
                    item.at("prerelease").GetType() == project_json::Value::Type::Bool &&
                    item.at("prerelease").AsBool();
                result.Releases.push_back(std::move(release));
                break;
            }
        }
        if (result.Releases.empty())
        {
            result.Error = "GitHub has no Windows editor downloads in its recent releases";
        }
        return result;
    });
}

void ProjectManagerPanel::PollVersionTasks()
{
    // Poll, do not wait: freezing the UI to display a loading spinner would be
    // impressively unhelpful.
    using namespace std::chrono_literals;
    if (m_ReleaseFetch.valid() && m_ReleaseFetch.wait_for(0ms) == std::future_status::ready)
    {
        ReleaseFetchResult result = m_ReleaseFetch.get();
        m_ReleasesLoading = false;
        if (!result.Error.empty())
        {
            m_VersionStatus = "GitHub check failed: " + result.Error;
        }
        else
        {
            m_Releases = std::move(result.Releases);
            m_LatestVersion = m_Releases.front().Tag;
            m_VersionStatus = "Latest GitHub release: " + m_LatestVersion;
        }
    }
    if (m_VersionTask.valid() && m_VersionTask.wait_for(0ms) == std::future_status::ready)
    {
        VersionTaskResult result = m_VersionTask.get();
        m_VersionStatus = result.Success ? result.Message : "Download failed: " + result.Message;
        m_ActiveVersionTask.clear();
    }
}

void ProjectManagerPanel::StartVersionDownload(const EditorRelease& release)
{
    if (!m_ActiveVersionTask.empty())
    {
        return;
    }
    m_ActiveVersionTask = release.Tag;
    m_VersionStatus = "Downloading " + release.Tag + "...";
    m_VersionTask = std::async(std::launch::async, [release]() -> VersionTaskResult {
        const std::filesystem::path destination =
            InstalledVersionPath(release.Tag, release.AssetName);
        const HttpResult result = DownloadVersion(release.DownloadUrl, destination);
        return {result.Success, result.Success
                ? ("Installed " + release.Tag + " in " + destination.parent_path().string())
                : result.Error};
    });
}

void ProjectManagerPanel::DeleteVersion(const EditorRelease& release)
{
    const std::filesystem::path installed =
        InstalledVersionPath(release.Tag, release.AssetName);
    std::error_code error;
    if (!std::filesystem::remove(installed, error) || error)
    {
        m_VersionStatus = error ? "Delete failed: " + error.message()
                                : release.Tag + " is not installed";
        return;
    }
    std::filesystem::remove(installed.parent_path(), error);
    m_VersionStatus = "Deleted downloaded editor " + release.Tag;
}

void ProjectManagerPanel::LaunchVersion(const EditorRelease& release)
{
    const std::filesystem::path installed =
        InstalledVersionPath(release.Tag, release.AssetName);
    std::error_code error;
    if (!std::filesystem::is_regular_file(installed, error) || error)
    {
        m_VersionStatus = release.Tag + " is not installed";
        return;
    }
#ifdef _WIN32
    const HINSTANCE launched = ShellExecuteW(nullptr, L"open", installed.c_str(), nullptr,
        installed.parent_path().c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32)
    {
        m_VersionStatus = "Could not launch " + release.Tag;
        return;
    }
#else
    OpenExternal(installed);
#endif
    m_VersionStatus = "Launched " + release.Tag;
}

void ProjectManagerPanel::DrawCreate(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    ImGui::SetNextWindowSize(ImVec2{520.0F, 0.0F}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("New Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("Create a fresh Fadix project in the folder you choose.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Project name");
    ImGui::SetNextItemWidth(-1.0F);
    ImGui::InputTextWithHint(
        "##create_name", "Leave empty for New Project", m_CreateName, sizeof(m_CreateName));
    ImGui::TextUnformatted("Location");
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - 76.0F, 80.0F));
    ImGui::InputTextWithHint(
        "##create_path", "Choose a folder", m_CreatePath, sizeof(m_CreatePath));
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

    ImGui::TextUnformatted("Template");
    const std::array<const char*, 3> templateNames{"Empty 3D", "Empty 2D", "Tiny Game"};
    int templateIndex = m_Template == ProjectTemplate::Empty2D ? 1
        : m_Template == ProjectTemplate::TinyGame ? 2 : 0;
    ImGui::SetNextItemWidth(-1.0F);
    if (ImGui::Combo("##project_template", &templateIndex,
            templateNames.data(), static_cast<int>(templateNames.size())))
    {
        m_Template = templateIndex == 1 ? ProjectTemplate::Empty2D
            : templateIndex == 2 ? ProjectTemplate::TinyGame : ProjectTemplate::Empty3D;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 196.0F);
    if (ImGui::Button("Cancel", ImVec2{84.0F, 28.0F}))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
    if (ImGui::Button("Create Project", ImVec2{104.0F, 28.0F}))
    {
        CreateProject(session, ui);
        if (ui.RequestEnterWorkbench)
        {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
}

void ProjectManagerPanel::DrawOpen(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    ImGui::SetNextWindowSize(ImVec2{520.0F, 0.0F}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Import Project", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("Add an existing project.fadix file to your library.");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted("Project file");
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
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - 196.0F);
    if (ImGui::Button("Cancel", ImVec2{84.0F, 28.0F}))
    {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, theme.Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1, 1, 1, 1});
    if (ImGui::Button("Import Project", ImVec2{104.0F, 28.0F}))
    {
        OpenPath(session, ui, m_OpenPath);
        if (ui.RequestEnterWorkbench)
        {
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::EndPopup();
}

void ProjectManagerPanel::DrawSelectedActions(
    EditorSession& session, EditorUiState& ui, const EditorTheme& theme)
{
    const bool hasSelection = m_SelectedRecent.has_value();
    const bool hasService = Service(session) != nullptr;
    constexpr float popupWidth = 320.0F;
    constexpr float popupHeight = 214.0F;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float right = viewport->WorkPos.x + viewport->WorkSize.x;
    const float bottom = viewport->WorkPos.y + viewport->WorkSize.y;
    const float x = std::clamp(m_ProjectActionsX - popupWidth,
        viewport->WorkPos.x + 8.0F, right - popupWidth - 8.0F);
    float y = m_ProjectActionsBelowY;
    if (y + popupHeight > bottom - 8.0F)
    {
        y = m_ProjectActionsAboveY - popupHeight;
    }
    y = std::clamp(y, viewport->WorkPos.y + 8.0F, bottom - popupHeight - 8.0F);
    ImGui::SetNextWindowPos(ImVec2{x, y}, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2{popupWidth, 0.0F}, ImVec2{popupWidth, std::numeric_limits<float>::max()});
    if (!ImGui::BeginPopup("Project Actions"))
    {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("Project Actions");
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

    if (hasSelection && ImGui::MenuItem(FADIX_ICON_PLAY "  Open Project"))
    {
        OpenSelected(session, ui);
    }
    if (hasSelection && hasService && ImGui::MenuItem("Rename"))
    {
        RenameSelected(session, ui);
    }
    if (hasSelection && hasService && ImGui::MenuItem("Reveal in File Explorer"))
    {
        RevealSelected(session, ui);
    }
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, theme.Error);
    if (hasSelection && hasService && ImGui::MenuItem("Remove from Library"))
    {
        RemoveSelected(session, ui);
    }
    ImGui::PopStyleColor();
    ImGui::EndPopup();
}

void ProjectManagerPanel::DrawUpdates(const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("NEWS");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Engine updates and development history");
    ImGui::Spacing();

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
}

void ProjectManagerPanel::DrawSettings(const EditorTheme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("SETTINGS");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Launcher preferences and useful developer locations");
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##launcher_preferences", ImVec2{0.0F, 116.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("Launcher");
    ImGui::PopStyleColor();
    ImGui::Checkbox("Interface sounds", &m_SoundsEnabled);
    ImGui::TextDisabled("Small selection and launch cues. This does not change project audio.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.Panel);
    ImGui::BeginChild("##version_locations", ImVec2{0.0F, 150.0F}, true);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.TextBright);
    ImGui::TextUnformatted("Editor versions");
    ImGui::PopStyleColor();
    ImGui::Text("Running build: %s", std::string{EngineVersion}.c_str());
    ImGui::Text("Latest on GitHub: %s",
        m_LatestVersion.empty() ? "checking..." : m_LatestVersion.c_str());
    ImGui::TextDisabled("%s", VersionsDirectory().string().c_str());
    if (ImGui::Button(FADIX_ICON_FOLDER_OPEN "  Open versions folder"))
    {
        std::error_code error;
        std::filesystem::create_directories(VersionsDirectory(), error);
        OpenExternal(VersionsDirectory());
    }
    ImGui::SameLine();
    if (ImGui::Button(FADIX_ICON_GLOBE "  GitHub releases"))
    {
        OpenExternal(std::filesystem::path{ReleasesPageUrl});
    }
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
    if (m_SoundsEnabled && m_SoundsReady && m_Audio != nullptr)
    {
        static_cast<void>(m_Audio->Play(SelectSoundId, 0, 0.55F));
    }
}

void ProjectManagerPanel::PlayLaunchSound()
{
    if (m_SoundsEnabled && m_SoundsReady && m_Audio != nullptr)
    {
        static_cast<void>(m_Audio->Play(LaunchSoundId, 0, 0.72F));
    }
}

void ProjectManagerPanel::CreateProject(EditorSession& session, EditorUiState& ui)
{
    std::string name = Trimmed(m_CreateName);
    std::string parentText = Trimmed(m_CreatePath);
    if (parentText.empty())
    {
        parentText = fadix::ProjectService::DefaultProjectsDirectory().string();
        std::snprintf(m_CreatePath, sizeof(m_CreatePath), "%s", parentText.c_str());
    }
    if (name.empty())
    {
        const std::filesystem::path parent{parentText};
        const auto recents = session.Projects().Recents();
        for (std::size_t suffix = 0;; ++suffix)
        {
            const std::string candidate = suffix == 0 ? "New Project"
                : "New Project (" + std::to_string(suffix) + ")";
            std::error_code error;
            const bool folderExists = std::filesystem::exists(parent / candidate, error);
            const bool recentExists = std::any_of(recents.begin(), recents.end(),
                [&candidate](const RecentProject& recent) {
                    return EqualsInsensitive(recent.Project.Name, candidate);
                });
            if (!folderExists && !error && !recentExists)
            {
                name = candidate;
                break;
            }
        }
        std::snprintf(m_CreateName, sizeof(m_CreateName), "%s", name.c_str());
    }
    m_Status = "Creating project...";
    auto result = session.Projects().Create(name, parentText, m_Template);
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
