#include "editor/imgui/ImGuiEditorApplication.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/EmbeddedAssetProvider.hpp"
#include "assets/GltfMeshCache.hpp"
#include "editor/camera/CameraSelection.hpp"
#include "editor/play/PlaySession.hpp"
#include "editor/scene/PrefabSerializer.hpp"
#include "editor/scene/SceneEditor.hpp"
#include "editor/ui/GameUIOverlay.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/audio/AudioEngine.hpp"
#include "engine/audio/AudioPlayback.hpp"
#include "engine/camera/EditorCamera.hpp"
#include "engine/render/RenderQuality.hpp"
#include "engine/scene/IWorld.hpp"
#include "project/ProjectJson.hpp"
#include "project/ProjectService.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/Components.hpp"
#include "scripting/LuaVM.hpp"
#include "scripting/NativeScriptLoader.hpp"
#include "scripting/ScriptRunner.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_SDL_GPU.h>
#include <imgui.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
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

    void Close(Rml::FileHandle file) override
    {
        delete reinterpret_cast<OpenedFile*>(file);
    }

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
}

ImGuiEditorApplication::ImGuiEditorApplication()
    : m_AssetRoot(RuntimeAssetRoot())
{
}

ImGuiEditorApplication::~ImGuiEditorApplication()
{
    Shutdown();
}

bool ImGuiEditorApplication::InitializeGameUi()
{
    if (m_Window == nullptr || !m_Device)
    {
        return false;
    }
    auto* nativeDevice = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device));
    if (nativeDevice == nullptr)
    {
        std::clog << "[Fadix] Game UI overlay disabled on Direct3D 11 compatibility renderer.\n";
        return true;
    }
    constexpr SDL_GPUShaderFormat rmlShaderFormats =
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    if ((SDL_GetGPUShaderFormats(nativeDevice) & rmlShaderFormats) == 0)
    {
        std::clog
            << "[Fadix] Game UI overlay disabled: this GPU supports DXBC only; "
               "RmlUi requires SPIR-V, DXIL or MSL.\n";
        return true;
    }

    auto system = std::make_unique<SystemInterface_SDL>();
    system->SetWindow(m_Window);
    auto render = std::make_unique<RenderInterface_SDL_GPU>(nativeDevice, m_Window);
    auto file = std::make_unique<DiskFileInterface>();
    m_RmlSystem = std::move(system);
    m_RmlRender = std::move(render);
    m_RmlFile = std::move(file);
    Rml::SetFileInterface(m_RmlFile.get());
    Rml::SetSystemInterface(m_RmlSystem.get());
    Rml::SetRenderInterface(m_RmlRender.get());
    if (!Rml::Initialise())
    {
        ShutdownGameUi();
        return false;
    }
    const std::filesystem::path fontPath = m_AssetRoot / "fonts" / "LatoLatin-Regular.ttf";
    if (!Rml::LoadFontFace(fontPath.string()))
    {
        static_cast<void>(Rml::LoadFontFace("C:/Windows/Fonts/arial.ttf"));
    }
    int pixelWidth = 1;
    int pixelHeight = 1;
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);
    m_GameUi = std::make_unique<GameUIOverlay>();
    if (!m_GameUi->Initialize(
            pixelWidth, pixelHeight, SDL_GetWindowDisplayScale(m_Window)))
    {
        ShutdownGameUi();
        return false;
    }
    return true;
}

void ImGuiEditorApplication::ShutdownGameUi()
{
    if (m_GameUi)
    {
        m_GameUi->Shutdown();
        m_GameUi.reset();
    }
    if (Rml::GetRenderInterface() != nullptr || Rml::GetSystemInterface() != nullptr)
    {
        Rml::Shutdown();
    }
    if (auto* gpuRender = dynamic_cast<RenderInterface_SDL_GPU*>(m_RmlRender.get()))
    {
        gpuRender->Shutdown();
    }
    m_RmlRender.reset();
    m_RmlSystem.reset();
    m_RmlFile.reset();
}

void ImGuiEditorApplication::SyncGameUi()
{
    if (!m_GameUi || !m_Ui.InWorkbench || m_Window == nullptr)
    {
        return;
    }
    const EditorPlayMode mode = m_Session.PlayMode();
    if (mode != EditorPlayMode::Play && mode != EditorPlayMode::Paused)
    {
        return;
    }
    IWorld* playWorld = m_Session.PlayWorld();
    if (playWorld == nullptr || !m_Session.assetDatabase)
    {
        return;
    }
    int pixelWidth = 1;
    int pixelHeight = 1;
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    const float vx = m_Viewports.GameViewImageMin().x * scale.x;
    const float vy = m_Viewports.GameViewImageMin().y * scale.y;
    const float vw = m_Viewports.GameViewImageSize().x * scale.x;
    const float vh = m_Viewports.GameViewImageSize().y * scale.y;
    m_GameUi->Sync(
        *playWorld,
        *m_Session.assetDatabase,
        pixelWidth,
        pixelHeight,
        SDL_GetWindowDisplayScale(m_Window),
        vx,
        vy,
        vw,
        vh);
    m_GameUi->Update();
}

void ImGuiEditorApplication::Shutdown()
{
    if (m_Window == nullptr)
    {
        return;
    }
    m_Shell.SaveLayout(m_Ui);
    if (m_ScriptEditor)
    {
        static_cast<void>(m_ScriptEditor->SaveSession());
    }
    m_ContentBrowser.Shutdown();
    m_ScriptEditor.reset();
    m_MaterialEditor.reset();
    m_AssetBrowser.reset();
    m_Thumbnails.reset();
    m_Scripts.reset();
    m_Viewports.Shutdown();
    m_Chrome.Unbind();
    if (m_Device)
    {
        auto* native = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device));
        m_Theme.ReleaseLogo(native);
    }
    ShutdownGameUi();
    m_ImGui.Shutdown();
    m_Device.reset();
    m_AudioPlayback.reset();
    if (m_AudioEngine)
    {
        m_AudioEngine->Shutdown();
        m_AudioEngine.reset();
    }
    if (m_Window != nullptr)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void ImGuiEditorApplication::OnDisplayScaleChanged()
{
    if (m_Window == nullptr)
    {
        return;
    }
    const float dpiScale = SDL_GetWindowDisplayScale(m_Window);
    if (dpiScale <= 0.0F || std::fabs(dpiScale - m_AppliedDpiScale) < 0.001F)
    {
        return;
    }
    m_Theme.Apply();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;
    m_AppliedDpiScale = dpiScale;
}

void ImGuiEditorApplication::RequestWithConfirm(const editor::PendingConfirmAction action)
{
    if (!m_Session.Document().Dirty)
    {
        ExecuteConfirmAction(action);
        return;
    }
    m_Ui.ConfirmAction = action;
    m_Ui.ConfirmResult = editor::ConfirmChoice::None;
    m_Ui.ShowConfirmDiscard = true;
}

