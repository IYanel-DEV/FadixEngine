#include "editor/imgui/ImGuiEditorApplication.hpp"

#include <exception>
#include <iostream>

int main()
{
    std::clog << std::unitbuf;
    try
    {
        fadix::ImGuiEditorApplication application;
        return application.Run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "[Fadix] Fatal error: " << error.what() << '\n';
        return 1;
    }
}
