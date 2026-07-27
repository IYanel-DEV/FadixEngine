// =============================================================================
// Fadix Engine — Entry Point
//
// Compiled under two mutually exclusive preprocessor modes:
//
//   FADIX_EDITOR     — full IDE: ImGui dockspace, panels, project browser,
//                      scene sandboxing, gizmos. (target: fadix_editor)
//
//   FADIX_STANDALONE — stripped player: no ImGui, no editor panels, boots
//                      directly into the serialised game scene and runs
//                      UpdatePlayRuntime on every frame tick. (target: fadix_player)
//
// =============================================================================

// ---- Standard library -------------------------------------------------------
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

// ---- Third-party (always required) ------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <glad/glad.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

// ---- Core engine subsystems (shared between both build targets) --------------
#include "core/Components.hpp"
#include "core/Entity.hpp"
#include "core/Input.hpp"
#include "core/Scene.hpp"
#include "core/SceneSerializer.hpp"
#include "core/Settings.hpp"
#include "scene/ScriptableEntity.hpp"   // complete type required by UpdatePlayRuntime
#include "renderer/EditorCamera.hpp"
#include "renderer/ViewportRenderer.hpp"

// ---- Editor-only includes (compiled out for fadix_player) -------------------
#ifdef FADIX_EDITOR
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include "core/EditorLayout.hpp"
#include "core/EditorTheme.hpp"
#include "core/EmbeddedAssetRegistry.hpp"
#include "core/EngineContext.hpp"
#include "core/EngineState.hpp"
#include "core/FileSystem.hpp"
#include "core/ProjectManager.hpp"
#include "core/ScriptCompiler.hpp"   // kOutputDllPath for session-start DLL load
#endif // FADIX_EDITOR

// Physics system — available in both editor and standalone player.
#include "core/PhysicsSystem.hpp"

// Network replication — available in both editor and standalone player.
// Stubs gracefully when FADIX_NET_INCLUDED is not defined.
#include "core/NetworkManager.hpp"

