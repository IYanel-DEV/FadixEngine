#pragma once

#include "engine/physics/IPhysicsWorld.hpp"
#include "scripting/ScriptRunner.hpp"

#include <entt/entity/fwd.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fadix
{
class AudioEngine;
class IWorld;
class SaveGameService;

enum class PlayState
{
    Stopped,
    Playing,
    Paused
};

class PlaySession final
{
public:
    explicit PlaySession(float fixedDeltaSeconds = 1.0F / 60.0F);
    ~PlaySession();

    [[nodiscard]] bool Start(const IWorld& authoredWorld, std::unique_ptr<IPhysicsWorld> physics);
    void Stop() noexcept;

    // Wire the script runtime. Scripts run on Play using the resolver to fetch
    // sources; the optional native loader enables the C++ path. Set once after
    // the project opens; safe to leave unset (scripts simply do not run).
    void SetScriptContext(
        ScriptRunner::SourceResolver resolver,
        ScriptRunner::Logger logger,
        NativeScriptLoader* nativeLoader = nullptr);
    void BindAudio(AudioEngine* engine);

    // Enables the gameplay world API. `createPhysics`
    // builds a fresh physics world for the next scene; `resolvePath` maps a
    // project-relative asset path to an absolute one. Both apps wire this; leaving
    // it unset simply disables spawn/scene-load.
    void SetGameServices(
        std::function<std::unique_ptr<IPhysicsWorld>()> createPhysics,
        std::function<std::filesystem::path(const std::string&)> resolvePath,
        std::filesystem::path saveDirectory);

    void Pause() noexcept;
    void Resume() noexcept;
    void SingleStep();
    void Update(float deltaSeconds);

    [[nodiscard]] PlayState State() const noexcept;
    [[nodiscard]] IWorld* RuntimeWorld() noexcept;
    [[nodiscard]] const IWorld* RuntimeWorld() const noexcept;

private:
    void Tick();
    std::optional<entt::entity> SpawnPrefab(const std::string& path, float x, float y, float z);
    void PerformSceneLoad();
    bool RequestSave(const std::string& slot);
    bool RequestLoad(const std::string& slot);
    void PerformSave();
    void PerformSaveLoad();
    std::unique_ptr<IWorld> m_RuntimeWorld;
    std::unique_ptr<IPhysicsWorld> m_Physics;
    ScriptRunner m_Scripts;
    ScriptRunner::SourceResolver m_ScriptResolver;
    ScriptRunner::Logger m_Logger;
    std::function<std::unique_ptr<IPhysicsWorld>()> m_CreatePhysics;
    std::function<std::filesystem::path(const std::string&)> m_ResolvePath;
    std::unique_ptr<SaveGameService> m_SaveGames;
    std::string m_PendingSceneLoad;
    std::string m_PendingSave;
    std::string m_PendingSaveLoad;
    PlayState m_State{PlayState::Stopped};
    float m_FixedDeltaSeconds;
    float m_Accumulator{0.0F};
};
}
