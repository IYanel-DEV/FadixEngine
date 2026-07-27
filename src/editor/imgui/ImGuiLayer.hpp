#pragma once

#include <cstdint>
#include <functional>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
union SDL_Event;

namespace fadix::editor
{
/// ImGui SDL3 / SDL_GPU lifecycle only. No panels, menus, or editor commands.
class ImGuiLayer final
{
public:
    using AfterImGuiPass = std::function<void(
        SDL_GPUCommandBuffer* commandBuffer,
        SDL_GPUTexture* swapchain,
        std::uint32_t width,
        std::uint32_t height)>;

    ImGuiLayer() = default;
    ~ImGuiLayer();

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    [[nodiscard]] bool Initialize(SDL_Window* window, SDL_GPUDevice* device);
    void Shutdown();

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    /// ImGui::Render + prepare + swapchain pass + optional overlay + submit.
    void RenderAndSubmit(const AfterImGuiPass& afterImGui = {});

    [[nodiscard]] bool Ready() const noexcept { return m_Ready; }

private:
    SDL_Window* m_Window{nullptr};
    SDL_GPUDevice* m_Device{nullptr};
    bool m_Ready{false};
};
}
