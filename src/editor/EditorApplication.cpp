#include "editor/EditorApplication.hpp"

#include "assets/AssetDatabase.hpp"
#include "assets/EmbeddedAssetProvider.hpp"
#include "editor/assets/AssetBrowserController.hpp"
#include "editor/camera/CameraModule.hpp"
#include "editor/camera/CameraSelection.hpp"
#include "editor/command/EntityCommands.hpp"
#include "editor/dock/DockManager.hpp"
#include "editor/gizmo/GizmoSystem.hpp"
#include "editor/play/PlaySession.hpp"
#include "editor/project/ProjectManagerController.hpp"
#include "editor/scene/SceneController.hpp"
#include "editor/scene/SceneSerializer.hpp"
#include "engine/app/ModuleRegistration.hpp"
#include "engine/app/WorkbenchLayoutIds.hpp"
#include "engine/assets/IAssetDatabase.hpp"
#include "engine/camera/EditorCamera.hpp"
#include "engine/project/IProjectService.hpp"
#include "engine/render/ViewportRenderer.hpp"
#include "engine/rhi/Device.hpp"
#include "engine/scene/IWorld.hpp"
#include "engine/scene/SceneDocument.hpp"
#include "render/ViewportRendererFactory.hpp"
#include "rhi/sdl/SdlRhi.hpp"
#include "runtime/Components.hpp"
#include "project/ProjectJson.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi_Platform_SDL.h>
#include <RmlUi_Renderer_SDL_GPU.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace fadix
{
namespace
{
struct OpenedFile
{
    std::vector<char> Bytes;
    std::size_t Offset{0};
};

[[nodiscard]] std::filesystem::path AssetRoot()
{
    return std::filesystem::path{FADIX_ASSET_ROOT};
}

[[nodiscard]] std::optional<std::vector<char>> LoadBytes(const std::string& path)
{
    std::string normalized = path;
    for (char& ch : normalized)
    {
        if (ch == '\\')
        {
            ch = '/';
        }
    }

    auto tryEmbedded = [&](std::string_view candidate) -> std::optional<std::vector<char>> {
        if (const auto view = FindEmbeddedAsset(candidate))
        {
            std::vector<char> bytes(view->Bytes.size());
            if (!view->Bytes.empty())
            {
                std::memcpy(bytes.data(), view->Bytes.data(), view->Bytes.size());
            }
            return bytes;
        }
        return std::nullopt;
    };

    if (auto embedded = tryEmbedded(normalized))
    {
        return embedded;
    }

    const std::filesystem::path asPath{normalized};
    if (asPath.is_absolute())
    {
        std::error_code error;
        const auto relative = std::filesystem::relative(asPath, AssetRoot(), error);
        if (!error && !relative.empty())
        {
            const std::string relativeText = relative.generic_string();
            if (relativeText.find("..") == std::string::npos)
            {
                if (auto embedded = tryEmbedded(relativeText))
                {
                    return embedded;
                }
            }
        }
    }
    else
    {
        const std::string underEditor = normalized.find("editor/") == 0 || normalized.find("fonts/") == 0
            ? normalized
            : ("editor/" + normalized);
        if (auto embedded = tryEmbedded(underEditor))
        {
            return embedded;
        }
    }

    std::filesystem::path disk = asPath.is_absolute() ? asPath : (AssetRoot() / asPath);
    std::ifstream stream(disk, std::ios::binary | std::ios::ate);
    if (!stream)
    {
        disk = AssetRoot() / "editor" / asPath.filename();
        stream.open(disk, std::ios::binary | std::ios::ate);
    }
    if (!stream)
    {
        return std::nullopt;
    }
    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0);
    std::vector<char> bytes(size);
    if (size > 0 && !stream.read(bytes.data(), static_cast<std::streamsize>(size)))
    {
        return std::nullopt;
    }
    return bytes;
}

[[nodiscard]] std::optional<Bounds> EntityBounds(const IWorld& world, const Uuid& id)
{
    const auto entity = world.Find(id);
    if (!entity)
    {
        return std::nullopt;
    }
    const TransformComponent* transform =
        world.Registry().try_get<TransformComponent>(*entity);
    if (transform == nullptr)
    {
        return std::nullopt;
    }
    const glm::vec3 half = transform->Scale * 0.5F;
    return Bounds{transform->Position - half, transform->Position + half};
}

[[nodiscard]] std::optional<glm::vec2> ProjectPoint(
    const glm::vec3& point,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec2 viewportSize)
{
    const glm::vec4 clip = projection * view * glm::vec4{point, 1.0F};
    if (clip.w <= 0.0001F)
    {
        return std::nullopt;
    }
    const glm::vec3 ndc = glm::vec3{clip} / clip.w;
    return glm::vec2{
        (ndc.x * 0.5F + 0.5F) * viewportSize.x,
        (0.5F - ndc.y * 0.5F) * viewportSize.y};
}

[[nodiscard]] float DistanceToSegment(
    const glm::vec2 point, const glm::vec2 begin, const glm::vec2 end)
{
    const glm::vec2 segment = end - begin;
    const float lengthSquared = glm::dot(segment, segment);
    if (lengthSquared <= 0.0001F)
    {
        return glm::length(point - begin);
    }
    const float amount = std::clamp(glm::dot(point - begin, segment) / lengthSquared, 0.0F, 1.0F);
    return glm::length(point - (begin + segment * amount));
}

[[nodiscard]] bool PointInsideElement(
    Rml::Element* element, const float mouseX, const float mouseY)
{
    if (element == nullptr)
    {
        return false;
    }
    const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
    return mouseX >= position.x && mouseY >= position.y &&
        mouseX < position.x + size.x && mouseY < position.y + size.y;
}
}

class EditorApplication::AssetFileInterface final : public Rml::FileInterface
{
public:
    Rml::FileHandle Open(const Rml::String& path) override
    {
        auto bytes = LoadBytes(std::string{path});
        if (!bytes)
        {
            return 0;
        }
        auto* file = new OpenedFile{std::move(*bytes), 0};
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
};

class EditorApplication::CommandListener final : public Rml::EventListener
{
public:
    explicit CommandListener(EditorApplication& application) : m_Application(application) {}
    void ProcessEvent(Rml::Event& event) override { m_Application.HandleCommand(event); }

private:
    EditorApplication& m_Application;
};

EditorApplication::EditorApplication()
    : m_Projects(project::CreateService()),
      m_Assets(assets::CreateDatabase()),
      m_EditWorld(sceneplay::CreateEditWorld()),
      m_ProjectManager(std::make_unique<ProjectManagerController>()),
      m_Scenes(std::make_unique<SceneService>()),
      m_Gizmo(std::make_unique<GizmoSystem>()),
      m_PlaySession(std::make_unique<PlaySession>()),
      m_CommandListener(std::make_unique<CommandListener>(*this))
{
    RegisterRuntimeComponentProperties(m_Properties);
    m_ProjectManager->Connect(*this, *m_Projects);
    m_Scene = std::make_unique<SceneController>(*m_EditWorld, m_History);
    m_DockManager = std::make_unique<editor::DockManager>(
        [this](std::string text, const char* severity) {
            Log(std::move(text), severity);
        });
}

EditorApplication::~EditorApplication()
{
    ShutdownUi();
}

int EditorApplication::Run()
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

    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

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
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        SDL_Quit();
        throw std::runtime_error("CreateDeviceFromWindow failed");
    }

    auto* nativeDevice = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device));
    auto system = std::make_unique<SystemInterface_SDL>();
    system->SetWindow(m_Window);
    auto render = std::make_unique<RenderInterface_SDL_GPU>(nativeDevice, m_Window);
    m_SystemInterface = std::move(system);
    m_RenderInterface = std::move(render);
    m_FileInterface = std::make_unique<AssetFileInterface>();

    Rml::SetFileInterface(m_FileInterface.get());
    Rml::SetSystemInterface(m_SystemInterface.get());
    Rml::SetRenderInterface(m_RenderInterface.get());
    if (!Rml::Initialise())
    {
        ShutdownUi();
        throw std::runtime_error("RmlUi failed to initialize");
    }

    const std::filesystem::path fontPath = AssetRoot() / "fonts" / "LatoLatin-Regular.ttf";
    if (!Rml::LoadFontFace(fontPath.string()))
    {
        // Embedded fallback path used by FileInterface-aware loaders; LoadFontFace still needs a path.
        if (!Rml::LoadFontFace("fonts/LatoLatin-Regular.ttf"))
        {
            ShutdownUi();
            throw std::runtime_error("Could not load editor font");
        }
    }

    SetViewportRenderer(CreateViewportRenderer(*m_Device));
    m_Picking = CreateViewportPicking(*m_Viewport);
    camera::RegisterModule(*this);

    int pixelWidth = windowWidth;
    int pixelHeight = windowHeight;
    SDL_GetWindowSizeInPixels(m_Window, &pixelWidth, &pixelHeight);
    m_Context = Rml::CreateContext("editor", Rml::Vector2i{pixelWidth, pixelHeight});
    if (m_Context == nullptr)
    {
        ShutdownUi();
        throw std::runtime_error("Could not create editor UI context");
    }
    m_Context->SetDensityIndependentPixelRatio(SDL_GetWindowDisplayScale(m_Window));

    ShowProjectManager();
    if (!SDL_ShowWindow(m_Window))
    {
        ShutdownUi();
        throw std::runtime_error(std::string{"Could not show editor window: "} + SDL_GetError());
    }
    std::clog << "[Fadix] Entering editor event loop\n";

    auto previous = std::chrono::steady_clock::now();
    while (m_Running && ProcessEvents())
    {
        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - previous).count();
        previous = now;
        UpdateFrame(deltaSeconds);
        m_Context->Update();
        DrawAndPresent();
    }

    ShutdownUi();
    return 0;
}