namespace
{

// =============================================================================
// Shared constants
// =============================================================================

constexpr int kWindowWidth  = 1280;
constexpr int kWindowHeight = 720;

// =============================================================================
// GLFW callbacks — used by both build targets
// =============================================================================

void framebufferSizeCallback(GLFWwindow* /*window*/, int width, int height)
{
    glViewport(0, 0, width, height);
}

void mouseButtonCallback(GLFWwindow* /*window*/, int button, int action, int /*mods*/)
{
    Input::OnMouseButton(button, action);
}

void cursorPosCallback(GLFWwindow* /*window*/, double x, double y)
{
    Input::OnCursorPos(x, y);
}

// =============================================================================
// Shared gameplay helpers
// =============================================================================

// Baseline content for a freshly created scene (no saved file present).
void PopulateDefaultScene(Scene& scene)
{
    Entity camera = scene.CreateEntity("Main Camera");
    camera.AddComponent<CameraComponent>();
    camera.GetComponent<TransformComponent>().Position = glm::vec3(0.0f, 2.0f, 6.0f);

    Entity sun = scene.CreateEntity("Sun");
    auto& light  = sun.AddComponent<LightComponent>();
    light.Type      = LightType::Directional;
    light.Intensity = 3.0f;
    sun.GetComponent<TransformComponent>().Rotation = glm::vec3(-45.0f, 30.0f, 0.0f);

    Entity ground = scene.CreateEntity("Ground");
    auto& mesh   = ground.AddComponent<MeshComponent>();
    mesh.CastShadows = false;
    ground.GetComponent<TransformComponent>().Scale = glm::vec3(10.0f, 1.0f, 10.0f);
    // Static world geometry: the collider-only entity receives an implicit
    // static rigid body from PhysicsSystem::SyncBodies on Play.
    ground.AddComponent<BoxCollider3DComponent>();
}

// Drives NativeScriptComponent::OnUpdate every Play frame.
// On the very first Play tick an uninitialised Instance is heap-allocated via
// InstantiateScript(), given its entity/scene context, and kicked with OnCreate().
void UpdatePlayRuntime(float deltaTime, Scene* scene)
{
    if (!scene) return;

    auto& reg = scene->GetRegistry();
    reg.view<NativeScriptComponent>().each(
        [deltaTime, scene](entt::entity entity, NativeScriptComponent& nsc)
        {
            if (!nsc.InstantiateScript) return;

            if (!nsc.Instance)
            {
                nsc.Instance = nsc.InstantiateScript();
                nsc.Instance->SetContext(entity, scene);

                // Register replicated entities with the NetworkManager so they
                // receive a stable NetId and have cross-DLL function pointers wired.
                if (nsc.Instance->IsNetworkReplicated_())
                    fadix::NetworkManager::Get().RegisterEntity(nsc.Instance);

#ifdef FADIX_EDITOR
                // Inject inspector-edited property overrides from ScriptComponent
                // before OnCreate() fires so scripts see their tweaked defaults.
                if (auto* sc = scene->GetRegistry().try_get<ScriptComponent>(entity))
                {
                    auto liveProps = nsc.Instance->GetProperties();
                    for (auto& desc : liveProps)
                    {
                        auto it = sc->PropertyOverrides.find(desc.Name);
                        if (it == sc->PropertyOverrides.end()) continue;
                        const fadix::PropertyValue& val = it->second;
                        switch (desc.Type)
                        {
                        case fadix::PropertyType::Int:
                            *static_cast<int*>(desc.DataPtr) = val.IntVal;
                            break;
                        case fadix::PropertyType::Float:
                            *static_cast<float*>(desc.DataPtr) = val.FloatVal;
                            break;
                        case fadix::PropertyType::Vec3:
                            *static_cast<glm::vec3*>(desc.DataPtr) = val.Vec3Val;
                            break;
                        case fadix::PropertyType::String:
                            *static_cast<std::string*>(desc.DataPtr) = val.StringVal;
                            break;
                        }
                    }
                }
#endif

                nsc.Instance->OnCreate();
            }

            nsc.Instance->OnUpdate(deltaTime);
        });
}

// =============================================================================
// Editor-only helpers and types (stripped from fadix_player)
// =============================================================================
#ifdef FADIX_EDITOR

constexpr const char* kEditorWindowTitle = "Fadix Engine | Initializing...";

// AppContext bundles everything the GLFW key callback needs in one pointer.
struct AppContext
{
    EditorLayout* layout;
    EngineState*  state;
};

void keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int mods)
{
    Input::OnKey(key, action);

    auto* ctx = static_cast<AppContext*>(glfwGetWindowUserPointer(window));
    if (!ctx) return;

    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);

    if (*ctx->state != EngineState::Editor || action != GLFW_PRESS) return;

    if (key == GLFW_KEY_SPACE && (mods & GLFW_MOD_CONTROL))
        ctx->layout->ToggleContentDrawer();

    if (key == GLFW_KEY_N && (mods & GLFW_MOD_CONTROL))
        ctx->layout->RequestNewScene();
}

// Builds the scene for a freshly opened project.
std::unique_ptr<Scene> MakeActiveScene(const std::filesystem::path& startupManifest)
{
    auto scene = std::make_unique<Scene>();

    std::error_code ec;
    if (!startupManifest.empty() &&
        std::filesystem::exists(startupManifest, ec) &&
        scene->LoadFromFile(startupManifest))
    {
        std::cout << "[Fadix] Scene loaded — "
                  << startupManifest.generic_string() << '\n';
    }
    else
    {
        PopulateDefaultScene(*scene);
    }
    return scene;
}

// Runs the VFS watcher and logs any directory changes (Edit mode only).
void TickAssetDirectory(float deltaTime, fadix::FileSystem& fileSystem)
{
    if (fileSystem.Tick(deltaTime))
    {
        std::cout << "[Fadix] Asset directory changed — index refreshed ("
                  << fileSystem.IndexedFileCount() << " files)\n";
    }
}

#endif // FADIX_EDITOR

// =============================================================================
// Standalone-only key callback — minimal, no editor layout references
// =============================================================================
#ifdef FADIX_STANDALONE

void keyCallback(GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/)
{
    Input::OnKey(key, action);
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
        glfwSetWindowShouldClose(window, GLFW_TRUE);
}

#endif // FADIX_STANDALONE

} // anonymous namespace

// =============================================================================
// FADIX_EDITOR — full editor entry point
// =============================================================================
#ifdef FADIX_EDITOR

