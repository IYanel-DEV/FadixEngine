#pragma once

#include <entt/entity/fwd.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fadix
{
class AudioEngine;

// The live world context behind the gameplay globals (World / Prefab / Scene).
// Owned by ScriptRunner; Registry/PendingDestroy are refreshed each Start/Update
// so scripts always see the current play world, while the two callbacks are
// installed once by the play session (null on smoke targets = the API no-ops).
struct ScriptWorldApi
{
    entt::registry* Registry{nullptr};
    std::vector<entt::entity>* PendingDestroy{nullptr};
    // Instantiate a prefab at (x,y,z); returns the new root entity or nullopt.
    std::function<std::optional<entt::entity>(const std::string& path, float x, float y, float z)>
        SpawnPrefab;
    // Queue a level transition to the project-relative scene path.
    std::function<void(const std::string& path)> LoadScene;
};

// A lightweight, non-owning handle to one entity in a specific world registry.
// It is created fresh for each lifecycle call and handed to Lua so scripts can
// read and mutate the entity. Transform access goes through TransformComponent.
struct ScriptEntityHandle
{
    entt::registry* Registry{nullptr};
    entt::entity Entity{};
    std::vector<entt::entity>* PendingDestroy{nullptr};
};

// Hidden so sol2 / Lua headers never leak into the rest of the tree.
class LuaVMImpl;

// Owns a single Lua state. Scripts are compiled by name, then instantiated into
// isolated per-entity environments so two entities running the same script do
// not share globals. All sol2 usage lives in the .cpp behind FADIX_ENABLE_LUA;
// when the option is off every method is a safe no-op.
class LuaVM
{
public:
    using LogFn = std::function<void(const std::string& message, const char* severity)>;

    LuaVM();
    ~LuaVM();
    LuaVM(LuaVM&&) noexcept;
    LuaVM& operator=(LuaVM&&) noexcept;
    LuaVM(const LuaVM&) = delete;
    LuaVM& operator=(const LuaVM&) = delete;

    // Where print(...) output and script errors are sent. Severity is one of
    // "info" / "warn" / "error", matching EditorApplication::Log.
    void SetLogger(LogFn logger);

    // Compile `source` under `name`, replacing any previous chunk with that
    // name. Returns false and records LastError on a syntax error.
    bool Compile(const std::string& name, const std::string& source);

    // Build a fresh instance environment from a compiled script. Returns a
    // non-negative instance id, or -1 if the name is unknown or the chunk
    // raised an error while defining its functions.
    [[nodiscard]] int Instantiate(const std::string& name);

    void CallStart(int instance, const ScriptEntityHandle& entity);
    void CallUpdate(int instance, const ScriptEntityHandle& entity, float deltaTime);
    void CallDestroy(int instance, const ScriptEntityHandle& entity);
    void DestroyInstance(int instance);

    // Drop every instance and compiled chunk and rebuild a clean Lua state.
    void Reset();

    // Bind global `audio` table to an engine (or clear it when null). Re-applied on Reset.
    void BindAudio(AudioEngine* engine);

    // Point the World/Prefab/Scene globals at a context. Not owned; must outlive
    // the VM. Re-applied on Reset. Null leaves the tables present but inert.
    void SetWorldApi(ScriptWorldApi* api);

    [[nodiscard]] bool HasError() const;
    [[nodiscard]] const std::string& LastError() const;

private:
    std::unique_ptr<LuaVMImpl> m_Impl;
};
}