void EditorApplication::ShutdownUi()
{
    if (m_Window == nullptr && m_Device == nullptr && m_Context == nullptr &&
        m_RenderInterface == nullptr)
    {
        return;
    }
    if (m_InWorkbench)
    {
        static_cast<void>(SaveWorkspaceLayout());
    }
    if (m_DockManager)
    {
        m_DockManager->Unbind();
    }
    if (m_Document != nullptr)
    {
        m_Document->Close();
        m_Document = nullptr;
    }
    m_AssetBrowser.reset();
    if (m_Context != nullptr)
    {
        Rml::RemoveContext("editor");
        m_Context = nullptr;
    }
    if (Rml::GetRenderInterface() != nullptr || Rml::GetSystemInterface() != nullptr)
    {
        Rml::Shutdown();
    }
    if (auto* gpuRender = dynamic_cast<RenderInterface_SDL_GPU*>(m_RenderInterface.get()))
    {
        gpuRender->Shutdown();
    }
    m_RenderInterface.reset();
    m_SystemInterface.reset();
    m_FileInterface.reset();
    m_Viewport.reset();
    m_Device.reset();
    if (m_Window != nullptr)
    {
        SDL_DestroyWindow(m_Window);
        m_Window = nullptr;
        SDL_Quit();
    }
}

void EditorApplication::ShowProjectManager()
{
    if (m_InWorkbench)
    {
        static_cast<void>(SaveWorkspaceLayout());
    }
    m_PlayMode = EditorPlayMode::Edit;
    m_InWorkbench = false;
    if (m_PlaySession)
    {
        m_PlaySession->Stop();
    }
    LoadDocument("project_manager.rml");
    m_ProjectManager->Bind(m_Context);
    m_ProjectManager->OnDocumentLoaded(m_Document);
    std::clog << "[Fadix] Project manager loaded\n";
}

void EditorApplication::EnterWorkbench(const ProjectMetadata& project)
{
    m_ActiveProject = project;
    m_InWorkbench = true;
    LoadDocument("workspace.rml");
    if (Rml::Element* projectName = m_Document->GetElementById(layout::ProjectName))
    {
        projectName->SetInnerRML(EscapeRml(project.Name));
    }

    m_SceneDocument = SceneDocument{
        Uuid::Generate(), "Main", project.RootPath / "Scenes" / "Main.scene", false};
    if (std::filesystem::exists(m_SceneDocument.Path))
    {
        if (auto result = m_Scenes->Load(m_SceneDocument, *m_EditWorld, m_SceneDocument.Path); !result)
        {
            Log("Could not load default scene: " + result.ErrorMessage(), "error");
        }
    }

    PrepareViewportElement();
    if (m_DockManager != nullptr && m_Document != nullptr)
    {
        m_DockManager->Bind(*m_Document);
    }
    LoadWorkspaceLayout();

    m_Scene = std::make_unique<SceneController>(*m_EditWorld, m_History);
    m_Scene->SetChangedCallback([this]() {
        m_SceneDocument.Dirty = true;
        RefreshSceneChrome();
    });
    m_Scene->Bind(*m_Document);

    SetAssetDatabase(CreateAssetDatabase(project.RootPath));
    m_AssetBrowser = std::make_unique<AssetBrowserController>(*m_Assets, *m_Projects);
    m_AssetBrowser->SetProjectRoot(project.RootPath);
    AssetBrowserCallbacks callbacks;
    callbacks.ReportError = [this](std::string_view message) {
        SetStatus(std::string{message});
        Log(std::string{message}, "error");
    };
    callbacks.EntriesChanged = [this]() { RefreshAssetBrowserUi(); };
    callbacks.LoadScene = [this](const AssetHandle&, const std::filesystem::path& path) {
        OpenScenePath(path);
    };
    callbacks.CreateMeshEntity = [this](const AssetHandle&, const std::filesystem::path& path) {
        auto command = std::make_unique<AddEntityCommand>(*m_EditWorld, path.stem().string());
        const Uuid created = command->EntityId();
        command->SetMesh(MeshComponent{});
        m_History.Push(std::move(command));
        m_Scene->SetSelection(created);
        m_SceneDocument.Dirty = true;
        m_Scene->Refresh();
        RefreshSceneChrome();
        SetStatus("Created mesh entity");
        Log("Created mesh entity from " + path.filename().string());
    };
    m_AssetBrowser->SetCallbacks(std::move(callbacks));
    static_cast<void>(m_AssetBrowser->Refresh());
    RefreshAssetBrowserUi();

    BindWorkbenchUi();
    SetGizmoToolMode(m_GizmoToolMode);
    RefreshSceneChrome();
    UpdatePlayChrome();
    UpdateStatusBar();
    SetStatus("Ready");
    Log("Opened project " + project.Name);
    std::clog << "[Fadix] Entered workbench for " << project.Name << '\n';
}

void EditorApplication::SetPlayMode(const EditorPlayMode mode)
{
    if (mode == EditorPlayMode::Play)
    {
        if (m_PlaySession->State() == PlayState::Paused)
        {
            m_PlaySession->Resume();
        }
        else if (m_PlaySession->State() == PlayState::Stopped)
        {
            auto physics = sceneplay::CreatePhysicsWorldAdapter();
            if (!m_PlaySession->Start(*m_EditWorld, std::move(physics)))
            {
                SetStatus("Play session failed to start (physics unavailable)");
                return;
            }
        }
        m_PlayMode = EditorPlayMode::Play;
        m_ViewportCameraMode = ViewportCameraMode::Game;
        if (m_Viewport)
        {
            m_Viewport->ResetTemporalHistory();
        }
        SetStatus("Running game world");
    }
    else if (mode == EditorPlayMode::Paused)
    {
        m_PlaySession->Pause();
        m_PlayMode = EditorPlayMode::Paused;
        SetStatus("Paused");
    }
    else
    {
        m_PlaySession->Stop();
        m_PlayMode = EditorPlayMode::Edit;
        m_ViewportCameraMode = ViewportCameraMode::Edit;
        if (m_Viewport)
        {
            m_Viewport->ResetTemporalHistory();
        }
        SetStatus("Stopped - edit mode");
    }

    UpdatePlayChrome();

    if (auto* camera = camera::Module())
    {
        camera->SetViewportMode(m_ViewportCameraMode);
    }
}

EditorPlayMode EditorApplication::PlayMode() const noexcept
{
    return m_PlayMode;
}

IProjectService& EditorApplication::Projects() noexcept
{
    return *m_Projects;
}

IWorld& EditorApplication::EditWorld() noexcept
{
    return *m_EditWorld;
}

IWorld* EditorApplication::PlayWorld() noexcept
{
    return m_PlaySession ? m_PlaySession->RuntimeWorld() : nullptr;
}

ViewportRenderer& EditorApplication::Viewport() noexcept
{
    return *m_Viewport;
}

UndoStack& EditorApplication::History() noexcept
{
    return m_History;
}

IAssetDatabase& EditorApplication::Assets() noexcept
{
    return *m_Assets;
}

void EditorApplication::SetProjectService(std::unique_ptr<IProjectService> service) noexcept
{
    if (service)
    {
        m_Projects = std::move(service);
        if (m_ProjectManager)
        {
            m_ProjectManager->Connect(*this, *m_Projects);
        }
    }
}

void EditorApplication::SetAssetDatabase(std::unique_ptr<IAssetDatabase> database) noexcept
{
    if (database)
    {
        m_Assets = std::move(database);
    }
}

void EditorApplication::SetViewportRenderer(std::unique_ptr<ViewportRenderer> renderer) noexcept
{
    if (renderer)
    {
        m_Viewport = std::move(renderer);
    }
}

void EditorApplication::SetEditWorld(std::unique_ptr<IWorld> world) noexcept
{
    if (world)
    {
        m_EditWorld = std::move(world);
        m_Scene = std::make_unique<SceneController>(*m_EditWorld, m_History);
        if (m_InWorkbench && m_Document != nullptr)
        {
            m_Scene->Bind(*m_Document);
        }
    }
}

void EditorApplication::LoadDocument(const char* fileName)
{
    if (m_Context == nullptr)
    {
        return;
    }
    if (m_DockManager)
    {
        m_DockManager->Unbind();
    }
    if (m_Document != nullptr)
    {
        m_Document->Close();
        m_Document = nullptr;
    }

    const std::string virtualPath = std::string{"editor/"} + fileName;
    m_Document = m_Context->LoadDocument(virtualPath);
    if (m_Document == nullptr)
    {
        const auto disk = AssetRoot() / "editor" / fileName;
        m_Document = m_Context->LoadDocument(disk.string());
    }
    if (m_Document == nullptr)
    {
        throw std::runtime_error(std::string{"Could not load editor document: "} + fileName);
    }
    m_Document->Show();
}

void EditorApplication::BindWorkbenchCommands()
{
    BindWorkbenchUi();
}

void EditorApplication::BindWorkbenchUi()
{
    if (m_Document == nullptr || m_CommandListener == nullptr)
    {
        return;
    }
    for (const char* id : {
             "menu-scene",
             "menu-edit",
             "menu-project",
             "menu-debug",
             "menu-editor",
             "add",
             layout::GizmoSelect,
             layout::GizmoMove,
             layout::GizmoRotate,
             layout::GizmoScale,
             layout::GizmoSpace,
             "gizmo-rail-toggle",
             layout::Play,
             layout::Pause,
             layout::Step,
             layout::Stop,
             layout::ModeEdit,
             layout::ModeGame,
             "save-all",
             "undo",
             "redo",
             layout::TabAssets,
             layout::TabOutput,
             "asset-up",
             "asset-refresh",
             "output-filter-info",
             "output-filter-warn",
             "output-filter-error",
             "output-autoscroll",
             "output-clear"})
    {
        if (Rml::Element* element = m_Document->GetElementById(id))
        {
            element->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
        }
    }
    if (Rml::Element* search = m_Document->GetElementById("asset-search"))
    {
        search->AddEventListener(Rml::EventId::Change, m_CommandListener.get());
    }
    if (Rml::Element* filter = m_Document->GetElementById("hierarchy-filter"))
    {
        filter->AddEventListener(Rml::EventId::Change, m_CommandListener.get());
    }
}