int main()
{
    // -----------------------------------------------------------------
    // 1. Initialize GLFW
    // -----------------------------------------------------------------
    if (!glfwInit())
    {
        std::cerr << "[Fadix] Failed to initialize GLFW\n";
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // -----------------------------------------------------------------
    // 2. Create window
    // -----------------------------------------------------------------
    GLFWwindow* window = glfwCreateWindow(
        kWindowWidth, kWindowHeight, kEditorWindowTitle, nullptr, nullptr);
    if (!window)
    {
        std::cerr << "[Fadix] Failed to create GLFW window\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);

    // -----------------------------------------------------------------
    // 3. Load OpenGL function pointers (GLAD)
    // -----------------------------------------------------------------
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
    {
        std::cerr << "[Fadix] Failed to initialise GLAD\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // -----------------------------------------------------------------
    // 4. Engine subsystems + GLFW callbacks
    // -----------------------------------------------------------------
    EngineState       engineState  = EngineState::Launcher;
    EditorLayout      editorLayout;           // loads editor/engine settings JSON
    EngineContext     simCtx;                 // Play / Pause / Stop state machine
    PhysicsSystem     physicsSystem;          // Box3D world wrapper
    ProjectManager    projectManager;
    fadix::FileSystem fileSystem;

    editorLayout.SetFileSystem(&fileSystem);
    editorLayout.SetEngineContext(&simCtx); // enables DLL hot-reload after async compiles
    editorLayout.SetPhysicsSystem(&physicsSystem);

    fadix::NetworkManager::Get().Init();

    bool vsyncApplied = editorLayout.GetEngineSettings().VSync;
    glfwSwapInterval(vsyncApplied ? 1 : 0);

    AppContext appCtx{ &editorLayout, &engineState };
    glfwSetWindowUserPointer(window, &appCtx);
    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);

    // -----------------------------------------------------------------
    // 5. Window icon (disk fallback -> embedded asset fallback)
    // -----------------------------------------------------------------
    {
        int iconW = 0, iconH = 0, iconC = 0;
        stbi_uc* px = stbi_load("assets/branding/logo.png", &iconW, &iconH, &iconC, 4);

#ifdef FADIX_EDITOR
        // Embedded monolithic fallback — load from compiled-in binary data
        if (!px)
        {
            const auto* entry = fadix::embedded::FindBinaryAsset("branding/logo.png");
            if (entry)
            {
                px = stbi_load_from_memory(entry->Data,
                                          static_cast<int>(entry->Size),
                                          &iconW, &iconH, &iconC, 4);
            }
        }
#endif

        if (px)
        {
            GLFWimage icon{ iconW, iconH, px };
            glfwSetWindowIcon(window, 1, &icon);
            stbi_image_free(px);
            std::cout << "[Fadix] Icon loaded — " << iconW << 'x' << iconH << '\n';
        }
        else
        {
            std::cerr << "[Fadix] Warning: icon not found — " << stbi_failure_reason() << '\n';
        }
    }

    // -----------------------------------------------------------------
    // 6. Initialize ImGui
    // -----------------------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef NDEBUG
    io.ConfigDebugHighlightIdConflicts = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
#endif

    // Font 0 (16 px) — primary UI text
    // Font 1 (24 px) — launcher title / headings
    bool fontOk =
        io.Fonts->AddFontFromFileTTF("assets/fonts/Roboto-Medium.ttf", 16.0f) != nullptr;

#ifdef FADIX_EDITOR
    // Embedded monolithic fallback — load font from compiled-in binary data
    if (!fontOk)
    {
        const auto* entry = fadix::embedded::FindBinaryAsset("fonts/Roboto-Medium.ttf");
        if (entry)
        {
            // ImGui AddFontFromMemoryTTF takes ownership — give it a heap copy
            void* buf = std::malloc(entry->Size);
            if (buf)
            {
                std::memcpy(buf, entry->Data, entry->Size);
                io.Fonts->AddFontFromMemoryTTF(buf, static_cast<int>(entry->Size),
                                               16.0f);
                fontOk = true;
                // Second font (24 px) from the same embedded source
                void* buf2 = std::malloc(entry->Size);
                if (buf2)
                {
                    std::memcpy(buf2, entry->Data, entry->Size);
                    io.Fonts->AddFontFromMemoryTTF(buf2, static_cast<int>(entry->Size),
                                                   24.0f);
                }
                std::cout << "[Fadix] Font loaded from embedded assets\n";
            }
        }
    }
#endif

    if (!fontOk)
    {
        io.Fonts->AddFontDefault();
        std::cerr << "[Fadix] Warning: Roboto-Medium.ttf not found — using default font\n";
    }
    else
    {
        // Ensure the 24 px variant exists even when loaded from disk
        io.Fonts->AddFontFromFileTTF("assets/fonts/Roboto-Medium.ttf", 24.0f);
    }

    ApplyFadixTheme();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    if (editorLayout.GetEngineSettings().LogDiagnosticsOnStartup)
    {
        std::cout << "[Fadix] Window created — " << kWindowWidth << 'x' << kWindowHeight << '\n';
        std::cout << "[Fadix] OpenGL:   "
                  << reinterpret_cast<const char*>(glGetString(GL_VERSION))  << '\n';
        std::cout << "[Fadix] Renderer: "
                  << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << '\n';
    }

    // -----------------------------------------------------------------
    // 6b. Viewport renderer + camera initialisation
    // -----------------------------------------------------------------
    if (!editorLayout.InitViewport())
    {
        std::cerr << "[Fadix] Warning: viewport renderer failed to initialise\n";
    }

    // -----------------------------------------------------------------
    // 7. Main loop
    // -----------------------------------------------------------------
    std::unique_ptr<Scene> activeScene;

    EngineState prevState     = EngineState::Launcher;
    double      lastFrameTime = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        const double currentFrameTime = glfwGetTime();
        const float  deltaTime        = static_cast<float>(currentFrameTime - lastFrameTime);
        lastFrameTime = currentFrameTime;

        glfwPollEvents();

        // ---------------------------------------------------------
        // Lifecycle: launcher <-> editor transitions
        // ---------------------------------------------------------
        if (engineState != prevState)
        {
            if (engineState == EngineState::Editor)
            {
                const std::string title =
                    "Fadix Engine  |  " + projectManager.GetActiveProjectName();
                glfwSetWindowTitle(window, title.c_str());

                const std::filesystem::path projectDir = projectManager.GetActiveProjectDir();
                std::filesystem::path assetsRoot  = "assets";
                std::filesystem::path startupScene;

                if (!projectDir.empty())
                {
                    editorLayout.SetProjectRoot(projectDir);
                    assetsRoot   = projectDir / "Assets";
                    startupScene = projectDir /
                        editorLayout.GetProjectSettings().DefaultStartupScene;
                }

                fileSystem.SetRoot(assetsRoot);
                std::cout << "[Fadix] Asset directory mounted — "
                          << fileSystem.GetRoot().generic_string()
                          << " (" << fileSystem.IndexedFileCount() << " files)\n";

                editorLayout.SetScene(nullptr);
                activeScene = MakeActiveScene(startupScene);
                editorLayout.SetScene(activeScene.get());

                // If a previous session compiled GameplayScripts.dll, load it
                // now so assigned scripts run without requiring a recompile.
                {
                    std::error_code ec;
                    if (std::filesystem::exists(
                            fadix::ScriptCompiler::kOutputDllPath, ec))
                    {
                        simCtx.ReloadScriptLibrary(activeScene.get(), &physicsSystem);
                        std::cout << "[Fadix] Existing script library loaded\n";
                    }
                }
            }
            else
            {
                if (simCtx.GetSceneState() != SceneState::Edit && activeScene)
                    simCtx.OnStop(activeScene);

                glfwSetWindowTitle(window, "Fadix Engine  |  Project Browser");
                editorLayout.SetScene(nullptr);
                activeScene.reset();
            }
            prevState = engineState;
        }

        // New scene request (File → New Scene / Ctrl+N)
        if (editorLayout.ConsumeNewSceneRequest())
        {
            if (simCtx.GetSceneState() != SceneState::Edit && activeScene)
                simCtx.OnStop(activeScene);

            editorLayout.SetScene(nullptr);
            activeScene = std::make_unique<Scene>();
            PopulateDefaultScene(*activeScene);
            editorLayout.SetScene(activeScene.get());
            editorLayout.SetSimulationState(SceneState::Edit);
            std::cout << "[Fadix] New scene created\n";
        }

        // ---- Simulation state machine requests (toolbar buttons) ---------
        if (editorLayout.ConsumePlayRequest() && activeScene)
        {
            // Wire NativeScriptComponents for any scripts assigned since the
            // last compile so they instantiate on the first Play tick.
            simCtx.EnsureScriptBindings(activeScene.get());
            simCtx.OnPlay(activeScene);
            // Initialise the physics world and build bodies for existing ECS data.
            physicsSystem.Init();
            physicsSystem.SyncBodies(activeScene->GetRegistry());
            std::cout << "[Fadix] Simulation started\n";
        }

        if (editorLayout.ConsumePauseRequest())
        {
            simCtx.OnPause();
            std::cout << "[Fadix] Simulation "
                      << (simCtx.GetSceneState() == SceneState::Pause ? "paused" : "resumed")
                      << "\n";
        }

        if (editorLayout.ConsumeStopRequest() && activeScene)
        {
            // Tear down the physics world before the scene is replaced.
            physicsSystem.Shutdown();
            simCtx.OnStop(activeScene);
            editorLayout.SetScene(activeScene.get());
            std::cout << "[Fadix] Simulation stopped — editor scene restored\n";
        }

        editorLayout.SetSimulationState(simCtx.GetSceneState());

        const fadix::EngineSettings& engineSettings = editorLayout.GetEngineSettings();
        if (engineSettings.VSync != vsyncApplied)
        {
            vsyncApplied = engineSettings.VSync;
            glfwSwapInterval(vsyncApplied ? 1 : 0);
        }

        // ---------------------------------------------------------
        // Update pass
        // ---------------------------------------------------------
        if (engineState == EngineState::Editor)
        {
            editorLayout.UpdateCamera(deltaTime, window);

            switch (simCtx.GetSceneState())
            {
            case SceneState::Edit:
                TickAssetDirectory(deltaTime, fileSystem);
                break;

            case SceneState::Play:
                // Reconcile bodies for components added/removed during Play
                // (cheap no-op when nothing changed).
                physicsSystem.SyncBodies(activeScene->GetRegistry());
                // Physics pre-step: push any transform dirty-flags into Box3D.
                physicsSystem.PreStep(activeScene->GetRegistry());
                // Advance the simulation (fixed-timestep accumulator inside).
                physicsSystem.Step(deltaTime);
                // Physics post-step: read back simulated positions/rotations.
                physicsSystem.PostStep(activeScene->GetRegistry());
                // Network tick: pump ENet events, broadcast snapshots (20 Hz), apply corrections.
                fadix::NetworkManager::Get().Tick(deltaTime, &activeScene->GetRegistry());
                // Script update (runs after physics/network so scripts see final positions).
                UpdatePlayRuntime(deltaTime, activeScene.get());
                break;

            case SceneState::Pause:
                break;
            }
        }

        // ---------------------------------------------------------
        // UI pass (ImGui dockspace + all editor panels)
        // ---------------------------------------------------------
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (engineState == EngineState::Launcher)
        {
            projectManager.DrawLauncher(engineState);
        }
        else
        {
            editorLayout.RenderWorkspace(window, engineState);
        }

        if (editorLayout.WantsExit())
            glfwSetWindowShouldClose(window, GLFW_TRUE);

        ImGui::Render();
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);

        // Frame limiter
        if (!engineSettings.VSync && engineSettings.LimitFramerate &&
            engineSettings.TargetFramerate > 0)
        {
            const double frameBudget = 1.0 / engineSettings.TargetFramerate;
            const double elapsed     = glfwGetTime() - currentFrameTime;
            if (elapsed < frameBudget)
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(frameBudget - elapsed));
        }
    }

    // -----------------------------------------------------------------
    // 8. Shutdown
    // -----------------------------------------------------------------
    std::cout << "[Fadix] Shutting down...\n";
    editorLayout.SetScene(nullptr);
    activeScene.reset();
    fadix::NetworkManager::Get().Shutdown();
    editorLayout.ShutdownViewport();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

