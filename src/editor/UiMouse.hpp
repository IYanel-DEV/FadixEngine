#pragma once

#include <SDL3/SDL.h>

struct SDL_Window;

namespace fadix::editor
{
// RmlUi context dimensions and element bounds are in physical pixels; SDL mouse
// events are logical until scaled the same way as RmlUi_Platform_SDL.cpp.
[[nodiscard]] inline float UiMouseScale(SDL_Window* window)
{
    return window != nullptr ? SDL_GetWindowPixelDensity(window) : 1.0F;
}

[[nodiscard]] inline float ToUiMouseX(SDL_Window* window, const float logicalX)
{
    return logicalX * UiMouseScale(window);
}

[[nodiscard]] inline float ToUiMouseY(SDL_Window* window, const float logicalY)
{
    return logicalY * UiMouseScale(window);
}
}
