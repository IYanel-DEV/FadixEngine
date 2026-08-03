#include "player/PlayerApplication.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/GltfMeshCache.hpp"
#include "assets/ScriptDatabase.hpp"
#include "editor/camera/CameraSelection.hpp"
#include "editor/play/PlaySession.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "editor/ui/GameUIOverlay.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/audio/AudioEngine.hpp"
#include "engine/audio/AudioPlayback.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/scene/IWorld.hpp"
#include "project/ProjectJson.hpp"
#include "project/ProjectService.hpp"
#include "render/ViewportRendererFactory.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#ifdef _WIN32
#include "rhi/d3d11/D3D11Rhi.hpp"
#endif
#include "runtime/Components.hpp"
#include "scripting/ScriptRunner.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_SDL_GPU.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace fadix
{
namespace
{
class DiskFileInterface final : public Rml::FileInterface
{
public:
    Rml::FileHandle Open(const Rml::String& path) override
    {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            return 0;
        }
        const auto size = stream.tellg();
        if (size < 0)
        {
            return 0;
        }
        stream.seekg(0);
        auto* file = new OpenedFile{};
        file->Bytes.resize(static_cast<std::size_t>(size));
        if (size > 0 &&
            !stream.read(file->Bytes.data(), static_cast<std::streamsize>(size)))
        {
            delete file;
            return 0;
        }
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle file) override { delete reinterpret_cast<OpenedFile*>(file); }

    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override
    {
        auto* opened = reinterpret_cast<OpenedFile*>(file);
        if (opened == nullptr || opened->Offset >= opened->Bytes.size())
        {
            return 0;
        }
        const size_t available = opened->Bytes.size() - opened->Offset;
        const size_t toRead = size < available ? size : available;
        std::memcpy(buffer, opened->Bytes.data() + opened->Offset, toRead);
        opened->Offset += toRead;
        return toRead;
    }

    bool Seek(Rml::FileHandle file, long offset, int origin) override
    {
        auto* opened = reinterpret_cast<OpenedFile*>(file);
        if (opened == nullptr)
        {
            return false;
        }
        long next = 0;
        if (origin == SEEK_SET)
        {
            next = offset;
        }
        else if (origin == SEEK_CUR)
        {
            next = static_cast<long>(opened->Offset) + offset;
        }
        else if (origin == SEEK_END)
        {
            next = static_cast<long>(opened->Bytes.size()) + offset;
        }
        else
        {
            return false;
        }
        if (next < 0 || static_cast<std::size_t>(next) > opened->Bytes.size())
        {
            return false;
        }
        opened->Offset = static_cast<std::size_t>(next);
        return true;
    }

    size_t Tell(Rml::FileHandle file) override
    {
        auto* opened = reinterpret_cast<OpenedFile*>(file);
        return opened == nullptr ? 0 : opened->Offset;
    }

    size_t Length(Rml::FileHandle file) override
    {
        auto* opened = reinterpret_cast<OpenedFile*>(file);
        return opened == nullptr ? 0 : opened->Bytes.size();
    }

private:
    struct OpenedFile
    {
        std::vector<char> Bytes;
        std::size_t Offset{0};
    };
};

} // namespace

PlayerApplication::PlayerApplication(PlayerLaunchOptions options) : m_Options(std::move(options)) {}

PlayerApplication::~PlayerApplication() = default;

