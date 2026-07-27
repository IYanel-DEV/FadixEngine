#include "player/PlayerApplication.hpp"
#include "project/ProjectJson.hpp"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace
{
[[nodiscard]] fadix::PlayerLaunchOptions ParseArgs(int argc, char** argv)
{
    fadix::PlayerLaunchOptions options;
    const std::filesystem::path exeDir =
        std::filesystem::absolute(std::filesystem::path{argv[0]}).parent_path();

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg{argv[i]};
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
            {
                throw std::runtime_error(std::string{"Missing value for "} + name);
            }
            return argv[++i];
        };
        if (arg == "--project")
        {
            options.ProjectFile = need("--project");
        }
        else if (arg == "--scene")
        {
            options.BootScene = need("--scene");
        }
        else if (arg == "--width")
        {
            options.Width = std::stoi(need("--width"));
        }
        else if (arg == "--height")
        {
            options.Height = std::stoi(need("--height"));
        }
        else if (arg == "--fullscreen")
        {
            options.Fullscreen = true;
        }
        else if (arg == "--windowed")
        {
            options.Fullscreen = false;
        }
        else if (arg == "--vsync")
        {
            options.VSync = true;
        }
        else if (arg == "--no-vsync")
        {
            options.VSync = false;
        }
        else if (arg == "--smoke-exit")
        {
            options.SmokeExit = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout
                << "fadix_player --project <project.fadix> [--scene Scenes/Main.scene]\n"
                << "  [--width N] [--height N] [--fullscreen|--windowed] [--vsync|--no-vsync]\n"
                << "  [--smoke-exit]\n";
            std::exit(0);
        }
    }

    if (options.ProjectFile.empty())
    {
        const auto beside = exeDir / "project.fadix";
        if (std::filesystem::exists(beside))
        {
            options.ProjectFile = beside;
        }
    }
    const auto manifest = exeDir / "export.manifest.json";
    if (std::filesystem::exists(manifest))
    {
        std::ifstream in{manifest, std::ios::binary};
        if (in)
        {
            const std::string text{std::istreambuf_iterator<char>{in}, {}};
            if (const auto json = fadix::project_json::Parse(text))
            {
                if (auto versionError = fadix::project_json::CheckFormatVersion(
                        *json,
                        "formatVersion",
                        fadix::project_json::kManifestFormatVersion,
                        "export.manifest.json"))
                {
                    throw std::runtime_error(std::move(*versionError));
                }
                if (options.BootScene.empty() && json->Contains("bootScene") &&
                    json->at("bootScene").IsString())
                {
                    options.BootScene = json->at("bootScene").AsString();
                }
                if (json->Contains("window") && json->at("window").IsObject())
                {
                    const auto& window = json->at("window");
                    if (window.Contains("width"))
                    {
                        options.Width = static_cast<int>(window.at("width").AsNumber());
                    }
                    if (window.Contains("height"))
                    {
                        options.Height = static_cast<int>(window.at("height").AsNumber());
                    }
                    if (window.Contains("fullscreen") &&
                        window.at("fullscreen").GetType() == fadix::project_json::Value::Type::Bool)
                    {
                        options.Fullscreen = window.at("fullscreen").AsBool();
                    }
                    if (window.Contains("vsync") &&
                        window.at("vsync").GetType() == fadix::project_json::Value::Type::Bool)
                    {
                        options.VSync = window.at("vsync").AsBool();
                    }
                }
            }
        }
    }
    if (options.ProjectFile.empty())
    {
        throw std::runtime_error("Pass --project <project.fadix> or place project.fadix beside the player");
    }
    return options;
}
}

int main(int argc, char** argv)
{
    std::clog << std::unitbuf;
    try
    {
        fadix::PlayerApplication application(ParseArgs(argc, argv));
        return application.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[FadixPlayer] Fatal: " << error.what() << '\n';
        return 1;
    }
}