void EditorApplication::DisableUnimplementedChrome()
{
    if (m_Document == nullptr)
    {
        return;
    }
    for (const char* id : {"network", "build"})
    {
        if (Rml::Element* element = m_Document->GetElementById(id))
        {
            element->SetAttribute("disabled", "disabled");
            element->SetAttribute(
                "title",
                id[0] == 'n' ? "Network multiplayer is not implemented yet"
                             : "Build/export is not implemented yet");
            element->SetProperty("opacity", "0.45");
        }
    }
}

void EditorApplication::PrepareViewportElement()
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* viewport = m_Document->GetElementById(layout::Viewport))
    {
        viewport->SetProperty("background-color", "transparent");
    }
}

void EditorApplication::MeasureViewport()
{
    if (m_Document == nullptr || m_Viewport == nullptr)
    {
        return;
    }
    Rml::Element* viewport = m_Document->GetElementById(layout::Viewport);
    if (viewport == nullptr)
    {
        return;
    }
    const Rml::Vector2f size = viewport->GetBox().GetSize(Rml::BoxArea::Content);
    const auto width = static_cast<std::uint32_t>(std::max(0.0F, size.x));
    const auto height = static_cast<std::uint32_t>(std::max(0.0F, size.y));
    if (width == 0 || height == 0)
    {
        return;
    }
    if (auto* module = camera::Module())
    {
        module->Camera().SetViewportSize({size.x, size.y});
    }
    if (width != m_ViewportWidth || height != m_ViewportHeight)
    {
        m_ViewportWidth = width;
        m_ViewportHeight = height;
        m_Viewport->Resize({width, height});
    }
}

bool EditorApplication::ViewportHovered() const
{
    if (m_Document == nullptr)
    {
        return false;
    }
    Rml::Element* viewport = m_Document->GetElementById(layout::Viewport);
    if (viewport == nullptr)
    {
        return false;
    }
    float mx = 0.0F;
    float my = 0.0F;
    SDL_GetMouseState(&mx, &my);
    if (PointInsideElement(m_Document->GetElementById("viewport-tool-rail"), mx, my))
    {
        return false;
    }
    const Rml::Vector2f pos = viewport->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = viewport->GetBox().GetSize(Rml::BoxArea::Content);
    return mx >= pos.x && my >= pos.y && mx < pos.x + size.x && my < pos.y + size.y;
}

bool EditorApplication::ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        HandleOpenMenuPointerLeave(event);
        if (event.type == SDL_EVENT_QUIT)
        {
            if (m_InWorkbench && m_SceneDocument.Dirty)
            {
                ShowConfirmDiscard([this]() { m_Running = false; });
                continue;
            }
            m_Running = false;
            return false;
        }

        if ((m_DockManager != nullptr && m_DockManager->HandleEvent(event)) ||
            HandleInspectorWheel(event) || HandleInspectorNumericScrub(event) ||
            HandleShortcut(event) || HandleViewportMouse(event))
        {
            continue;
        }

        bool cameraConsumed = false;
        if (auto* camera = camera::Module())
        {
            cameraConsumed = camera->Input().HandleEvent(event, ViewportHovered());
        }
        if (!cameraConsumed)
        {
            RmlSDL::InputEventHandler(m_Context, m_Window, event);
        }
    }
    return m_Running;
}

void EditorApplication::UpdateFrame(const float deltaSeconds)
{
    if (m_Assets)
    {
        m_Assets->PollImports();
    }
    if (m_AssetBrowser)
    {
        m_AssetBrowser->PollImports();
    }

    std::vector<std::function<void()>> queued;
    {
        std::scoped_lock lock{m_MainThreadQueueMutex};
        queued.swap(m_MainThreadQueue);
    }
    for (auto& action : queued)
    {
        action();
    }

    if (m_PlaySession &&
        (m_PlayMode == EditorPlayMode::Play || m_PlayMode == EditorPlayMode::Paused))
    {
        m_PlaySession->Update(deltaSeconds);
    }

    UpdateCameraModule(deltaSeconds);
    UpdateGizmoVisual();
    UpdateStatusBar();
    RefreshImportChrome();
}

void EditorApplication::UpdateCameraModule(const float deltaSeconds)
{
    auto* camera = camera::Module();
    if (camera == nullptr || m_Document == nullptr)
    {
        return;
    }

    MeasureViewport();

    SyncSelectionFocusBounds();
    SyncGameCameraFromWorld();
    camera->SetViewportMode(m_ViewportCameraMode);
    camera->Update(deltaSeconds);
    camera->ApplyToViewport(*m_Viewport);
}

void EditorApplication::SyncSelectionFocusBounds()
{
    auto* camera = camera::Module();
    if (camera == nullptr || m_Scene == nullptr)
    {
        return;
    }
    const auto selection = m_Scene->Selection();
    if (!selection)
    {
        camera->Input().SetFocusBounds(std::nullopt);
        return;
    }
    camera->Input().SetFocusBounds(EntityBounds(*m_EditWorld, *selection));
}

void EditorApplication::SyncGameCameraFromWorld()
{
    auto* camera = camera::Module();
    if (camera == nullptr)
    {
        return;
    }

    IWorld* world = PlayWorld();
    if (world == nullptr)
    {
        world = m_EditWorld.get();
    }
    if (world == nullptr)
    {
        return;
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
        camera->SetGameCamera(std::nullopt);
        return;
    }

    const entt::entity entity = entities[*primary];
    const TransformComponent* transform =
        world->Registry().try_get<TransformComponent>(entity);
    const CameraComponent* component = world->Registry().try_get<CameraComponent>(entity);
    if (transform == nullptr || component == nullptr)
    {
        camera->SetGameCamera(std::nullopt);
        return;
    }

    const glm::vec3 forward = transform->Rotation * glm::vec3{0.0F, 0.0F, -1.0F};
    const glm::vec3 up = transform->Rotation * glm::vec3{0.0F, 1.0F, 0.0F};
    const float aspect = camera->Camera().AspectRatio();
    editor::CameraPreview preview;
    preview.View = editor::BuildGameCameraView(transform->Position, forward, up);
    preview.Projection =
        editor::BuildGameCameraProjection(editor::CameraSettingsFromComponent(*component), aspect);
    camera->SetGameCamera(preview);
}

void EditorApplication::DrawAndPresent()
{
    if (m_Device == nullptr || m_Window == nullptr || m_Context == nullptr)
    {
        return;
    }

    if (m_InWorkbench && m_Document != nullptr &&
        (m_ViewportWidth == 0 || m_ViewportHeight == 0))
    {
        m_Context->Update();
        MeasureViewport();
    }

    IWorld* drawWorld = PlayWorld();
    if (drawWorld == nullptr)
    {
        drawWorld = m_EditWorld.get();
    }
    if (m_InWorkbench && drawWorld != nullptr && m_ViewportWidth > 0 && m_ViewportHeight > 0)
    {
        const bool sceneView = m_ViewportCameraMode == ViewportCameraMode::Edit;
        m_Viewport->SetEditorVisualsEnabled(sceneView);
        m_Viewport->SetPostDebugView(sceneView ? m_PostDebugView : PostDebugView::None);
        m_Viewport->DrawWorld(*drawWorld);
    }

    auto* nativeDevice = static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device));
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(nativeDevice);
    if (commandBuffer == nullptr)
    {
        return;
    }

    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, m_Window, &swapchain, &width, &height) ||
        swapchain == nullptr || width == 0 || height == 0)
    {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return;
    }

    SDL_GPUColorTargetInfo colorInfo{};
    colorInfo.texture = swapchain;
    colorInfo.load_op = SDL_GPU_LOADOP_CLEAR;
    colorInfo.store_op = SDL_GPU_STOREOP_STORE;
    colorInfo.clear_color = {0.08F, 0.09F, 0.10F, 1.0F};
    SDL_GPURenderPass* clearPass = SDL_BeginGPURenderPass(commandBuffer, &colorInfo, 1, nullptr);
    SDL_EndGPURenderPass(clearPass);

    if (m_InWorkbench && m_Document != nullptr)
    {
        if (rhi::Texture* color = m_Viewport->ColorTarget())
        {
            if (SDL_GPUTexture* source = static_cast<SDL_GPUTexture*>(GetNativeTextureHandle(*color)))
            {
                if (Rml::Element* viewport = m_Document->GetElementById(layout::Viewport))
                {
                    const Rml::Vector2f pos =
                        viewport->GetAbsoluteOffset(Rml::BoxArea::Content);
                    const Rml::Vector2f size =
                        viewport->GetBox().GetSize(Rml::BoxArea::Content);
                    if (size.x > 1.0F && size.y > 1.0F)
                    {
                        SDL_GPUBlitInfo blit{};
                        blit.source.texture = source;
                        blit.source.w = m_ViewportWidth;
                        blit.source.h = m_ViewportHeight;
                        blit.destination.texture = swapchain;
                        blit.destination.x = static_cast<int32_t>(pos.x);
                        blit.destination.y = static_cast<int32_t>(pos.y);
                        blit.destination.w = static_cast<int32_t>(size.x);
                        blit.destination.h = static_cast<int32_t>(size.y);
                        blit.load_op = SDL_GPU_LOADOP_LOAD;
                        blit.filter = SDL_GPU_FILTER_LINEAR;
                        SDL_BlitGPUTexture(commandBuffer, &blit);
                    }
                }
            }
        }
    }

    auto* gpuRender = dynamic_cast<RenderInterface_SDL_GPU*>(m_RenderInterface.get());
    if (gpuRender != nullptr)
    {
        gpuRender->BeginFrame(commandBuffer, swapchain, width, height);
        m_Context->Render();
        gpuRender->EndFrame();
    }

    SDL_SubmitGPUCommandBuffer(commandBuffer);
}

void EditorApplication::BindAssetBrowserUi()
{
    // Click handlers are attached when the list is rebuilt.
}