void ImGuiEditorApplication::ExecuteConfirmAction(const editor::PendingConfirmAction action)
{
    using editor::PendingConfirmAction;
    switch (action)
    {
    case PendingConfirmAction::NewScene:
        m_Session.NewScene();
        m_Viewports.ResetTemporalHistory();
        m_Ui.StatusText = "Created a new scene";
        break;
    case PendingConfirmAction::OpenScene:
        m_Ui.ShowOpenSceneDialog = true;
        break;
    case PendingConfirmAction::CreateSceneAsset:
        if (m_PendingSceneAsset)
        {
            const std::filesystem::path path = *m_PendingSceneAsset;
            m_Session.NewScene(path.stem().string());
            m_Viewports.ResetTemporalHistory();
            static_cast<void>(SaveScenePath(path));
            m_PendingSceneAsset.reset();
        }
        break;
    case PendingConfirmAction::OpenSceneAsset:
        if (m_PendingSceneAsset)
        {
            OpenScenePath(*m_PendingSceneAsset);
            m_PendingSceneAsset.reset();
        }
        break;
    case PendingConfirmAction::ExitToProjectManager:
        ExitToProjectManager();
        break;
    case PendingConfirmAction::Quit:
        m_Running = false;
        break;
    case PendingConfirmAction::None:
        break;
    }
    m_Ui.ConfirmAction = PendingConfirmAction::None;
}

void ImGuiEditorApplication::WireAssetBrowser()
{
    if (!m_Session.assetDatabase || !m_Session.projectService)
    {
        return;
    }
    m_Thumbnails = std::make_unique<AssetThumbnailCache>(*m_Session.assetDatabase);
    m_Thumbnails->SetProjectRoot(m_Session.ActiveProject().RootPath);
    m_AssetBrowser = std::make_unique<AssetBrowserController>(
        *m_Session.assetDatabase, *m_Session.projectService);
    m_AssetBrowser->SetProjectRoot(m_Session.ActiveProject().RootPath);
    m_Scripts = std::make_unique<ScriptDatabase>();
    m_Scripts->ScanFolder(m_Session.ActiveProject().RootPath);
    m_ScriptEditor = std::make_unique<editor::ScriptEditorController>();
    m_ScriptEditor->Bind(*m_Scripts);
    m_ScriptEditor->SetSessionPath(
        m_Session.ActiveProject().RootPath / "Saved" / "Editor" / "fxs_session.json");
    m_ScriptEditor->SetValidationDebounceMs(400);
    m_ScriptEditor->SetValidator(
        [this](const ScriptAsset& script) {
            editor::ScriptValidationResult result;
            const std::string source{script.SourceCode.begin(), script.SourceCode.end()};
            if (script.Language == ScriptLanguage::Lua)
            {
#ifdef FADIX_ENABLE_LUA
                // Fresh VM: load()/compile only — never runs the chunk or touches the scene.
                LuaVM validator;
                if (!validator.Compile(script.SourcePath.filename().string(), source))
                {
                    result.Success = false;
                    result.Message = validator.LastError();
                    if (result.Message.empty())
                    {
                        result.Message = "Lua validation is unavailable.";
                    }
                }
#else
                static_cast<void>(source);
                result.Success = true;
#endif
            }
            else if (m_NativeScriptLoader != nullptr)
            {
                // Native validate is compile-only (existing NativeScriptLoader::Validate).
                if (const std::optional<std::string> error =
                        m_NativeScriptLoader->Validate(script.SourcePath))
                {
                    result.Success = false;
                    result.Message = *error;
                }
            }
            else
            {
                result.Success = false;
                result.Message = "Native script compiler is unavailable.";
            }
            return result;
        },
        [this](const ScriptAsset& script, const editor::ScriptValidationResult& result) {
            if (result.Success)
            {
                m_Log.ClearScriptDiagnostics(script.Name);
                return;
            }
            m_Log.LogScriptDiagnostic(script.Name, result.Line, result.Column, result.Message);
            // Missing MSVC toolchain is expected on many machines — don't steal focus.
            const bool toolchainMissing =
                result.Message.find("cl.exe") != std::string::npos
                || result.Message.find("toolchain") != std::string::npos
                || result.Message.find("Developer Command Prompt") != std::string::npos;
            if (!toolchainMissing)
            {
                m_Ui.ShowOutput = true;
            }
        });
    m_ScriptPanel.Bind(*m_ScriptEditor, m_Scripts.get(), m_Window);
    static_cast<void>(m_ScriptEditor->LoadSession());

    const auto& projectRoot = m_Session.ActiveProject().RootPath;
    m_NativeScriptLoader =
        std::make_unique<NativeScriptLoader>(projectRoot / "Scripts" / ".native");
    std::vector<std::filesystem::path> scriptIncludeDirs;
#ifdef FADIX_SRC_DIR
    scriptIncludeDirs.emplace_back(FADIX_SRC_DIR);
#endif
#ifdef FADIX_NATIVE_INCLUDES_FILE
    if (std::ifstream includes{FADIX_NATIVE_INCLUDES_FILE})
    {
        for (std::string line; std::getline(includes, line);)
        {
            for (std::size_t start = 0; start <= line.size();)
            {
                const std::size_t sep = line.find(';', start);
                const std::string dir = line.substr(
                    start, sep == std::string::npos ? std::string::npos : sep - start);
                if (!dir.empty())
                {
                    scriptIncludeDirs.emplace_back(dir);
                }
                if (sep == std::string::npos)
                {
                    break;
                }
                start = sep + 1;
            }
        }
    }
#endif
    m_NativeScriptLoader->SetIncludeDirs(std::move(scriptIncludeDirs));
    m_NativeScriptLoader->SetLogger([this](const std::string& message, const char* severity) {
        m_Log.Log(message, severity != nullptr ? severity : "info");
        m_Ui.ShowOutput = true;
    });

    // Scripts/audio must be wired before Play — otherwise PlayerController never runs.
    m_Session.Play().SetScriptContext(
        [this](const std::string& name) -> std::optional<ScriptRunner::ResolvedScript> {
            if (!m_Scripts)
            {
                return std::nullopt;
            }
            auto asset = m_Scripts->Get(name);
            if (!asset)
            {
                return std::nullopt;
            }
            ScriptAsset& script = asset->get();
            if (!script.Loaded)
            {
                m_Scripts->LoadSource(script);
            }
            return ScriptRunner::ResolvedScript{
                std::string(script.SourceCode.begin(), script.SourceCode.end()),
                script.Language,
                script.SourcePath};
        },
        [this](const std::string& message, const char* severity) {
            m_Log.Log(message, severity != nullptr ? severity : "info");
            m_Ui.ShowOutput = true;
        },
        m_NativeScriptLoader.get());
    if (m_AudioEngine)
    {
        m_Session.Play().BindAudio(m_AudioEngine.get());
    }
    // Gameplay world API: Prefab.spawn / Scene.load need a physics factory and a
    // project-relative path resolver. Both read live state so they follow the
    // active project and collision meshes.
    m_Session.Play().SetGameServices(
        [this]() { return sceneplay::CreatePhysicsWorldAdapter(m_GltfMeshes.get()); },
        [this](const std::string& relative) {
            return m_Session.ActiveProject().RootPath / relative;
        });

    if (m_Device)
    {
        m_MaterialEditor = std::make_unique<editor::MaterialEditorController>();
        m_MaterialEditor->Bind(*m_Session.assetDatabase, *m_Device);
        m_MaterialPanel.Bind(*m_MaterialEditor);
    }

    auto* native = m_Device
        ? static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device))
        : nullptr;
    m_ContentBrowser.Bind(
        *m_AssetBrowser, *m_Thumbnails, m_Scripts.get(), &m_Log, native);
    m_Output.Bind(m_Log);
    m_Output.SetOpenDiagnostic([this](const editor::OutputEntry& entry) {
        m_Ui.ShowScriptEditor = true;
        if (m_ScriptEditor)
        {
            m_ScriptEditor->SelectScriptAt(entry.ScriptName, entry.Line, entry.Column);
            m_ScriptEditor->SetNativeVisible(true);
        }
        m_Ui.StatusText = "Open " + entry.ScriptName + ':' + std::to_string(entry.Line);
    });

    editor::ContentBrowserCallbacks callbacks;
    callbacks.ReportStatus = [this](std::string_view message) {
        m_Ui.StatusText = std::string{message};
    };
    callbacks.ReportError = [this](std::string_view message) {
        m_Ui.StatusText = std::string{message};
        m_Log.Log(std::string{message}, "error");
        m_Ui.ShowOutput = true;
    };
    callbacks.LoadScene = [this](const AssetHandle&, const std::filesystem::path& path) {
        m_PendingSceneAsset = path;
        RequestWithConfirm(editor::PendingConfirmAction::OpenSceneAsset);
    };
    callbacks.CreateScene = [this](const std::filesystem::path& path) {
        m_PendingSceneAsset = path;
        RequestWithConfirm(editor::PendingConfirmAction::CreateSceneAsset);
    };
    callbacks.OpenMaterial = [this](const AssetHandle& handle, const std::filesystem::path& path) {
        m_Ui.ShowMaterialEditor = true;
        if (m_MaterialEditor)
        {
            m_MaterialEditor->Open(handle);
        }
        m_Ui.StatusText = "Opened material " + path.filename().string();
    };
    callbacks.OpenScript = [this](const std::string& scriptName) {
        m_Ui.ShowScriptEditor = true;
        if (m_ScriptEditor)
        {
            m_ScriptEditor->OpenScript(scriptName);
            m_ScriptEditor->SetNativeVisible(true);
        }
        m_Ui.StatusText = "Opened script " + scriptName;
    };
    callbacks.RevealPath = [this](const std::filesystem::path& path) {
        if (auto* service = dynamic_cast<ProjectService*>(m_Session.projectService.get()))
        {
            if (const Result<void> revealed = service->RevealInFileManager(path))
            {
                m_Ui.StatusText = "Revealed " + path.filename().string();
            }
            else
            {
                m_Ui.StatusText = revealed.ErrorMessage();
            }
        }
    };
    const auto createMeshEntity =
        [this](const AssetHandle& handle,
            const std::filesystem::path& path,
            const glm::vec3& gridPosition) {
            if (!m_SceneEditor)
            {
                return;
            }
            if (const auto id = m_SceneEditor->CreateImportedMeshEntity(
                    path.stem().string(), handle, gridPosition))
            {
                m_SceneEditor->SetSelection(id, true);
                m_Ui.RequestFocusSelection = true;
                m_Ui.StatusText = "Created mesh entity from " + path.filename().string();
                m_Log.Log(m_Ui.StatusText);
            }
        };
    callbacks.CreateMeshEntity =
        [createMeshEntity](const AssetHandle& handle, const std::filesystem::path& path) {
            createMeshEntity(handle, path, glm::vec3{0.0F});
        };
    // DropInScene left empty — ApplySceneDrop / controller CreateMeshEntity handle drops.
    m_ContentBrowser.SetCallbacks(callbacks);
    m_Viewports.SetAssetDropHandler([this](const AssetDragPayload& payload) {
        m_ContentBrowser.ApplySceneDrop(payload, m_SceneEditor.get());
    });
    m_Viewports.SetMeshDropHandler(
        [createMeshEntity](const AssetDragPayload& payload, const glm::vec3& gridPosition) {
            createMeshEntity(payload.Handle, payload.SourcePath, gridPosition);
        });
    static_cast<void>(m_AssetBrowser->Refresh());
}

