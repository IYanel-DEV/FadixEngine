#pragma once

#include "assets/ScriptAsset.hpp"
#include "scripting/LuaVM.hpp"
#include "scripting/NativeScriptLoader.hpp"

#include <entt/entity/fwd.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fadix
{
class AudioEngine;

// Runs the scripts attached via ScriptComponent against a play-mode world.
// Owns one LuaVM and the per-entity instance table. C++ (native) scripts are
// resolved but not executed here yet; that path lives in NativeScriptLoader.
class ScriptRunner
{
public:
    struct ResolvedScript
    {
        std::string Source;
        ScriptLanguage Language{ScriptLanguage::Lua};
        std::filesystem::path SourcePath; // needed to compile native (C++) scripts
    };
    // Looks a script up by name and returns its current source. Returning the
    // live editor buffer means unsaved edits run on Play.
    using SourceResolver = std::function<std::optional<ResolvedScript>(const std::string& name)>;
    using Logger = std::function<void(const std::string& message, const char* severity)>;

    ScriptRunner();
    ~ScriptRunner();
    ScriptRunner(ScriptRunner&&) noexcept;
    ScriptRunner& operator=(ScriptRunner&&) noexcept;
    ScriptRunner(const ScriptRunner&) = delete;
    ScriptRunner& operator=(const ScriptRunner&) = delete;

    void SetLogger(Logger logger);
    // Optional: enables the C++ (native) script path. Not owned; must outlive
    // the runner. When null, native scripts are skipped.
    void SetNativeLoader(NativeScriptLoader* loader) { m_NativeLoader = loader; }
    void BindAudio(AudioEngine* engine);

    // Install the gameplay world callbacks exposed to scripts. Kept across VM
    // resets. Leave unset callbacks for smoke targets.
    void SetGameCallbacks(
        std::function<std::optional<entt::entity>(const std::string&, float, float, float)> spawnPrefab,
        std::function<void(const std::string&)> loadScene,
        std::function<bool(const std::string&)> writeSave = {},
        std::function<bool(const std::string&)> loadSave = {});

    // Compile + instantiate every enabled script and fire OnStart.
    void Start(entt::registry& registry, const SourceResolver& resolver);
    // Compile + instantiate + OnStart the scripts on one entity (used by
    // Prefab.spawn so a freshly spawned entity's scripts come alive mid-play).
    void StartEntity(entt::registry& registry, entt::entity entity, const SourceResolver& resolver);
    // Fire OnUpdate on every live instance, then apply deferred destroys.
    void Update(entt::registry& registry, float deltaTime);
    // Fire OnDestroy, drop all instances, and reset the VM.
    void Stop(entt::registry& registry);

private:
    struct Instance
    {
        entt::entity Entity;
        int Id;
    };
    struct NativeInstance
    {
        entt::entity Entity;
        NativeScriptLoader::Loaded Loaded;
    };
    void DispatchAnimationEvents(entt::registry& registry);
    void ApplyPendingDestroy(entt::registry& registry);

    LuaVM m_Vm;
    ScriptWorldApi m_WorldApi;
    NativeScriptLoader* m_NativeLoader{nullptr};
    std::vector<Instance> m_Instances;
    std::vector<NativeInstance> m_NativeInstances;
    std::vector<entt::entity> m_PendingDestroy;
    Logger m_Logger;
    bool m_Started{false};
};
}
