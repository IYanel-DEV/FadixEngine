#pragma once

#include "editor/imgui/EditorUiState.hpp"

#include <SDL3/SDL.h>

namespace fadix::editor
{
/// SDL borderless window chrome helpers (hit-test, work-area maximize, caption commands).
class WindowChrome final
{
public:
    void Bind(SDL_Window* window);
    void Unbind();

    void SetUiState(EditorUiState* ui) noexcept { m_Ui = ui; }
    void UpdateMaximizedPadding(EditorUiState& ui);
    void ApplyPendingWindowCommands(EditorUiState& ui, bool& running);
    [[nodiscard]] bool HandleEvent(const SDL_Event& event, EditorUiState& ui);
    [[nodiscard]] SDL_Window* Window() const noexcept { return m_Window; }
    [[nodiscard]] bool IsMaximized() const;
    /// If the OS maximized over the taskbar, snap back to the display work area.
    void ClampMaximizedToWorkArea();

    static void DrawCaptionButton(
        const char* id,
        float x,
        float y,
        float size,
        bool closeStyle,
        int iconKind,
        EditorUiState& ui);

private:
    void SaveRestoreBounds();
    void FillDisplayWorkArea();
    void RestoreFromWorkArea();

    static SDL_HitTestResult SDLCALL HitTestCallback(
        SDL_Window* window, const SDL_Point* area, void* data);

    SDL_Window* m_Window{nullptr};
    EditorUiState* m_Ui{nullptr};
    bool m_HitTestInstalled{false};
    bool m_WorkAreaMaximized{false};
    int m_RestoreX{0};
    int m_RestoreY{0};
    int m_RestoreW{1280};
    int m_RestoreH{720};
};
}