void ImGuiEditorApplication::EnterWorkbench(const ProjectMetadata& project)
{
    if (const Result<void> opened = m_Session.BeginProject(project); !opened)
    {
        m_Ui.StatusText = "Could not load default scene: " + opened.ErrorMessage();
        m_Log.Log(m_Ui.StatusText, "error");
    }
    m_Session.LeavePlay();
    m_Session.SetAssetDatabase(CreateAssetDatabase(project.RootPath));
    if (!m_SceneEditor)
    {
        m_SceneEditor =
            std::make_unique<SceneEditor>(m_Session.EditWorld(), m_Session.History());
    }
    m_SceneEditor->SetChangedCallback([this]() { m_Session.Document().Dirty = true; });
    m_SceneEditor->SetStatusReporter([this](std::string_view message) {
        m_Ui.StatusText = std::string{message};
    });
    m_SceneEditor->SetAssetDatabase(dynamic_cast<AssetDatabase*>(m_Session.assetDatabase.get()));
    if (m_Device && m_Session.assetDatabase)
    {
        if (!m_GltfMeshes)
        {
            m_GltfMeshes = std::make_unique<GltfMeshCache>(*m_Device);
        }
        else
        {
            m_GltfMeshes->Clear();
        }
        m_GltfMeshes->SetAssetDatabase(m_Session.assetDatabase.get());
        m_Session.SetCollisionMeshProvider(m_GltfMeshes.get());
        m_SceneEditor->SetGltfMeshCache(m_GltfMeshes.get());
        m_Viewports.Initialize(*m_Device, *m_Session.assetDatabase);
        m_Viewports.SetAssetDatabase(*m_Session.assetDatabase);
        m_Viewports.SetGltfMeshCache(m_GltfMeshes.get());
        m_Viewports.SetQualityChangedHandler([this]() { SaveGraphicsSettings(); });
        LoadGraphicsSettings(project.RootPath);
    }
    WireAssetBrowser();
    m_Shell.SetProjectIniPath(project.RootPath, m_Ui);
    m_Ui.InWorkbench = true;
    m_Ui.ShowHierarchy = true;
    m_Ui.ShowInspector = true;
    m_Ui.ShowSceneView = true;
    m_Ui.ShowGameView = true;
    m_Ui.ShowContentBrowser = true;
    m_Ui.ShowOutput = true;
    m_Ui.ProjectName = project.Name;
    m_Ui.PendingProject.reset();
    m_Ui.RequestEnterWorkbench = false;
    m_Ui.StatusText = "Opened project " + project.Name;
    m_Log.Log("Opened project " + project.Name);
    std::clog << "[Fadix] ImGui workbench: " << project.Name << '\n';
}