void EditorApplication::RefreshAssetBrowserUi()
{
    if (m_Document == nullptr || m_AssetBrowser == nullptr)
    {
        return;
    }
    Rml::Element* list = m_Document->GetElementById("asset-list");
    if (list == nullptr)
    {
        return;
    }

    std::ostringstream html;
    const auto entries = m_AssetBrowser->Entries();
    if (entries.empty())
    {
        html << "<div class='asset-empty'>No assets in this folder</div>";
    }
    else
    {
        std::size_t index = 0;
        for (const AssetBrowserEntry& entry : entries)
        {
            const bool selected = m_SelectedAsset && *m_SelectedAsset == index;
            const char* iconClass = entry.Kind == AssetBrowserEntryKind::Folder ? "folder"
                : entry.AssetType == "Scene" ? "scene"
                : entry.AssetType == "Mesh" ? "mesh"
                : entry.AssetType == "Texture" ? "texture"
                : entry.AssetType == "Material" ? "material"
                : entry.AssetType == "Script" ? "script"
                : entry.AssetType == "Audio" ? "audio" : "file";
            html << "<button class=\"asset-row" << (selected ? " selected" : "")
                 << "\" id=\"asset-entry-" << index++ << "\">"
                 << "<span class=\"icon " << iconClass << "\"></span>"
                 << "<span class=\"name\">" << EscapeRml(entry.DisplayName) << "</span>";
            if (!entry.AssetType.empty())
            {
                html << "<span class=\"type\">" << EscapeRml(entry.AssetType) << "</span>";
            }
            html << "</button>";
        }
    }
    list->SetInnerRML(html.str());

    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        if (Rml::Element* row =
                m_Document->GetElementById("asset-entry-" + std::to_string(index)))
        {
            row->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
            row->AddEventListener(Rml::EventId::Dblclick, m_CommandListener.get());
        }
    }

    if (Rml::Element* crumb = m_Document->GetElementById("asset-breadcrumb"))
    {
        crumb->SetInnerRML(EscapeRml(m_AssetBrowser->CurrentFolder().generic_string()));
    }
}

void EditorApplication::OnAssetActivated(const std::string& path)
{
    if (m_AssetBrowser == nullptr)
    {
        return;
    }
    for (const AssetBrowserEntry& entry : m_AssetBrowser->Entries())
    {
        if (entry.Path.generic_string() == path || entry.DisplayName == path)
        {
            m_AssetBrowser->Activate(entry);
            RefreshAssetBrowserUi();
            return;
        }
    }
}

void EditorApplication::HandleCommand(Rml::Event& event)
{
    const std::string id = event.GetCurrentElement()->GetId();
    if (id.rfind("menu-", 0) == 0 || id == "add")
    {
        OpenMenu(id);
    }
    else if (id.rfind("cmd-", 0) == 0)
    {
        HandleMenuCommand(id);
    }
    else if (id == "modal-cancel")
    {
        m_SaveAllPending = false;
        CloseModal();
    }
    else if (id == "modal-discard")
    {
        auto proceed = std::move(m_ModalProceed);
        CloseModal();
        if (proceed)
        {
            proceed();
        }
    }
    else if (id == "modal-save")
    {
        if (m_SceneDocument.Path.empty())
        {
            CommandSaveSceneAs();
            return;
        }
        auto proceed = m_ModalProceed;
        CommandSaveScene();
        if (!m_SceneDocument.Dirty)
        {
            CloseModal();
            if (proceed)
            {
                proceed();
            }
        }
    }
    else if (id == "modal-open-path" || id == "modal-save-path")
    {
        if (Rml::Element* pathElement = m_Document->GetElementById("scene-path"))
        {
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControl*>(pathElement))
            {
                const std::filesystem::path path{std::string{input->GetValue()}};
                if (id == "modal-open-path")
                {
                    CloseModal();
                    OpenScenePath(path);
                }
                else
                {
                    auto proceed = m_ModalProceed;
                    CloseModal();
                    const bool sceneSaved = SaveScenePath(path);
                    if (m_SaveAllPending)
                    {
                        const bool layoutSaved = SaveWorkspaceLayout();
                        SetStatus(sceneSaved && layoutSaved ? "Saved all project changes" : "Save All incomplete");
                        m_SaveAllPending = false;
                    }
                    if (!m_SceneDocument.Dirty && proceed)
                    {
                        proceed();
                    }
                }
            }
        }
    }
    else if (id == layout::Play)
    {
        SetPlayMode(m_PlayMode == EditorPlayMode::Edit ? EditorPlayMode::Play
                                                       : EditorPlayMode::Edit);
    }
    else if (id == layout::Pause)
    {
        if (m_PlayMode == EditorPlayMode::Play)
        {
            SetPlayMode(EditorPlayMode::Paused);
        }
        else if (m_PlayMode == EditorPlayMode::Paused)
        {
            SetPlayMode(EditorPlayMode::Play);
        }
    }
    else if (id == layout::Step)
    {
        if (m_PlayMode == EditorPlayMode::Paused && m_PlaySession)
        {
            m_PlaySession->SingleStep();
            SetStatus("Stepped one physics tick");
        }
    }
    else if (id == layout::Stop)
    {
        SetPlayMode(EditorPlayMode::Edit);
    }
    else if (id == layout::ModeEdit)
    {
        SetViewportCameraMode(ViewportCameraMode::Edit);
    }
    else if (id == layout::ModeGame)
    {
        SetViewportCameraMode(ViewportCameraMode::Game);
    }
    else if (id == layout::GizmoSelect) SetGizmoToolMode(0);
    else if (id == layout::GizmoMove) SetGizmoToolMode(1);
    else if (id == layout::GizmoRotate) SetGizmoToolMode(2);
    else if (id == layout::GizmoScale) SetGizmoToolMode(3);
    else if (id == layout::GizmoSpace) ToggleGizmoSpace();
    else if (id == "gizmo-rail-toggle") ToggleViewportToolRail();
    else if (id == "save-all") CommandSaveAll();
    else if (id == "undo")
    {
        m_History.Undo();
        m_SceneDocument.Dirty = true;
        if (m_Scene) m_Scene->Refresh();
        RefreshSceneChrome();
    }
    else if (id == "redo")
    {
        m_History.Redo();
        m_SceneDocument.Dirty = true;
        if (m_Scene) m_Scene->Refresh();
        RefreshSceneChrome();
    }
    else if (id == layout::TabAssets) SelectBottomTab(BottomTab::Assets);
    else if (id == layout::TabOutput) SelectBottomTab(BottomTab::Output);
    else if (id == "output-clear")
    {
        m_OutputEntries.clear();
        RebuildOutput();
    }
    else if (id == "output-filter-info" || id == "output-filter-warn" || id == "output-filter-error")
    {
        bool* value = id == "output-filter-info" ? &m_OutputInfo
            : id == "output-filter-warn" ? &m_OutputWarn : &m_OutputError;
        *value = !*value;
        event.GetCurrentElement()->SetClass("toggle-on", *value);
        RebuildOutput();
    }
    else if (id == "output-autoscroll")
    {
        m_OutputAutoScroll = !m_OutputAutoScroll;
        event.GetCurrentElement()->SetClass("toggle-on", m_OutputAutoScroll);
    }
    else if (id == "asset-up" && m_AssetBrowser)
    {
        static_cast<void>(m_AssetBrowser->NavigateUp());
        RefreshAssetBrowserUi();
    }
    else if (id == "asset-refresh" && m_AssetBrowser)
    {
        static_cast<void>(m_AssetBrowser->Refresh());
        RefreshAssetBrowserUi();
    }
    else if (id == "hierarchy-filter" && m_Scene)
    {
        if (auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement()))
        {
            m_Scene->SetFilter(std::string{control->GetValue()});
        }
    }
    else if (id == "asset-search" && m_AssetBrowser)
    {
        if (auto* control =
                rmlui_dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement()))
        {
            m_AssetBrowser->SetSearch(std::string{control->GetValue()});
            RefreshAssetBrowserUi();
        }
    }
    else if (id.size() >= 12 && id.compare(0, 12, "asset-entry-") == 0 && m_AssetBrowser)
    {
        try
        {
            const std::size_t index = static_cast<std::size_t>(std::stoul(std::string{id.substr(12)}));
            const auto entries = m_AssetBrowser->Entries();
            if (index < entries.size())
            {
                m_SelectedAsset = index;
                if (event.GetId() == Rml::EventId::Dblclick)
                {
                    m_AssetBrowser->Activate(entries[index]);
                    m_SelectedAsset.reset();
                }
                RefreshAssetBrowserUi();
            }
        }
        catch (...)
        {
            SetStatus("Invalid asset entry");
        }
    }
}

std::string EditorApplication::EscapeRml(const std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped.push_back(character); break;
        }
    }
    return escaped;
}

void EditorApplication::Log(std::string text, const char* severity)
{
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::ostringstream entry;
    entry << severity << '\t';
    if (local.tm_hour < 10) entry << '0';
    entry << local.tm_hour << ':';
    if (local.tm_min < 10) entry << '0';
    entry << local.tm_min << ':';
    if (local.tm_sec < 10) entry << '0';
    entry << local.tm_sec << '\t' << std::move(text);
    m_OutputEntries.push_back(entry.str());
    if (m_OutputEntries.size() > 500)
    {
        m_OutputEntries.erase(m_OutputEntries.begin(), m_OutputEntries.begin() + 100);
    }
    RebuildOutput();
}

