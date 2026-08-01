#include "editor/imgui/ImGuiEditorApplication.hpp"

#include <exception>
#include <iostream>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// Separated so __try/__except in main() has no C++ objects with dtors in scope.
static int RunApplication()
{
    std::clog << std::unitbuf;
    try
    {
        fadix::ImGuiEditorApplication application;
        return application.Run();
    }
    catch (const std::exception& error)
    {
        const std::string message =
            std::string{"[Fadix] Fatal error: "} + error.what() +
            "\n\nIf this says 'D3D11' or 'GPU', try running with:\n"
            "  set FADIX_RENDERER=d3d11\n"
            "in a Command Prompt before launching.";
        std::cerr << "[Fadix] Fatal error: " << error.what() << '\n';
#ifdef _WIN32
        MessageBoxA(nullptr, message.c_str(), "Fadix Engine - Fatal Error", MB_OK | MB_ICONERROR);
#endif
        return 1;
    }
}

int main()
{
#ifdef _WIN32
    __try { return RunApplication(); }
    __except (1)
    {
        const char* msg =
            "Fadix Engine crashed before the window could open.\n\n"
            "This is usually caused by an incompatible or outdated graphics driver.\n\n"
            "Quick fix: open a Command Prompt and run:\n"
            "  set FADIX_RENDERER=d3d11\n"
            "  fadix_editor.exe\n\n"
            "Or update your Intel/AMD/NVIDIA graphics driver.";
        MessageBoxA(nullptr, msg, "Fadix Engine - Fatal Error", MB_OK | MB_ICONERROR);
        return 2;
    }
#else
    return RunApplication();
#endif
}