void ImGuiEditorApplication::ExitToProjectManager()
{
    m_Shell.ClearProjectIni(m_Ui);
    if (m_GameUi)
    {
        m_GameUi->SetActive(false);
    }
    m_Session.LeavePlay();
    if (m_ScriptEditor)
    {
        static_cast<void>(m_ScriptEditor->SaveSession());
    }
    m_ContentBrowser.Shutdown();
    m_ScriptEditor.reset();
    m_MaterialEditor.reset();
    m_AssetBrowser.reset();
    m_Thumbnails.reset();
    m_Scripts.reset();
    m_NativeScriptLoader.reset();
    m_Viewports.Shutdown();
    m_Camera.Input().Reset();
    m_Ui.InWorkbench = false;
    m_Ui.ProjectName = "(no project)";
    if (m_SceneEditor)
    {
        m_SceneEditor->SetSelection(std::nullopt, false);
    }
    m_ProjectManager.Reset(m_Session);
    if (!m_Ui.DefaultIniPath.empty())
    {
        ImGui::LoadIniSettingsFromDisk(m_Ui.DefaultIniPath.c_str());
    }
    m_Ui.StatusText = "Project manager";
}

void ImGuiEditorApplication::LoadGraphicsSettings(const std::filesystem::path& root)
{
    if (root.empty())
    {
        return;
    }
    const std::filesystem::path path = root / "Saved" / "Editor" / "graphics.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return; // No saved choices yet: keep the Scene=Low / Game=High defaults.
    }
    std::ostringstream text;
    text << input.rdbuf();
    editor::GraphicsPreferences prefs = editor::GraphicsPreferences::Defaults();
    if (!editor::ParseGraphicsPreferences(text.str(), prefs))
    {
        m_Log.Log("Ignoring invalid graphics settings " + path.generic_string(), "warn");
        return;
    }
    m_GraphicsPrefs = prefs;
    m_Viewports.SetGraphicsPreferences(prefs);
}

void ImGuiEditorApplication::ApplyGraphicsPreferences()
{
    m_Viewports.SetGraphicsPreferences(m_GraphicsPrefs);
    SaveGraphicsSettings();
}

void ImGuiEditorApplication::DrawGraphicsWindow()
{
    if (!m_Ui.ShowGraphicsWindow)
    {
        return;
    }
    if (!ImGui::Begin("Graphics###Graphics", &m_Ui.ShowGraphicsWindow))
    {
        ImGui::End();
        return;
    }

    editor::GraphicsPreferences& prefs = m_GraphicsPrefs;
    bool changed = false;

    static constexpr std::array<const char*, 4> kQualityLabels{"Low", "Medium", "High", "Epic"};
    int sceneIndex =
        std::clamp(static_cast<int>(prefs.SceneQuality), 0, static_cast<int>(kQualityLabels.size()) - 1);
    if (ImGui::Combo(
            "Scene quality",
            &sceneIndex,
            kQualityLabels.data(),
            static_cast<int>(kQualityLabels.size())))
    {
        prefs.SceneQuality = static_cast<RenderQualityPreset>(sceneIndex);
        changed = true;
    }
    int gameIndex =
        std::clamp(static_cast<int>(prefs.GameQuality), 0, static_cast<int>(kQualityLabels.size()) - 1);
    if (ImGui::Combo(
            "Game quality",
            &gameIndex,
            kQualityLabels.data(),
            static_cast<int>(kQualityLabels.size())))
    {
        prefs.GameQuality = static_cast<RenderQualityPreset>(gameIndex);
        changed = true;
    }

    bool followPreset = prefs.ResolutionScaleOverride <= 0.0F;
    if (ImGui::Checkbox("Follow preset resolution scale", &followPreset))
    {
        prefs.ResolutionScaleOverride = followPreset ? 0.0F : 1.0F;
        changed = true;
    }
    if (!followPreset)
    {
        float scale = prefs.ResolutionScaleOverride;
        if (ImGui::SliderFloat("Resolution scale", &scale, 0.25F, 1.0F, "%.2f"))
        {
            prefs.ResolutionScaleOverride = scale;
            changed = true;
        }
    }

    static constexpr std::array<const char*, 4> kAaLabels{
        "Auto", "Off", "FXAA", "TAA (Experimental)"};
    int aaIndex = std::clamp(static_cast<int>(prefs.AaOverride), 0, static_cast<int>(kAaLabels.size()) - 1);
    if (ImGui::Combo(
            "Anti-aliasing override",
            &aaIndex,
            kAaLabels.data(),
            static_cast<int>(kAaLabels.size())))
    {
        prefs.AaOverride = static_cast<AntiAliasMode>(aaIndex);
        changed = true;
    }

    auto capInput = [&](const char* label, int& value) {
        if (ImGui::InputInt(label, &value))
        {
            value = std::max(0, value);
            changed = true;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("0 = follow quality preset");
        }
    };
    capInput("Shadow cascade cap", prefs.ShadowCascadeCap);
    capInput("Shadow resolution cap", prefs.ShadowResolutionCap);
    capInput("Spot shadow budget cap", prefs.SpotShadowBudgetCap);
    capInput("Point shadow budget cap", prefs.PointShadowBudgetCap);

    ImGui::Separator();
    ImGui::TextUnformatted("Status (read-only)");

    const EnvironmentComponent* environment = nullptr;
    bool bestPrimary = false;
    int bestPriority = 0;
    std::string bestKey;
    for (const auto [entity, env, uuid] :
         m_Session.EditWorld().Registry()
             .view<const EnvironmentComponent, const UuidComponent>()
             .each())
    {
        const std::string key = uuid.Id.ToString();
        const bool better = environment == nullptr ||
            (env.Primary != bestPrimary ? env.Primary
                : env.Priority != bestPriority ? env.Priority > bestPriority
                                               : key < bestKey);
        if (better)
        {
            environment = &env;
            bestPrimary = env.Primary;
            bestPriority = env.Priority;
            bestKey = key;
        }
        static_cast<void>(entity);
    }

    auto statusLine = [&](const char* label, const RenderQualitySettings& quality) {
        const bool aoOn =
            quality.AoEnabled && (environment == nullptr || (environment != nullptr && environment->AoEnabled));
        const bool hdrOn = environment == nullptr || environment->TonemapEnabled;
        const bool bloomOn = environment == nullptr || environment->BloomEnabled;
        ImGui::Text(
            "%s — AO: %s, HDR: %s, Bloom: %s",
            label,
            aoOn ? "On" : "Off",
            hdrOn ? "On" : "Off",
            bloomOn ? "On" : "Off");
    };
    statusLine("Scene view", prefs.ApplyTo(prefs.SceneQuality));
    statusLine("Game view", prefs.ApplyTo(prefs.GameQuality));

    if (ImGui::Button("Reset to Defaults"))
    {
        prefs.ResetToDefaults();
        changed = true;
    }

    if (changed)
    {
        ApplyGraphicsPreferences();
    }

    ImGui::Separator();
    if (const std::optional<RenderDiagnostics> diag = m_Viewports.FocusedViewportDiagnostics())
    {
        ImGui::TextDisabled("Focused viewport (%s)", ToString(diag->Preset));
        ImGui::Text(
            "Logical %dx%d  Internal %dx%d",
            diag->LogicalWidth,
            diag->LogicalHeight,
            diag->InternalWidth,
            diag->InternalHeight);
        ImGui::Text(
            "Draw %d  Meshes %d  Post %d",
            diag->DrawCalls,
            diag->VisibleMeshes,
            diag->PostPasses);
        ImGui::Text(
            "Lights P %d/%d  S %d/%d  Shadow lights %d  Shadow passes %d",
            diag->ActivePointLights,
            diag->TotalPointLights,
            diag->ActiveSpotLights,
            diag->TotalSpotLights,
            diag->ShadowedLights,
            diag->ShadowPasses);
        ImGui::Text(
            "Cascades %d  Shadow res %d  Shadow dist %.0f",
            diag->ActiveCascades,
            diag->ShadowResolution,
            diag->ShadowDistance);
        ImGui::Text("CPU render %.2f ms", diag->CpuRenderMs);
        if (diag->GpuRenderMs >= 0.0F)
        {
            ImGui::Text("GPU render %.2f ms", diag->GpuRenderMs);
        }
    }
    else
    {
        ImGui::TextDisabled("Focus Scene or Game View for render diagnostics");
    }

    ImGui::End();
}