void EditorApplication::RebuildOutput()
{
    if (m_Document == nullptr)
    {
        return;
    }
    Rml::Element* list = m_Document->GetElementById("output-list");
    if (list == nullptr)
    {
        return;
    }
    std::ostringstream html;
    for (const std::string& entry : m_OutputEntries)
    {
        const std::size_t first = entry.find('\t');
        const std::size_t second = first == std::string::npos ? first : entry.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos)
        {
            continue;
        }
        const std::string_view severity{entry.data(), first};
        const bool visible = (severity == "info" && m_OutputInfo) ||
            (severity == "warn" && m_OutputWarn) || (severity == "error" && m_OutputError);
        if (!visible)
        {
            continue;
        }
        html << "<div class='output-row'><span class='time'>"
             << EscapeRml(std::string_view{entry}.substr(first + 1, second - first - 1))
             << "</span><span class='severity " << severity << "'>";
        if (severity == "warn") html << "WARN";
        else if (severity == "error") html << "ERROR";
        else html << "INFO";
        html << "</span><span class='output-message'>"
             << EscapeRml(std::string_view{entry}.substr(second + 1)) << "</span></div>";
    }
    if (html.str().empty())
    {
        html << "<div class='output-empty'>No messages match the active filters</div>";
    }
    list->SetInnerRML(html.str());
    if (m_OutputAutoScroll)
    {
        list->SetScrollTop(list->GetScrollHeight());
    }
}

void EditorApplication::OpenMenu(const std::string& menuId)
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (m_OpenMenuId == menuId)
    {
        CloseMenu();
        return;
    }
    Rml::Element* popup = m_Document->GetElementById(layout::MenuPopup);
    Rml::Element* anchor = m_Document->GetElementById(menuId);
    if (popup == nullptr || anchor == nullptr)
    {
        return;
    }

    const auto item = [](const std::string_view id,
                          const std::string_view label,
                          const bool danger = false) {
        return std::string{"<button class=\"popup-item"} + (danger ? " danger" : "") +
            "\" id=\"" + std::string{id} + "\"><span class=\"popup-label\">" +
            std::string{label} + "</span></button>";
    };
    const std::string separator = "<div class=\"popup-sep\"></div>";
    std::string markup;
    if (menuId == "menu-scene")
        markup = item("cmd-scene-new", "New Scene") + item("cmd-scene-open", "Open Scene...") +
            separator + item("cmd-scene-save", "Save Scene") + item("cmd-scene-save-as", "Save Scene As...") +
            item("cmd-save-all", "Save All") +
            separator + item("cmd-project-manager", "Exit to Project Manager");
    else if (menuId == "menu-edit")
        markup = item("cmd-undo", "Undo") + item("cmd-redo", "Redo") + separator +
            item("cmd-duplicate", "Duplicate") + item("cmd-delete", "Delete", true) +
            item("cmd-focus", "Focus Selection");
    else if (menuId == "menu-project")
        markup = item("cmd-project-manager", "Return to Project Manager");
    else if (menuId == "menu-debug")
        markup = item("cmd-show-output", "Show Output") + separator +
            item("cmd-debug-motion-vectors", "Motion Vectors") +
            item("cmd-debug-taa-rejection", "TAA Rejection");
    else if (menuId == "menu-editor")
        markup = item("cmd-reset-layout", "Reset Docking Layout");
    else
        markup = item("cmd-add-empty", "Empty Entity") + item("cmd-add-cube", "Cube") +
            item("cmd-add-camera", "Camera") + item("cmd-add-light", "Directional Light") +
            separator + item("cmd-add-jolt", "3D Physics Body") +
            item("cmd-add-box2d", "2D Physics Body");

    popup->SetInnerRML(markup);
    popup->SetClass("hidden", false);
    const Rml::Vector2f position = anchor->GetAbsoluteOffset(Rml::BoxArea::Border);
    popup->SetProperty("left", std::to_string(static_cast<int>(position.x)) + "px");
    popup->SetProperty("top", std::to_string(static_cast<int>(position.y + anchor->GetBox().GetSize().y)) + "px");
    for (Rml::Element* child = popup->GetFirstChild(); child != nullptr; child = child->GetNextSibling())
    {
        if (child->GetId().rfind("cmd-", 0) == 0)
        {
            child->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
        }
    }
    m_OpenMenuId = menuId;
}

void EditorApplication::CloseMenu()
{
    if (m_Document != nullptr)
    {
        if (Rml::Element* popup = m_Document->GetElementById(layout::MenuPopup))
        {
            popup->SetClass("hidden", true);
        }
    }
    m_OpenMenuId.clear();
}

void EditorApplication::HandleMenuCommand(const std::string& commandId)
{
    CloseMenu();
    if (commandId == "cmd-scene-new") return CommandNewScene();
    if (commandId == "cmd-scene-open") return CommandOpenScene();
    if (commandId == "cmd-scene-save") return CommandSaveScene();
    if (commandId == "cmd-scene-save-as") return CommandSaveSceneAs();
    if (commandId == "cmd-save-all") return CommandSaveAll();
    if (commandId == "cmd-project-manager") return CommandExitToProjectManager();
    if (commandId == "cmd-show-output") return SelectBottomTab(BottomTab::Output);
    if (commandId == "cmd-debug-motion-vectors")
    {
        m_PostDebugView = m_PostDebugView == PostDebugView::MotionVectors ? PostDebugView::None
                                                                          : PostDebugView::MotionVectors;
        return;
    }
    if (commandId == "cmd-debug-taa-rejection")
    {
        m_PostDebugView = m_PostDebugView == PostDebugView::TaaRejection ? PostDebugView::None
                                                                         : PostDebugView::TaaRejection;
        return;
    }
    if (commandId == "cmd-focus") return FocusSelection();
    if (commandId == "cmd-reset-layout")
    {
        SetGizmoToolMode(0);
        ResetWorkspaceLayout();
        return;
    }
    if (commandId == "cmd-undo" || commandId == "cmd-redo")
    {
        if (commandId == "cmd-undo" && m_History.CanUndo()) m_History.Undo();
        if (commandId == "cmd-redo" && m_History.CanRedo()) m_History.Redo();
        m_SceneDocument.Dirty = true;
        if (m_Scene) m_Scene->Refresh();
        RefreshSceneChrome();
        return;
    }
    const auto selection = m_Scene ? m_Scene->Selection() : std::nullopt;
    if (commandId == "cmd-delete" && selection)
    {
        m_History.Push(std::make_unique<DeleteEntityCommand>(*m_EditWorld, *selection));
        m_Scene->SetSelection(std::nullopt);
    }
    else if (commandId == "cmd-duplicate" && selection)
    {
        auto command = std::make_unique<DuplicateEntityCommand>(*m_EditWorld, *selection);
        const Uuid duplicate = command->DuplicateId();
        m_History.Push(std::move(command));
        m_Scene->SetSelection(duplicate);
    }
    else if (commandId.rfind("cmd-add-", 0) == 0)
    {
        std::string name = "Entity";
        if (commandId == "cmd-add-cube") name = "Cube";
        else if (commandId == "cmd-add-camera") name = "Camera";
        else if (commandId == "cmd-add-light") name = "Directional Light";
        else if (commandId == "cmd-add-jolt") name = "3D Physics Body";
        else if (commandId == "cmd-add-box2d") name = "2D Physics Body";
        auto command = std::make_unique<AddEntityCommand>(*m_EditWorld, name);
        const Uuid created = command->EntityId();
        if (commandId == "cmd-add-cube") command->SetMesh(MeshComponent{});
        else if (commandId == "cmd-add-camera") command->SetCamera(CameraComponent{});
        else if (commandId == "cmd-add-light") command->SetDirectionalLight(DirectionalLightComponent{});
        else if (commandId == "cmd-add-jolt") command->SetJoltBody(JoltBodyComponent{});
        else if (commandId == "cmd-add-box2d") command->SetBox2DBody(Box2DBodyComponent{});
        m_History.Push(std::move(command));
        m_Scene->SetSelection(created);
    }
    else
    {
        return;
    }
    m_SceneDocument.Dirty = true;
    m_Scene->Refresh();
    RefreshSceneChrome();
}

void EditorApplication::ShowConfirmDiscard(std::function<void()> onProceed)
{
    if (!m_SceneDocument.Dirty)
    {
        onProceed();
        return;
    }
    Rml::Element* modal = m_Document ? m_Document->GetElementById("modal-root") : nullptr;
    if (modal == nullptr)
    {
        return;
    }
    m_ModalProceed = std::move(onProceed);
    modal->SetInnerRML("<div class='modal'><div class='modal-title'>Unsaved scene</div><div class='modal-text'>Save your changes before continuing?</div><div class='modal-buttons'><button id='modal-cancel'>Cancel</button><button id='modal-discard' class='danger'>Discard</button><button id='modal-save' class='primary'>Save</button></div></div>");
    modal->SetClass("hidden", false);
    for (const char* id : {"modal-cancel", "modal-discard", "modal-save"})
        if (Rml::Element* element = m_Document->GetElementById(id)) element->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
}

void EditorApplication::CloseModal()
{
    if (m_Document != nullptr)
    {
        if (Rml::Element* modal = m_Document->GetElementById("modal-root"))
        {
            modal->SetClass("hidden", true);
        }
    }
    m_ModalProceed = {};
}

void EditorApplication::SelectBottomTab(const BottomTab tab)
{
    m_BottomTab = tab;
    if (m_Document == nullptr) return;
    if (Rml::Element* assets = m_Document->GetElementById(layout::AssetBrowser)) assets->SetClass("hidden", tab != BottomTab::Assets);
    if (Rml::Element* output = m_Document->GetElementById(layout::Output)) output->SetClass("hidden", tab != BottomTab::Output);
    if (Rml::Element* button = m_Document->GetElementById(layout::TabAssets)) button->SetClass("active", tab == BottomTab::Assets);
    if (Rml::Element* button = m_Document->GetElementById(layout::TabOutput)) button->SetClass("active", tab == BottomTab::Output);
    if (tab == BottomTab::Output) RebuildOutput();
}

void EditorApplication::SetGizmoToolMode(const int mode)
{
    m_GizmoToolMode = std::clamp(mode, 0, 3);
    if (m_Gizmo)
    {
        const GizmoMode modes[]{GizmoMode::Select, GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale};
        m_Gizmo->SetMode(modes[m_GizmoToolMode]);
    }
    if (m_Document != nullptr)
    {
        const char* ids[]{layout::GizmoSelect, layout::GizmoMove, layout::GizmoRotate, layout::GizmoScale};
        for (int index = 0; index < 4; ++index)
            if (Rml::Element* element = m_Document->GetElementById(ids[index])) element->SetClass("active", index == m_GizmoToolMode);
    }
}

