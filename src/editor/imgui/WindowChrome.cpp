#include "editor/imgui/WindowChrome.hpp"

#include <imgui.h>

#include <SDL3/SDL.h>

#include <algorithm>

namespace fadix::editor
{
namespace
{
[[nodiscard]] bool PointInRect(
    const float x,
    const float y,
    const float minX,
    const float minY,
    const float maxX,
    const float maxY)
{
    return x >= minX && x < maxX && y >= minY && y < maxY;
}

void DrawIcon(ImDrawList* draw, const ImVec2 center, const float size, const int kind, const ImU32 color)
{
    const float half = size * 0.5F;
    if (kind == 0)
    {
        draw->AddLine(
            ImVec2{center.x - half, center.y}, ImVec2{center.x + half, center.y}, color, 1.4F);
    }
    else if (kind == 1)
    {
        draw->AddRect(
            ImVec2{center.x - half, center.y - half},
            ImVec2{center.x + half, center.y + half},
            color,
            0.0F,
            0,
            1.4F);
    }
    else if (kind == 2)
    {
        const float inset = half * 0.35F;
        draw->AddRect(
            ImVec2{center.x - half + inset, center.y - half},
            ImVec2{center.x + half, center.y + half - inset},
            color,
            0.0F,
            0,
            1.2F);
        draw->AddRect(
            ImVec2{center.x - half, center.y - half + inset},
            ImVec2{center.x + half - inset, center.y + half},
            color,
            0.0F,
            0,
            1.2F);
    }
    else
    {
        draw->AddLine(
            ImVec2{center.x - half, center.y - half},
            ImVec2{center.x + half, center.y + half},
            color,
            1.4F);
        draw->AddLine(
            ImVec2{center.x + half, center.y - half},
            ImVec2{center.x - half, center.y + half},
            color,
            1.4F);
    }
}
}

void WindowChrome::Bind(SDL_Window* window)
{
    Unbind();
    m_Window = window;
    if (m_Window == nullptr)
    {
        return;
    }
    m_HitTestInstalled = SDL_SetWindowHitTest(m_Window, &WindowChrome::HitTestCallback, this);
}

void WindowChrome::Unbind()
{
    if (m_Window != nullptr && m_HitTestInstalled)
    {
        SDL_SetWindowHitTest(m_Window, nullptr, nullptr);
    }
    m_HitTestInstalled = false;
    m_Window = nullptr;
}

bool WindowChrome::IsMaximized() const
{
    if (m_Window == nullptr)
    {
        return false;
    }
    if (m_WorkAreaMaximized)
    {
        return true;
    }
    return (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) != 0;
}

void WindowChrome::SaveRestoreBounds()
{
    if (m_Window == nullptr || m_WorkAreaMaximized)
    {
        return;
    }
    SDL_GetWindowPosition(m_Window, &m_RestoreX, &m_RestoreY);
    SDL_GetWindowSize(m_Window, &m_RestoreW, &m_RestoreH);
}

void WindowChrome::FillDisplayWorkArea()
{
    if (m_Window == nullptr)
    {
        return;
    }
    SaveRestoreBounds();
    const SDL_DisplayID display = SDL_GetDisplayForWindow(m_Window);
    SDL_Rect usable{};
    if (!SDL_GetDisplayUsableBounds(display, &usable))
    {
        SDL_MaximizeWindow(m_Window);
        m_WorkAreaMaximized = false;
        return;
    }
    // Borderless SDL_MaximizeWindow covers the taskbar; size to the work area instead.
    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) != 0)
    {
        SDL_RestoreWindow(m_Window);
    }
    SDL_SetWindowPosition(m_Window, usable.x, usable.y);
    SDL_SetWindowSize(m_Window, usable.w, usable.h);
    m_WorkAreaMaximized = true;
}

void WindowChrome::RestoreFromWorkArea()
{
    if (m_Window == nullptr)
    {
        return;
    }
    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) != 0)
    {
        SDL_RestoreWindow(m_Window);
    }
    SDL_SetWindowPosition(m_Window, m_RestoreX, m_RestoreY);
    SDL_SetWindowSize(m_Window, std::max(640, m_RestoreW), std::max(480, m_RestoreH));
    m_WorkAreaMaximized = false;
}

void WindowChrome::ClampMaximizedToWorkArea()
{
    if (m_Window == nullptr)
    {
        return;
    }
    if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MAXIMIZED) == 0 && !m_WorkAreaMaximized)
    {
        return;
    }
    FillDisplayWorkArea();
}

void WindowChrome::UpdateMaximizedPadding(EditorUiState& ui)
{
    // Work-area maximize already matches usable bounds — no fake black pads.
    ui.MaximizedPadL = ui.MaximizedPadT = ui.MaximizedPadR = ui.MaximizedPadB = 0.0F;
}