void ImGuiEditorApplication::SaveGraphicsSettings()
{
    const std::filesystem::path& root = m_Session.activeProject.RootPath;
    if (root.empty())
    {
        return;
    }
    const std::filesystem::path folder = root / "Saved" / "Editor";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        m_Log.Log("Could not create graphics settings folder: " + error.message(), "error");
        return;
    }
    m_GraphicsPrefs.SceneQuality = m_Viewports.SceneQuality();
    m_GraphicsPrefs.GameQuality = m_Viewports.GameQuality();
    const std::string json = editor::StringifyGraphicsPreferences(m_GraphicsPrefs);

    const std::filesystem::path path = folder / "graphics.json";
    const std::filesystem::path temporary = folder / "graphics.json.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            m_Log.Log("Could not write graphics settings " + temporary.generic_string(), "error");
            return;
        }
        output << json;
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        m_Log.Log(
            "Could not replace graphics settings " + path.generic_string() + ": " + error.message(),
            "error");
    }
}

void ImGuiEditorApplication::SyncGameCameraFromWorld()
{
    IWorld* world = m_Session.PlayWorld();
    if (world == nullptr)
    {
        world = &m_Session.EditWorld();
    }
    std::vector<editor::CameraCandidate> candidates;
    std::vector<entt::entity> entities;
    for (const auto [entity, component] :
         world->Registry().view<const CameraComponent>().each())
    {
        editor::CameraCandidate candidate;
        candidate.Id = candidates.size();
        candidate.Settings = editor::CameraSettingsFromComponent(component);
        candidates.push_back(candidate);
        entities.push_back(entity);
    }
    const auto primary = editor::SelectPrimaryCamera(candidates);
    if (!primary || *primary >= entities.size())
    {
        m_Camera.SetGameCamera(std::nullopt);
        return;
    }
    const entt::entity entity = entities[*primary];
    const TransformComponent* transform =
        world->Registry().try_get<TransformComponent>(entity);
    const CameraComponent* component = world->Registry().try_get<CameraComponent>(entity);
    if (transform == nullptr || component == nullptr)
    {
        m_Camera.SetGameCamera(std::nullopt);
        return;
    }
    const glm::vec3 forward = transform->Rotation * glm::vec3{0.0F, 0.0F, -1.0F};
    const glm::vec3 up = transform->Rotation * glm::vec3{0.0F, 1.0F, 0.0F};
    const float aspect = m_Viewports.GameViewAspect();
    editor::CameraPreview preview;
    preview.View = editor::BuildGameCameraView(transform->Position, forward, up);
    preview.Projection =
        editor::BuildGameCameraProjection(editor::CameraSettingsFromComponent(*component), aspect);
    m_Camera.SetGameCamera(preview);
}

void ImGuiEditorApplication::UpdateFrame(const float deltaSeconds)
{
    m_FrameDelta = deltaSeconds;
    if (!m_Ui.InWorkbench)
    {
        return;
    }
    if (m_Session.PlayMode() != EditorPlayMode::Edit)
    {
        m_Session.Play().Update(deltaSeconds);
    }
    // Relative-mouse look clears ImGui hover; keep updating while navigating.
    if (m_Session.PlayMode() == EditorPlayMode::Edit &&
        ((m_Viewports.SceneViewHovered() && !ImGui::GetIO().WantTextInput) ||
            m_Camera.Input().IsNavigating()))
    {
        m_Camera.Update(deltaSeconds);
    }
    SyncGameCameraFromWorld();
    m_ContentBrowser.Poll();
}

bool ImGuiEditorApplication::SaveScenePath(const std::filesystem::path& path)
{
    const Result<void> result = m_Session.SaveSceneTo(path);
    if (!result)
    {
        m_Ui.StatusText = result.ErrorMessage();
        return false;
    }
    if (m_AssetBrowser)
    {
        static_cast<void>(m_AssetBrowser->Refresh());
    }
    m_Ui.StatusText = "Saved " + path.filename().string();
    return true;
}

void ImGuiEditorApplication::OpenScenePath(const std::filesystem::path& path)
{
    const Result<void> result = m_Session.OpenScene(path);
    if (!result)
    {
        m_Ui.StatusText = path.empty() ? "Choose a scene file" : result.ErrorMessage();
        return;
    }
    m_Viewports.ResetTemporalHistory();
    m_Ui.StatusText = "Opened " + path.filename().string();
}

void ImGuiEditorApplication::CommandNewScene()
{
    RequestWithConfirm(editor::PendingConfirmAction::NewScene);
}

void ImGuiEditorApplication::CommandOpenScene()
{
    RequestWithConfirm(editor::PendingConfirmAction::OpenScene);
}

