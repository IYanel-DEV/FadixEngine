#pragma once

#include "engine/camera/EditorCamera.hpp"

#include <SDL3/SDL_events.h>
#include <glm/vec3.hpp>

#include <array>
#include <optional>

struct SDL_Window;

namespace fadix::editor
{
class WorkbenchCamera;

class EditorCameraInput
{
public:
    [[nodiscard]] bool HandleEvent(const SDL_Event& event, bool viewportHovered);
    void Update(WorkbenchCamera& camera, float deltaSeconds);
    void SetFocusBounds(std::optional<Bounds> bounds) noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool IsNavigating() const noexcept;

private:
    enum class Key
    {
        Forward,
        Backward,
        Left,
        Right,
        Down,
        Up,
        Fast,
        Count
    };

    void SetKey(SDL_Scancode scancode, bool pressed) noexcept;
    [[nodiscard]] bool KeyDown(Key key) const noexcept;
    void BeginCapture(SDL_WindowID windowId) noexcept;
    void ReleaseCapture() noexcept;
    void UpdateCapture() noexcept;

    std::array<bool, static_cast<std::size_t>(Key::Count)> m_Keys{};
    std::optional<Bounds> m_FocusBounds;
    glm::vec2 m_OrbitDelta{0.0F};
    glm::vec2 m_PanDelta{0.0F};
    glm::vec2 m_LookDelta{0.0F};
    float m_SpeedWheelDelta{0.0F};
    float m_DollyDelta{0.0F};
    SDL_Window* m_CaptureWindow{nullptr};
    // Global cursor position saved when capture starts, restored on release so
    // the pointer does not end up wherever relative mode left it.
    glm::vec2 m_SavedCursor{0.0F};
    bool m_HasSavedCursor{false};
    bool m_Orbiting{false};
    bool m_Panning{false};
    bool m_Looking{false};
    bool m_FocusRequested{false};
    // Set when relative capture starts so the first (potentially large)
    // synthetic motion delta is dropped instead of jerking the camera.
    bool m_SkipNextMotion{false};
};
}