void EditorApplication::ToggleGizmoSpace()
{
    m_GizmoLocalSpace = !m_GizmoLocalSpace;
    RefreshViewportToolRail();
}

void EditorApplication::ToggleViewportToolRail()
{
    m_ViewportToolRailCollapsed = !m_ViewportToolRailCollapsed;
    RefreshViewportToolRail();
}

void EditorApplication::RefreshViewportToolRail()
{
    if (m_Document == nullptr)
    {
        return;
    }
    if (Rml::Element* rail = m_Document->GetElementById("viewport-tool-rail"))
    {
        rail->SetClass("collapsed", m_ViewportToolRailCollapsed);
    }
    if (Rml::Element* space = m_Document->GetElementById(layout::GizmoSpace))
    {
        space->SetClass("local", m_GizmoLocalSpace);
    }
    if (Rml::Element* label = m_Document->GetElementById("gizmo-space-tooltip-label"))
    {
        label->SetInnerRML(m_GizmoLocalSpace ? "Local Space" : "World Space");
    }
    if (Rml::Element* label = m_Document->GetElementById("gizmo-rail-tooltip-label"))
    {
        label->SetInnerRML(m_ViewportToolRailCollapsed ? "Show Tools" : "Hide Tools");
    }
}

void EditorApplication::ResetWorkspaceLayout(const bool persist)
{
    if (m_DockManager != nullptr)
    {
        m_DockManager->Reset();
    }
    m_ViewportToolRailCollapsed = false;
    m_GizmoLocalSpace = false;
    SetGizmoToolMode(0);
    SetViewportCameraMode(ViewportCameraMode::Edit);
    SelectBottomTab(BottomTab::Assets);
    RefreshViewportToolRail();
    if (persist)
    {
        static_cast<void>(SaveWorkspaceLayout());
        SetStatus("Workspace layout reset");
    }
}

void EditorApplication::LoadWorkspaceLayout()
{
    ResetWorkspaceLayout(false);
    if (m_ActiveProject.RootPath.empty())
    {
        return;
    }
    const std::filesystem::path path =
        m_ActiveProject.RootPath / "Saved" / "Editor" / "layout.json";
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return;
    }
    std::ostringstream text;
    text << input.rdbuf();
    const auto parsed = project_json::Parse(text.str());
    if (!parsed || !parsed->IsObject())
    {
        Log("Ignoring invalid editor layout " + path.generic_string(), "warn");
        return;
    }

    std::string error;
    if (m_DockManager == nullptr || !m_DockManager->Deserialize(*parsed, error))
    {
        Log("Ignoring invalid editor layout " + path.generic_string() +
            (error.empty() ? std::string{} : (": " + error)),
            "warn");
        return;
    }

    if (parsed->Contains("viewportCamera") && parsed->at("viewportCamera").IsString())
    {
        SetViewportCameraMode(parsed->at("viewportCamera").AsString() == "game"
            ? ViewportCameraMode::Game
            : ViewportCameraMode::Edit);
    }
    if (parsed->Contains("contentTab") && parsed->at("contentTab").IsString())
    {
        SelectBottomTab(parsed->at("contentTab").AsString() == "output"
            ? BottomTab::Output
            : BottomTab::Assets);
    }
    if (parsed->Contains("toolRailCollapsed"))
    {
        m_ViewportToolRailCollapsed = parsed->at("toolRailCollapsed").AsBool();
    }
    if (parsed->Contains("localGizmo"))
    {
        m_GizmoLocalSpace = parsed->at("localGizmo").AsBool();
    }
    RefreshViewportToolRail();
}

bool EditorApplication::SaveWorkspaceLayout()
{
    if (m_ActiveProject.RootPath.empty() || m_DockManager == nullptr)
    {
        return false;
    }
    const std::filesystem::path folder = m_ActiveProject.RootPath / "Saved" / "Editor";
    const std::filesystem::path path = folder / "layout.json";
    const std::filesystem::path temporary = folder / "layout.json.tmp";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error)
    {
        Log("Could not create editor layout folder: " + error.message(), "error");
        return false;
    }

    project_json::Value root = m_DockManager->Serialize();
    root["viewportCamera"] = project_json::Value::MakeString(
        m_ViewportCameraMode == ViewportCameraMode::Game ? "game" : "edit");
    root["contentTab"] = project_json::Value::MakeString(
        m_BottomTab == BottomTab::Output ? "output" : "assets");
    root["toolRailCollapsed"] = project_json::Value::MakeBool(m_ViewportToolRailCollapsed);
    root["localGizmo"] = project_json::Value::MakeBool(m_GizmoLocalSpace);

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            Log("Could not write editor layout " + temporary.generic_string(), "error");
            return false;
        }
        output << project_json::Stringify(root) << '\n';
        if (!output)
        {
            Log("Could not finish editor layout " + temporary.generic_string(), "error");
            return false;
        }
    }
    std::filesystem::remove(path, error);
    error.clear();
    std::filesystem::rename(temporary, path, error);
    if (error)
    {
        Log("Could not replace editor layout " + path.generic_string() + ": " + error.message(), "error");
        return false;
    }
    return true;
}

void EditorApplication::SetViewportCameraMode(const ViewportCameraMode mode)
{
    m_ViewportCameraMode = mode;
    if (auto* module = camera::Module()) module->SetViewportMode(mode);
    if (m_Document != nullptr)
    {
        if (Rml::Element* element = m_Document->GetElementById(layout::ModeEdit)) element->SetClass("active", mode == ViewportCameraMode::Edit);
        if (Rml::Element* element = m_Document->GetElementById(layout::ModeGame)) element->SetClass("active", mode == ViewportCameraMode::Game);
    }
    UpdateStatusBar();
}

void EditorApplication::CommandNewScene()
{
    ShowConfirmDiscard([this]() {
        m_Scenes->New(m_SceneDocument, *m_EditWorld, "Untitled Scene");
        m_History.Clear();
        if (m_Scene)
        {
            m_Scene->SetSelection(std::nullopt);
            m_Scene->Refresh();
        }
        if (m_Viewport)
        {
            m_Viewport->ResetTemporalHistory();
        }
        RefreshSceneChrome();
        Log("Created a new scene");
    });
}

void EditorApplication::CommandOpenScene()
{
    ShowConfirmDiscard([this]() {
        Rml::Element* modal = m_Document ? m_Document->GetElementById("modal-root") : nullptr;
        if (modal == nullptr) return;
        const std::filesystem::path initial = m_SceneDocument.Path.empty()
            ? m_ActiveProject.RootPath / "Scenes" / "Main.scene" : m_SceneDocument.Path;
        modal->SetInnerRML("<div class='modal wide'><div class='modal-title'>Open Scene</div><div class='modal-text'><label>Scene file</label><input id='scene-path' type='text' value='" + EscapeRml(initial.generic_string()) + "'/></div><div class='modal-buttons'><button id='modal-cancel'>Cancel</button><button id='modal-open-path' class='primary'>Open</button></div></div>");
        modal->SetClass("hidden", false);
        for (const char* id : {"modal-cancel", "modal-open-path"})
            if (Rml::Element* element = m_Document->GetElementById(id)) element->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
    });
}

void EditorApplication::CommandSaveScene()
{
    if (m_SceneDocument.Path.empty())
    {
        CommandSaveSceneAs();
        return;
    }
    static_cast<void>(SaveScenePath(m_SceneDocument.Path));
}

void EditorApplication::CommandSaveAll()
{
    const bool layoutSaved = SaveWorkspaceLayout();
    if (m_SceneDocument.Path.empty())
    {
        m_SaveAllPending = true;
        CommandSaveSceneAs();
        return;
    }
    const bool sceneSaved = SaveScenePath(m_SceneDocument.Path);
    SetStatus(sceneSaved && layoutSaved ? "Saved all project changes" : "Save All incomplete");
    if (sceneSaved && layoutSaved)
    {
        Log("Saved all project changes");
    }
}

void EditorApplication::CommandSaveSceneAs()
{
    Rml::Element* modal = m_Document ? m_Document->GetElementById("modal-root") : nullptr;
    if (modal == nullptr) return;
    const std::filesystem::path initial = m_SceneDocument.Path.empty()
        ? m_ActiveProject.RootPath / "Scenes" / (m_SceneDocument.Name + ".scene")
        : m_SceneDocument.Path;
    modal->SetInnerRML("<div class='modal wide'><div class='modal-title'>Save Scene As</div><div class='modal-text'><label>Scene file</label><input id='scene-path' type='text' value='" + EscapeRml(initial.generic_string()) + "'/></div><div class='modal-buttons'><button id='modal-cancel'>Cancel</button><button id='modal-save-path' class='primary'>Save</button></div></div>");
    modal->SetClass("hidden", false);
    for (const char* id : {"modal-cancel", "modal-save-path"})
        if (Rml::Element* element = m_Document->GetElementById(id)) element->AddEventListener(Rml::EventId::Click, m_CommandListener.get());
}

void EditorApplication::CommandExitToProjectManager()
{
    ShowConfirmDiscard([this]() { ShowProjectManager(); });
}

void EditorApplication::OpenScenePath(const std::filesystem::path& path)
{
    if (path.empty())
    {
        SetStatus("Choose a scene file");
        return;
    }
    const auto result = m_Scenes->Load(m_SceneDocument, *m_EditWorld, path);
    if (!result)
    {
        SetStatus("Could not open scene");
        Log("Could not open " + path.generic_string() + ": " + result.ErrorMessage(), "error");
        return;
    }
    m_History.Clear();
    if (m_Scene)
    {
        m_Scene->SetSelection(std::nullopt);
        m_Scene->Refresh();
    }
    if (m_Viewport)
    {
        m_Viewport->ResetTemporalHistory();
    }
    RefreshSceneChrome();
    SetStatus("Opened " + path.filename().string());
    Log("Opened scene " + path.generic_string());
}