void ImGuiEditorApplication::CommandSaveScene()
{
    if (m_Session.Document().Path.empty())
    {
        CommandSaveSceneAs();
        return;
    }
    static_cast<void>(SaveScenePath(m_Session.Document().Path));
}

void ImGuiEditorApplication::CommandSaveSceneAs()
{
    m_Ui.ShowSaveSceneAsDialog = true;
}

void ImGuiEditorApplication::CommandSaveAll()
{
    m_Shell.SaveLayout(m_Ui);
    if (m_Session.Document().Path.empty())
    {
        m_Session.SetSaveAllPending(true);
        CommandSaveSceneAs();
        return;
    }
    const bool sceneSaved = SaveScenePath(m_Session.Document().Path);
    m_Ui.StatusText = sceneSaved ? "Saved all project changes" : "Save All incomplete";
}

void ImGuiEditorApplication::ProcessCommands()
{
    using editor::ConfirmChoice;
    using editor::PendingConfirmAction;

    if (m_Ui.ConfirmResult != ConfirmChoice::None)
    {
        const ConfirmChoice choice = m_Ui.ConfirmResult;
        m_Ui.ConfirmResult = ConfirmChoice::None;
        if (choice == ConfirmChoice::Cancel)
        {
            m_PendingSceneAsset.reset();
            m_Ui.ConfirmAction = PendingConfirmAction::None;
        }
        else if (choice == ConfirmChoice::Discard)
        {
            ExecuteConfirmAction(m_Ui.ConfirmAction);
        }
        else if (choice == ConfirmChoice::Save)
        {
            if (m_Session.Document().Path.empty())
            {
                m_Ui.ShowSaveSceneAsDialog = true;
            }
            else if (SaveScenePath(m_Session.Document().Path) && !m_Session.Document().Dirty)
            {
                ExecuteConfirmAction(m_Ui.ConfirmAction);
            }
        }
    }

    if (m_Ui.RequestEnterWorkbench && m_Ui.PendingProject)
    {
        m_Ui.RequestEnterWorkbench = false;
        EnterWorkbench(*m_Ui.PendingProject);
    }

    if (m_Ui.OpenScenePathReady)
    {
        m_Ui.OpenScenePathReady = false;
        OpenScenePath(m_Ui.ScenePathBuf);
    }

    if (m_Ui.SaveScenePathReady)
    {
        m_Ui.SaveScenePathReady = false;
        const bool saved = SaveScenePath(m_Ui.ScenePathBuf);
        if (saved && m_Session.SaveAllPending())
        {
            m_Shell.SaveLayout(m_Ui);
            m_Session.SetSaveAllPending(false);
            m_Ui.StatusText = "Saved all project changes";
        }
        if (saved && !m_Session.Document().Dirty &&
            m_Ui.ConfirmAction != PendingConfirmAction::None)
        {
            ExecuteConfirmAction(m_Ui.ConfirmAction);
        }
    }

    if (!m_Ui.InWorkbench)
    {
        // Quit from launcher skips scene confirm.
        if (m_Ui.RequestClose)
        {
            m_Ui.RequestClose = false;
            m_Running = false;
        }
        return;
    }

    if (m_Ui.RequestNewScene)
    {
        m_Ui.RequestNewScene = false;
        CommandNewScene();
    }
    if (m_Ui.RequestOpenScene)
    {
        m_Ui.RequestOpenScene = false;
        CommandOpenScene();
    }
    if (m_Ui.RequestSaveScene)
    {
        m_Ui.RequestSaveScene = false;
        CommandSaveScene();
    }
    if (m_Ui.RequestSaveSceneAs)
    {
        m_Ui.RequestSaveSceneAs = false;
        CommandSaveSceneAs();
    }
    if (m_Ui.RequestSaveAll)
    {
        m_Ui.RequestSaveAll = false;
        CommandSaveAll();
    }
    if (m_Ui.RequestExitToProjectManager)
    {
        m_Ui.RequestExitToProjectManager = false;
        RequestWithConfirm(PendingConfirmAction::ExitToProjectManager);
    }
    if (m_Ui.RequestUndo)
    {
        m_Ui.RequestUndo = false;
        if (m_Session.History().CanUndo())
        {
            m_Session.History().Undo();
            m_Ui.StatusText = "Undo";
        }
    }
    if (m_Ui.RequestRedo)
    {
        m_Ui.RequestRedo = false;
        if (m_Session.History().CanRedo())
        {
            m_Session.History().Redo();
            m_Ui.StatusText = "Redo";
        }
    }
    if (m_Ui.RequestFocusSelection)
    {
        m_Ui.RequestFocusSelection = false;
        if (m_SceneEditor && m_SceneEditor->Selection())
        {
            const auto entity = m_Session.EditWorld().Find(*m_SceneEditor->Selection());
            if (entity)
            {
                if (const TransformComponent* transform =
                        m_Session.EditWorld().Registry().try_get<TransformComponent>(*entity))
                {
                    Bounds bounds;
                    bool importedBounds = false;
                    const MeshComponent* mesh =
                        m_Session.EditWorld().Registry().try_get<MeshComponent>(*entity);
                    if (mesh != nullptr && mesh->ImportedMesh.IsValid() && m_GltfMeshes)
                    {
                        if (const GltfMeshAsset* gltf = m_GltfMeshes->Get(mesh->ImportedMesh))
                        {
                            bounds.Minimum = glm::vec3{std::numeric_limits<float>::max()};
                            bounds.Maximum = glm::vec3{std::numeric_limits<float>::lowest()};
                            for (int corner = 0; corner < 8; ++corner)
                            {
                                const glm::vec3 local{
                                    (corner & 1) ? gltf->BoundingBoxMax.x : gltf->BoundingBoxMin.x,
                                    (corner & 2) ? gltf->BoundingBoxMax.y : gltf->BoundingBoxMin.y,
                                    (corner & 4) ? gltf->BoundingBoxMax.z : gltf->BoundingBoxMin.z};
                                const glm::vec3 world = transform->Position +
                                    transform->Rotation * (local * transform->Scale);
                                bounds.Minimum = glm::min(bounds.Minimum, world);
                                bounds.Maximum = glm::max(bounds.Maximum, world);
                            }
                            importedBounds = true;
                        }
                    }
                    if (!importedBounds)
                    {
                        const glm::vec3 half = glm::abs(transform->Scale) * 0.5F;
                        bounds = {transform->Position - half, transform->Position + half};
                    }
                    m_Camera.Input().SetFocusBounds(bounds);
                    m_Camera.Camera().FocusBounds(bounds);
                    m_Ui.StatusText = "Focused selection";
                }
            }
        }
    }
    if (m_Ui.RequestImportModel)
    {
        m_Ui.RequestImportModel = false;
        m_ContentBrowser.PromptImportModel(m_Ui, m_Window);
    }
    if (m_Ui.RequestSavePrefab)
    {
        m_Ui.RequestSavePrefab = false;
        if (m_SceneEditor && m_SceneEditor->Selection() && m_Session.projectService)
        {
            const auto selection = *m_SceneEditor->Selection();
            std::filesystem::path dir = m_Session.ActiveProject().RootPath / "Assets";
            if (m_AssetBrowser)
            {
                dir = m_AssetBrowser->CurrentFolder();
            }
            std::string fileName = "Prefab";
            if (const auto entity = m_Session.EditWorld().Find(selection))
            {
                if (const NameComponent* name =
                        m_Session.EditWorld().Registry().try_get<NameComponent>(*entity))
                {
                    if (!name->Name.empty())
                    {
                        fileName = name->Name;
                    }
                }
            }
            std::error_code ec;
            std::filesystem::path output = dir / (fileName + ".prefab");
            int suffix = 2;
            while (std::filesystem::exists(output, ec))
            {
                output = dir / (fileName + "_" + std::to_string(suffix++) + ".prefab");
            }
            if (const Result<void> saved =
                    PrefabSerializer::Save(m_Session.EditWorld(), selection, output))
            {
                if (m_AssetBrowser)
                {
                    static_cast<void>(m_AssetBrowser->Refresh());
                }
                m_Ui.StatusText = "Saved prefab: " + output.filename().string();
                m_Log.Log(m_Ui.StatusText);
            }
            else
            {
                m_Ui.StatusText = saved.ErrorMessage();
            }
        }
        else
        {
            m_Ui.StatusText = "Select an entity to save as prefab";
        }
    }
    if (m_Ui.RequestDuplicateEntity)
    {
        m_Ui.RequestDuplicateEntity = false;
        if (m_SceneEditor && m_SceneEditor->DuplicateSelection())
        {
            m_Ui.StatusText = "Duplicated entity";
        }
    }
    if (m_Ui.RequestDeleteEntity)
    {
        m_Ui.RequestDeleteEntity = false;
        if (m_SceneEditor && m_SceneEditor->Selection() &&
            !m_SceneEditor->IsSceneRoot(*m_SceneEditor->Selection()) &&
            m_SceneEditor->DeleteSelection())
        {
            m_Ui.StatusText = "Deleted entity";
        }
    }
    if (m_Ui.RequestPlay)
    {
        m_Ui.RequestPlay = false;
        // Play draws the Game view at its preset (default High); entering play does not lower quality.
        if (m_Session.SetPlayMode(EditorPlayMode::Play))
        {
            m_Viewports.ResetTemporalHistory();
            if (m_ScriptEditor)
            {
                m_ScriptEditor->SuspendNative(true);
            }
            if (m_GameUi)
            {
                m_GameUi->SetActive(true);
            }
            m_Ui.StatusText = "Play";
        }
    }
    if (m_Ui.RequestPause)
    {
        m_Ui.RequestPause = false;
        static_cast<void>(m_Session.SetPlayMode(EditorPlayMode::Paused));
        if (m_GameUi)
        {
            m_GameUi->SetActive(true);
        }
        m_Ui.StatusText = "Paused";
    }
    if (m_Ui.RequestStop)
    {
        m_Ui.RequestStop = false;
        if (m_GameUi)
        {
            m_GameUi->SetActive(false);
        }
        static_cast<void>(m_Session.SetPlayMode(EditorPlayMode::Edit));
        m_Viewports.ResetTemporalHistory();
        m_Ui.StatusText = "Stopped";
    }
    if (m_Ui.RequestStep)
    {
        m_Ui.RequestStep = false;
        if (m_Session.PlayMode() == EditorPlayMode::Paused)
        {
            m_Session.Play().SingleStep();
            m_Ui.StatusText = "Step";
        }
        else
        {
            m_Ui.StatusText = "Pause play mode to step";
        }
    }
    if (m_Ui.RequestClose)
    {
        m_Ui.RequestClose = false;
        RequestWithConfirm(PendingConfirmAction::Quit);
    }
}