int PlayerApplication::Run()
{
#ifdef _WIN32
    if (SDL_GetHint(SDL_HINT_GPU_DRIVER) == nullptr)
    {
        SDL_SetHint(SDL_HINT_GPU_DRIVER, "direct3d12");
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        throw std::runtime_error(std::string{"SDL_Init failed: "} + SDL_GetError());
    }

    ProjectService projects;
    auto opened = projects.Open(m_Options.ProjectFile);
    if (!opened)
    {
        throw std::runtime_error(opened.ErrorMessage());
    }
    ProjectMetadata project = std::move(opened).Value();
    const std::string bootRelative = !m_Options.BootScene.empty()
        ? m_Options.BootScene
        : (project.DefaultScene.empty() ? "Scenes/Main.scene" : project.DefaultScene);
    const std::filesystem::path bootPath = project.RootPath / bootRelative;
    if (!std::filesystem::exists(bootPath))
    {
        throw std::runtime_error("Boot scene missing: " + bootPath.string());
    }

    auto world = sceneplay::CreateEditWorld();
    SceneDocument document{
        Uuid::Generate(),
        std::filesystem::path{bootRelative}.stem().string(),
        bootPath,
        false};
    SceneService scenes;
    if (const Result<void> loaded = scenes.Load(document, *world, bootPath); !loaded)
    {
        throw std::runtime_error(loaded.ErrorMessage());
    }

    std::size_t entityCount = 0;
    for (const auto entity : world->Registry().view<NameComponent>())
    {
        static_cast<void>(entity);
        ++entityCount;
    }
    std::clog << "[FadixPlayer] Loaded boot scene '" << bootRelative << "' (" << entityCount
              << " entities)\n";
    if (m_Options.SmokeExit)
    {
        std::cout << "SMOKE_OK boot=" << bootRelative << " entities=" << entityCount << '\n';
        SDL_Quit();
        return 0;
    }

    SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_RESIZABLE;
    if (m_Options.Fullscreen)
    {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    SDL_Window* window = SDL_CreateWindow(
        project.Name.c_str(),
        std::max(320, m_Options.Width),
        std::max(240, m_Options.Height),
        flags);
    if (window == nullptr)
    {
        SDL_Quit();
        throw std::runtime_error(std::string{"SDL_CreateWindow failed: "} + SDL_GetError());
    }

    auto device = CreateDeviceFromWindow(window);
    if (!device)
    {
        const std::string gpuError = SDL_GetError();
        SDL_DestroyWindow(window);
        SDL_Quit();
        throw std::runtime_error(
            "CreateDeviceFromWindow failed: " +
            (gpuError.empty() ? std::string{"unknown SDL GPU error"} : gpuError));
    }
    auto* nativeDevice = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*device));
    if (nativeDevice != nullptr && m_Options.VSync)
    {
        static_cast<void>(SDL_SetGPUSwapchainParameters(
            nativeDevice,
            window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_VSYNC));
    }
    else if (nativeDevice != nullptr)
    {
        static_cast<void>(SDL_SetGPUSwapchainParameters(
            nativeDevice,
            window,
            SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
            SDL_GPU_PRESENTMODE_IMMEDIATE));
    }

    auto assets = CreateAssetDatabase(project.RootPath);
    auto* assetDb = dynamic_cast<AssetDatabase*>(assets.get());
    if (assetDb != nullptr)
    {
        static_cast<void>(assetDb->Refresh());
    }
    auto viewport = CreateViewportRenderer(*device, *assets);
    viewport->SetEditorVisualsEnabled(false);
    auto gltf = std::make_unique<GltfMeshCache>(*device);
    gltf->SetAssetDatabase(assets.get());
    viewport->SetGltfMeshCache(gltf.get());

    ScriptDatabase scriptDb;
    scriptDb.ScanFolder(project.RootPath);
    PlaySession play;
    play.SetScriptContext(
        [&scriptDb](const std::string& name) -> std::optional<ScriptRunner::ResolvedScript> {
            auto asset = scriptDb.Get(name);
            if (!asset)
            {
                return std::nullopt;
            }
            ScriptAsset& script = asset->get();
            if (!script.Loaded)
            {
                scriptDb.LoadSource(script);
            }
            return ScriptRunner::ResolvedScript{
                std::string(script.SourceCode.begin(), script.SourceCode.end()),
                script.Language,
                script.SourcePath};
        },
        [](const std::string& message, const char* severity) {
            std::clog << "[script:" << (severity != nullptr ? severity : "info") << "] " << message
                      << '\n';
        },
        nullptr);
    // Gameplay world API for the shipped runtime: same Prefab.spawn / Scene.load
    // the editor exposes, backed by the player's mesh cache and project root.
    std::filesystem::path saveDirectory = project.RootPath / "Saved" / "Saves";
    const std::string saveId = project.Id.ToString();
    if (char* pref = SDL_GetPrefPath("FadixEngine", saveId.c_str()); pref != nullptr)
    {
        saveDirectory = std::filesystem::path{pref} / "Saves";
        SDL_free(pref);
    }
    play.SetGameServices(
        [&gltf]() { return sceneplay::CreatePhysicsWorldAdapter(gltf.get()); },
        [&project](const std::string& relative) { return project.RootPath / relative; },
        std::move(saveDirectory));

    std::unique_ptr<AudioEngine> audio = std::make_unique<AudioEngine>();
    std::unique_ptr<AudioPlayback> audioPlayback;
    if (audio->Initialize())
    {
        play.BindAudio(audio.get());
        audioPlayback = std::make_unique<AudioPlayback>(*audio);
    }
    else
    {
        audio.reset();
    }

    auto physics = sceneplay::CreatePhysicsWorldAdapter(gltf.get());
    if (!play.Start(*world, std::move(physics)))
    {
        throw std::runtime_error("PlaySession::Start failed");
    }
    if (audioPlayback && play.RuntimeWorld() != nullptr)
    {
        audioPlayback->Start(*play.RuntimeWorld(), assets.get());
    }

    int pixelW = m_Options.Width;
    int pixelH = m_Options.Height;
    SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);

    std::unique_ptr<SystemInterface_SDL> system;
    std::unique_ptr<RenderInterface_SDL_GPU> render;
    std::unique_ptr<DiskFileInterface> file;
    std::unique_ptr<GameUIOverlay> gameUi;
    constexpr SDL_GPUShaderFormat rmlShaderFormats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    if (nativeDevice != nullptr &&
        (SDL_GetGPUShaderFormats(nativeDevice) & rmlShaderFormats) != 0)
    {
        system = std::make_unique<SystemInterface_SDL>();
        system->SetWindow(window);
        render = std::make_unique<RenderInterface_SDL_GPU>(nativeDevice, window);
        file = std::make_unique<DiskFileInterface>();
        Rml::SetFileInterface(file.get());
        Rml::SetSystemInterface(system.get());
        Rml::SetRenderInterface(render.get());
        if (!Rml::Initialise())
        {
            throw std::runtime_error("RmlUi initialise failed");
        }
        static_cast<void>(Rml::LoadFontFace("C:/Windows/Fonts/arial.ttf"));
        gameUi = std::make_unique<GameUIOverlay>();
        if (!gameUi->Initialize(pixelW, pixelH, SDL_GetWindowDisplayScale(window)))
        {
            throw std::runtime_error("GameUIOverlay initialise failed");
        }
        gameUi->SetActive(true);
    }
    else
    {
        std::clog << "[FadixPlayer] Game UI overlay disabled on compatibility renderer.\n";
    }

    bool running = true;
    auto previous = std::chrono::steady_clock::now();
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE)
            {
                running = false;
            }
        }
        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(10);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dt =
            std::min(std::chrono::duration<float>(now - previous).count(), 0.1F);
        previous = now;

        play.Update(dt);
        if (audioPlayback && play.RuntimeWorld() != nullptr)
        {
            audioPlayback->Update(*play.RuntimeWorld(), dt);
        }
        IWorld* runtime = play.RuntimeWorld();
        if (runtime == nullptr)
        {
            break;
        }

        SDL_GetWindowSizeInPixels(window, &pixelW, &pixelH);
        if (pixelW > 0 && pixelH > 0)
        {
            viewport->Resize(
                rhi::Extent2D{static_cast<std::uint32_t>(pixelW), static_cast<std::uint32_t>(pixelH)});
        }

        std::vector<editor::CameraCandidate> candidates;
        std::vector<entt::entity> entities;
        for (const auto [entity, component] :
             runtime->Registry().view<const CameraComponent>().each())
        {
            editor::CameraCandidate candidate;
            candidate.Id = candidates.size();
            candidate.Settings = editor::CameraSettingsFromComponent(component);
            candidates.push_back(candidate);
            entities.push_back(entity);
        }
        const auto primary = editor::SelectPrimaryCamera(candidates);
        if (primary && *primary < entities.size())
        {
            const entt::entity entity = entities[*primary];
            const TransformComponent* transform =
                runtime->Registry().try_get<TransformComponent>(entity);
            const CameraComponent* component =
                runtime->Registry().try_get<CameraComponent>(entity);
            if (transform != nullptr && component != nullptr)
            {
                const glm::vec3 forward = transform->Rotation * glm::vec3{0.0F, 0.0F, -1.0F};
                const glm::vec3 up = transform->Rotation * glm::vec3{0.0F, 1.0F, 0.0F};
                const float aspect =
                    pixelH > 0 ? static_cast<float>(pixelW) / static_cast<float>(pixelH) : 16.0F / 9.0F;
                viewport->SetCamera(
                    editor::BuildGameCameraView(transform->Position, forward, up),
                    editor::BuildGameCameraProjection(
                        editor::CameraSettingsFromComponent(*component), aspect));
            }
        }

        viewport->SetSimDelta(dt);
        viewport->UpdateAnimations(*runtime, dt);
        viewport->DrawWorld(*runtime);

        if (gameUi)
        {
            gameUi->Sync(
                *runtime,
                *assets,
                pixelW,
                pixelH,
                SDL_GetWindowDisplayScale(window),
                0.0F,
                0.0F,
                static_cast<float>(pixelW),
                static_cast<float>(pixelH));
            gameUi->Update();
        }

        // Fix blit source extents to the color target size.
        rhi::Texture* color = viewport->ColorTarget();
