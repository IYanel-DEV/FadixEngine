#pragma once

#include "editor/imgui/EditorUiState.hpp"
#include "project/ExportService.hpp"

#include <filesystem>
#include <string>

struct SDL_Window;

namespace fadix
{
class EditorSession;
}

namespace fadix::editor
{
class ExportPanel final
{
public:
    void Draw(EditorSession& session, EditorUiState& ui, SDL_Window* window);

private:
    void RunExport(EditorSession& session, EditorUiState& ui);
    void OpenOutputFolder() const;

    char m_ExecutableName[128]{"Game.exe"};
    char m_Destination[512]{};
    char m_BootScene[260]{"Scenes/Main.scene"};
    bool m_Fullscreen{false};
    bool m_VSync{true};
    int m_Width{1280};
    int m_Height{720};
    float m_Progress{0.0F};
    std::string m_ProgressStage;
    std::string m_Summary;
    ExportResult m_LastResult{};
    std::filesystem::path m_PlayerSource;
    bool m_Initialized{false};
};
}
