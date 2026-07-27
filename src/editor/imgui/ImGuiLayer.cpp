#include "editor/imgui/ImGuiLayer.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace fadix::editor
{
ImGuiLayer::~ImGuiLayer()
{
    Shutdown();
}

bool ImGuiLayer::Initialize(SDL_Window* window, SDL_GPUDevice* device)
{
    if (window == nullptr || device == nullptr)
    {
        return false;
    }
    Shutdown();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#ifdef NDEBUG
    // Dear ImGui diagnostics are developer tools. Keep recovery enabled, but
    // never surface programmer-facing conflict/error overlays to players.
    io.ConfigDebugHighlightIdConflicts = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
#endif
    // Multi-viewports stay off: the script editor owns a native Win32 child HWND.
    // Style + fonts are applied by EditorTheme after Initialize.

    if (!ImGui_ImplSDL3_InitForSDLGPU(window))
    {
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = device;
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
    initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&initInfo))
    {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    m_Window = window;
    m_Device = device;
    m_Ready = true;
    return true;
}

void ImGuiLayer::Shutdown()
{
    if (!m_Ready)
    {
        return;
    }
    if (m_Device != nullptr)
    {
        SDL_WaitForGPUIdle(m_Device);
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    m_Window = nullptr;
    m_Device = nullptr;
    m_Ready = false;
}

void ImGuiLayer::ProcessEvent(const SDL_Event& event)
{
    if (m_Ready)
    {
        ImGui_ImplSDL3_ProcessEvent(&event);
    }
}

void ImGuiLayer::BeginFrame()
{
    if (!m_Ready)
    {
        return;
    }
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::RenderAndSubmit(const AfterImGuiPass& afterImGui)
{
    if (!m_Ready || m_Device == nullptr || m_Window == nullptr)
    {
        return;
    }

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized =
        drawData == nullptr || drawData->DisplaySize.x <= 0.0F || drawData->DisplaySize.y <= 0.0F;

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_Device);
    if (commandBuffer == nullptr)
    {
        return;
    }

    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t swapW = 0;
    std::uint32_t swapH = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            commandBuffer, m_Window, &swapchain, &swapW, &swapH) ||
        swapchain == nullptr)
    {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return;
    }

    if (!minimized)
    {
        // Mandatory before the render pass: uploads vertex/index buffers.
        ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);

        SDL_GPUColorTargetInfo targetInfo{};
        targetInfo.texture = swapchain;
        targetInfo.clear_color = SDL_FColor{0.08F, 0.09F, 0.10F, 1.0F};
        targetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        targetInfo.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commandBuffer, &targetInfo, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, pass);
        SDL_EndGPURenderPass(pass);

        // Rml GameUI uses LOADOP_LOAD so it composites over ImGui on the same CB.
        if (afterImGui)
        {
            afterImGui(commandBuffer, swapchain, swapW, swapH);
        }
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
}
}