#ifdef _WIN32
        if (auto* d3d11 = rhi::d3d11::AsD3D11Device(*device); d3d11 != nullptr)
        {
            d3d11->ResizeBackbuffer(
                static_cast<std::uint32_t>(std::max(pixelW, 1)),
                static_cast<std::uint32_t>(std::max(pixelH, 1)));
            d3d11->PresentTexture(color, m_Options.VSync);
        }
        else
#endif
        if (color != nullptr && nativeDevice != nullptr)
        {
            SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(nativeDevice);
            if (commandBuffer != nullptr)
            {
                SDL_GPUTexture* swapchain = nullptr;
                std::uint32_t sw = 0;
                std::uint32_t sh = 0;
                if (SDL_WaitAndAcquireGPUSwapchainTexture(
                        commandBuffer, window, &swapchain, &sw, &sh) &&
                    swapchain != nullptr)
                {
                    auto* source =
                        static_cast<SDL_GPUTexture*>(GetNativeTextureHandle(*color));
                    if (source != nullptr && sw > 0 && sh > 0)
                    {
                        SDL_GPUBlitInfo blit{};
                        blit.source.texture = source;
                        blit.source.w = static_cast<Uint32>(pixelW);
                        blit.source.h = static_cast<Uint32>(pixelH);
                        blit.destination.texture = swapchain;
                        blit.destination.w = sw;
                        blit.destination.h = sh;
                        blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                        blit.filter = SDL_GPU_FILTER_LINEAR;
                        SDL_BlitGPUTexture(commandBuffer, &blit);
                    }
                    if (render && gameUi)
                    {
                        render->BeginFrame(commandBuffer, swapchain, sw, sh);
                        gameUi->Render();
                        render->EndFrame();
                    }
                    SDL_SubmitGPUCommandBuffer(commandBuffer);
                }
                else
                {
                    SDL_CancelGPUCommandBuffer(commandBuffer);
                }
            }
        }
        else if (nativeDevice != nullptr)
        {
            SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(nativeDevice);
            if (commandBuffer != nullptr)
            {
                SDL_GPUTexture* swapchain = nullptr;
                std::uint32_t sw = 0;
                std::uint32_t sh = 0;
                if (SDL_WaitAndAcquireGPUSwapchainTexture(
                        commandBuffer, window, &swapchain, &sw, &sh) &&
                    swapchain != nullptr)
                {
                    SDL_GPUColorTargetInfo clear{};
                    clear.texture = swapchain;
                    clear.load_op = SDL_GPU_LOADOP_CLEAR;
                    clear.store_op = SDL_GPU_STOREOP_STORE;
                    clear.clear_color = SDL_FColor{0.05F, 0.06F, 0.08F, 1.0F};
                    SDL_GPURenderPass* pass =
                        SDL_BeginGPURenderPass(commandBuffer, &clear, 1, nullptr);
                    SDL_EndGPURenderPass(pass);
                    if (render && gameUi)
                    {
                        render->BeginFrame(commandBuffer, swapchain, sw, sh);
                        gameUi->Render();
                        render->EndFrame();
                    }
                    SDL_SubmitGPUCommandBuffer(commandBuffer);
                }
                else
                {
                    SDL_CancelGPUCommandBuffer(commandBuffer);
                }
            }
        }
    }

    play.Stop();
    if (audioPlayback)
    {
        audioPlayback->Stop();
    }
    if (gameUi)
    {
        gameUi->Shutdown();
        gameUi.reset();
        Rml::Shutdown();
        render->Shutdown();
        render.reset();
    }
    system.reset();
    file.reset();
    viewport.reset();
    gltf.reset();
    device.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
}