void WindowChrome::DrawCaptionButton(
    const char* id,
    const float x,
    const float y,
    const float size,
    const bool closeStyle,
    const int iconKind,
    EditorUiState& ui)
{
    ImGui::SetCursorScreenPos(ImVec2{x, y});
    ImGui::InvisibleButton(id, ImVec2{size, size});
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 bg = 0;
    if (closeStyle && (hovered || active))
    {
        bg = IM_COL32(196, 43, 28, hovered ? 220 : 255);
    }
    else if (hovered || active)
    {
        bg = active ? IM_COL32(60, 60, 66, 255) : IM_COL32(42, 46, 52, 255);
    }
    if (bg != 0)
    {
        draw->AddRectFilled(ImVec2{x, y}, ImVec2{x + size, y + size}, bg);
    }
    const ImU32 iconColor =
        closeStyle && hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(204, 204, 204, 255);
    DrawIcon(draw, ImVec2{x + size * 0.5F, y + size * 0.5F}, size * 0.28F, iconKind, iconColor);

    if (ImGui::IsItemClicked())
    {
        if (iconKind == 0)
        {
            ui.RequestMinimize = true;
        }
        else if (iconKind == 1 || iconKind == 2)
        {
            ui.RequestMaximizeToggle = true;
        }
        else
        {
            ui.RequestClose = true;
        }
    }
}

void WindowChrome::ApplyPendingWindowCommands(EditorUiState& ui, bool& running)
{
    if (m_Window == nullptr)
    {
        return;
    }
    if (ui.RequestMinimize)
    {
        SDL_MinimizeWindow(m_Window);
        ui.RequestMinimize = false;
    }
    if (ui.RequestMaximizeToggle)
    {
        if (IsMaximized())
        {
            RestoreFromWorkArea();
        }
        else
        {
            FillDisplayWorkArea();
        }
        ui.RequestMaximizeToggle = false;
    }
    if (ui.RequestClose)
    {
        running = false;
        ui.RequestClose = false;
    }
}

bool WindowChrome::HandleEvent(const SDL_Event& event, EditorUiState& ui)
{
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F4 &&
        (SDL_GetModState() & SDL_KMOD_ALT) != 0)
    {
        ui.RequestClose = true;
        return true;
    }
    if (event.type == SDL_EVENT_WINDOW_MAXIMIZED)
    {
        // OS maximize on borderless covers the taskbar — snap to work area.
        ClampMaximizedToWorkArea();
        return true;
    }
    if (event.type == SDL_EVENT_WINDOW_RESTORED)
    {
        m_WorkAreaMaximized = false;
    }
    return false;
}

SDL_HitTestResult SDLCALL WindowChrome::HitTestCallback(
    SDL_Window* window, const SDL_Point* area, void* data)
{
    auto* self = static_cast<WindowChrome*>(data);
    if (self == nullptr || self->m_Ui == nullptr || area == nullptr || window == nullptr)
    {
        return SDL_HITTEST_NORMAL;
    }

    EditorUiState& state = *self->m_Ui;
    int sizeW = 0;
    int sizeH = 0;
    SDL_GetWindowSize(window, &sizeW, &sizeH);
    const float x = static_cast<float>(area->x);
    const float y = static_cast<float>(area->y);

    const bool maximized = self->IsMaximized();
    const float border = state.ResizeBorder;
    if (!maximized)
    {
        const bool left = x < border;
        const bool right = x >= static_cast<float>(sizeW) - border;
        const bool top = y < border;
        const bool bottom = y >= static_cast<float>(sizeH) - border;
        if (top && left)
        {
            return SDL_HITTEST_RESIZE_TOPLEFT;
        }
        if (top && right)
        {
            return SDL_HITTEST_RESIZE_TOPRIGHT;
        }
        if (bottom && left)
        {
            return SDL_HITTEST_RESIZE_BOTTOMLEFT;
        }
        if (bottom && right)
        {
            return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
        }
        if (left)
        {
            return SDL_HITTEST_RESIZE_LEFT;
        }
        if (right)
        {
            return SDL_HITTEST_RESIZE_RIGHT;
        }
        if (top)
        {
            return SDL_HITTEST_RESIZE_TOP;
        }
        if (bottom)
        {
            return SDL_HITTEST_RESIZE_BOTTOM;
        }
    }

    if (PointInRect(
            x, y, state.TitleBarMinX, state.TitleBarMinY, state.TitleBarMaxX, state.TitleBarMaxY))
    {
        if (x >= state.SysButtonsMinX || x < state.MenusMaxX)
        {
            return SDL_HITTEST_NORMAL;
        }
        return SDL_HITTEST_DRAGGABLE;
    }
    return SDL_HITTEST_NORMAL;
}
}