bool EditorApplication::SaveScenePath(const std::filesystem::path& path)
{
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
    {
        SetStatus("Could not create scene folder");
        Log("Could not create " + path.parent_path().generic_string() + ": " + error.message(), "error");
        return false;
    }
    const auto result = m_Scenes->SaveAs(m_SceneDocument, *m_EditWorld, path);
    if (!result)
    {
        SetStatus("Could not save scene");
        Log("Could not save " + path.generic_string() + ": " + result.ErrorMessage(), "error");
        return false;
    }
    m_SceneDocument.Name = path.stem().string();
    RefreshSceneChrome();
    SetStatus("Saved " + path.filename().string());
    Log("Saved scene " + path.generic_string());
    return true;
}

void EditorApplication::QueueMainThread(std::function<void()> action)
{
    std::scoped_lock lock{m_MainThreadQueueMutex};
    m_MainThreadQueue.push_back(std::move(action));
}

void EditorApplication::RefreshSceneChrome()
{
    if (m_Document == nullptr) return;
    if (Rml::Element* label = m_Document->GetElementById(layout::SceneLabel))
        label->SetInnerRML(EscapeRml(m_SceneDocument.Name + (m_SceneDocument.Dirty ? " *" : "")));
    if (Rml::Element* undo = m_Document->GetElementById("undo"))
    {
        if (m_History.CanUndo()) undo->RemoveAttribute("disabled"); else undo->SetAttribute("disabled", "disabled");
    }
    if (Rml::Element* redo = m_Document->GetElementById("redo"))
    {
        if (m_History.CanRedo()) redo->RemoveAttribute("disabled"); else redo->SetAttribute("disabled", "disabled");
    }
}

void EditorApplication::RefreshImportChrome()
{
    if (m_Document == nullptr) return;
    Rml::Element* progress = m_Document->GetElementById("asset-progress");
    Rml::Element* label = m_Document->GetElementById("asset-progress-label");
    Rml::Element* value = m_Document->GetElementById("asset-progress-value");
    Rml::Element* state = m_Document->GetElementById("import-state");
    const auto* database = dynamic_cast<const AssetDatabase*>(m_Assets.get());
    const AssetImportStatus* active = nullptr;
    if (database != nullptr)
    {
        for (const AssetImportStatus& status : database->ImportStatuses())
            if (status.State == AssetImportState::Queued || status.State == AssetImportState::Importing) active = &status;
    }
    if (progress != nullptr) progress->SetClass("hidden", active == nullptr);
    if (active == nullptr)
    {
        if (state != nullptr) state->SetInnerRML("");
        return;
    }
    const int percent = static_cast<int>(std::clamp(active->Progress, 0.0F, 1.0F) * 100.0F);
    if (label != nullptr) label->SetInnerRML("Importing " + EscapeRml(active->SourcePath.filename().string()));
    if (value != nullptr) value->SetProperty("width", std::to_string(percent) + "%");
    if (state != nullptr) state->SetInnerRML(std::to_string(percent) + "%");
}

void EditorApplication::UpdateStatusBar()
{
    if (m_Document == nullptr || !m_InWorkbench) return;
    if (Rml::Element* scene = m_Document->GetElementById(layout::StatusScene))
        scene->SetInnerRML(EscapeRml(m_SceneDocument.Name + (m_SceneDocument.Dirty ? " *" : "")));
    if (Rml::Element* cameraStatus = m_Document->GetElementById(layout::StatusCamera))
        cameraStatus->SetInnerRML(m_ViewportCameraMode == ViewportCameraMode::Edit ? "Edit Camera" : "Game Camera");
    if (Rml::Element* backend = m_Document->GetElementById(layout::StatusBackend))
    {
        const char* driver = m_Device
            ? SDL_GetGPUDeviceDriver(static_cast<SDL_GPUDevice*>(GetNativeDeviceHandle(*m_Device)))
            : nullptr;
        backend->SetInnerRML(EscapeRml(driver ? std::string{"SDL GPU / "} + driver : "SDL GPU"));
    }
    if (Rml::Element* entities = m_Document->GetElementById(layout::StatusEntities))
        entities->SetInnerRML(std::to_string(m_EditWorld->Registry().view<UuidComponent>().size()) + " entities");
}

void EditorApplication::UpdatePlayChrome()
{
    if (m_Document == nullptr) return;
    const bool active = m_PlayMode != EditorPlayMode::Edit;
    const bool paused = m_PlayMode == EditorPlayMode::Paused;
    if (Rml::Element* banner = m_Document->GetElementById(layout::PlayBanner))
    {
        banner->SetClass("hidden", !active);
        banner->SetInnerRML(paused ? "Paused" : "Running");
        banner->SetClass("paused", paused);
    }
    if (Rml::Element* play = m_Document->GetElementById(layout::Play))
    {
        play->SetClass("active", active && !paused);
        play->SetInnerRML(active ? "Stop" : "Play");
    }
    if (Rml::Element* pause = m_Document->GetElementById(layout::Pause))
    {
        pause->SetClass("active", paused);
        if (active) pause->RemoveAttribute("disabled"); else pause->SetAttribute("disabled", "disabled");
    }
    if (Rml::Element* step = m_Document->GetElementById(layout::Step))
    {
        if (paused) step->RemoveAttribute("disabled"); else step->SetAttribute("disabled", "disabled");
    }
    if (Rml::Element* stop = m_Document->GetElementById(layout::Stop))
    {
        if (active) stop->RemoveAttribute("disabled"); else stop->SetAttribute("disabled", "disabled");
    }
    for (const char* id : {"add", layout::GizmoSelect, layout::GizmoMove, layout::GizmoRotate,
             layout::GizmoScale, layout::GizmoSpace, "save-all", "undo", "redo"})
    {
        if (Rml::Element* element = m_Document->GetElementById(id))
        {
            if (active) element->SetAttribute("disabled", "disabled");
            else element->RemoveAttribute("disabled");
        }
    }
    if (Rml::Element* note = m_Document->GetElementById("no-game-camera"))
    {
        const auto* module = camera::Module();
        note->SetClass("hidden", m_ViewportCameraMode != ViewportCameraMode::Game || (module != nullptr && module->GameCamera().has_value()));
    }
}

void EditorApplication::UpdateGizmoVisual()
{
    if (m_Viewport == nullptr || m_Scene == nullptr) return;
    const auto selection = m_Scene->Selection();
    m_Viewport->SetSelection(selection);
    GizmoVisual visual;
    visual.Visible = selection.has_value() && m_GizmoToolMode != 0 && m_PlayMode == EditorPlayMode::Edit;
    visual.Mode = m_GizmoToolMode == 2 ? GizmoMode::Rotate : m_GizmoToolMode == 3 ? GizmoMode::Scale : GizmoMode::Translate;
    visual.Hover = m_GizmoHoverAxis ? std::optional<GizmoAxis>{static_cast<GizmoAxis>(*m_GizmoHoverAxis)} : std::nullopt;
    visual.Active = m_GizmoDragging ? visual.Hover : std::nullopt;
    if (selection)
    {
        if (const auto entity = m_EditWorld->Find(*selection))
        {
            if (const TransformComponent* transform = m_EditWorld->Registry().try_get<TransformComponent>(*entity))
            {
                visual.Position = transform->Position;
                if (m_GizmoLocalSpace) visual.Orientation = transform->Rotation;
            }
        }
    }
    m_Viewport->SetGizmo(visual);
}

bool EditorApplication::TextInputFocused() const
{
    if (m_Context == nullptr) return false;
    Rml::Element* focused = m_Context->GetFocusElement();
    return focused != nullptr && rmlui_dynamic_cast<Rml::ElementFormControl*>(focused) != nullptr;
}

bool EditorApplication::HandleShortcut(const SDL_Event& event)
{
    if (!m_InWorkbench || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat || TextInputFocused())
        return false;
    if (auto* module = camera::Module();
        module != nullptr && module->Input().IsNavigating())
    {
        return false;
    }
    if (m_PlayMode != EditorPlayMode::Edit)
        return false;

    const SDL_Keymod modifiers = SDL_GetModState();
    const bool control = (modifiers & SDL_KMOD_CTRL) != 0;
    const bool shift = (modifiers & SDL_KMOD_SHIFT) != 0;
    if (control && event.key.scancode == SDL_SCANCODE_S)
    {
        if (shift) CommandSaveAll(); else CommandSaveScene();
        return true;
    }
    if (control && event.key.scancode == SDL_SCANCODE_N) { CommandNewScene(); return true; }
    if (control && event.key.scancode == SDL_SCANCODE_O) { CommandOpenScene(); return true; }
    if (control && event.key.scancode == SDL_SCANCODE_Z)
    {
        HandleMenuCommand(shift ? "cmd-redo" : "cmd-undo");
        return true;
    }
    if (control && event.key.scancode == SDL_SCANCODE_Y) { HandleMenuCommand("cmd-redo"); return true; }
    if (control && event.key.scancode == SDL_SCANCODE_D) { HandleMenuCommand("cmd-duplicate"); return true; }
    if (event.key.scancode == SDL_SCANCODE_F2 && m_Scene && m_Scene->Selection())
    {
        if (Rml::Element* name = m_Document->GetElementById("entity-name"))
        {
            name->Focus();
            if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(name))
                input->SetSelectionRange(0, static_cast<int>(input->GetValue().size()));
        }
        return true;
    }
    if (event.key.scancode == SDL_SCANCODE_DELETE) { HandleMenuCommand("cmd-delete"); return true; }
    if (event.key.scancode == SDL_SCANCODE_F) { FocusSelection(); return true; }
    if (event.key.scancode == SDL_SCANCODE_Q) { SetGizmoToolMode(0); return true; }
    if (event.key.scancode == SDL_SCANCODE_W) { SetGizmoToolMode(1); return true; }
    if (event.key.scancode == SDL_SCANCODE_E) { SetGizmoToolMode(2); return true; }
    if (event.key.scancode == SDL_SCANCODE_R) { SetGizmoToolMode(3); return true; }
    if (event.key.scancode == SDL_SCANCODE_X) { ToggleGizmoSpace(); return true; }
    if (event.key.scancode == SDL_SCANCODE_T) { ToggleViewportToolRail(); return true; }
    if (event.key.scancode == SDL_SCANCODE_ESCAPE)
    {
        if (m_GizmoDragging && m_Gizmo)
        {
            m_Gizmo->Cancel(*m_EditWorld);
            m_GizmoDragging = false;
            m_GizmoStartTransform.reset();
        }
        CloseMenu();
        return true;
    }
    return false;
}