void ImGuiEditorApplication::HandleShortcuts(const SDL_Event& event)
{
    if (!m_Ui.InWorkbench || event.type != SDL_EVENT_KEY_DOWN)
    {
        return;
    }
    if (ImGui::GetIO().WantTextInput)
    {
        return;
    }
    if (m_Session.PlayMode() != EditorPlayMode::Edit)
    {
        return;
    }

    const SDL_Keymod modifiers = SDL_GetModState();
    const bool control = (modifiers & SDL_KMOD_CTRL) != 0;
    const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;

    if (!control)
    {
        switch (event.key.scancode)
        {
        case SDL_SCANCODE_Q: m_Viewports.SetGizmoTool(0); return;
        case SDL_SCANCODE_W: m_Viewports.SetGizmoTool(1); return;
        case SDL_SCANCODE_E: m_Viewports.SetGizmoTool(2); return;
        case SDL_SCANCODE_R: m_Viewports.SetGizmoTool(3); return;
        case SDL_SCANCODE_X:
            m_Viewports.SetGizmoLocalSpace(!m_Viewports.GizmoLocalSpace());
            return;
        case SDL_SCANCODE_F: m_Ui.RequestFocusSelection = true; return;
        case SDL_SCANCODE_DELETE: m_Ui.RequestDeleteEntity = true; return;
        default: break;
        }
        return;
    }

    if (event.key.scancode == SDL_SCANCODE_S)
    {
        if (shift)
        {
            m_Ui.RequestSaveAll = true;
        }
        else
        {
            m_Ui.RequestSaveScene = true;
        }
    }
    else if (event.key.scancode == SDL_SCANCODE_N)
    {
        m_Ui.RequestNewScene = true;
    }
    else if (event.key.scancode == SDL_SCANCODE_O)
    {
        m_Ui.RequestOpenScene = true;
    }
    else if (event.key.scancode == SDL_SCANCODE_D)
    {
        m_Ui.RequestDuplicateEntity = true;
    }
    else if (event.key.scancode == SDL_SCANCODE_Z)
    {
        if (shift)
        {
            m_Ui.RequestRedo = true;
        }
        else
        {
            m_Ui.RequestUndo = true;
        }
    }
    else if (event.key.scancode == SDL_SCANCODE_Y)
    {
        m_Ui.RequestRedo = true;
    }
}

bool ImGuiEditorApplication::ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (m_Chrome.HandleEvent(event, m_Ui))
        {
            continue;
        }
        m_ImGui.ProcessEvent(event);
        HandleShortcuts(event);
        if (m_Ui.InWorkbench)
        {
            m_ContentBrowser.HandleExternalDrop(event, m_Ui);
        }
        if (m_Ui.InWorkbench && m_SceneEditor)
        {
            m_Viewports.HandleEvent(
                event,
                *m_SceneEditor,
                m_Camera,
                m_Gizmo,
                m_Session.PlayMode(),
                m_Session.History(),
                m_Session.EditWorld(),
                m_Session.Document());
        }
        if (event.type == SDL_EVENT_QUIT)
        {
            if (m_Ui.InWorkbench)
            {
                RequestWithConfirm(editor::PendingConfirmAction::Quit);
            }
            else
            {
                m_Running = false;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(m_Window))
        {
            if (m_Ui.InWorkbench)
            {
                RequestWithConfirm(editor::PendingConfirmAction::Quit);
            }
            else
            {
                m_Running = false;
            }
        }
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED)
        {
            OnDisplayScaleChanged();
        }
    }
    return m_Running;
}

