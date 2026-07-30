#include "editor/imgui/ImGuiLayer.hpp"

#include "render/ShaderCompiler.hpp"
#include "rhi/d3d11/D3D11Rhi.hpp"
#include "rhi/sdl/SdlRhi.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#ifdef _WIN32
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstddef>
#include <algorithm>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <vector>

namespace
{
constexpr char LegacyImGuiShader[] = R"(
cbuffer VertexUniforms : register(b0, space1)
{
    float2 Scale;
    float2 Translation;
};

struct VertexInput
{
    float2 Position : TEXCOORD0;
    float2 Uv : TEXCOORD1;
    float4 Color : TEXCOORD2;
};

struct VertexOutput
{
    float4 Position : SV_Position;
    float4 Color : TEXCOORD0;
    float2 Uv : TEXCOORD1;
};

VertexOutput VertexMain(VertexInput input)
{
    VertexOutput output;
    output.Position = float4(input.Position * Scale + Translation, 0.0, 1.0);
    output.Position.y *= -1.0;
    output.Color = input.Color;
    output.Uv = input.Uv;
    return output;
}

Texture2D FontTexture : register(t0, space2);
SamplerState FontSampler : register(s0, space2);

float4 FragmentMain(VertexOutput input) : SV_Target0
{
    return input.Color * FontTexture.Sample(FontSampler, input.Uv);
}
)";
}

namespace fadix::editor
{
ImGuiLayer::~ImGuiLayer()
{
    Shutdown();
}

bool ImGuiLayer::Initialize(SDL_Window* window, rhi::Device& device)
{
    if (window == nullptr)
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

    m_Window = window;
    m_RhiDevice = &device;
#ifdef _WIN32
    m_D3D11Device = rhi::d3d11::AsD3D11Device(device);
    if (m_D3D11Device != nullptr)
    {
        if (!ImGui_ImplSDL3_InitForD3D(window) ||
            !ImGui_ImplDX11_Init(
                m_D3D11Device->NativeDevice(), m_D3D11Device->NativeContext()))
        {
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            m_Window = nullptr;
            m_RhiDevice = nullptr;
            m_D3D11Device = nullptr;
            return false;
        }
        m_Ready = true;
        return true;
    }
#endif
    auto* nativeDevice = rhi::sdl::AsSdlDevice(device);
    if (nativeDevice == nullptr)
    {
        ImGui::DestroyContext();
        return false;
    }
    m_Device = nativeDevice->NativeDevice();
    if (!ImGui_ImplSDL3_InitForSDLGPU(window))
    {
        ImGui::DestroyContext();
        return false;
    }

    ImGui_ImplSDLGPU3_InitInfo initInfo{};
    initInfo.Device = m_Device;
    initInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_Device, window);
    initInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&initInfo))
    {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return false;
    }

    const bool legacyDxbc = std::strcmp(SDL_GetGPUDeviceDriver(m_Device), "direct3d12") == 0 &&
        (SDL_GetGPUShaderFormats(m_Device) & SDL_GPU_SHADERFORMAT_DXIL) == 0;
    if (legacyDxbc && !CreateLegacyDxbcPipeline())
    {
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_Window = nullptr;
        m_Device = nullptr;
        return false;
    }
    if (legacyDxbc)
    {
        SDL_Log("[Fadix] Using runtime-compiled DXBC ImGui pipeline for legacy D3D12 GPU");
    }

    m_Ready = true;
    return true;
}