// =============================================================================
// FADIX_STANDALONE — stripped player entry point
// =============================================================================
#elif defined(FADIX_STANDALONE)

int main(int argc, char** argv)
{
    // -----------------------------------------------------------------------
    // Resolve the base directory from which all relative asset paths are
    // computed.  argv[0]'s parent directory is preferred so that running the
    // player from a terminal with an arbitrary CWD still finds assets.
    // Falls back to current_path() — which is set to the exe's directory by
    // most OS launchers (double-click, desktop shortcut, Start Menu pin).
    // This lets a zipped export (fadix_player.exe + assets/) run on any
    // machine without path configuration.
    // -----------------------------------------------------------------------
    std::filesystem::path exeDir;
    if (argc > 0 && argv[0] != nullptr)
    {
        const std::filesystem::path p(argv[0]);
        if (p.has_parent_path() && p.parent_path() != std::filesystem::path("."))
            exeDir = p.parent_path();
    }
    if (exeDir.empty())
        exeDir = std::filesystem::current_path();

    // -----------------------------------------------------------------------
    // 1. Engine settings (VSync, frame limiter)
    //    Read from fadix_engine_settings.json if present; otherwise defaults
    //    (VSync=true, no frame cap) are safe for any distribution target.
    // -----------------------------------------------------------------------
    fadix::EngineSettings engineSettings;
    fadix::LoadEngineSettings("fadix_engine_settings.json", engineSettings);

    // -----------------------------------------------------------------------
    // 2. GLFW + window
    // -----------------------------------------------------------------------
    if (!glfwInit())
    {
        std::cerr << "[Fadix Player] Failed to initialize GLFW\n";
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(
        kWindowWidth, kWindowHeight, "Fadix Player", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "[Fadix Player] Failed to create GLFW window\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(engineSettings.VSync ? 1 : 0);

    // -----------------------------------------------------------------------
    // 3. OpenGL function pointers (GLAD)
    // -----------------------------------------------------------------------
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
    {
        std::cerr << "[Fadix Player] Failed to initialise GLAD\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // -----------------------------------------------------------------------
    // 4. GLFW event callbacks (minimal — no editor layout references)
    // -----------------------------------------------------------------------
    glfwSetKeyCallback(window, keyCallback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetCursorPosCallback(window, cursorPosCallback);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);

    // -----------------------------------------------------------------------
    // 5. Window icon
    // -----------------------------------------------------------------------
    {
        const std::filesystem::path iconPath = exeDir / "assets" / "branding" / "logo.png";
        int      iconW = 0, iconH = 0, iconC = 0;
        stbi_uc* px   = stbi_load(iconPath.string().c_str(), &iconW, &iconH, &iconC, 4);
        if (px)
        {
            GLFWimage icon{ iconW, iconH, px };
            glfwSetWindowIcon(window, 1, &icon);
            stbi_image_free(px);
        }
    }

    // -----------------------------------------------------------------------
    // 6. Viewport renderer
    //    The forward-lighting + grid pipeline runs into an off-screen FBO;
    //    the player blits it directly to the swapchain each frame.
    // -----------------------------------------------------------------------
    fadix::ViewportRenderer viewport;
    fadix::EditorCamera     camera;

    if (!viewport.Initialise())
    {
        std::cerr << "[Fadix Player] Viewport renderer failed to initialise\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    // Resolve mesh / texture paths relative to the player's own directory.
    viewport.SetAssetRoot(exeDir);

    // -----------------------------------------------------------------------
    // 7. Scene load — deserialise from assets/scenes/default.fadix,
    //    fall back to built-in defaults if the file is absent or corrupt.
    // -----------------------------------------------------------------------
    auto activeScene = std::make_unique<Scene>();
    {
        const std::filesystem::path scenePath =
            exeDir / "assets" / "scenes" / "default.fadix";

        // Non-owning alias so SceneSerializer can hold a shared_ptr without
        // taking ownership away from the unique_ptr below.
        std::shared_ptr<Scene> sceneRef(activeScene.get(), [](Scene* /*s*/) {});
        SceneSerializer serializer(sceneRef);

        if (serializer.Deserialize(scenePath.generic_string()))
        {
            std::cout << "[Fadix Player] Scene loaded — "
                      << scenePath.generic_string() << '\n';
        }
        else
        {
            std::cerr << "[Fadix Player] No scene file at \""
                      << scenePath.generic_string()
                      << "\" — using built-in defaults\n";
            PopulateDefaultScene(*activeScene);
        }
    }

    if (engineSettings.LogDiagnosticsOnStartup)
    {
        std::cout << "[Fadix Player] OpenGL:   "
                  << reinterpret_cast<const char*>(glGetString(GL_VERSION))  << '\n';
        std::cout << "[Fadix Player] Renderer: "
                  << reinterpret_cast<const char*>(glGetString(GL_RENDERER)) << '\n';
    }

    // -----------------------------------------------------------------------
    // 7b. Physics initialisation for the standalone player
    // -----------------------------------------------------------------------
    PhysicsSystem playerPhysics;
    playerPhysics.Init();
    playerPhysics.SyncBodies(activeScene->GetRegistry());

    // -----------------------------------------------------------------------
    // 7c. Network initialisation (no-op if FADIX_NET_INCLUDED is not defined)
    // -----------------------------------------------------------------------
    fadix::NetworkManager::Get().Init();

    // -----------------------------------------------------------------------
    // 8. Game loop — permanently in Play mode, no Edit / Pause transitions
    // -----------------------------------------------------------------------
    double lastFrameTime = glfwGetTime();

    while (!glfwWindowShouldClose(window))
    {
        const double currentFrameTime = glfwGetTime();
        const float  deltaTime = static_cast<float>(currentFrameTime - lastFrameTime);
        lastFrameTime = currentFrameTime;

        glfwPollEvents();

        // Physics update (fixed-step accumulator, deterministic).
        playerPhysics.PreStep(activeScene->GetRegistry());
        playerPhysics.Step(deltaTime);
        playerPhysics.PostStep(activeScene->GetRegistry());

        // Network tick: pump ENet events, broadcast snapshots (20 Hz), apply corrections.
        fadix::NetworkManager::Get().Tick(deltaTime, &activeScene->GetRegistry());

        // Gameplay simulation tick (no SceneState gating — always running)
        UpdatePlayRuntime(deltaTime, activeScene.get());

        // ---- Render --------------------------------------------------------
        int fbW = 0, fbH = 0;
        glfwGetFramebufferSize(window, &fbW, &fbH);

        if (fbW > 0 && fbH > 0)
        {
            viewport.Resize(fbW, fbH);

            // Render from the scene's primary camera (game POV); the free
            // editor camera is only a fallback for camera-less scenes.
            glm::mat4 camView, camProj;
            glm::vec3 camPos;
            const float aspect =
                static_cast<float>(fbW) / static_cast<float>(fbH);
            if (fadix::ViewportRenderer::GetPrimaryCameraMatrices(
                    activeScene.get(), aspect, camView, camProj, camPos))
            {
                fadix::RenderOptions options;
                options.DrawGrid = false;
                viewport.Render(camView, camProj, camPos,
                                activeScene.get(), options);
            }
            else
            {
                viewport.Render(camera, activeScene.get(), /*drawGrid=*/false);
            }

            // Blit the FBO colour attachment directly to the window swapchain.
            // No ImGui overhead: the rendered image goes straight to the screen
            // at the native framebuffer resolution.
            glBindFramebuffer(GL_READ_FRAMEBUFFER, viewport.GetFramebuffer());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glViewport(0, 0, fbW, fbH);
            glBlitFramebuffer(
                0, 0, fbW, fbH,
                0, 0, fbW, fbH,
                GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        glfwSwapBuffers(window);

        // Optional frame limiter (respects fadix_engine_settings.json)
        if (!engineSettings.VSync && engineSettings.LimitFramerate &&
            engineSettings.TargetFramerate > 0)
        {
            const double frameBudget =
                1.0 / static_cast<double>(engineSettings.TargetFramerate);
            const double elapsed = glfwGetTime() - currentFrameTime;
            if (elapsed < frameBudget)
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(frameBudget - elapsed));
        }
    }

    // -----------------------------------------------------------------------
    // 9. Shutdown
    // -----------------------------------------------------------------------
    std::cout << "[Fadix Player] Shutting down...\n";
    activeScene.reset();
    fadix::NetworkManager::Get().Shutdown();
    viewport.Shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

#endif // FADIX_STANDALONE