void ImGuiEditorApplication::DrawUi()
{
    if (m_Ui.InWorkbench && m_SceneEditor)
    {
        IWorld* drawWorld = m_Session.PlayWorld();
        if (drawWorld == nullptr)
        {
            drawWorld = &m_Session.EditWorld();
        }
        m_Viewports.SetDebugView(m_Ui.SceneDebugView);
        m_Viewports.SetCollisionVisualizationEnabled(m_Ui.ShowCollisionShapes);
        m_Viewports.RenderTargets(
            *drawWorld,
            *m_SceneEditor,
            m_Camera,
            m_Gizmo,
            m_Session.PlayMode() != EditorPlayMode::Edit,
            m_FrameDelta);
        m_Shell.Draw(
            m_Session,
            m_Ui,
            m_Theme,
            m_Chrome,
            m_SceneEditor.get(),
            &m_Viewports,
            &m_Camera,
            &m_Gizmo,
            &m_ContentBrowser,
            &m_Output,
            &m_ScriptPanel,
            &m_MaterialPanel,
            &m_ExportPanel);
        m_Shell.DrawModals(m_Session, m_Ui, m_Theme);
        DrawGraphicsWindow();
    }
    else
    {
        m_Shell.DrawLauncherChrome(m_Ui, m_Theme, m_Chrome);
        m_ProjectManager.Draw(m_Session, m_Ui, m_Theme, m_AudioEngine.get(), m_Window);
    }
    ProcessCommands();
    m_Chrome.ApplyPendingWindowCommands(m_Ui, m_Running);
}

int ImGuiEditorApplication::Run()
{
#ifdef _WIN32
    if (SDL_GetHint(SDL_HINT_GPU_DRIVER) == nullptr)
    {
        SDL_SetHint(SDL_HINT_GPU_DRIVER, "direct3d12");
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        throw std::runtime_error(std::string{"SDL_Init failed: "} + SDL_GetError());
    }

    const SDL_DisplayID primaryDisplay = SDL_GetPrimaryDisplay();
    SDL_Rect usableBounds{};
    int windowWidth = 1600;
    int windowHeight = 900;
    if (SDL_GetDisplayUsableBounds(primaryDisplay, &usableBounds))
    {
        windowWidth = std::min(windowWidth, std::max(1024, usableBounds.w * 9 / 10));
        windowHeight = std::min(windowHeight, std::max(640, usableBounds.h * 9 / 10));
    }

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Fadix Engine");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, windowWidth);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, windowHeight);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
    m_Window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (m_Window == nullptr)
    {
        SDL_Quit();
        throw std::runtime_error(std::string{"SDL_CreateWindow failed: "} + SDL_GetError());
    }

    m_Device = CreateDeviceFromWindow(m_Window);
    if (!m_Device)
    {
        const std::string gpuError = SDL_GetError();
        Shutdown();
        throw std::runtime_error(
            "CreateDeviceFromWindow failed: " +
            (gpuError.empty() ? std::string{"unknown SDL GPU error"} : gpuError));
    }

    auto* nativeDevice = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device));
    if (!m_ImGui.Initialize(m_Window, *m_Device))
    {
        const std::string imguiError = SDL_GetError();
        Shutdown();
        throw std::runtime_error(
            "ImGuiLayer::Initialize failed: " +
            (imguiError.empty() ? std::string{"unknown SDL GPU error"} : imguiError));
    }

    m_AudioEngine = std::make_unique<AudioEngine>();
    if (!m_AudioEngine->Initialize())
    {
        std::clog << "[Fadix] AudioEngine initialize failed: " << SDL_GetError() << '\n';
        m_AudioEngine.reset();
    }
    else
    {
        m_AudioPlayback = std::make_unique<AudioPlayback>(*m_AudioEngine);
    }

    if (!InitializeGameUi())
    {
        Shutdown();
        throw std::runtime_error("GameUIOverlay Rml bootstrap failed");
    }

    m_Theme.Apply();
    static_cast<void>(m_Theme.LoadFonts(m_AssetRoot));
    if (nativeDevice != nullptr)
    {
        static_cast<void>(m_Theme.LoadLogo(nativeDevice, m_AssetRoot));
    }
    ImGui::GetIO().ConfigDpiScaleFonts = true;
    m_AppliedDpiScale = 1.0F;
    OnDisplayScaleChanged();

    m_Shell.InitializePaths(m_AssetRoot);
    m_Ui.DefaultIniPath = (m_AssetRoot / "editor" / "imgui_default.ini").string();
    ImGui::GetIO().IniFilename = nullptr;
    if (!m_Ui.DefaultIniPath.empty())
    {
        ImGui::LoadIniSettingsFromDisk(m_Ui.DefaultIniPath.c_str());
    }

    m_Chrome.Bind(m_Window);
    m_Chrome.SetUiState(&m_Ui);
    m_ProjectManager.Reset(m_Session);
    m_Ui.InWorkbench = false;
    m_Ui.StatusText = "Select or create a project.";

    if (!SDL_ShowWindow(m_Window))
    {
        Shutdown();
        throw std::runtime_error(std::string{"Could not show window: "} + SDL_GetError());
    }
    std::clog << "[Fadix] ImGui project manager ready\n";

    auto previous = std::chrono::steady_clock::now();
    while (m_Running && ProcessEvents())
    {
        if ((SDL_GetWindowFlags(m_Window) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(10);
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds =
            std::min(std::chrono::duration<float>(now - previous).count(), 0.1F);
        previous = now;
        m_ImGui.BeginFrame();
        UpdateFrame(deltaSeconds);
        DrawUi();
        SyncGameUi();
        m_ImGui.RenderAndSubmit(
            [this](
                SDL_GPUCommandBuffer* commandBuffer,
                SDL_GPUTexture* swapchain,
                std::uint32_t width,
                std::uint32_t height) {
                if (!m_GameUi || m_RmlRender == nullptr)
                {
                    return;
                }
                auto* gpuRender = dynamic_cast<RenderInterface_SDL_GPU*>(m_RmlRender.get());
                if (gpuRender == nullptr)
                {
                    return;
                }
                gpuRender->BeginFrame(commandBuffer, swapchain, width, height);
                m_GameUi->Render();
                gpuRender->EndFrame();
            });
    }

    Shutdown();
    return 0;
}
}
