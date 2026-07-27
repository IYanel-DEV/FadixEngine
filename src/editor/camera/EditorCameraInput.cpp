#include "editor/camera/EditorCameraInput.hpp"

#include "editor/camera/WorkbenchCamera.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

namespace fadix::editor
{
void EditorCameraInput::BeginCapture(const SDL_WindowID windowId) noexcept
{
    SDL_Window* window = SDL_GetWindowFromID(windowId);
    if (window == nullptr)
    {
        SDL_Log("Camera capture: SDL_GetWindowFromID failed: %s", SDL_GetError());
        return;
    }
    if (m_CaptureWindow == nullptr)
    {
        SDL_GetGlobalMouseState(&m_SavedCursor.x, &m_SavedCursor.y);
        m_HasSavedCursor = true;
    }
    m_CaptureWindow = window;
    // Relative mode hides the cursor, locks it in place, and reports unbounded
    // motion deltas; the grab keeps the OS from routing input elsewhere.
    if (!SDL_SetWindowRelativeMouseMode(window, true))
    {
        SDL_Log("Camera capture: SDL_SetWindowRelativeMouseMode(on) failed: %s", SDL_GetError());
    }
    if (!SDL_SetWindowMouseGrab(window, true))
    {
        SDL_Log("Camera capture: SDL_SetWindowMouseGrab(on) failed: %s", SDL_GetError());
    }
    m_SkipNextMotion = true;
}

void EditorCameraInput::ReleaseCapture() noexcept
{
    if (m_CaptureWindow == nullptr)
    {
        return;
    }
    if (!SDL_SetWindowRelativeMouseMode(m_CaptureWindow, false))
    {
        SDL_Log("Camera capture: SDL_SetWindowRelativeMouseMode(off) failed: %s", SDL_GetError());
    }
    if (!SDL_SetWindowMouseGrab(m_CaptureWindow, false))
    {
        SDL_Log("Camera capture: SDL_SetWindowMouseGrab(off) failed: %s", SDL_GetError());
    }
    if (m_HasSavedCursor)
    {
        if (!SDL_WarpMouseGlobal(m_SavedCursor.x, m_SavedCursor.y))
        {
            SDL_Log("Camera capture: SDL_WarpMouseGlobal failed: %s", SDL_GetError());
        }
        m_HasSavedCursor = false;
    }
    m_CaptureWindow = nullptr;
}

void EditorCameraInput::UpdateCapture() noexcept
{
    if (!m_Orbiting && !m_Panning && !m_Looking)
    {
        ReleaseCapture();
    }
}

bool EditorCameraInput::HandleEvent(const SDL_Event& event, const bool viewportHovered)
{
    if (event.type == SDL_EVENT_KEY_UP)
    {
        SetKey(event.key.scancode, false);
    }
    else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
    {
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            m_Orbiting = false;
        }
        else if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            m_Panning = false;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT)
        {
            m_Looking = false;
        }
        UpdateCapture();
    }
    else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST)
    {
        Reset();
    }

    // Hover is only required to START navigation; once a capture is active,
    // motion and movement keys must keep flowing even though relative mouse
    // mode makes ViewportHovered() report false.
    if (!viewportHovered && !IsNavigating())
    {
        return false;
    }

    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
        SetKey(event.key.scancode, true);
        if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_F)
        {
            m_FocusRequested = true;
        }
        return event.key.scancode == SDL_SCANCODE_W ||
            event.key.scancode == SDL_SCANCODE_A ||
            event.key.scancode == SDL_SCANCODE_S ||
            event.key.scancode == SDL_SCANCODE_D ||
            event.key.scancode == SDL_SCANCODE_Q ||
            event.key.scancode == SDL_SCANCODE_E ||
            event.key.scancode == SDL_SCANCODE_LSHIFT ||
            event.key.scancode == SDL_SCANCODE_RSHIFT ||
            event.key.scancode == SDL_SCANCODE_F;
    case SDL_EVENT_KEY_UP:
        return event.key.scancode == SDL_SCANCODE_W ||
            event.key.scancode == SDL_SCANCODE_A ||
            event.key.scancode == SDL_SCANCODE_S ||
            event.key.scancode == SDL_SCANCODE_D ||
            event.key.scancode == SDL_SCANCODE_Q ||
            event.key.scancode == SDL_SCANCODE_E ||
            event.key.scancode == SDL_SCANCODE_LSHIFT ||
            event.key.scancode == SDL_SCANCODE_RSHIFT;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (!viewportHovered)
        {
            break;
        }
        if (event.button.button == SDL_BUTTON_RIGHT)
        {
            m_Looking = true;
            BeginCapture(event.button.windowID);
            return true;
        }
        if (event.button.button == SDL_BUTTON_MIDDLE)
        {
            m_Panning = true;
            BeginCapture(event.button.windowID);
            return true;
        }
        if (event.button.button == SDL_BUTTON_LEFT && (SDL_GetModState() & SDL_KMOD_ALT) != 0)
        {
            m_Orbiting = true;
            BeginCapture(event.button.windowID);
            return true;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.button == SDL_BUTTON_LEFT ||
            event.button.button == SDL_BUTTON_MIDDLE ||
            event.button.button == SDL_BUTTON_RIGHT;
    case SDL_EVENT_MOUSE_MOTION:
    {
        if (m_SkipNextMotion && (m_Looking || m_Panning || m_Orbiting))
        {
            m_SkipNextMotion = false;
            return true;
        }
        const glm::vec2 delta{event.motion.xrel, event.motion.yrel};
        if (m_Looking)
        {
            m_LookDelta += delta;
            return true;
        }
        if (m_Panning)
        {
            m_PanDelta += delta;
            return true;
        }
        if (m_Orbiting)
        {
            m_OrbitDelta += delta;
            return true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    {
        const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        if (m_Looking)
        {
            // While fly-looking the wheel tunes the fly speed.
            m_SpeedWheelDelta += event.wheel.y * direction;
        }
        else
        {
            m_DollyDelta += event.wheel.y * direction;
        }
        return true;
    }
    default:
        break;
    }
    return false;
}

void EditorCameraInput::Update(WorkbenchCamera& camera, const float deltaSeconds)
{
    if (m_OrbitDelta != glm::vec2{0.0F})
    {
        camera.Orbit(m_OrbitDelta);
    }
    if (m_PanDelta != glm::vec2{0.0F})
    {
        camera.Pan(m_PanDelta);
    }
    if (m_LookDelta != glm::vec2{0.0F})
    {
        camera.Look(m_LookDelta);
    }
    if (m_SpeedWheelDelta != 0.0F)
    {
        camera.AdjustFlySpeed(m_SpeedWheelDelta);
    }
    if (m_DollyDelta != 0.0F)
    {
        camera.Dolly(m_DollyDelta);
    }
    if (m_FocusRequested && m_FocusBounds.has_value())
    {
        camera.FocusBounds(*m_FocusBounds);
    }

    if (m_Looking)
    {
        const glm::vec3 direction{
            static_cast<float>(KeyDown(Key::Right)) - static_cast<float>(KeyDown(Key::Left)),
            static_cast<float>(KeyDown(Key::Up)) - static_cast<float>(KeyDown(Key::Down)),
            static_cast<float>(KeyDown(Key::Forward)) - static_cast<float>(KeyDown(Key::Backward))};
        camera.Fly(direction, deltaSeconds, KeyDown(Key::Fast));
    }

    m_OrbitDelta = glm::vec2{0.0F};
    m_PanDelta = glm::vec2{0.0F};
    m_LookDelta = glm::vec2{0.0F};
    m_SpeedWheelDelta = 0.0F;
    m_DollyDelta = 0.0F;
    m_FocusRequested = false;
}

void EditorCameraInput::SetFocusBounds(std::optional<Bounds> bounds) noexcept
{
    m_FocusBounds = bounds;
}

void EditorCameraInput::Reset() noexcept
{
    m_Keys.fill(false);
    m_OrbitDelta = glm::vec2{0.0F};
    m_PanDelta = glm::vec2{0.0F};
    m_LookDelta = glm::vec2{0.0F};
    m_SpeedWheelDelta = 0.0F;
    m_DollyDelta = 0.0F;
    m_Orbiting = false;
    m_Panning = false;
    m_Looking = false;
    m_FocusRequested = false;
    m_SkipNextMotion = false;
    UpdateCapture();
}

bool EditorCameraInput::IsNavigating() const noexcept
{
    return m_Orbiting || m_Panning || m_Looking;
}

void EditorCameraInput::SetKey(const SDL_Scancode scancode, const bool pressed) noexcept
{
    switch (scancode)
    {
    case SDL_SCANCODE_W:
        m_Keys[static_cast<std::size_t>(Key::Forward)] = pressed;
        break;
    case SDL_SCANCODE_S:
        m_Keys[static_cast<std::size_t>(Key::Backward)] = pressed;
        break;
    case SDL_SCANCODE_A:
        m_Keys[static_cast<std::size_t>(Key::Left)] = pressed;
        break;
    case SDL_SCANCODE_D:
        m_Keys[static_cast<std::size_t>(Key::Right)] = pressed;
        break;
    case SDL_SCANCODE_Q:
        m_Keys[static_cast<std::size_t>(Key::Down)] = pressed;
        break;
    case SDL_SCANCODE_E:
        m_Keys[static_cast<std::size_t>(Key::Up)] = pressed;
        break;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
        m_Keys[static_cast<std::size_t>(Key::Fast)] = pressed;
        break;
    default:
        break;
    }
}

bool EditorCameraInput::KeyDown(const Key key) const noexcept
{
    return m_Keys[static_cast<std::size_t>(key)];
}
}