bool EditorApplication::HandleInspectorWheel(const SDL_Event& event)
{
    if (!m_InWorkbench || m_Document == nullptr || event.type != SDL_EVENT_MOUSE_WHEEL)
    {
        return false;
    }
    Rml::Element* inspector = m_Document->GetElementById("inspector-content");
    if (inspector == nullptr)
    {
        return false;
    }
    float mouseX = 0.0F;
    float mouseY = 0.0F;
    SDL_GetMouseState(&mouseX, &mouseY);
    const Rml::Vector2f position = inspector->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = inspector->GetBox().GetSize(Rml::BoxArea::Content);
    if (mouseX < position.x || mouseY < position.y ||
        mouseX >= position.x + size.x || mouseY >= position.y + size.y)
    {
        return false;
    }

    const float direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
    const bool horizontal = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
    if (horizontal || event.wheel.x != 0.0F)
    {
        const float delta = event.wheel.x != 0.0F ? event.wheel.x : event.wheel.y;
        inspector->SetScrollLeft(inspector->GetScrollLeft() - delta * direction * 36.0F);
    }
    else
    {
        inspector->SetScrollTop(inspector->GetScrollTop() - event.wheel.y * direction * 36.0F);
    }
    return true;
}

bool EditorApplication::HandleInspectorNumericScrub(const SDL_Event& event)
{
    if (!m_InWorkbench || m_Scene == nullptr)
    {
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
    {
        // Keep the press available to RmlUi so a click still focuses the field for typing.
        static_cast<void>(m_Scene->BeginNumericScrub(event.button.x, event.button.y));
        return false;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && m_Scene->NumericScrubActive())
    {
        return m_Scene->UpdateNumericScrub(event.motion.x);
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
        m_Scene->NumericScrubActive())
    {
        return m_Scene->EndNumericScrub();
    }
    return false;
}

void EditorApplication::HandleOpenMenuPointerLeave(const SDL_Event& event)
{
    if (event.type != SDL_EVENT_MOUSE_MOTION || m_Document == nullptr || m_OpenMenuId.empty())
    {
        return;
    }
    Rml::Element* anchor = m_Document->GetElementById(m_OpenMenuId);
    Rml::Element* popup = m_Document->GetElementById(layout::MenuPopup);
    if (anchor == nullptr || popup == nullptr)
    {
        CloseMenu();
        return;
    }
    const auto contains = [](Rml::Element* element, const float x, const float y) {
        const Rml::Vector2f position = element->GetAbsoluteOffset(Rml::BoxArea::Border);
        const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
        return x >= position.x && y >= position.y && x < position.x + size.x &&
            y < position.y + size.y;
    };
    if (!contains(anchor, event.motion.x, event.motion.y) &&
        !contains(popup, event.motion.x, event.motion.y))
    {
        CloseMenu();
    }
}

bool EditorApplication::HandleViewportMouse(const SDL_Event& event)
{
    if (!m_InWorkbench || m_PlayMode != EditorPlayMode::Edit || m_Document == nullptr || m_Scene == nullptr)
        return false;
    if (auto* module = camera::Module();
        module != nullptr && module->Input().IsNavigating())
    {
        m_GizmoHoverAxis.reset();
        return false;
    }
    const bool pointerEvent = event.type == SDL_EVENT_MOUSE_MOTION ||
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
    if (!pointerEvent) return false;

    if (!m_OpenMenuId.empty())
    {
        return false;
    }
    if (Rml::Element* modal = m_Document->GetElementById("modal-root");
        modal != nullptr && !modal->IsClassSet("hidden"))
    {
        return false;
    }

    Rml::Element* viewport = m_Document->GetElementById(layout::Viewport);
    if (viewport == nullptr) return false;
    const Rml::Vector2f origin = viewport->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f extent = viewport->GetBox().GetSize(Rml::BoxArea::Content);
    const float mouseX = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
    const float mouseY = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
    if (PointInsideElement(
            m_Document->GetElementById("viewport-tool-rail"), mouseX, mouseY))
    {
        return false;
    }
    const glm::vec2 localMouse{mouseX - origin.x, mouseY - origin.y};
    const bool inside = localMouse.x >= 0.0F && localMouse.y >= 0.0F &&
        localMouse.x < extent.x && localMouse.y < extent.y;

    auto hitTestGizmo = [&]() -> std::optional<int> {
        if (!inside || m_GizmoToolMode == 0) return std::nullopt;
        const auto selection = m_Scene->Selection();
        auto* module = camera::Module();
        if (!selection || module == nullptr) return std::nullopt;
        const auto entity = m_EditWorld->Find(*selection);
        if (!entity) return std::nullopt;
        const TransformComponent* transform = m_EditWorld->Registry().try_get<TransformComponent>(*entity);
        if (transform == nullptr) return std::nullopt;
        const auto start = ProjectPoint(transform->Position, module->Camera().View(), module->Camera().Projection(), {extent.x, extent.y});
        if (!start) return std::nullopt;
        const float size = GizmoWorldSize(module->Camera().Position(), transform->Position);
        float nearest = 12.0F;
        std::optional<int> axis;
        for (int index = 0; index < 3; ++index)
        {
            glm::vec3 direction = GizmoAxisVector(static_cast<GizmoAxis>(index));
            if (m_GizmoLocalSpace) direction = transform->Rotation * direction;
            const auto end = ProjectPoint(transform->Position + direction * size, module->Camera().View(), module->Camera().Projection(), {extent.x, extent.y});
            if (!end) continue;
            const float distance = DistanceToSegment(localMouse, *start, *end);
            if (distance < nearest)
            {
                nearest = distance;
                axis = index;
            }
        }
        return axis;
    };

    if (event.type == SDL_EVENT_MOUSE_MOTION)
    {
        if (m_GizmoDragging && m_Gizmo)
        {
            auto* module = camera::Module();
            const auto selection = m_Scene->Selection();
            if (module != nullptr && selection)
            {
                const auto entity = m_EditWorld->Find(*selection);
                const TransformComponent* transform = entity ? m_EditWorld->Registry().try_get<TransformComponent>(*entity) : nullptr;
                const float distance = transform ? glm::length(module->Camera().Position() - transform->Position) : 5.0F;
                m_GizmoDragAmount += (event.motion.xrel - event.motion.yrel) * std::max(distance, 1.0F) * 0.0025F;
                static_cast<void>(m_Gizmo->ApplyDrag(*m_EditWorld, m_GizmoDragAmount));
                if (m_Scene) m_Scene->Refresh();
            }
            return true;
        }
        m_GizmoHoverAxis = hitTestGizmo();
        return m_GizmoHoverAxis.has_value();
    }

    if (event.button.button != SDL_BUTTON_LEFT) return false;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && m_GizmoDragging)
    {
        const auto selection = m_Scene->Selection();
        if (selection && m_GizmoStartTransform)
        {
            if (const auto entity = m_EditWorld->Find(*selection))
            {
                if (const TransformComponent* after = m_EditWorld->Registry().try_get<TransformComponent>(*entity))
                    m_History.Push(std::make_unique<TransformEntityCommand>(*m_EditWorld, *selection, *m_GizmoStartTransform, *after));
            }
        }
        m_Gizmo->End();
        m_GizmoDragging = false;
        m_GizmoStartTransform.reset();
        m_SceneDocument.Dirty = true;
        RefreshSceneChrome();
        return true;
    }
    if (event.type != SDL_EVENT_MOUSE_BUTTON_DOWN || !inside) return false;

    m_GizmoHoverAxis = hitTestGizmo();
    const auto selection = m_Scene->Selection();
    if (m_GizmoHoverAxis && selection && m_Gizmo)
    {
        const auto entity = m_EditWorld->Find(*selection);
        if (entity)
        {
            if (const TransformComponent* transform = m_EditWorld->Registry().try_get<TransformComponent>(*entity))
                m_GizmoStartTransform = *transform;
        }
        m_GizmoDragAmount = 0.0F;
        m_GizmoDragging = m_Gizmo->Begin(*m_EditWorld, *selection, static_cast<GizmoAxis>(*m_GizmoHoverAxis));
        return m_GizmoDragging;
    }

    if (m_Picking != nullptr && extent.x > 1.0F && extent.y > 1.0F)
    {
        m_Picking->Request({
            static_cast<std::uint32_t>(std::clamp(localMouse.x, 0.0F, extent.x - 1.0F)),
            static_cast<std::uint32_t>(std::clamp(localMouse.y, 0.0F, extent.y - 1.0F))});
        const auto result = m_Picking->Poll();
        m_Scene->SetSelection(result ? std::optional<Uuid>{result->Entity} : std::nullopt);
        return true;
    }
    return false;
}

void EditorApplication::FocusSelection()
{
    if (m_Scene == nullptr) return;
    const auto selection = m_Scene->Selection();
    auto* module = camera::Module();
    if (!selection || module == nullptr) return;
    if (const auto bounds = EntityBounds(*m_EditWorld, *selection))
    {
        module->Camera().FocusBounds(*bounds);
        SetViewportCameraMode(ViewportCameraMode::Edit);
        SetStatus("Focused selection");
    }
}

void EditorApplication::SetStatus(const std::string& text)
{
    if (m_Document != nullptr)
    {
        if (Rml::Element* element = m_Document->GetElementById(layout::Status))
        {
            element->SetInnerRML(EscapeRml(text));
        }
    }
}
}
