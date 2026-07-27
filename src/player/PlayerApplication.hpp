#pragma once

#include <filesystem>
#include <string>

namespace fadix
{
struct PlayerLaunchOptions
{
    std::filesystem::path ProjectFile;
    std::string BootScene; // optional override
    bool Fullscreen{false};
    int Width{1280};
    int Height{720};
    bool VSync{true};
    /// Load project + boot scene, print confirmation, exit (no window loop).
    bool SmokeExit{false};
};

class PlayerApplication final
{
public:
    explicit PlayerApplication(PlayerLaunchOptions options);
    ~PlayerApplication();

    int Run();

private:
    PlayerLaunchOptions m_Options;
};
}