bool ImGuiLayer::CreateLegacyDxbcPipeline()
{
    try
    {
        const auto fail = [this]() {
            const std::string error = SDL_GetError();
            DestroyLegacyDxbcPipeline();
            SDL_SetError("%s", error.c_str());
            return false;
        };
        const auto source = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(LegacyImGuiShader), sizeof(LegacyImGuiShader) - 1};
        const std::vector<std::byte> vertexCode =
            render::CompileShader(source, "VertexMain", "vs_5_1", "FadixImGuiLegacy.hlsl");
        const std::vector<std::byte> fragmentCode =
            render::CompileShader(source, "FragmentMain", "ps_5_1", "FadixImGuiLegacy.hlsl");

        SDL_GPUShaderCreateInfo shader{};
        shader.format = SDL_GPU_SHADERFORMAT_DXBC;
        shader.entrypoint = "VertexMain";
        shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        shader.num_uniform_buffers = 1;
        shader.code = reinterpret_cast<const Uint8*>(vertexCode.data());
        shader.code_size = vertexCode.size();
        m_LegacyDxbcVertexShader = SDL_CreateGPUShader(m_Device, &shader);
        if (m_LegacyDxbcVertexShader == nullptr)
        {
            return fail();
        }

        shader.entrypoint = "FragmentMain";
        shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        shader.num_uniform_buffers = 0;
        shader.num_samplers = 1;
        shader.code = reinterpret_cast<const Uint8*>(fragmentCode.data());
        shader.code_size = fragmentCode.size();
        m_LegacyDxbcFragmentShader = SDL_CreateGPUShader(m_Device, &shader);
        if (m_LegacyDxbcFragmentShader == nullptr)
        {
            return fail();
        }

        SDL_GPUVertexBufferDescription buffer{};
        buffer.slot = 0;
        buffer.pitch = sizeof(ImDrawVert);
        buffer.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attributes[3]{};
        attributes[0] = {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ImDrawVert, pos)};
        attributes[1] = {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(ImDrawVert, uv)};
        attributes[2] = {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(ImDrawVert, col)};

        SDL_GPUColorTargetDescription color{};
        color.format = SDL_GetGPUSwapchainTextureFormat(m_Device, m_Window);
        color.blend_state.enable_blend = true;
        color.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        color.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        color.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        color.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        color.blend_state.color_write_mask = 0xF;

        SDL_GPUGraphicsPipelineCreateInfo pipeline{};
        pipeline.vertex_shader = m_LegacyDxbcVertexShader;
        pipeline.fragment_shader = m_LegacyDxbcFragmentShader;
        pipeline.vertex_input_state = {&buffer, 1, attributes, 3};
        pipeline.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipeline.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipeline.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipeline.rasterizer_state.enable_depth_clip = true;
        pipeline.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipeline.target_info.color_target_descriptions = &color;
        pipeline.target_info.num_color_targets = 1;
        m_LegacyDxbcPipeline = SDL_CreateGPUGraphicsPipeline(m_Device, &pipeline);
        if (m_LegacyDxbcPipeline == nullptr)
        {
            return fail();
        }
        return true;
    }
    catch (const std::exception& error)
    {
        SDL_SetError("Legacy ImGui shader compilation failed: %s", error.what());
        DestroyLegacyDxbcPipeline();
        return false;
    }
}

void ImGuiLayer::DestroyLegacyDxbcPipeline()
{
    if (m_LegacyDxbcPipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_Device, m_LegacyDxbcPipeline);
        m_LegacyDxbcPipeline = nullptr;
    }
    if (m_LegacyDxbcVertexShader != nullptr)
    {
        SDL_ReleaseGPUShader(m_Device, m_LegacyDxbcVertexShader);
        m_LegacyDxbcVertexShader = nullptr;
    }
    if (m_LegacyDxbcFragmentShader != nullptr)
    {
        SDL_ReleaseGPUShader(m_Device, m_LegacyDxbcFragmentShader);
        m_LegacyDxbcFragmentShader = nullptr;
    }
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
#ifdef _WIN32
    if (m_D3D11Device != nullptr)
    {
        ImGui_ImplDX11_Shutdown();
    }
    else
#endif
    {
        ImGui_ImplSDLGPU3_Shutdown();
        DestroyLegacyDxbcPipeline();
    }
    ImGui::DestroyContext();
    m_Window = nullptr;
    m_RhiDevice = nullptr;
    m_Device = nullptr;
    m_D3D11Device = nullptr;
    m_BackbufferWidth = 0;
    m_BackbufferHeight = 0;
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
#ifdef _WIN32
    if (m_D3D11Device != nullptr)
    {
        ImGui_ImplDX11_NewFrame();
    }
    else
#endif
    {
        ImGui_ImplSDLGPU3_NewFrame();
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::RenderAndSubmit(const AfterImGuiPass& afterImGui)
{
    if (!m_Ready || m_Window == nullptr)
    {
        return;
    }

    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    const bool minimized =
        drawData == nullptr || drawData->DisplaySize.x <= 0.0F || drawData->DisplaySize.y <= 0.0F;

#ifdef _WIN32
    if (m_D3D11Device != nullptr)
    {
        if (!minimized)
        {
            int width = 0;
            int height = 0;
            SDL_GetWindowSizeInPixels(m_Window, &width, &height);
            const auto pixelWidth = static_cast<std::uint32_t>((std::max)(width, 1));
            const auto pixelHeight = static_cast<std::uint32_t>((std::max)(height, 1));
            if (pixelWidth != m_BackbufferWidth || pixelHeight != m_BackbufferHeight)
            {
                if (!m_D3D11Device->ResizeBackbuffer(pixelWidth, pixelHeight))
                {
                    return;
                }
                m_BackbufferWidth = pixelWidth;
                m_BackbufferHeight = pixelHeight;
            }
            ID3D11RenderTargetView* view = m_D3D11Device->BackbufferView();
            const float clear[4]{0.08F, 0.09F, 0.10F, 1.0F};
            m_D3D11Device->NativeContext()->OMSetRenderTargets(1, &view, nullptr);
            m_D3D11Device->NativeContext()->ClearRenderTargetView(view, clear);
            ImGui_ImplDX11_RenderDrawData(drawData);
        }
        m_D3D11Device->Present();
        return;
    }
#endif

    if (m_Device == nullptr)
    {
        return;
    }

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
        ImGui_ImplSDLGPU3_RenderDrawData(
            drawData, commandBuffer, pass, m_LegacyDxbcPipeline);
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
