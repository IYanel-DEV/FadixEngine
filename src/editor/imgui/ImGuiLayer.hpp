#pragma once

#include <cstdint>
#include <functional>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUShader;
union SDL_Event;

namespace fadix::rhi
{
class Device;
namespace d3d11
{
class D3D11Device;
}
}

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

    [[nodiscard]] bool Initialize(SDL_Window* window, rhi::Device& device);
    void Shutdown();

    void ProcessEvent(const SDL_Event& event);
    void BeginFrame();
    /// ImGui::Render + prepare + swapchain pass + optional overlay + submit.
    void RenderAndSubmit(const AfterImGuiPass& afterImGui = {});

    [[nodiscard]] bool Ready() const noexcept { return m_Ready; }

private:
    [[nodiscard]] bool CreateLegacyDxbcPipeline();
    void DestroyLegacyDxbcPipeline();

    SDL_Window* m_Window{nullptr};
    rhi::Device* m_RhiDevice{nullptr};
    SDL_GPUDevice* m_Device{nullptr};
    rhi::d3d11::D3D11Device* m_D3D11Device{nullptr};
    SDL_GPUGraphicsPipeline* m_LegacyDxbcPipeline{nullptr};
    SDL_GPUShader* m_LegacyDxbcVertexShader{nullptr};
    SDL_GPUShader* m_LegacyDxbcFragmentShader{nullptr};
    std::uint32_t m_BackbufferWidth{0};
    std::uint32_t m_BackbufferHeight{0};
    bool m_Ready{false};
};
}
